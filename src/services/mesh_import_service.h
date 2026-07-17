#pragma once

#include <memory>

#include <QPair>
#include <QStringList>
#include <QVector>

#include "core/workspace_state.h"
#include "infrastructure/meshlab/meshlab_mesh_repository.h"

struct LoadedMesh {
    MeshResourceId resourceId = 0;
    QString sourcePath;
    QString displayName;
    QStringList uuidCandidates;
};

class IMeshLoader {
public:
    virtual ~IMeshLoader() = default;
    virtual OperationResult loadFile(
        const QString& path,
        MeshLabMeshRepository& destination,
        QVector<LoadedMesh>* loaded) = 0;
};

struct StagedWorkspace {
    OperationResult result;
    std::unique_ptr<MeshLabMeshRepository> repository;
    QVector<MeshEntry> entries;
    QVector<QPair<QString, QString>> fileErrors;
    SceneLayoutMode layoutMode = SceneLayoutMode::ComparisonGrid;
};

class IMeshImportService {
public:
    virtual ~IMeshImportService() = default;
    virtual StagedWorkspace stage(const QStringList& paths) = 0;
};

class MeshImportService final : public IMeshImportService {
public:
    explicit MeshImportService(IMeshLoader& loader);
    StagedWorkspace stage(const QStringList& paths) override;

private:
    IMeshLoader& loader_;
};
