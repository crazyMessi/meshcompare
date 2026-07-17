#include "workspace_state.h"

#include <QSet>

#include <cmath>
#include <exception>
#include <utility>

WorkspacePhase WorkspaceState::phase() const
{
    return phase_;
}

quint64 WorkspaceState::generation() const
{
    return generation_;
}

const QVector<MeshEntry>& WorkspaceState::meshes() const
{
    return meshes_;
}

const MeshEntry* WorkspaceState::mesh(MeshId id) const
{
    for (const MeshEntry& entry : meshes_) {
        if (entry.id == id)
            return &entry;
    }
    return nullptr;
}

MeshId WorkspaceState::selectedMeshId() const
{
    return selectedMeshId_;
}

MeshId WorkspaceState::referenceId() const
{
    return referenceId_;
}

void WorkspaceState::beginLoading()
{
    if (phase_ != WorkspacePhase::Loading)
        phaseBeforeLoading_ = phase_;
    phase_ = WorkspacePhase::Loading;
}

void WorkspaceState::cancelLoading()
{
    if (phase_ == WorkspacePhase::Loading)
        phase_ = phaseBeforeLoading_;
}

OperationResult WorkspaceState::validateWorkspace(
    const QVector<MeshEntry>& meshes,
    MeshId referenceId) const
{
    if (meshes.size() < 2 || meshes.size() > 8)
        return OperationResult::failure(
            QStringLiteral("A workspace must contain between 2 and 8 meshes."));

    QSet<MeshId> meshIds;
    for (const MeshEntry& mesh : meshes) {
        if (meshIds.contains(mesh.id))
            return OperationResult::failure(
                QStringLiteral("Workspace mesh IDs must be unique."));
        meshIds.insert(mesh.id);
    }

    bool hasReference = false;
    for (const MeshEntry& mesh : meshes)
        hasReference = hasReference || mesh.id == referenceId;
    if (!hasReference)
        return OperationResult::failure(
            QStringLiteral("Reference mesh does not exist in this workspace."));

    return OperationResult::success();
}

OperationResult WorkspaceState::commitWorkspace(QVector<MeshEntry> meshes, MeshId referenceId)
{
    const OperationResult validation = validateWorkspace(meshes, referenceId);
    if (!validation.ok)
        return validation;

    for (MeshEntry& mesh : meshes) {
        mesh.isReference = mesh.id == referenceId;
        mesh.score = 0.0;
        mesh.hasScore = false;
    }

    meshes_ = std::move(meshes);
    referenceId_ = referenceId;
    selectedMeshId_ = meshes_.front().id;
    phase_ = WorkspacePhase::Ready;
    phaseBeforeLoading_ = WorkspacePhase::Ready;
    ++generation_;
    return OperationResult::success();
}

OperationResult WorkspaceState::setSelectedMesh(MeshId id)
{
    if (!contains(id))
        return OperationResult::failure(
            QStringLiteral("Selected mesh does not exist in this workspace."));

    selectedMeshId_ = id;
    return OperationResult::success();
}

OperationResult WorkspaceState::setReference(MeshId id)
{
    if (!contains(id))
        return OperationResult::failure(
            QStringLiteral("Reference mesh does not exist in this workspace."));

    referenceId_ = id;
    for (MeshEntry& mesh : meshes_) {
        mesh.isReference = mesh.id == id;
        if (mesh.presentation.mode == ColorMode::PrecisionResult ||
            mesh.presentation.mode == ColorMode::NormalAgreementResult)
            mesh.presentation = {};
        mesh.score = 0.0;
        mesh.hasScore = false;
    }
    return OperationResult::success();
}

OperationResult WorkspaceState::beginAnalysis()
{
    if (phase_ != WorkspacePhase::Ready || meshes_.isEmpty()) {
        return OperationResult::failure(
            QStringLiteral("Analysis requires a ready workspace."));
    }
    phase_ = WorkspacePhase::Analyzing;
    return OperationResult::success();
}

void WorkspaceState::finishAnalysis()
{
    if (phase_ == WorkspacePhase::Analyzing)
        phase_ = WorkspacePhase::Ready;
}

