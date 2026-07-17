#pragma once

#include "services/mesh_import_service.h"

#include <QHash>

class FakeMeshLoader final : public IMeshLoader
{
public:
    void succeed(const QString& path, const QVector<LoadedMesh>& meshes);
    void succeedWithoutPositiveArea(const QString& path, const QVector<LoadedMesh>& meshes);
    void fail(const QString& path, const QString& error);

    OperationResult loadFile(
        const QString& path,
        MeshLabMeshRepository& destination,
        QVector<LoadedMesh>* loaded) override;

private:
    struct PlannedLoad {
        bool succeeds = false;
        bool hasPositiveArea = true;
        QString error;
        QVector<LoadedMesh> meshes;
    };

    QHash<QString, PlannedLoad> loads_;
};
