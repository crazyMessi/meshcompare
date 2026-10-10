#include "render_scene_context.h"

#include <array>
#include <cmath>
#include <exception>
#include <memory>
#include <limits>
#include <vector>

#include <QSet>

#include <common/mlexception.h>
#include <common/ml_document/mesh_document.h>
#include <common/ml_document/mesh_model.h>
#include <common/ml_shared_data_context/ml_scene_gl_shared_data_context.h>
#include <common/ml_shared_data_context/ml_shared_data_context.h>
#include <vcg/complex/allocate.h>
#include <wrap/qt/qt_thread_safe_memory_info.h>

namespace MeshCompareRenderDetail
{
void ensureFaceColorDisabled(MeshModel& model) noexcept
{
    model.clearDataMask(MeshModel::MM_FACECOLOR);
    // EnableColor() sets the OCF flag before resizing its storage, while
    // MeshModel sets MM_FACECOLOR only after EnableColor() returns.  A failed
    // first allocation can therefore leave the flag set with no mask bit for
    // clearDataMask() to observe.
    if (model.cm.face.IsColorEnabled())
        model.cm.face.DisableColor();
    Q_ASSERT(!model.hasDataMask(MeshModel::MM_FACECOLOR));
    Q_ASSERT(!model.cm.face.IsColorEnabled());
}
} // namespace MeshCompareRenderDetail

