#include "mesh_lab_renderer_adapter.h"

#include <exception>
#include <utility>

#include <QPointer>
#include <QWidget>

#include <common/mlexception.h>
#include <common/parameters/rich_parameter_list.h>

#include "mesh_lab_viewport.h"
#include "render_scene_context.h"
#include "viewport_factory.h"
#include "viewport_grid.h"

namespace
{
const std::array<DiagnosticFlag, 4> diagnosticFlags{{
    DiagnosticFlag::Orthographic,
    DiagnosticFlag::Wireframe,
    DiagnosticFlag::Normals,
    DiagnosticFlag::DoubleSided}};

class MeshLabViewportHandle final : public IViewport
{
public:
    explicit MeshLabViewportHandle(MeshLabViewport* viewport)
        : viewport_(viewport)
    {
    }

    ~MeshLabViewportHandle() override
    {
        delete viewport_.data();
    }

    QWidget* widget() const override { return viewport_.data(); }
    OperationResult initializeForScenePreparation() override
    {
        if (viewport_.isNull()) {
            return OperationResult::failure(
                QStringLiteral("The viewport is no longer available."));
        }
        return viewport_->initializeForScenePreparation();
    }
    CameraPose captureCamera() const override
    {
        return viewport_.isNull() ? CameraPose{} : viewport_->captureCamera();
    }
    OperationResult captureImage(QImage& image) override
    {
        if (viewport_.isNull()) {
            return OperationResult::failure(
                QStringLiteral("The viewport is no longer available."));
        }
        return viewport_->captureImage(image);
    }
    OperationResult restoreCamera(const CameraPose& pose) override
    {
        if (viewport_.isNull())
            return OperationResult::failure(QStringLiteral("The viewport is no longer available."));
        return viewport_->restoreCamera(pose);
    }
    void resetCamera() override
    {
        if (!viewport_.isNull())
            viewport_->resetCamera();
    }
    void setSelected(bool selected) override
    {
        if (!viewport_.isNull())
            viewport_->setSelected(selected);
    }
    void setLabel(QString label) override
    {
        if (!viewport_.isNull())
            viewport_->setLabel(std::move(label));
    }
    void setReference(bool reference) override
    {
        if (!viewport_.isNull())
            viewport_->setReference(reference);
    }
    void setMeshVisible(int meshModelId, bool visible) override
    {
        if (!viewport_.isNull())
            viewport_->setMeshVisible(meshModelId, visible);
    }
    void setScoreLabel(QString label) override
    {
        if (!viewport_.isNull())
            viewport_->setScoreLabel(std::move(label));
    }
    void setColorLegend(ColorLegendSpec legend) override
    {
        if (!viewport_.isNull())
            viewport_->setColorLegend(std::move(legend));
    }
    void setDiagnostic(DiagnosticFlag flag, bool enabled) override
    {
        if (!viewport_.isNull())
            viewport_->setDiagnostic(flag, enabled);
    }
    void requestRepaint() override
    {
        if (!viewport_.isNull())
            viewport_->update();
    }

private:
    QPointer<MeshLabViewport> viewport_;
};
} // namespace

class MeshLabRendererAdapter::AdapterViewportCallbacks final : public IViewportCallbacks
{
public:
    explicit AdapterViewportCallbacks(MeshLabRendererAdapter& adapter)
        : adapter_(adapter)
    {
    }

    void viewportActivated(int viewportId) override
    {
        adapter_.viewportActivated(viewportId);
    }
    void cameraChanged(int viewportId, const CameraPose& pose) override
    {
        adapter_.viewportCameraChanged(viewportId, pose);
    }
    void rendererError(int viewportId, const QString& message) override
    {
        adapter_.viewportRendererError(viewportId, message);
    }

private:
    MeshLabRendererAdapter& adapter_;
};

class MeshLabRendererAdapter::SceneBundle
{
public:
    OperationResult addResources(
        const SceneDescriptor& scene,
        const IMeshResourceProvider& resources)
    {
        generation_ = scene.generation;
        return context_.addResources(scene, resources);
    }

    OperationResult createViewportGrid(
        const SceneDescriptor& scene,
        QWidget* viewportHost,
        IViewportFactory& factory,
        IViewportCallbacks& callbacks,
        MeshId selectedMeshId)
    {
        grid_.reset(new ViewportGrid(factory, callbacks));
        const ViewportGridSceneDependencies dependencies{
            context_.document(),
            context_.sharedContext(),
            settings_,
            [this](MeshId meshId) { return context_.modelIdFor(meshId); }};
        return grid_->create(viewportHost, scene, dependencies, selectedMeshId);
    }

