#include "meshlab_mesh_repository.h"

#include <array>
#include <cmath>
#include <exception>
#include <unordered_map>
#include <utility>

#include <common/ml_document/mesh_document.h>
#include <vcg/complex/allocate.h>
#include <vcg/complex/algorithms/update/bounding.h>
#include <vcg/complex/algorithms/update/normal.h>

namespace
{
MeshPoint3D faceNormal(
    const MeshPoint3D& first,
    const MeshPoint3D& second,
    const MeshPoint3D& third)
{
    const double abX = second[0] - first[0];
    const double abY = second[1] - first[1];
    const double abZ = second[2] - first[2];
    const double acX = third[0] - first[0];
    const double acY = third[1] - first[1];
    const double acZ = third[2] - first[2];
    return MeshPoint3D{{
        abY * acZ - abZ * acY,
        abZ * acX - abX * acZ,
        abX * acY - abY * acX}};
}

double squaredLength(const MeshPoint3D& value)
{
    return value[0] * value[0] +
           value[1] * value[1] +
           value[2] * value[2];
}
} // namespace

class MeshLabGeometryView final : public IMeshGeometryView
{
public:
    MeshLabGeometryView(
        const MeshModel& mesh,
        bool unlit,
        const QVector<int>& wrapS,
        const QVector<int>& wrapT)
        : unlit_(unlit)
    {
        textures_.reserve(static_cast<int>(mesh.cm.textures.size()));
        for (const std::string& name : mesh.cm.textures)
            textures_.append(mesh.getTexture(name));
        for (int index = 0; index < textures_.size(); ++index) {
            wrapS_.append(validTextureWrap(wrapS.value(index, 10497)));
            wrapT_.append(validTextureWrap(wrapT.value(index, 10497)));
        }

        const Scalarm determinant = mesh.cm.Tr.Determinant();
        const bool invertible =
            std::isfinite(static_cast<double>(determinant)) &&
            determinant != Scalarm(0);
        Matrix44m normalTransform;
        normalTransform.SetIdentity();
        if (invertible) {
            normalTransform = vcg::Inverse(mesh.cm.Tr);
            normalTransform.transposeInPlace();
        }
        bool recomputeNormals = !invertible;
        const bool hasVertexColors =
            mesh.hasDataMask(MeshModel::MM_VERTCOLOR);
        const bool hasFaceColors = mesh.hasDataMask(MeshModel::MM_FACECOLOR);
        const bool hasWedgeTexCoords = mesh.hasDataMask(MeshModel::MM_WEDGTEXCOORD);
        const bool hasVertexTexCoords = mesh.hasDataMask(MeshModel::MM_VERTTEXCOORD);

        positions_.reserve(mesh.cm.vn);
        normals_.reserve(mesh.cm.vn);
        if (hasVertexColors)
            colors_.reserve(mesh.cm.vn);
        std::unordered_map<const CVertexO*, int> denseIndices;
        denseIndices.reserve(static_cast<std::size_t>(mesh.cm.vn));
        for (const CVertexO& vertex : mesh.cm.vert) {
            if (vertex.IsD())
                continue;
            const int denseIndex = positions_.size();
            denseIndices.emplace(&vertex, denseIndex);
            const CMeshO::CoordType point = mesh.cm.Tr * vertex.P();
            if (!std::isfinite(static_cast<double>(point[0])) ||
                !std::isfinite(static_cast<double>(point[1])) ||
                !std::isfinite(static_cast<double>(point[2]))) {
                validationError_ = QStringLiteral(
                    "Mesh transform produces non-finite vertex positions.");
                return;
            }
            const CMeshO::CoordType& normal = vertex.N();
            const Point4m transformedNormal4 = normalTransform * Point4m(
                normal[0], normal[1], normal[2], 0);
            CMeshO::CoordType transformedNormal(
                transformedNormal4[0],
                transformedNormal4[1],
                transformedNormal4[2]);
            const Scalarm normalLength = transformedNormal.Norm();
            if (std::isfinite(static_cast<double>(normalLength)) &&
                normalLength > 0) {
                transformedNormal /= normalLength;
            }
            else {
                transformedNormal = CMeshO::CoordType(0, 0, 0);
                recomputeNormals = true;
            }
            positions_.append(
                MeshPoint3D{{double(point[0]), double(point[1]), double(point[2])}});
            normals_.append(
                MeshPoint3D{{double(transformedNormal[0]),
                             double(transformedNormal[1]),
                             double(transformedNormal[2])}});
            if (hasVertexColors) {
                const auto& color = vertex.C();
                colors_.append(QColor(
                    int(color[0]),
                    int(color[1]),
                    int(color[2]),
                    int(color[3])));
            }
        }

        faces_.reserve(mesh.cm.fn);
        if (hasFaceColors)
            faceColors_.reserve(mesh.cm.fn);
        if (hasWedgeTexCoords || hasVertexTexCoords)
            faceTexCoords_.reserve(mesh.cm.fn);
        for (const CFaceO& face : mesh.cm.face) {
            if (face.IsD())
                continue;
            const auto first = denseIndices.find(face.V(0));
            const auto second = denseIndices.find(face.V(1));
            const auto third = denseIndices.find(face.V(2));
            if (first == denseIndices.end() || second == denseIndices.end() ||
                third == denseIndices.end()) {
                validationError_ = QStringLiteral(
                    "Mesh contains a live face that references a deleted or unmapped vertex.");
                return;
            }
            faces_.append(std::array<int, 3>{
                {first->second, second->second, third->second}});
            if (hasFaceColors) {
                const auto& color = face.C();
                faceColors_.append(QColor(
                    int(color[0]), int(color[1]), int(color[2]), int(color[3])));
            }
            if (hasWedgeTexCoords || hasVertexTexCoords) {
                std::array<MeshTexCoord, 3> coordinates;
                for (int corner = 0; corner < 3; ++corner) {
                    const auto coordinate = hasWedgeTexCoords
                        ? face.WT(corner) : face.V(corner)->T();
                    MeshTexCoord& target = coordinates[corner];
                    if (!std::isfinite(double(coordinate.U())) ||
                        !std::isfinite(double(coordinate.V())))
                        continue;
                    target.u = coordinate.U();
                    target.v = coordinate.V();
                    const int textureIndex = coordinate.N();
                    if (textureIndex >= 0 && textureIndex < textures_.size() &&
                        !textures_.at(textureIndex).isNull())
                        target.textureIndex = textureIndex;
                }
                // One triangle can bind one texture. An incomplete or inconsistent
                // assignment falls back to its source colors instead of failing import.
                if (coordinates[0].textureIndex < 0 ||
                    coordinates[1].textureIndex != coordinates[0].textureIndex ||
                    coordinates[2].textureIndex != coordinates[0].textureIndex) {
                    for (MeshTexCoord& coordinate : coordinates)
                        coordinate.textureIndex = -1;
                }
                faceTexCoords_.append(coordinates);
            }
        }
        if (recomputeNormals)
            recomputeVertexNormals();
    }