namespace
{
class MeshLabRenderPresentationPublisher final
    : public IRenderPresentationPublisher
{
public:
    void publish(
        MLSceneGLSharedDataContext& sharedContext,
        int modelId,
        const MLRenderingData& renderingData,
        RenderPresentationUpload upload) override
    {
        sharedContext.setRenderingDataPerAllMeshViews(modelId, renderingData);
        if (upload.faceColorsChanged || upload.vertexColorsChanged) {
            MLRenderingData::RendAtts changedAttributes;
            changedAttributes[MLRenderingData::ATT_NAMES::ATT_FACECOLOR] =
                upload.faceColorsChanged;
            changedAttributes[MLRenderingData::ATT_NAMES::ATT_VERTCOLOR] =
                upload.vertexColorsChanged;
            sharedContext.meshAttributesUpdated(
                modelId, false, changedAttributes);
        }
        // MeshLab's implementation always returns false even after doing work;
        // backend exceptions are the only observable failure boundary here.
        sharedContext.manageBuffers(modelId);
    }
};

vcg::Color4b meshColor(const QColor& color);

bool isFinite(const MeshPoint3D& value)
{
    return std::isfinite(value[0]) && std::isfinite(value[1]) &&
           std::isfinite(value[2]);
}

OperationResult copyGeometry(
    MeshDocument& document,
    MLSceneGLSharedDataContext& sharedContext,
    const SceneMesh& sceneMesh,
    const IMeshGeometryView& geometry,
    QHash<MeshId, int>& modelIds)
{
    const int vertexCount = geometry.vertexCount();
    const int faceCount = geometry.faceCount();
    if (vertexCount <= 0 || faceCount < 0) {
        return OperationResult::failure(
            QStringLiteral("%1 has invalid geometry counts.").arg(sceneMesh.label));
    }
    if (modelIds.contains(sceneMesh.id)) {
        return OperationResult::failure(
            QStringLiteral("Scene contains duplicate mesh IDs."));
    }

    MeshModel* model = document.addNewMesh(QString(), sceneMesh.label, false);
    std::vector<CMeshO::VertexPointer> vertices(static_cast<std::size_t>(vertexCount));
    CMeshO::VertexIterator vertex =
        vcg::tri::Allocator<CMeshO>::AddVertices(model->cm, static_cast<std::size_t>(vertexCount));
    for (int index = 0; index < vertexCount; ++index, ++vertex) {
        const MeshPoint3D position = geometry.vertexPosition(index);
        const MeshPoint3D normal = geometry.vertexNormal(index);
        if (!isFinite(position) || !isFinite(normal)) {
            return OperationResult::failure(
                QStringLiteral("%1 contains non-finite geometry.").arg(sceneMesh.label));
        }
        vertices[static_cast<std::size_t>(index)] = &*vertex;
        vertex->P() = CMeshO::CoordType(position[0], position[1], position[2]);
        vertex->N() = CMeshO::CoordType(normal[0], normal[1], normal[2]);
        if (geometry.hasVertexColors()) {
            const QColor color = geometry.vertexColor(index);
            if (!color.isValid())
                return OperationResult::failure(QStringLiteral("Invalid source vertex color."));
            vertex->C() = meshColor(color);
        }
    }

    if (geometry.hasVertexColors())
        model->updateDataMask(MeshModel::MM_VERTCOLOR);
    if (geometry.hasFaceColors())
        model->updateDataMask(MeshModel::MM_FACECOLOR);
    if (geometry.hasFaceTexCoords())
        model->updateDataMask(MeshModel::MM_WEDGTEXCOORD);

    CMeshO::FaceIterator face =
        vcg::tri::Allocator<CMeshO>::AddFaces(model->cm, static_cast<std::size_t>(faceCount));
    for (int index = 0; index < faceCount; ++index, ++face) {
        const std::array<int, 3> indices = geometry.faceVertexIndices(index);
        for (const int vertexIndex : indices) {
            if (vertexIndex < 0 || vertexIndex >= vertexCount) {
                return OperationResult::failure(
                    QStringLiteral("%1 contains an invalid face index.").arg(sceneMesh.label));
            }
        }
        face->V(0) = vertices[static_cast<std::size_t>(indices[0])];
        face->V(1) = vertices[static_cast<std::size_t>(indices[1])];
        face->V(2) = vertices[static_cast<std::size_t>(indices[2])];
        if (geometry.hasFaceColors()) {
            const QColor color = geometry.faceColor(index);
            if (!color.isValid())
                return OperationResult::failure(QStringLiteral("Invalid source face color."));
            face->C() = meshColor(color);
        }
        if (geometry.hasFaceTexCoords()) {
            const auto coordinates = geometry.faceTexCoords(index);
            for (int corner = 0; corner < 3; ++corner) {
                const MeshTexCoord& coordinate = coordinates[corner];
                if (!std::isfinite(coordinate.u) || !std::isfinite(coordinate.v) ||
                    coordinate.textureIndex < -1 ||
                    coordinate.textureIndex >= geometry.textureCount() ||
                    coordinate.textureIndex > std::numeric_limits<short>::max() ||
                    coordinate.textureIndex != coordinates[0].textureIndex) {
                    return OperationResult::failure(QStringLiteral("Invalid source texture coordinates."));
                }
                face->WT(corner).U() = coordinate.u;
                face->WT(corner).V() = coordinate.v;
                face->WT(corner).N() = static_cast<short>(coordinate.textureIndex);
            }
        }
    }

    vcg::tri::UpdateBounding<CMeshO>::Box(model->cm);
    if (model->cm.FN() > 0)
        vcg::tri::UpdateNormal<CMeshO>::PerFaceNormalized(model->cm);
    model->updateDataMask();
    if (!geometry.hasVertexColors())
        model->clearDataMask(MeshModel::MM_VERTCOLOR);
    sharedContext.meshInserted(model->id());
    // MeshLab initializes its texture container with a zero-name sentinel.
    // Clear it so imported texture index zero maps to the first uploaded image.
    sharedContext.deAllocateTexturesPerMesh(model->id());
    for (int index = 0; index < geometry.textureCount(); ++index) {
        QImage image = geometry.textureImage(index);
        if (image.isNull()) {
            // Unavailable source images have no textured faces, but their
            // reserved slots keep subsequent wedge texture indices stable.
            image = QImage(1, 1, QImage::Format_ARGB32);
            image.fill(Qt::white);
        }
        const GLuint texture = sharedContext.allocateTexturePerMesh(model->id(), image, 256);
        if (texture == 0)
            return OperationResult::failure(QStringLiteral("A source texture could not be uploaded."));
        QGLContext* previousContext = const_cast<QGLContext*>(QGLContext::currentContext());
        sharedContext.makeCurrent();
        glBindTexture(GL_TEXTURE_2D, texture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, geometry.textureWrapS(index));
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, geometry.textureWrapT(index));
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        sharedContext.doneCurrent();
        if (previousContext != nullptr)
            previousContext->makeCurrent();
    }

