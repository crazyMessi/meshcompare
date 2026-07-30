#pragma once

#include <QVector>

#include "core/meshcompare_types.h"

class MeshLabMeshRepository;
class MeshModel;
class QString;

OperationResult loadGlbMeshModels(
    const QString& path,
    MeshLabMeshRepository& destination,
    QVector<MeshModel*>* loadedModels);