    void mount(QWidget* viewportHost)
    {
        Q_UNUSED(viewportHost);
        if (grid_)
            grid_->showCommitted();
    }

    void setSelectedMesh(MeshId meshId)
    {
        if (grid_)
            grid_->setSelectedMesh(meshId);
    }

    void setReferenceMesh(MeshId meshId)
    {
        if (grid_)
            grid_->setReferenceMesh(meshId);
    }

    OperationResult setMeshVisible(MeshId meshId, bool visible)
    {
        if (!grid_) {
            return OperationResult::failure(
                QStringLiteral("No committed viewport is available for layer visibility."));
        }
        return grid_->setMeshVisible(meshId, visible);
    }

    OperationResult setColorPresentations(
        const QVector<MeshColorPresentationUpdate>& updates)
    {
        const OperationResult result = context_.setColorPresentations(updates);
        if (result.ok && grid_) {
            grid_->setColorLegends(updates);
            if (diagnosticEnabled_[static_cast<std::size_t>(
                    DiagnosticFlag::Wireframe)]) {
                grid_->setDiagnostic(DiagnosticFlag::Wireframe, true);
            }
            grid_->requestRepaint();
        }
        return result;
    }

    OperationResult setAnalysisOverlays(
        const QVector<MeshAnalysisOverlayUpdate>& updates)
    {
        if (!grid_) {
            return OperationResult::failure(QStringLiteral(
                "No committed viewport is available for analysis overlays."));
        }
        return grid_->setAnalysisOverlays(updates);
    }

    CameraPose captureCamera() const
    {
        return grid_ ? grid_->captureCamera() : CameraPose{};
    }

    OperationResult captureImage(QImage& image)
    {
        if (!grid_) {
            return OperationResult::failure(
                QStringLiteral("No committed viewport is available for image capture."));
        }
        return grid_->captureImage(image);
    }

    OperationResult restoreCamera(const CameraPose& pose)
    {
        if (!grid_)
            return OperationResult::failure(QStringLiteral("No committed viewport is available."));
        return grid_->restoreCamera(pose);
    }

    void resetCamera()
    {
        if (grid_)
            grid_->resetCamera();
    }

    void setDiagnostic(DiagnosticFlag flag, bool enabled)
    {
        diagnosticEnabled_[static_cast<std::size_t>(flag)] = enabled;
        if (grid_)
            grid_->setDiagnostic(flag, enabled);
    }

    MeshId meshIdForViewport(int viewportId) const
    {
        return grid_ ? grid_->meshIdForViewport(viewportId) : 0;
    }

    int viewportCount() const { return grid_ ? grid_->viewportCount() : 0; }
    IViewport* viewportAt(int index) const
    {
        return grid_ ? grid_->viewportAt(index) : nullptr;
    }

    quint64 generation() const { return generation_; }

    ~SceneBundle()
    {
        // MeshLabViewport removes its shared QGL view during destruction, so
        // it must go before RenderSceneContext tears down the shared context.
        grid_.reset();
    }

private:
    RenderSceneContext context_;
    RichParameterList settings_;
    std::unique_ptr<ViewportGrid> grid_;
    quint64 generation_ = 0;
    std::array<bool, 4> diagnosticEnabled_{{false, false, false, false}};
};

OperationResult MeshLabViewportFactory::createViewport(
    QWidget* parent,
    const ViewportDependencies& dependencies,
    std::unique_ptr<IViewport>& viewport)
{
    if (parent == nullptr) {
        return OperationResult::failure(
            QStringLiteral("A viewport host must be mounted before preparing a MeshLab scene."));
    }

    try {
        std::unique_ptr<MeshLabViewport> meshLabViewport(
            new MeshLabViewport(parent, dependencies));
        std::unique_ptr<IViewport> candidate(
            new MeshLabViewportHandle(meshLabViewport.release()));
        viewport = std::move(candidate);
        return OperationResult::success();
    }
    catch (const MLException& exception) {
        return OperationResult::failure(QString::fromLocal8Bit(exception.what()));
    }
    catch (const std::exception& exception) {
        return OperationResult::failure(QString::fromLocal8Bit(exception.what()));
    }
}