    modelIds.insert(sceneMesh.id, model->id());
    return OperationResult::success();
}

int liveFaceCount(const MeshModel& model)
{
    int count = 0;
    for (const CFaceO& face : model.cm.face) {
        if (!face.IsD())
            ++count;
    }
    return count;
}

int liveVertexCount(const MeshModel& model)
{
    int count = 0;
    for (const CVertexO& vertex : model.cm.vert) {
        if (!vertex.IsD())
            ++count;
    }
    return count;
}

vcg::Color4b meshColor(const QColor& color)
{
    return vcg::Color4b(
        color.red(), color.green(), color.blue(), color.alpha());
}

OperationResult validatePresentation(
    const ColorPresentation& presentation,
    int expectedFaceCount,
    int expectedVertexCount)
{
    switch (presentation.mode) {
    case ColorMode::Default:
        if (presentation.uniformColor.isValid() ||
            !presentation.faceColors.isEmpty() ||
            !presentation.vertexColors.isEmpty() ||
            presentation.referenceDependent) {
            return OperationResult::failure(QStringLiteral(
                "Default presentation cannot contain color payloads."));
        }
        break;
    case ColorMode::UniformColor:
        if (!presentation.uniformColor.isValid() ||
            !presentation.faceColors.isEmpty() ||
            !presentation.vertexColors.isEmpty() ||
            presentation.referenceDependent) {
            return OperationResult::failure(QStringLiteral(
                "Uniform presentation requires one valid fixed color."));
        }
        break;
    case ColorMode::PrecisionResult:
    case ColorMode::NormalAgreementResult: {
        if (presentation.uniformColor.isValid()) {
            return OperationResult::failure(QStringLiteral(
                "Analysis presentation cannot contain a fixed color."));
        }
        const bool hasFaceColors = !presentation.faceColors.isEmpty();
        const bool hasVertexColors = !presentation.vertexColors.isEmpty();
        if (hasFaceColors == hasVertexColors) {
            return OperationResult::failure(QStringLiteral(
                "Analysis presentation requires exactly one color domain."));
        }
        if (hasFaceColors &&
            presentation.faceColors.size() != expectedFaceCount) {
            return OperationResult::failure(QStringLiteral(
                "Analysis presentation face colors must match the live face count."));
        }
        if (hasVertexColors &&
            presentation.vertexColors.size() != expectedVertexCount) {
            return OperationResult::failure(QStringLiteral(
                "Analysis presentation vertex colors must match the live vertex count."));
        }
        const QVector<QColor>& colors = hasFaceColors
                                            ? presentation.faceColors
                                            : presentation.vertexColors;
        for (const QColor& color : colors) {
            if (!color.isValid()) {
                return OperationResult::failure(QStringLiteral(
                "Analysis presentation contains an invalid color."));
            }
        }
        break;
    }
    case ColorMode::VertexColor:
        if (presentation.uniformColor.isValid() ||
            !presentation.faceColors.isEmpty() ||
            presentation.vertexColors.size() != expectedVertexCount) {
            return OperationResult::failure(QStringLiteral(
                "Vertex presentation colors must match the live vertex count."));
        }
        for (const QColor& color : presentation.vertexColors) {
            if (!color.isValid()) {
                return OperationResult::failure(QStringLiteral(
                    "Vertex presentation contains an invalid color."));
            }
        }
        break;
    default:
        return OperationResult::failure(
            QStringLiteral("Color presentation mode is invalid."));
    }
    return OperationResult::success();
}

