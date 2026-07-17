#pragma once

#include <QString>

#include "workspace_state.h"

struct ReferenceResolution {
    MeshId referenceId = 0;
    QString notice;
};

ReferenceResolution resolveReference(const QVector<MeshEntry>& meshes);