OperationResult WorkspaceState::validateColorUpdates(
    const QVector<MeshColorStateUpdate>& updates) const
{
    if (phase_ != WorkspacePhase::Ready && phase_ != WorkspacePhase::Analyzing) {
        return OperationResult::failure(
            QStringLiteral("Coloring requires a ready workspace."));
    }
    if (updates.isEmpty()) {
        return OperationResult::failure(
            QStringLiteral("A color update batch cannot be empty."));
    }

    QSet<MeshId> updatedIds;
    for (const MeshColorStateUpdate& update : updates) {
        if (updatedIds.contains(update.meshId)) {
            return OperationResult::failure(
                QStringLiteral("Color update mesh IDs must be unique."));
        }
        updatedIds.insert(update.meshId);
        if (mesh(update.meshId) == nullptr) {
            return OperationResult::failure(
                QStringLiteral("Color update mesh does not exist in this workspace."));
        }
        if (update.hasScore && !std::isfinite(update.score)) {
            return OperationResult::failure(
                QStringLiteral("Color update scores must be finite."));
        }

        const ColorPresentation& presentation = update.presentation;
        switch (presentation.mode) {
        case ColorMode::Default:
            if (presentation.uniformColor.isValid() ||
                !presentation.faceColors.isEmpty() || update.hasScore ||
                update.score != 0.0) {
                return OperationResult::failure(
                    QStringLiteral("Default coloring cannot retain color or score data."));
            }
            break;
        case ColorMode::UniformColor:
            if (!presentation.uniformColor.isValid() ||
                !presentation.faceColors.isEmpty() || update.hasScore ||
                update.score != 0.0) {
                return OperationResult::failure(
                    QStringLiteral("Uniform coloring requires a valid color and no analysis score."));
            }
            break;
        case ColorMode::PrecisionResult:
        case ColorMode::NormalAgreementResult:
            if (presentation.uniformColor.isValid() || !update.hasScore ||
                update.score < 0.0 || update.score > 1.0 ||
                presentation.faceColors.isEmpty()) {
                return OperationResult::failure(
                    QStringLiteral("Analytical coloring requires face colors and a score."));
            }
            for (const QColor& color : presentation.faceColors) {
                if (!color.isValid()) {
                    return OperationResult::failure(
                        QStringLiteral("Analytical face colors must be valid."));
                }
            }
            break;
        default:
            return OperationResult::failure(
                QStringLiteral("Color update mode is invalid."));
        }
    }
    return OperationResult::success();
}

OperationResult WorkspaceState::prepareColorUpdates(
    const QVector<MeshColorStateUpdate>& updates,
    PreparedColorStateUpdate* prepared) const
{
    if (prepared == nullptr) {
        return OperationResult::failure(
            QStringLiteral("A prepared color-state destination is required."));
    }
    *prepared = PreparedColorStateUpdate{};

    const OperationResult validation = validateColorUpdates(updates);
    if (!validation.ok)
        return validation;

    try {
        PreparedColorStateUpdate candidate;
        candidate.meshes_ = meshes_;
        for (const MeshColorStateUpdate& update : updates) {
            for (MeshEntry& entry : candidate.meshes_) {
                if (entry.id != update.meshId)
                    continue;
                entry.presentation = update.presentation;
                entry.score = update.hasScore ? update.score : 0.0;
                entry.hasScore = update.hasScore;
                break;
            }
        }
        candidate.generation_ = generation_;
        candidate.phase_ = phase_;
        candidate.valid_ = true;
        *prepared = std::move(candidate);
    }
    catch (const std::exception& exception) {
        return OperationResult::failure(QString::fromLocal8Bit(exception.what()));
    }
    catch (...) {
        return OperationResult::failure(
            QStringLiteral("Preparing the color-state transaction failed."));
    }
    return OperationResult::success();
}

void WorkspaceState::commitPreparedColorUpdates(
    PreparedColorStateUpdate&& prepared) noexcept
{
    const bool canCommit = prepared.valid_ && prepared.generation_ == generation_ &&
                           prepared.phase_ == phase_;
    if (!canCommit)
        return;
    Q_ASSERT_X(
        canCommit,
        "WorkspaceState::commitPreparedColorUpdates",
        "prepared color state must be committed without an intervening state change");
    meshes_.swap(prepared.meshes_);
    prepared.valid_ = false;
}

void WorkspaceState::enterFatalError()
{
    phase_ = WorkspacePhase::FatalError;
}

bool WorkspaceState::contains(MeshId id) const
{
    for (const MeshEntry& mesh : meshes_) {
        if (mesh.id == id)
            return true;
    }
    return false;
}
