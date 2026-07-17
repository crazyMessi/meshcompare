#pragma once

#include "services/mesh_import_service.h"

class MeshLabMeshLoader final : public IMeshLoader
{
public:
    OperationResult loadFile(
        const QString& path,
        MeshLabMeshRepository& destination,
        QVector<LoadedMesh>* loaded) override;
};