MLRenderingData desiredRenderingData(
    const MLRenderingData& source,
    const ColorPresentation& presentation)
{
    MLRenderingData result = source;
    if (presentation.mode == ColorMode::Default)
        return result;

    // User colors and analysis intentionally replace source texture/tint in
    // both triangle and point-cloud views. The source snapshot is never edited.
    for (const auto modality : {MLRenderingData::PR_SOLID, MLRenderingData::PR_POINTS}) {
        if (!result.isPrimitiveActive(modality))
            continue;
        MLRenderingData::RendAtts attributes;
        result.get(modality, attributes);
        attributes[MLRenderingData::ATT_NAMES::ATT_VERTCOLOR] =
            !presentation.vertexColors.isEmpty();
        attributes[MLRenderingData::ATT_NAMES::ATT_FACECOLOR] =
            !presentation.faceColors.isEmpty();
        attributes[MLRenderingData::ATT_NAMES::ATT_VERTTEXTURE] = false;
        attributes[MLRenderingData::ATT_NAMES::ATT_WEDGETEXTURE] = false;
        result.set(modality, attributes);
    }

    MLPerViewGLOptions options;
    result.get(options);
    options._persolid_mesh_color_enabled = false;
    options._perpoint_mesh_color_enabled = false;
    options._persolid_noshading = false;
    options._perpoint_noshading = false;
    options._persolid_fixed_color_enabled =
        presentation.mode == ColorMode::UniformColor;
    options._perpoint_fixed_color_enabled = options._persolid_fixed_color_enabled;
    if (presentation.mode == ColorMode::UniformColor) {
        options._persolid_fixed_color = meshColor(presentation.uniformColor);
        options._perpoint_fixed_color = options._persolid_fixed_color;
    }
    result.set(options);
    return result;
}

struct PreparedPresentationMutation
{
    MeshId meshId = 0;
    int modelId = -1;
    MeshModel* model = nullptr;
    bool hadFaceColorMask = false;
    bool faceColorsChanged = false;
    bool hadVertexColorMask = false;
    bool vertexColorsChanged = false;
    std::vector<vcg::Color4b> priorFaceColors;
    std::vector<vcg::Color4b> desiredFaceColors;
    std::vector<vcg::Color4b> priorVertexColors;
    std::vector<vcg::Color4b> desiredVertexColors;
    std::shared_ptr<const MLRenderingData> priorRenderingData;
    std::shared_ptr<const MLRenderingData> desiredRenderingData;
};

void restoreCpuPresentation(PreparedPresentationMutation& mutation) noexcept
{
    if (!mutation.hadFaceColorMask) {
        MeshCompareRenderDetail::ensureFaceColorDisabled(*mutation.model);
    }
    else {
        // DisableColor() clears CV but std::vector retains its capacity.
        Q_ASSERT(mutation.model->cm.face.CV.capacity() >=
                 mutation.model->cm.face.size());
        mutation.model->updateDataMask(MeshModel::MM_FACECOLOR);
        Q_ASSERT(mutation.model->cm.face.IsColorEnabled());
        Q_ASSERT(mutation.model->hasDataMask(MeshModel::MM_FACECOLOR));
        Q_ASSERT(mutation.model->cm.face.CV.size() ==
                 mutation.model->cm.face.size());
        Q_ASSERT(static_cast<std::size_t>(liveFaceCount(*mutation.model)) ==
                 mutation.priorFaceColors.size());
        std::size_t colorIndex = 0;
        for (CFaceO& face : mutation.model->cm.face) {
            if (face.IsD())
                continue;
            face.C() = mutation.priorFaceColors[colorIndex++];
        }
        Q_ASSERT(colorIndex == mutation.priorFaceColors.size());
    }

    if (!mutation.hadVertexColorMask) {
        mutation.model->clearDataMask(MeshModel::MM_VERTCOLOR);
    }
    else {
        mutation.model->updateDataMask(MeshModel::MM_VERTCOLOR);
        Q_ASSERT(static_cast<std::size_t>(liveVertexCount(*mutation.model)) ==
                 mutation.priorVertexColors.size());
        std::size_t colorIndex = 0;
        for (CVertexO& vertex : mutation.model->cm.vert) {
            if (vertex.IsD())
                continue;
            vertex.C() = mutation.priorVertexColors[colorIndex++];
        }
        Q_ASSERT(colorIndex == mutation.priorVertexColors.size());
    }
}

} // namespace

RenderSceneContext::RenderSceneContext(const RenderSceneSettings& settings)
    : RenderSceneContext(settings, nullptr)
{
}

RenderSceneContext::RenderSceneContext(
    const RenderSceneSettings& settings,
    IRenderPresentationPublisher& publisher)
    : RenderSceneContext(settings, &publisher)
{
}

