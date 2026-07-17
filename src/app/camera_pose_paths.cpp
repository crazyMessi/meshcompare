#include "camera_pose_paths.h"

#include <QDir>
#include <QFileInfo>

namespace
{
constexpr auto LibraryFileName = "meshlab_lizd_camera_poses.json";

QString applicationLibraryPath(const QString& root)
{
    if (root.isEmpty())
        return {};
    return QDir(root).filePath(QString::fromLatin1(LibraryFileName));
}

QString legacyLibraryPath(const QString& root, const QString& applicationName)
{
    if (root.isEmpty())
        return {};
    return QDir(root).filePath(
        QStringLiteral("VCG/%1/%2")
            .arg(applicationName, QString::fromLatin1(LibraryFileName)));
}

bool isExistingFile(const QString& path)
{
    const QFileInfo info(path);
    return info.exists() && info.isFile();
}
} // namespace

CameraPosePaths resolveCameraPosePaths(
    const QString& applicationLocalDataRoot,
    const QString& genericDataRoot)
{
    CameraPosePaths paths;
    paths.storagePath = applicationLibraryPath(applicationLocalDataRoot);
    if (genericDataRoot.isEmpty())
        return paths;

    const QString floatPath = legacyLibraryPath(
        genericDataRoot, QStringLiteral("MeshLab_64bit_fp"));
    const QString doublePath = legacyLibraryPath(
        genericDataRoot, QStringLiteral("MeshLab_64bit_dp"));
    if (isExistingFile(floatPath))
        paths.legacyPath = floatPath;
    else if (isExistingFile(doublePath))
        paths.legacyPath = doublePath;
    else
        paths.legacyPath = floatPath;
    return paths;
}
