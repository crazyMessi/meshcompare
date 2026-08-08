#include "mesh_import_service.h"

#include <QFileInfo>

namespace
{
bool isMeshLabProject(const QString& path)
{
    return QFileInfo(path).suffix().compare(
               QStringLiteral("mlp"), Qt::CaseInsensitive) == 0;
}
} // namespace

MeshImportService::MeshImportService(IMeshLoader& loader)
    : loader_(loader)
{
}

StagedWorkspace MeshImportService::stage(const QStringList& paths)
{
    int projectCount = 0;
    for (const QString& path : paths) {
        if (isMeshLabProject(path))
            ++projectCount;
    }
    if (projectCount > 0 && (projectCount != 1 || paths.size() != 1)) {
        return {OperationResult::failure(QStringLiteral(
                    "Open one MeshLab project by itself.")),
                nullptr,
                {},
                {}};
    }

    std::unique_ptr<MeshLabMeshRepository> repository(new MeshLabMeshRepository);
    QVector<MeshEntry> entries;
    QVector<QPair<QString, QString>> errors;
    MeshId nextMeshId = 1;

    for (const QString& path : paths) {
        QVector<LoadedMesh> loaded;
        const OperationResult result = loader_.loadFile(path, *repository, &loaded);
        if (!result.ok) {
            errors.push_back(qMakePair(path, result.error));
            continue;
        }

        for (const LoadedMesh& mesh : loaded) {
            MeshEntry entry;
            entry.id = nextMeshId++;
            entry.resourceId = mesh.resourceId;
            entry.sourcePath = mesh.sourcePath;
            entry.displayName = mesh.displayName;
            entry.uuidCandidates = mesh.uuidCandidates;
            entries.push_back(entry);
        }
    }

    if (!errors.isEmpty())
        return {OperationResult::failure(QStringLiteral("One or more meshes could not be loaded.")),
                nullptr,
                {},
                errors};
    if (entries.isEmpty() || entries.size() > 8)
        return {OperationResult::failure(QStringLiteral("Import must produce between 1 and 8 mesh layers.")),
                nullptr,
                {},
                {}};
    for (const MeshEntry& entry : entries) {
        const IMeshGeometryView* geometry = nullptr;
        const OperationResult snapshot =
            repository->snapshotGeometry(entry.resourceId, &geometry);
        if (!snapshot.ok)
            return {OperationResult::failure(
                        entry.displayName + QStringLiteral(": ") + snapshot.error),
                    nullptr,
                    {},
                    {}};
        if (!repository->hasPositiveAreaFaces(entry.resourceId))
            return {OperationResult::failure(
                        entry.displayName + QStringLiteral(" has no positive-area triangles.")),
                    nullptr,
                    {},
                    {}};
    }

    return {OperationResult::success(),
            std::move(repository),
            entries,
            {},
            projectCount == 1 ? SceneLayoutMode::Overlay
                              : SceneLayoutMode::ComparisonGrid};
}