RenderSceneContext::RenderSceneContext(
    const RenderSceneSettings& settings,
    IRenderPresentationPublisher* publisher)
    : settings_(settings),
      memoryInfo_(new vcg::QtThreadSafeMemoryInfo(settings_.gpuMemoryBytes)),
      document_(new MeshDocument),
      publisher_(publisher)
{
    if (publisher_ == nullptr) {
        ownedPublisher_.reset(new MeshLabRenderPresentationPublisher);
        publisher_ = ownedPublisher_.get();
    }
    // Keep temporary ownership until GL initialization has succeeded.  If it
    // throws, the context is destroyed before its referenced document and
    // memory tracker unwind.
    std::unique_ptr<MLSceneGLSharedDataContext> sharedContext(
        new MLSceneGLSharedDataContext(
            *document_,
            *memoryInfo_,
            settings_.highPrecisionRendering,
            settings_.primitivesPerBatch,
            settings_.minFacesForSmoothRendering));
    sharedContext->setHidden(true);
    if (!sharedContext->tryInitializeGL()) {
        throw MLException(
            QStringLiteral("A valid OpenGL context is required to prepare the MeshLab render scene."));
    }
    sharedContext_ = sharedContext.release();
}

RenderSceneContext::~RenderSceneContext()
{
    if (!sharedContext_.isNull()) {
        if (sharedContext_->tryInitializeGL())
            sharedContext_->deAllocateGPUSharedData();
        delete sharedContext_.data();
        sharedContext_.clear();
    }
    document_.reset();
    memoryInfo_.reset();
}

MeshDocument& RenderSceneContext::document()
{
    return *document_;
}

MLSceneGLSharedDataContext& RenderSceneContext::sharedContext()
{
    return *sharedContext_;
}

OperationResult RenderSceneContext::addResources(
    const SceneDescriptor& scene,
    const IMeshResourceProvider& resources)
{
    if (!modelIds_.isEmpty())
        return OperationResult::failure(QStringLiteral("Render resources were already added."));
    if (scene.meshes.isEmpty())
        return OperationResult::failure(QStringLiteral("Scene has no meshes to render."));

    try {
        for (const SceneMesh& sceneMesh : scene.meshes) {
            if (sceneMesh.id == 0 || sceneMesh.resourceId == 0) {
                return OperationResult::failure(
                    QStringLiteral("Scene contains an invalid mesh resource."));
            }
            const IMeshGeometryView* geometry = resources.geometry(sceneMesh.resourceId);
            if (geometry == nullptr) {
                return OperationResult::failure(
                    QStringLiteral("%1 has no renderable geometry.").arg(sceneMesh.label));
            }
            const OperationResult result = copyGeometry(
                *document_,
                *sharedContext_,
                sceneMesh,
                *geometry,
                modelIds_);
            if (!result.ok)
                return result;

            const int modelId = modelIds_.value(sceneMesh.id, -1);
            MeshModel* model = document_->getMesh(modelId);
            if (model == nullptr) {
                return OperationResult::failure(
                    QStringLiteral("The renderer-private mesh copy is unavailable."));
            }
            MLRenderingData defaultRendering;
            MLPoliciesStandAloneFunctions::suggestedDefaultPerViewRenderingData(
                model,
                defaultRendering,
                settings_.minFacesForSmoothRendering);
            MLPerViewGLOptions options;
            defaultRendering.get(options);
            options._persolid_noshading = geometry->isUnlit();
            options._perpoint_noshading = geometry->isUnlit();
            if (geometry->hasFaceTexCoords() && geometry->textureCount() > 0)
                options._persolid_fixed_color = vcg::Color4b(vcg::Color4b::White);
            defaultRendering.set(options);
            ColorPresentation sourceColors;
            if (geometry->hasVertexColors()) {
                sourceColors.vertexColors.reserve(geometry->vertexCount());
                for (int index = 0; index < geometry->vertexCount(); ++index)
                    sourceColors.vertexColors.append(geometry->vertexColor(index));
            }
            if (geometry->hasFaceColors()) {
                sourceColors.faceColors.reserve(geometry->faceCount());
                for (int index = 0; index < geometry->faceCount(); ++index)
                    sourceColors.faceColors.append(geometry->faceColor(index));
            }
            sourceColors_.insert(sceneMesh.id, std::move(sourceColors));
            const auto source = std::make_shared<const MLRenderingData>(defaultRendering);
            sourceRenderingData_.insert(sceneMesh.id, source);
            presentations_.insert(sceneMesh.id, ColorPresentation{});
            renderingData_.insert(sceneMesh.id, source);
        }
    }
    catch (const MLException& exception) {
        return OperationResult::failure(QString::fromLocal8Bit(exception.what()));
    }
    catch (const std::exception& exception) {
        return OperationResult::failure(QString::fromLocal8Bit(exception.what()));
    }

    return OperationResult::success();
}

