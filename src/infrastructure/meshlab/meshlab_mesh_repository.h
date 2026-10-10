#pragma once

#include <array>
#include <memory>
#include <unordered_map>

#include "core/mesh_resource_provider.h"

class MeshDocument;
class MeshLabMeshLoader;
class MeshModel;
class MeshLabGeometryView;

// Owns imported mesh resources and can retain resources from another import.
// MeshDocument itself never crosses this infrastructure boundary.
class MeshLabMeshRepository final : public IMeshResourceProvider
{
public:
    MeshLabMeshRepository();
    ~MeshLabMeshRepository() override;

    MeshModel* allocateMesh(const QString& sourcePath, const QString& displayName);
    MeshResourceId resourceIdFor(const MeshModel& mesh) const;
    // Set during import, before exposing this resource's geometry snapshot.
    void setAppearanceMetadata(
        MeshResourceId resourceId,
        bool unlit,
        const QVector<int>& wrapS = {},
        const QVector<int>& wrapT = {});
    OperationResult retainMesh(
        const MeshLabMeshRepository& source,
        MeshResourceId sourceId,
        MeshResourceId* destinationId);
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
    struct MeshResource;
    MeshModel* mesh(MeshResourceId resourceId) const;

    std::shared_ptr<MeshDocument> document_;
    MeshResourceId nextResourceId_ = 1;
    std::unordered_map<MeshResourceId, std::shared_ptr<MeshResource>> resources_;
    mutable std::unordered_map<MeshResourceId, std::unique_ptr<MeshLabGeometryView>> views_;
};