    int vertexCount() const override { return positions_.size(); }
    MeshPoint3D vertexPosition(int index) const override
    {
        return positions_.at(index);
    }
    MeshPoint3D vertexNormal(int index) const override
    {
        return normals_.at(index);
    }
    bool hasVertexColors() const override
    {
        return !colors_.isEmpty();
    }
    QColor vertexColor(int index) const override
    {
        return colors_.at(index);
    }
    int faceCount() const override { return faces_.size(); }
    std::array<int, 3> faceVertexIndices(int index) const override
    {
        return faces_.at(index);
    }
    bool hasFaceColors() const override { return !faceColors_.isEmpty(); }
    QColor faceColor(int index) const override { return faceColors_.value(index); }
    int textureCount() const override { return textures_.size(); }
    QImage textureImage(int index) const override { return textures_.value(index); }
    bool hasFaceTexCoords() const override { return !faceTexCoords_.isEmpty(); }
    std::array<MeshTexCoord, 3> faceTexCoords(int index) const override
    {
        return faceTexCoords_.value(index);
    }
    bool isUnlit() const override { return unlit_; }
    int textureWrapS(int index) const override { return wrapS_.value(index, 10497); }
    int textureWrapT(int index) const override { return wrapT_.value(index, 10497); }
    bool isValid() const { return validationError_.isEmpty(); }
    const QString& validationError() const { return validationError_; }

private:
    static int validTextureWrap(int wrap)
    {
        // glTF and OpenGL use the same wrap values.
        return wrap == 33071 || wrap == 33648 ? wrap : 10497;
    }

    void recomputeVertexNormals()
    {
        normals_.fill(MeshPoint3D{{0.0, 0.0, 0.0}}, positions_.size());
        for (const std::array<int, 3>& face : faces_) {
            const MeshPoint3D& first = positions_.at(face[0]);
            const MeshPoint3D& second = positions_.at(face[1]);
            const MeshPoint3D& third = positions_.at(face[2]);
            const MeshPoint3D normal = faceNormal(first, second, third);
            for (int vertexIndex : face) {
                for (int coordinate = 0; coordinate < 3; ++coordinate)
                    normals_[vertexIndex][coordinate] += normal[coordinate];
            }
        }
        for (MeshPoint3D& normal : normals_) {
            const double length = std::sqrt(squaredLength(normal));
            if (!std::isfinite(length) || length <= 0)
                continue;
            for (double& coordinate : normal)
                coordinate /= length;
        }
    }