MeshLabRendererAdapter::MeshLabRendererAdapter()
    : ownedViewportFactory_(new MeshLabViewportFactory),
      viewportFactory_(ownedViewportFactory_.get()),
      callbacks_(new AdapterViewportCallbacks(*this))
{
}

MeshLabRendererAdapter::MeshLabRendererAdapter(IViewportFactory& viewportFactory)
    : viewportFactory_(&viewportFactory), callbacks_(new AdapterViewportCallbacks(*this))
{
}

MeshLabRendererAdapter::~MeshLabRendererAdapter()
{
    clearScene();
    destroyBundle(prepared_);
}

OperationResult MeshLabRendererAdapter::mount(QWidget* viewportHost)
{
    if (viewportHost == nullptr)
        return OperationResult::failure(QStringLiteral("A viewport host is required."));
    if (viewportHost_ != viewportHost && (prepared_ || committed_)) {
        return OperationResult::failure(
            QStringLiteral("The viewport host cannot change while a MeshLab scene is staged or committed."));
    }

    viewportHost_ = viewportHost;
    if (committed_)
        committed_->mount(viewportHost_);
    return OperationResult::success();
}

void MeshLabRendererAdapter::setEvents(RendererEvents events)
{
    events_ = std::move(events);
}

OperationResult MeshLabRendererAdapter::prepareScene(
    const SceneDescriptor& scene,
    const IMeshResourceProvider& resources)
{
    if (viewportHost_.isNull()) {
        return OperationResult::failure(
            QStringLiteral("A viewport host must be mounted before preparing a MeshLab scene."));
    }

    discardPreparedScene();
    try {
        std::unique_ptr<SceneBundle> candidate(new SceneBundle);
        OperationResult result = candidate->addResources(scene, resources);
        if (!result.ok)
            return result;
        result = candidate->createViewportGrid(
            scene,
            viewportHost_.data(),
            *viewportFactory_,
            *callbacks_,
            selectedMeshId_);
        if (!result.ok)
            return result;
        for (DiagnosticFlag flag : diagnosticFlags) {
            candidate->setDiagnostic(
                flag,
                diagnosticEnabled_[static_cast<std::size_t>(flag)]);
        }
        QVector<MeshColorPresentationUpdate> presentations;
        for (const SceneMesh& mesh : scene.meshes) {
            if (mesh.presentation.mode != ColorMode::Default)
                presentations.append({mesh.id, mesh.presentation});
        }
        if (!presentations.isEmpty()) {
            result = candidate->setColorPresentations(presentations);
            if (!result.ok)
                return result;
        }
        QVector<MeshAnalysisOverlayUpdate> overlays;
        for (const SceneMesh& mesh : scene.meshes) {
            if (!mesh.analysisLabel.isEmpty())
                overlays.append({mesh.id, mesh.analysisLabel});
        }
        if (!overlays.isEmpty()) {
            result = candidate->setAnalysisOverlays(overlays);
            if (!result.ok)
                return result;
        }
        if (!scene.initialCamera.viewStateXml.isEmpty()) {
            result = candidate->restoreCamera(scene.initialCamera);
            if (!result.ok)
                return result;
        }
        prepared_ = std::move(candidate);
        return OperationResult::success();
    }
    catch (const MLException& exception) {
        return OperationResult::failure(QString::fromLocal8Bit(exception.what()));
    }
    catch (const std::exception& exception) {
        return OperationResult::failure(QString::fromLocal8Bit(exception.what()));
    }
}

void MeshLabRendererAdapter::commitPreparedScene()
{
    if (!prepared_)
        return;

    destroyBundle(committed_);
    committed_ = std::move(prepared_);
    committed_->setSelectedMesh(selectedMeshId_);
    committed_->mount(viewportHost_);
}

void MeshLabRendererAdapter::discardPreparedScene()
{
    destroyBundle(prepared_);
}

void MeshLabRendererAdapter::clearScene()
{
    destroyBundle(prepared_);
    destroyBundle(committed_);
}

void MeshLabRendererAdapter::setSelectedMesh(MeshId meshId)
{
    selectedMeshId_ = meshId;
    if (committed_)
        committed_->setSelectedMesh(meshId);
}

void MeshLabRendererAdapter::setReferenceMesh(MeshId meshId)
{
    referenceMeshId_ = meshId;
    if (committed_)
        committed_->setReferenceMesh(meshId);
}