OperationResult RenderSceneContext::setColorPresentations(
    const QVector<MeshColorPresentationUpdate>& updates)
{
    if (modelIds_.isEmpty()) {
        return OperationResult::failure(
            QStringLiteral("No render scene is available for coloring."));
    }
    if (updates.isEmpty()) {
        return OperationResult::failure(
            QStringLiteral("A presentation batch cannot be empty."));
    }

    try {
        QSet<MeshId> updateIds;
        for (const MeshColorPresentationUpdate& update : updates) {
            if (updateIds.contains(update.meshId)) {
                return OperationResult::failure(
                    QStringLiteral("Presentation mesh IDs must be unique."));
            }
            updateIds.insert(update.meshId);
            const int modelId = modelIds_.value(update.meshId, -1);
            MeshModel* model = document_->getMesh(modelId);
            if (modelId < 0 || model == nullptr) {
                return OperationResult::failure(
                    QStringLiteral("Presentation target does not exist in the committed scene."));
            }
            const OperationResult validation = validatePresentation(
                update.presentation,
                liveFaceCount(*model),
                liveVertexCount(*model));
            if (!validation.ok)
                return validation;
        }

        // First allocate/copy the complete logical candidate and every prior or
        // desired color payload.  No renderer-private model is touched in this
        // phase.
        QHash<MeshId, ColorPresentation> candidatePresentations = presentations_;
        QHash<MeshId, std::shared_ptr<const MLRenderingData>>
            candidateRenderingData = renderingData_;
        candidatePresentations.reserve(presentations_.size());
        candidateRenderingData.reserve(renderingData_.size());
        std::vector<PreparedPresentationMutation> prepared;
        prepared.reserve(static_cast<std::size_t>(updates.size()));
        for (const MeshColorPresentationUpdate& update : updates) {
            PreparedPresentationMutation mutation;
            mutation.meshId = update.meshId;
            mutation.modelId = modelIds_.value(update.meshId);
            mutation.model = document_->getMesh(mutation.modelId);
            const ColorPresentation& colors = update.presentation.mode == ColorMode::Default
                ? sourceColors_[update.meshId] : update.presentation;
            mutation.hadFaceColorMask =
                mutation.model->hasDataMask(MeshModel::MM_FACECOLOR);
            mutation.faceColorsChanged = mutation.hadFaceColorMask ||
                                         !colors.faceColors.isEmpty();
            mutation.hadVertexColorMask =
                mutation.model->hasDataMask(MeshModel::MM_VERTCOLOR);
            mutation.vertexColorsChanged =
                mutation.hadVertexColorMask ||
                !colors.vertexColors.isEmpty();
            if (mutation.hadFaceColorMask) {
                mutation.priorFaceColors.reserve(
                    static_cast<std::size_t>(liveFaceCount(*mutation.model)));
                for (const CFaceO& face : mutation.model->cm.face) {
                    if (!face.IsD())
                        mutation.priorFaceColors.push_back(face.C());
                }
            }
            mutation.desiredFaceColors.reserve(
                static_cast<std::size_t>(colors.faceColors.size()));
            for (const QColor& color : colors.faceColors)
                mutation.desiredFaceColors.push_back(meshColor(color));
            if (mutation.hadVertexColorMask) {
                mutation.priorVertexColors.reserve(
                    static_cast<std::size_t>(liveVertexCount(*mutation.model)));
                for (const CVertexO& vertex : mutation.model->cm.vert) {
                    if (!vertex.IsD())
                        mutation.priorVertexColors.push_back(vertex.C());
                }
            }
            mutation.desiredVertexColors.reserve(
                static_cast<std::size_t>(colors.vertexColors.size()));
            for (const QColor& color : colors.vertexColors)
                mutation.desiredVertexColors.push_back(meshColor(color));

            const auto prior = renderingData_.constFind(update.meshId);
            Q_ASSERT(prior != renderingData_.cend());
            mutation.priorRenderingData = prior.value();
            candidatePresentations[update.meshId] = update.presentation;
            prepared.push_back(std::move(mutation));
        }

        // Rendering candidates are derived from the immutable imported
        // appearance, without temporarily mutating the committed CPU model.
        for (PreparedPresentationMutation& mutation : prepared) {
            mutation.desiredRenderingData =
                std::make_shared<const MLRenderingData>(desiredRenderingData(
                    *sourceRenderingData_[mutation.meshId],
                    candidatePresentations[mutation.meshId]));
            candidateRenderingData[mutation.meshId] = mutation.desiredRenderingData;
        }

        std::vector<std::size_t> touched;
        touched.reserve(prepared.size());
        try {
            for (std::size_t index = 0; index < prepared.size(); ++index) {
                PreparedPresentationMutation& mutation = prepared[index];
                touched.push_back(index);
                if (!mutation.desiredFaceColors.empty()) {
                    mutation.model->updateDataMask(MeshModel::MM_FACECOLOR);
                    std::size_t colorIndex = 0;
                    for (CFaceO& face : mutation.model->cm.face) {
                        if (face.IsD())
                            continue;
                        face.C() = mutation.desiredFaceColors.at(colorIndex++);
                    }
                }
                else {
                    MeshCompareRenderDetail::ensureFaceColorDisabled(
                        *mutation.model);
                }
                if (!mutation.desiredVertexColors.empty()) {
                    mutation.model->updateDataMask(MeshModel::MM_VERTCOLOR);
                    std::size_t colorIndex = 0;
                    for (CVertexO& vertex : mutation.model->cm.vert) {
                        if (vertex.IsD())
                            continue;
                        vertex.C() =
                            mutation.desiredVertexColors.at(colorIndex++);
                    }
                }
                else {
                    mutation.model->clearDataMask(MeshModel::MM_VERTCOLOR);
                }
                publisher_->publish(
                    *sharedContext_,
                    mutation.modelId,
                    *mutation.desiredRenderingData,
                    {mutation.faceColorsChanged,
                     mutation.vertexColorsChanged});
            }
        }
        catch (...) {
            // CPU/logical state is rolled back strictly under ordinary backend
            // exceptions. GPU publication rollback is best effort because
            // manageBuffers has no reliable success result.
            for (auto touchedIt = touched.rbegin();
                 touchedIt != touched.rend();
                 ++touchedIt) {
                PreparedPresentationMutation& mutation = prepared[*touchedIt];
                try {
                    restoreCpuPresentation(mutation);
                    publisher_->publish(
                        *sharedContext_,
                        mutation.modelId,
                        *mutation.priorRenderingData,
                        {true, true});
                }
                catch (...) {
                    // Continue restoring the remaining already-touched meshes.
                }
            }
            throw;
        }

        presentations_.swap(candidatePresentations);
        renderingData_.swap(candidateRenderingData);
    }
    catch (const MLException& exception) {
        return OperationResult::failure(QString::fromLocal8Bit(exception.what()));
    }
    catch (const std::exception& exception) {
        return OperationResult::failure(QString::fromLocal8Bit(exception.what()));
    }
    catch (...) {
        return OperationResult::failure(
            QStringLiteral("Applying color presentations failed."));
    }
    return OperationResult::success();
}

const ColorPresentation* RenderSceneContext::colorPresentation(MeshId meshId) const
{
    const auto found = presentations_.constFind(meshId);
    return found == presentations_.cend() ? nullptr : &found.value();
}

const MLRenderingData* RenderSceneContext::currentRenderingData(MeshId meshId) const
{
    const auto found = renderingData_.constFind(meshId);
    return found == renderingData_.cend() ? nullptr : found.value().get();
}

int RenderSceneContext::modelIdFor(MeshId meshId) const
{
    return modelIds_.value(meshId, -1);
}