    QString validationError_;
    QVector<MeshPoint3D> positions_;
    QVector<MeshPoint3D> normals_;
    QVector<QColor> colors_;
    QVector<std::array<int, 3>> faces_;
    QVector<QColor> faceColors_;
    QVector<std::array<MeshTexCoord, 3>> faceTexCoords_;
    QVector<QImage> textures_;
    QVector<int> wrapS_;
    QVector<int> wrapT_;
    bool unlit_ = false;
};

struct MeshLabMeshRepository::MeshResource
{
    MeshResource(std::shared_ptr<MeshDocument> document, MeshModel* mesh)
        : document(std::move(document)), model(mesh)
    {
    }

    ~MeshResource()
    {
        document->delMesh(static_cast<unsigned int>(model->id()));
    }

    std::shared_ptr<MeshDocument> document;
    MeshModel* model;
    bool unlit = false;
    QVector<int> wrapS;
    QVector<int> wrapT;
};

MeshLabMeshRepository::MeshLabMeshRepository()
    : document_(std::make_shared<MeshDocument>())
{
}

MeshLabMeshRepository::~MeshLabMeshRepository() = default;

MeshModel* MeshLabMeshRepository::allocateMesh(const QString& sourcePath, const QString& displayName)
{
    if (nextResourceId_ == 0)
        return nullptr;
    MeshModel* model = document_->addNewMesh(sourcePath, displayName);
    if (model == nullptr)
        return nullptr;
    resources_.emplace(
        nextResourceId_++, std::make_shared<MeshResource>(document_, model));
    return model;
}

MeshResourceId MeshLabMeshRepository::resourceIdFor(const MeshModel& mesh) const
{
    for (const auto& resource : resources_) {
        if (resource.second->model == &mesh)
            return resource.first;
    }
    return 0;
}

void MeshLabMeshRepository::setAppearanceMetadata(
    MeshResourceId resourceId,
    bool unlit,
    const QVector<int>& wrapS,
    const QVector<int>& wrapT)
{
    const auto resource = resources_.find(resourceId);
    if (resource == resources_.end())
        return;
    resource->second->unlit = unlit;
    resource->second->wrapS = wrapS;
    resource->second->wrapT = wrapT;
    views_.erase(resourceId);
}

OperationResult MeshLabMeshRepository::retainMesh(
    const MeshLabMeshRepository& source,
    MeshResourceId sourceId,
    MeshResourceId* destinationId)
{
    if (destinationId == nullptr) {
        return OperationResult::failure(
            QStringLiteral("No retained mesh resource output was provided."));
    }
    *destinationId = 0;
    const auto sourceResource = source.resources_.find(sourceId);
    if (sourceResource == source.resources_.end()) {
        return OperationResult::failure(
            QStringLiteral("The source mesh resource does not exist."));
    }
    if (nextResourceId_ == 0) {
        return OperationResult::failure(
            QStringLiteral("No more mesh resource IDs are available."));
    }
    try {
        resources_.emplace(nextResourceId_, sourceResource->second);
    }
    catch (const std::exception& exception) {
        return OperationResult::failure(QString::fromLocal8Bit(exception.what()));
    }
    *destinationId = nextResourceId_++;
    return OperationResult::success();
}

void MeshLabMeshRepository::removeMesh(MeshResourceId resourceId)
{
    if (resourceId == 0)
        return;
    views_.erase(resourceId);
    resources_.erase(resourceId);
}

