#pragma once

#include <utility>

#include <QColor>
#include <QString>
#include <QStringList>
#include <QVector>

#include "meshcompare_types.h"

struct MeshEntry {
    MeshId id = 0;
    MeshResourceId resourceId = 0;
    QString sourcePath;
    QString displayName;
    QStringList uuidCandidates;
    bool isReference = false;
    ColorPresentation presentation;
    double score = 0.0;
    bool hasScore = false;
};

struct MeshColorStateUpdate {
    MeshId meshId = 0;
    ColorPresentation presentation;
    double score = 0.0;
    bool hasScore = false;
};

class PreparedColorStateUpdate {
public:
    PreparedColorStateUpdate() = default;
    PreparedColorStateUpdate(const PreparedColorStateUpdate&) = delete;
    PreparedColorStateUpdate& operator=(const PreparedColorStateUpdate&) = delete;
    PreparedColorStateUpdate(PreparedColorStateUpdate&& other) noexcept
    {
        *this = std::move(other);
    }
    PreparedColorStateUpdate& operator=(PreparedColorStateUpdate&& other) noexcept
    {
        if (this == &other)
            return *this;
        meshes_.swap(other.meshes_);
        generation_ = other.generation_;
        phase_ = other.phase_;
        valid_ = other.valid_;
        other.generation_ = 0;
        other.phase_ = WorkspacePhase::Empty;
        other.valid_ = false;
        return *this;
    }

    bool isValid() const { return valid_; }

private:
    friend class WorkspaceState;
    QVector<MeshEntry> meshes_;
    quint64 generation_ = 0;
    WorkspacePhase phase_ = WorkspacePhase::Empty;
    bool valid_ = false;
};

class WorkspaceState {
public:
    WorkspacePhase phase() const;
    quint64 generation() const;
    const QVector<MeshEntry>& meshes() const;
    const MeshEntry* mesh(MeshId id) const;
    MeshId selectedMeshId() const;
    MeshId referenceId() const;

    void beginLoading();
    void cancelLoading();
    OperationResult validateWorkspace(
        const QVector<MeshEntry>& meshes,
        MeshId referenceId) const;
    OperationResult commitWorkspace(QVector<MeshEntry> meshes, MeshId referenceId);
    OperationResult setSelectedMesh(MeshId id);
    OperationResult setReference(MeshId id);
    OperationResult beginAnalysis();
    void finishAnalysis();
    OperationResult validateColorUpdates(
        const QVector<MeshColorStateUpdate>& updates) const;
    OperationResult prepareColorUpdates(
        const QVector<MeshColorStateUpdate>& updates,
        PreparedColorStateUpdate* prepared) const;
    void commitPreparedColorUpdates(PreparedColorStateUpdate&& prepared) noexcept;
    void enterFatalError();

private:
    bool contains(MeshId id) const;

    WorkspacePhase phase_ = WorkspacePhase::Empty;
    WorkspacePhase phaseBeforeLoading_ = WorkspacePhase::Empty;
    quint64 generation_ = 0;
    QVector<MeshEntry> meshes_;
    MeshId selectedMeshId_ = 0;
    MeshId referenceId_ = 0;
};
