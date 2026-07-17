#pragma once

#include <QString>

struct CameraPosePaths
{
    QString storagePath;
    QString legacyPath;
};

CameraPosePaths resolveCameraPosePaths(
    const QString& applicationLocalDataRoot,
    const QString& genericDataRoot);