OperationResult MeshLabMeshRepository::createMesh(
    const QString& sourcePath,
    const QString& displayName,
    const QVector<MeshPoint3D>& vertices,
    const QVector<std::array<int, 3>>& faces,
    MeshResourceId* resourceId)
{
    if (resourceId == nullptr) {
        return OperationResult::failure(
            QStringLiteral("No mesh resource output was provided."));
    }
    *resourceId = 0;
    if (vertices.isEmpty() || faces.isEmpty()) {
        return OperationResult::failure(
            QStringLiteral("A generated mesh must contain vertices and triangles."));
    }
    for (const MeshPoint3D& point : vertices) {
        if (!std::isfinite(point[0]) ||
            !std::isfinite(point[1]) ||
            !std::isfinite(point[2])) {
            return OperationResult::failure(
                QStringLiteral("Generated mesh vertices must be finite."));
        }
    }
    for (const std::array<int, 3>& face : faces) {
        for (int index : face) {
            if (index < 0 || index >= vertices.size()) {
                return OperationResult::failure(
                    QStringLiteral("Generated mesh contains an invalid face index."));
            }
        }
        if (face[0] == face[1] ||
            face[1] == face[2] ||
            face[2] == face[0]) {
            return OperationResult::failure(
                QStringLiteral("Generated mesh contains a degenerate face."));
        }
    }

    MeshModel* model = allocateMesh(sourcePath, displayName);
    if (model == nullptr) {
        return OperationResult::failure(
            QStringLiteral("The generated mesh layer could not be allocated."));
    }
    const MeshResourceId allocatedId = resourceIdFor(*model);
    try {
        std::vector<CMeshO::VertexPointer> modelVertices(
            static_cast<std::size_t>(vertices.size()));
        CMeshO::VertexIterator vertex =
            vcg::tri::Allocator<CMeshO>::AddVertices(
                model->cm,
                static_cast<std::size_t>(vertices.size()));
        for (int index = 0; index < vertices.size(); ++index, ++vertex) {
            modelVertices[static_cast<std::size_t>(index)] = &*vertex;
            const MeshPoint3D& point = vertices[index];
            vertex->P() =
                CMeshO::CoordType(point[0], point[1], point[2]);
        }

        CMeshO::FaceIterator face =
            vcg::tri::Allocator<CMeshO>::AddFaces(
                model->cm,
                static_cast<std::size_t>(faces.size()));
        for (const std::array<int, 3>& indices : faces) {
            face->V(0) =
                modelVertices[static_cast<std::size_t>(indices[0])];
            face->V(1) =
                modelVertices[static_cast<std::size_t>(indices[1])];
            face->V(2) =
                modelVertices[static_cast<std::size_t>(indices[2])];
            ++face;
        }

        model->cm.Tr.SetIdentity();
        vcg::tri::UpdateBounding<CMeshO>::Box(model->cm);
        vcg::tri::UpdateNormal<CMeshO>::PerFaceNormalized(model->cm);
        vcg::tri::UpdateNormal<CMeshO>::PerVertexAngleWeighted(model->cm);
        model->updateDataMask();
    }
    catch (...) {
        removeMesh(allocatedId);
        return OperationResult::failure(
            QStringLiteral("The generated mesh layer could not be populated."));
    }

    *resourceId = allocatedId;
    return OperationResult::success();
}

MeshModel* MeshLabMeshRepository::mesh(MeshResourceId resourceId) const
{
    const auto resource = resources_.find(resourceId);
    return resource == resources_.end() ? nullptr : resource->second->model;
}

bool MeshLabMeshRepository::hasPositiveAreaFaces(MeshResourceId resourceId) const
{
    const IMeshGeometryView* geometry = nullptr;
    if (!snapshotGeometry(resourceId, &geometry).ok || geometry == nullptr)
        return false;
    for (int index = 0; index < geometry->faceCount(); ++index) {
        const std::array<int, 3> face = geometry->faceVertexIndices(index);
        const MeshPoint3D first = geometry->vertexPosition(face[0]);
        const MeshPoint3D second = geometry->vertexPosition(face[1]);
        const MeshPoint3D third = geometry->vertexPosition(face[2]);
        const double areaSquared = squaredLength(
            faceNormal(first, second, third));
        if (std::isfinite(areaSquared) && areaSquared > 0)
            return true;
    }
    return false;
}

const IMeshGeometryView* MeshLabMeshRepository::geometry(MeshResourceId resourceId) const
{
    const IMeshGeometryView* result = nullptr;
    if (!snapshotGeometry(resourceId, &result).ok)
        return nullptr;
    return result;
}

OperationResult MeshLabMeshRepository::snapshotGeometry(
    MeshResourceId resourceId,
    const IMeshGeometryView** geometry) const
{
    if (geometry == nullptr)
        return OperationResult::failure(
            QStringLiteral("No geometry snapshot output was provided."));
    *geometry = nullptr;

    const MeshModel* model = mesh(resourceId);
    if (model == nullptr)
        return OperationResult::failure(QStringLiteral("Mesh resource does not exist."));
    const auto it = views_.find(resourceId);
    if (it != views_.end()) {
        *geometry = it->second.get();
        return OperationResult::success();
    }
    const MeshResource& resource = *resources_.at(resourceId);
    std::unique_ptr<MeshLabGeometryView> view(new MeshLabGeometryView(
        *model, resource.unlit, resource.wrapS, resource.wrapT));
    if (!view->isValid())
        return OperationResult::failure(view->validationError());
    *geometry = view.get();
    views_.emplace(resourceId, std::move(view));
    return OperationResult::success();
}