OperationResult MeshLabRendererAdapter::setMeshVisible(
    MeshId meshId,
    bool visible)
{
    if (!committed_) {
        return OperationResult::failure(
            QStringLiteral("No committed scene is available for layer visibility."));
    }
    return committed_->setMeshVisible(meshId, visible);
}

OperationResult MeshLabRendererAdapter::setColorPresentations(
    const QVector<MeshColorPresentationUpdate>& updates)
{
    if (!committed_) {
        return OperationResult::failure(
            QStringLiteral("No committed scene is available for coloring."));
    }
    return committed_->setColorPresentations(updates);
}

OperationResult MeshLabRendererAdapter::setAnalysisOverlays(
    const QVector<MeshAnalysisOverlayUpdate>& updates)
{
    if (!committed_) {
        return OperationResult::failure(QStringLiteral(
            "No committed scene is available for analysis overlays."));
    }
    return committed_->setAnalysisOverlays(updates);
}

CameraPose MeshLabRendererAdapter::captureCamera() const
{
    return committed_ ? committed_->captureCamera() : CameraPose{};
}

OperationResult MeshLabRendererAdapter::captureImage(QImage& image)
{
    if (!committed_) {
        return OperationResult::failure(
            QStringLiteral("No committed scene is available for image capture."));
    }
    return committed_->captureImage(image);
}

OperationResult MeshLabRendererAdapter::restoreCamera(const CameraPose& pose)
{
    if (!committed_)
        return OperationResult::failure(QStringLiteral("No committed scene is available."));
    return committed_->restoreCamera(pose);
}

void MeshLabRendererAdapter::resetCamera()
{
    if (committed_)
        committed_->resetCamera();
}

void MeshLabRendererAdapter::setDiagnostic(
    DiagnosticFlag flag,
    bool enabled)
{
    diagnosticEnabled_[static_cast<std::size_t>(flag)] = enabled;
    if (prepared_)
        prepared_->setDiagnostic(flag, enabled);
    if (committed_)
        committed_->setDiagnostic(flag, enabled);
}

RendererDiagnostics MeshLabRendererAdapter::diagnostics() const
{
    RendererDiagnostics result;
    result.backendName = QStringLiteral("MeshLab standalone renderer");
    MeshLabViewport* viewport = viewportForTest(0);
    if (viewport != nullptr) {
        const RendererDiagnostics graphics = viewport->rendererDiagnostics();
        result.openGlVendor = graphics.openGlVendor;
        result.openGlRenderer = graphics.openGlRenderer;
        result.openGlVersion = graphics.openGlVersion;
    }
    return result;
}

quint64 MeshLabRendererAdapter::preparedGeneration() const
{
    return prepared_ ? prepared_->generation() : 0;
}

quint64 MeshLabRendererAdapter::committedGeneration() const
{
    return committed_ ? committed_->generation() : 0;
}

int MeshLabRendererAdapter::validViewportCount() const
{
    if (!committed_)
        return 0;

    int validCount = 0;
    for (int index = 0; index < committed_->viewportCount(); ++index) {
        MeshLabViewport* viewport = viewportForTest(index);
        if (viewport != nullptr && viewport->isValid() && viewport->context() != nullptr &&
            viewport->context()->isValid()) {
            ++validCount;
        }
    }
    return validCount;
}

MeshLabViewport* MeshLabRendererAdapter::viewportForTest(int index) const
{
    if (!committed_)
        return nullptr;
    IViewport* viewport = committed_->viewportAt(index);
    return viewport == nullptr ? nullptr : dynamic_cast<MeshLabViewport*>(viewport->widget());
}

void MeshLabRendererAdapter::destroyBundle(std::unique_ptr<SceneBundle>& bundle)
{
    bundle.reset();
}

void MeshLabRendererAdapter::viewportActivated(int viewportId)
{
    if (!committed_)
        return;
    const MeshId meshId = committed_->meshIdForViewport(viewportId);
    if (meshId == 0)
        return;
    setSelectedMesh(meshId);
    if (events_.selectedMeshChanged)
        events_.selectedMeshChanged(meshId);
}

void MeshLabRendererAdapter::viewportCameraChanged(int, const CameraPose& pose)
{
    if (events_.cameraChanged)
        events_.cameraChanged(pose);
}

void MeshLabRendererAdapter::viewportRendererError(int, const QString& message)
{
    if (events_.rendererError)
        events_.rendererError(message);
}
