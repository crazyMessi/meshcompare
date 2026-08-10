#include "meshlab_mesh_repository.h"

#include <array>
#include <cmath>
#include <unordered_map>

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
    explicit MeshLabGeometryView(const MeshModel& mesh)
    {
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
    bool isValid() const { return validationError_.isEmpty(); }
    const QString& validationError() const { return validationError_; }

private:
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
};

MeshLabMeshRepository::MeshLabMeshRepository()
    : document_(new MeshDocument)
{
}

MeshLabMeshRepository::~MeshLabMeshRepository() = default;

MeshModel* MeshLabMeshRepository::allocateMesh(const QString& sourcePath, const QString& displayName)
{
    return document_->addNewMesh(sourcePath, displayName);
}

MeshResourceId MeshLabMeshRepository::resourceIdFor(const MeshModel& mesh) const
{
    return static_cast<MeshResourceId>(mesh.id()) + 1;
}

void MeshLabMeshRepository::removeMesh(MeshResourceId resourceId)
{
    if (resourceId == 0)
        return;
    views_.erase(resourceId);
    document_->delMesh(static_cast<unsigned int>(resourceId - 1));
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
    if (resourceId == 0)
        return nullptr;
    return document_->getMesh(static_cast<unsigned int>(resourceId - 1));
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
    std::unique_ptr<MeshLabGeometryView> view(new MeshLabGeometryView(*model));
    if (!view->isValid())
        return OperationResult::failure(view->validationError());
    *geometry = view.get();
    views_.emplace(resourceId, std::move(view));
    return OperationResult::success();
}
