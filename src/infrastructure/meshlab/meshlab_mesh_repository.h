#pragma once

#include <array>
#include <memory>
#include <unordered_map>

#include "core/mesh_resource_provider.h"

class MeshDocument;
class MeshLabMeshLoader;
class MeshModel;
class MeshLabGeometryView;

// Owns the MeshLab document used for one staged import.  MeshDocument itself
// never crosses this infrastructure boundary.
class MeshLabMeshRepository final : public IMeshResourceProvider
{
public:
    MeshLabMeshRepository();
    ~MeshLabMeshRepository() override;

    MeshModel* allocateMesh(const QString& sourcePath, const QString& displayName);
    MeshResourceId resourceIdFor(const MeshModel& mesh) const;
    void removeMesh(MeshResourceId resourceId);
    OperationResult createMesh(
        const QString& sourcePath,
        const QString& displayName,
        const QVector<MeshPoint3D>& vertices,
        const QVector<std::array<int, 3>>& faces,
        MeshResourceId* resourceId);

    bool hasPositiveAreaFaces(MeshResourceId resourceId) const;
    OperationResult snapshotGeometry(
        MeshResourceId resourceId,
        const IMeshGeometryView** geometry) const;
    const IMeshGeometryView* geometry(MeshResourceId resourceId) const override;

private:
    MeshModel* mesh(MeshResourceId resourceId) const;

    std::unique_ptr<MeshDocument> document_;
    mutable std::unordered_map<MeshResourceId, std::unique_ptr<MeshLabGeometryView>> views_;
};
