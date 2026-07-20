#include "workspace_controller.h"

#include <algorithm>
#include <utility>

#include <QDateTime>
#include <QFileInfo>
#include <QScopedValueRollback>

#include "../core/reference_resolver.h"
#include "../core/renderer_adapter.h"
#include "../core/workspace_state.h"
#include "../services/camera_pose_store.h"
#include "../services/mesh_import_service.h"
#include "../services/mesh_color_service.h"

namespace
{
QString analysisScoreLabel(const MeshEntry& mesh);

SceneDescriptor makeSceneDescriptor(
    quint64 generation,
    const QVector<MeshEntry>& entries,
    MeshId referenceId,
    SceneLayoutMode layoutMode,
    const CameraPose& initialCamera = {})
{
    SceneDescriptor scene;
    scene.generation = generation;
    scene.referenceId = referenceId;
    scene.layoutMode = layoutMode;
    scene.initialCamera = initialCamera;
    scene.meshes.reserve(entries.size());
    for (const MeshEntry& entry : entries) {
        scene.meshes.append(
            {entry.id,
             entry.resourceId,
             entry.displayName,
             entry.id == referenceId,
             entry.presentation,
             analysisScoreLabel(entry),
             entry.visible});
    }
    return scene;
}

WorkspaceImportOutcome failureOutcome(
    const OperationResult& result,
    QVector<QPair<QString, QString>> fileErrors = {})
{
    return {result, std::move(fileErrors), {}};
}

QString analysisScoreLabel(const MeshEntry& mesh)
{
    if (mesh.analysisSummary.kind == AnalysisKind::DistanceToReference) {
        return QStringLiteral("> threshold %1%").arg(
            mesh.analysisSummary.distance.aboveThresholdVertexFraction * 100.0,
            0,
            'f',
            1);
    }
    if (mesh.analysisSummary.kind == AnalysisKind::DoubleLayer) {
        return QStringLiteral("DL %1%").arg(
            mesh.analysisSummary.doubleLayer.affectedFaceFraction * 100.0,
            0,
            'f',
            1);
    }
    if (!mesh.hasScore)
        return {};

    QString prefix;
    if (mesh.presentation.mode == ColorMode::PrecisionResult)
        prefix = QStringLiteral("P");
    else if (mesh.presentation.mode == ColorMode::NormalAgreementResult)
        prefix = QStringLiteral("N");
    else
        return {};
    return QStringLiteral("%1 %2").arg(prefix).arg(mesh.score, 0, 'f', 3);
}

QString appendNotice(const QString& first, const QString& second)
{
    if (first.isEmpty())
        return second;
    if (second.isEmpty())
        return first;
    return first + QLatin1Char('\n') + second;
}

QDateTime savedAt(const QString& value)
{
    QDateTime parsed = QDateTime::fromString(value, Qt::ISODateWithMs);
    if (!parsed.isValid())
        parsed = QDateTime::fromString(value, Qt::ISODate);
    return parsed;
}

bool savedCameraPoseLess(
    const SavedCameraPose& left,
    const SavedCameraPose& right)
{
    const QDateTime leftSavedAt = savedAt(left.savedAtUtc);
    const QDateTime rightSavedAt = savedAt(right.savedAtUtc);
    if (leftSavedAt.isValid() != rightSavedAt.isValid())
        return !leftSavedAt.isValid();
    if (leftSavedAt.isValid() && leftSavedAt != rightSavedAt)
        return leftSavedAt < rightSavedAt;
    if (!leftSavedAt.isValid() && left.savedAtUtc != right.savedAtUtc)
        return left.savedAtUtc < right.savedAtUtc;
    return left.viewId < right.viewId;
}

QString firstUuidCandidate(const MeshEntry& mesh)
{
    for (const QString& candidate : mesh.uuidCandidates) {
        const QString normalized = CameraPoseStore::normalizeUuid(candidate);
        if (!normalized.isEmpty())
            return normalized;
    }
    return {};
}

QString suggestedWorkspaceUid(const QStringList& paths)
{
    for (const QString& path : paths) {
        const QFileInfo info(path);
        if (info.suffix().compare(QStringLiteral("mlp"), Qt::CaseInsensitive) != 0)
            continue;
        return info.completeBaseName().trimmed();
    }
    return {};
}
} // namespace

QString resolveWorkspaceUuid(
    const QVector<MeshEntry>& meshes,
    MeshId referenceId)
{
    for (const MeshEntry& mesh : meshes) {
        if (mesh.id != referenceId)
            continue;
        const QString resolved = firstUuidCandidate(mesh);
        if (!resolved.isEmpty())
            return resolved;
        break;
    }

    for (const MeshEntry& mesh : meshes) {
        if (mesh.id == referenceId)
            continue;
        const QString resolved = firstUuidCandidate(mesh);
        if (!resolved.isEmpty())
            return resolved;
    }
    return {};
}

WorkspaceController::WorkspaceController(
    WorkspaceState& state,
    IMeshImportService& importer,
    IRendererAdapter& renderer,
    ISurfaceComparer& comparer,
    ICameraPoseStore& cameraStore,
    QObject* parent)
    : QObject(parent),
      state_(state),
      importer_(importer),
      renderer_(renderer),
      comparer_(comparer),
      cameraStore_(cameraStore)
{
    connectRendererEvents();
}

void WorkspaceController::connectRendererEvents()
{
    RendererEvents events;
    events.selectedMeshChanged = [this](MeshId id) {
        const OperationResult result = selectMesh(id);
        if (!result.ok)
            publishStatusMessage(result.error);
    };
    events.rendererError = [this](const QString& error) {
        publishStatusMessage(error);
    };
    renderer_.setEvents(std::move(events));
}

void WorkspaceController::disconnectRendererEvents()
{
    renderer_.setEvents({});
}

WorkspaceController::~WorkspaceController()
{
    colorService_.reset();
    disconnectRendererEvents();
    renderer_.clearScene();
    repository_.reset();
}

WorkspaceImportOutcome WorkspaceController::importMeshes(const QStringList& paths)
{
    if (handlingCallback_) {
        return failureOutcome(OperationResult::failure(QStringLiteral(
            "Workspace replacement is unavailable during a renderer callback.")));
    }
    if (replacingWorkspace_) {
        return failureOutcome(OperationResult::failure(
            QStringLiteral("A workspace replacement is already in progress.")));
    }
    if (cameraCommandInProgress_) {
        return failureOutcome(OperationResult::failure(QStringLiteral(
            "Workspace replacement is unavailable during a camera-pose command.")));
    }
    QScopedValueRollback<bool> replacementGuard(replacingWorkspace_, true);

    if (colorService_)
        colorService_->cancelAnalysis();
    state_.beginLoading();
    StagedWorkspace staged = importer_.stage(paths);
    if (!staged.result.ok) {
        state_.cancelLoading();
        return failureOutcome(staged.result, std::move(staged.fileErrors));
    }
    if (!staged.repository) {
        state_.cancelLoading();
        return failureOutcome(OperationResult::failure(
            QStringLiteral("Import staging returned no mesh repository.")));
    }

    const ReferenceResolution reference = resolveReference(staged.entries);
    const OperationResult validation =
        state_.validateWorkspace(staged.entries, reference.referenceId);
    if (!validation.ok) {
        state_.cancelLoading();
        return failureOutcome(validation);
    }

    const SceneDescriptor scene = makeSceneDescriptor(
        state_.generation() + 1,
        staged.entries,
        reference.referenceId,
        staged.layoutMode);
    const OperationResult prepared =
        renderer_.prepareScene(scene, *staged.repository);
    if (!prepared.ok) {
        renderer_.discardPreparedScene();
        state_.cancelLoading();
        return failureOutcome(prepared);
    }

    // Old viewports can call directly into the controller. Disconnect those
    // callbacks before the adapter destroys an existing committed grid, then
    // restore the same application-level event contract for the new grid.
    const bool replacesCommittedWorkspace = repository_ != nullptr;
    if (replacesCommittedWorkspace)
        disconnectRendererEvents();
    renderer_.commitPreparedScene();
    if (replacesCommittedWorkspace)
        connectRendererEvents();
    colorService_.reset();
    repository_ = std::move(staged.repository);
    const OperationResult committed =
        state_.commitWorkspace(
            std::move(staged.entries),
            reference.referenceId,
            staged.layoutMode);
    Q_ASSERT_X(
        committed.ok,
        "WorkspaceController::importMeshes",
        "prevalidated workspace commit must succeed");
    if (!committed.ok) {
        state_.enterFatalError();
        return failureOutcome(OperationResult::failure(
            QStringLiteral("The validated workspace could not be committed.")));
    }

    renderer_.setReferenceMesh(state_.referenceId());
    renderer_.setSelectedMesh(state_.selectedMeshId());
    suggestedWorkspaceUid_ = suggestedWorkspaceUid(paths);
    workspaceUidManuallyAssigned_ = false;
    workspaceUuid_ =
        resolveWorkspaceUuid(state_.meshes(), state_.referenceId());
    const QString cameraNotice = restoreLatestCameraPose();
    createColorService();
    publishWorkspaceChanged();
    return {OperationResult::success(),
            {},
            appendNotice(reference.notice, cameraNotice)};
}

OperationResult WorkspaceController::setLayoutMode(SceneLayoutMode layoutMode)
{
    if (handlingCallback_) {
        return OperationResult::failure(
            QStringLiteral("View switching is unavailable during a renderer callback."));
    }
    if (replacingWorkspace_) {
        return OperationResult::failure(
            QStringLiteral("View switching is unavailable during workspace replacement."));
    }
    if (cameraCommandInProgress_) {
        return OperationResult::failure(
            QStringLiteral("View switching is unavailable during a camera-pose command."));
    }
    if (state_.phase() != WorkspacePhase::Ready || repository_ == nullptr) {
        return OperationResult::failure(
            QStringLiteral("View switching requires an idle ready workspace."));
    }
    if (state_.layoutMode() == layoutMode)
        return OperationResult::success();

    QScopedValueRollback<bool> replacementGuard(replacingWorkspace_, true);
    const SceneDescriptor scene = makeSceneDescriptor(
        state_.generation(),
        state_.meshes(),
        state_.referenceId(),
        layoutMode,
        renderer_.captureCamera());
    const OperationResult prepared =
        renderer_.prepareScene(scene, *repository_);
    if (!prepared.ok) {
        renderer_.discardPreparedScene();
        return prepared;
    }

    disconnectRendererEvents();
    renderer_.commitPreparedScene();
    connectRendererEvents();
    const OperationResult changed = state_.setLayoutMode(layoutMode);
    Q_ASSERT_X(
        changed.ok,
        "WorkspaceController::setLayoutMode",
        "a synchronously prevalidated layout change must succeed");
    if (!changed.ok) {
        state_.enterFatalError();
        return OperationResult::failure(
            QStringLiteral("The prepared view layout could not be committed."));
    }

    renderer_.setReferenceMesh(state_.referenceId());
    renderer_.setSelectedMesh(state_.selectedMeshId());
    publishWorkspaceChanged();
    return OperationResult::success();
}

OperationResult WorkspaceController::setMeshVisible(MeshId meshId, bool visible)
{
    if (handlingCallback_) {
        return OperationResult::failure(
            QStringLiteral("Layer visibility is unavailable during a renderer callback."));
    }
    if (replacingWorkspace_) {
        return OperationResult::failure(
            QStringLiteral("Layer visibility is unavailable during workspace replacement."));
    }
    if (cameraCommandInProgress_) {
        return OperationResult::failure(
            QStringLiteral("Layer visibility is unavailable during a camera-pose command."));
    }
    if ((state_.phase() != WorkspacePhase::Ready &&
         state_.phase() != WorkspacePhase::Analyzing) ||
        state_.layoutMode() != SceneLayoutMode::Overlay) {
        return OperationResult::failure(
            QStringLiteral("Layer visibility is available in Overlay view."));
    }

    const MeshEntry* target = state_.mesh(meshId);
    if (target == nullptr) {
        return OperationResult::failure(
            QStringLiteral("Visibility target does not exist in this workspace."));
    }
    if (target->visible == visible)
        return OperationResult::success();
    if (!visible) {
        int visibleCount = 0;
        for (const MeshEntry& mesh : state_.meshes())
            visibleCount += mesh.visible ? 1 : 0;
        if (visibleCount <= 1) {
            return OperationResult::failure(
                QStringLiteral("At least one mesh layer must remain visible."));
        }
    }

    const OperationResult rendered =
        renderer_.setMeshVisible(meshId, visible);
    if (!rendered.ok)
        return rendered;

    const OperationResult changed = state_.setMeshVisible(meshId, visible);
    Q_ASSERT_X(
        changed.ok,
        "WorkspaceController::setMeshVisible",
        "a synchronously prevalidated visibility change must succeed");
    if (!changed.ok) {
        state_.enterFatalError();
        return OperationResult::failure(
            QStringLiteral("The rendered layer visibility could not be committed."));
    }
    publishWorkspaceChanged();
    return OperationResult::success();
}

OperationResult WorkspaceController::selectMesh(MeshId id)
{
    if (handlingCallback_) {
        return OperationResult::failure(
            QStringLiteral("Mesh selection is unavailable during a renderer callback."));
    }
    if (replacingWorkspace_) {
        return OperationResult::failure(
            QStringLiteral("Mesh selection is unavailable during workspace replacement."));
    }
    if (cameraCommandInProgress_) {
        return OperationResult::failure(
            QStringLiteral("Mesh selection is unavailable during a camera-pose command."));
    }
    const OperationResult result = state_.setSelectedMesh(id);
    if (!result.ok)
        return result;
    renderer_.setSelectedMesh(id);
    publishWorkspaceChanged();
    return OperationResult::success();
}

OperationResult WorkspaceController::setReference(MeshId id)
{
    if (handlingCallback_) {
        return OperationResult::failure(
            QStringLiteral("Reference selection is unavailable during a renderer callback."));
    }
    if (replacingWorkspace_) {
        return OperationResult::failure(
            QStringLiteral("Reference selection is unavailable during workspace replacement."));
    }
    if (cameraCommandInProgress_) {
        return OperationResult::failure(
            QStringLiteral("Reference selection is unavailable during a camera-pose command."));
    }
    if (state_.phase() != WorkspacePhase::Ready) {
        return OperationResult::failure(
            QStringLiteral("Reference selection requires an idle ready workspace."));
    }
    if (state_.mesh(id) == nullptr) {
        return OperationResult::failure(
            QStringLiteral("Reference mesh does not exist in this workspace."));
    }
    if (id == state_.referenceId())
        return OperationResult::success();

    QVector<MeshColorPresentationUpdate> clearUpdates;
    for (const MeshEntry& mesh : state_.meshes()) {
        if (isReferenceDependent(mesh.presentation)) {
            clearUpdates.append({mesh.id, ColorPresentation{}});
        }
    }
    if (!clearUpdates.isEmpty()) {
        const OperationResult rendered =
            renderer_.setColorPresentations(clearUpdates);
        if (!rendered.ok)
            return rendered;
    }

    const OperationResult changed = state_.setReference(id);
    Q_ASSERT_X(
        changed.ok,
        "WorkspaceController::setReference",
        "a synchronously prevalidated Reference change must succeed");
    if (!changed.ok)
        return changed;

    renderer_.setReferenceMesh(id);
    if (!workspaceUidManuallyAssigned_) {
        workspaceUuid_ =
            resolveWorkspaceUuid(state_.meshes(), state_.referenceId());
    }
    publishOverlayUpdates(committedAnalysisOverlays());
    publishWorkspaceChanged();
    return OperationResult::success();
}

OperationResult WorkspaceController::startAnalysis(
    SurfaceComparisonMetric metric,
    const SurfaceComparisonOptions& options)
{
    if (handlingCallback_) {
        return publishAnalysisStartFailure(
            metric,
            OperationResult::failure(QStringLiteral(
                "Analysis is unavailable during a renderer callback.")));
    }
    if (replacingWorkspace_) {
        return publishAnalysisStartFailure(
            metric,
            OperationResult::failure(QStringLiteral(
                "Analysis is unavailable during workspace replacement.")));
    }
    if (cameraCommandInProgress_) {
        return publishAnalysisStartFailure(
            metric,
            OperationResult::failure(QStringLiteral(
                "Analysis is unavailable during a camera-pose command.")));
    }
    if (!colorService_) {
        return publishAnalysisStartFailure(
            metric,
            OperationResult::failure(QStringLiteral(
                "Analysis requires a committed workspace.")));
    }

    AnalysisRequest request;
    request.generation = state_.generation();
    request.referenceId = state_.referenceId();
    request.metric = metric;
    request.options = options;
    for (const MeshEntry& mesh : state_.meshes()) {
        if (metric == SurfaceComparisonMetric::DoubleLayer ||
            !mesh.isReference) {
            request.targetIds.append(mesh.id);
        }
    }
    const OperationResult started = colorService_->startAnalysis(request);
    if (!started.ok)
        return publishAnalysisStartFailure(metric, started);

    QVector<MeshAnalysisOverlayUpdate> overlays;
    overlays.reserve(request.targetIds.size());
    for (MeshId targetId : request.targetIds)
        overlays.append({targetId, QStringLiteral("0%")});
    publishOverlayUpdates(overlays);
    publishWorkspaceChanged();
    return OperationResult::success();
}

void WorkspaceController::cancelAnalysis()
{
    if (handlingCallback_ || cameraCommandInProgress_)
        return;
    if (colorService_)
        colorService_->cancelAnalysis();
}

OperationResult WorkspaceController::setUniformColor(
    MeshId meshId,
    const QColor& color)
{
    if (handlingCallback_) {
        return OperationResult::failure(
            QStringLiteral("Coloring is unavailable during a renderer callback."));
    }
    if (replacingWorkspace_) {
        return OperationResult::failure(
            QStringLiteral("Coloring is unavailable during workspace replacement."));
    }
    if (cameraCommandInProgress_) {
        return OperationResult::failure(
            QStringLiteral("Coloring is unavailable during a camera-pose command."));
    }
    if (!colorService_) {
        return OperationResult::failure(
            QStringLiteral("Coloring requires a committed workspace."));
    }
    const OperationResult colored = colorService_->setUniformColor(meshId, color);
    if (!colored.ok)
        return colored;
    publishOverlayUpdates({{meshId, QString()}});
    publishWorkspaceChanged();
    return OperationResult::success();
}

OperationResult WorkspaceController::clearColoring()
{
    if (handlingCallback_) {
        return OperationResult::failure(QStringLiteral(
            "Clearing coloring is unavailable during a renderer callback."));
    }
    if (replacingWorkspace_) {
        return OperationResult::failure(
            QStringLiteral("Clearing coloring is unavailable during workspace replacement."));
    }
    if (cameraCommandInProgress_) {
        return OperationResult::failure(QStringLiteral(
            "Clearing coloring is unavailable during a camera-pose command."));
    }
    if (!colorService_) {
        return OperationResult::failure(
            QStringLiteral("Clearing coloring requires a committed workspace."));
    }
    const OperationResult cleared = colorService_->clearColoring();
    if (!cleared.ok)
        return cleared;
    publishOverlayUpdates(committedAnalysisOverlays());
    publishWorkspaceChanged();
    return OperationResult::success();
}

CameraPanelSnapshot WorkspaceController::cameraPanelSnapshot() const
{
    CameraPanelSnapshot snapshot;
    snapshot.workspaceUuid = workspaceUuid_;
    snapshot.suggestedWorkspaceUid = suggestedWorkspaceUid_;
    if (workspaceUuid_.isEmpty())
        return snapshot;
    if (cameraCommandInProgress_) {
        snapshot.result = OperationResult::failure(
            QStringLiteral("A camera-pose command is already in progress."));
        return snapshot;
    }

    QScopedValueRollback<bool> commandGuard(cameraCommandInProgress_, true);
    OperationResult listed;
    const QVector<SavedCameraPose> poses =
        cameraStore_.list(workspaceUuid_, &listed);
    if (!listed.ok) {
        snapshot.result = listed;
        return snapshot;
    }

    snapshot.poses.reserve(poses.size());
    for (const SavedCameraPose& saved : poses)
        snapshot.poses.append({saved.viewId, saved.savedAtUtc, saved.tags});
    return snapshot;
}

OperationResult WorkspaceController::setCameraPoseUid(const QString& uid)
{
    if (handlingCallback_) {
        return OperationResult::failure(QStringLiteral(
            "Camera UID changes are unavailable during an application callback."));
    }
    if (replacingWorkspace_) {
        return OperationResult::failure(QStringLiteral(
            "Camera UID changes are unavailable during workspace replacement."));
    }
    if (cameraCommandInProgress_) {
        return OperationResult::failure(
            QStringLiteral("A camera-pose command is already in progress."));
    }
    if (state_.phase() != WorkspacePhase::Ready || repository_ == nullptr) {
        return OperationResult::failure(QStringLiteral(
            "Camera UID changes require an idle ready workspace."));
    }

    const QString normalized = CameraPoseStore::normalizeUuid(uid);
    if (normalized.isEmpty()) {
        return OperationResult::failure(QStringLiteral(
            "UID must be a non-empty identifier of at most 255 characters."));
    }
    if (normalized == workspaceUuid_)
        return OperationResult::success();

    workspaceUuid_ = normalized;
    workspaceUidManuallyAssigned_ = true;
    publishWorkspaceChanged();
    return OperationResult::success();
}

OperationResult WorkspaceController::saveCurrentCameraPose(QString* savedViewId)
{
    return saveCurrentCameraPoseImpl(nullptr, savedViewId);
}

OperationResult WorkspaceController::saveCurrentCameraPoseWithScreenshot(
    QImage& screenshot,
    QString* savedViewId)
{
    return saveCurrentCameraPoseImpl(&screenshot, savedViewId);
}

OperationResult WorkspaceController::saveCurrentCameraPoseImpl(
    QImage* screenshot,
    QString* savedViewId)
{
    const OperationResult validation = validateCameraMutation();
    if (!validation.ok) {
        publishCameraSaveFinished(validation);
        return validation;
    }

    QScopedValueRollback<bool> commandGuard(cameraCommandInProgress_, true);
    QImage captured;
    if (screenshot != nullptr) {
        const OperationResult imageResult = renderer_.captureImage(captured);
        if (!imageResult.ok) {
            publishCameraSaveFinished(imageResult);
            return imageResult;
        }
        if (captured.isNull()) {
            const OperationResult result = OperationResult::failure(
                QStringLiteral("The renderer returned an empty screenshot."));
            publishCameraSaveFinished(result);
            return result;
        }
    }

    const CameraPose pose = renderer_.captureCamera();
    QString committedViewId;
    const OperationResult saved =
        cameraStore_.save(workspaceUuid_, pose, &committedViewId);
    if (!saved.ok) {
        publishCameraSaveFinished(saved);
        return saved;
    }

    if (screenshot != nullptr)
        *screenshot = std::move(captured);
    if (savedViewId != nullptr)
        *savedViewId = committedViewId;
    const OperationResult result = OperationResult::success();
    publishCameraSaveFinished(result);
    return result;
}

OperationResult WorkspaceController::applyCameraPose(const QString& viewId)
{
    const OperationResult validation = validateCameraMutation();
    if (!validation.ok) {
        publishCameraApplyFinished(validation);
        return validation;
    }
    if (viewId.isEmpty()) {
        const OperationResult result = OperationResult::failure(
            QStringLiteral("Select a saved camera pose to apply."));
        publishCameraApplyFinished(result);
        return result;
    }

    QScopedValueRollback<bool> commandGuard(cameraCommandInProgress_, true);
    CameraPose pose;
    const OperationResult loaded =
        cameraStore_.load(workspaceUuid_, viewId, &pose);
    if (!loaded.ok) {
        publishCameraApplyFinished(loaded);
        return loaded;
    }
    const OperationResult restored = renderer_.restoreCamera(pose);
    publishCameraApplyFinished(restored);
    return restored;
}

OperationResult WorkspaceController::setCameraPoseTags(
    const QString& viewId,
    const QStringList& tags)
{
    const OperationResult validation = validateCameraMutation();
    if (!validation.ok)
        return validation;
    if (viewId.isEmpty()) {
        return OperationResult::failure(
            QStringLiteral("Select a saved camera pose to update its tags."));
    }

    QScopedValueRollback<bool> commandGuard(cameraCommandInProgress_, true);
    return cameraStore_.setTags(workspaceUuid_, viewId, tags);
}

OperationResult WorkspaceController::deleteCameraPose(const QString& viewId)
{
    const OperationResult validation = validateCameraMutation();
    if (!validation.ok)
        return validation;
    if (viewId.isEmpty()) {
        return OperationResult::failure(
            QStringLiteral("Select a saved camera pose to delete."));
    }

    QScopedValueRollback<bool> commandGuard(cameraCommandInProgress_, true);
    return cameraStore_.remove(workspaceUuid_, viewId);
}

QString WorkspaceController::restoreLatestCameraPose()
{
    if (workspaceUuid_.isEmpty())
        return QStringLiteral("No UID was found in this workspace.");

    QScopedValueRollback<bool> commandGuard(cameraCommandInProgress_, true);
    const auto publishAutoRestoreFailure = [this](
                                               const OperationResult& failure) {
        const QString notice =
            QStringLiteral("Camera pose auto-restore was skipped: %1")
                .arg(failure.error);
        publishCameraApplyFinished(OperationResult::failure(notice));
        return notice;
    };
    OperationResult listed;
    QVector<SavedCameraPose> poses = cameraStore_.list(workspaceUuid_, &listed);
    if (!listed.ok)
        return publishAutoRestoreFailure(listed);
    if (poses.isEmpty())
        return {};

    std::sort(poses.begin(), poses.end(), savedCameraPoseLess);
    const SavedCameraPose& latest = poses.back();
    CameraPose pose;
    const OperationResult loaded =
        cameraStore_.load(workspaceUuid_, latest.viewId, &pose);
    if (!loaded.ok)
        return publishAutoRestoreFailure(loaded);
    const OperationResult restored = renderer_.restoreCamera(pose);
    if (!restored.ok)
        return publishAutoRestoreFailure(restored);
    publishCameraApplyFinished(restored);
    return {};
}

OperationResult WorkspaceController::validateCameraMutation() const
{
    if (handlingCallback_) {
        return OperationResult::failure(QStringLiteral(
            "Camera pose changes are unavailable during an application callback."));
    }
    if (replacingWorkspace_) {
        return OperationResult::failure(QStringLiteral(
            "Camera pose changes are unavailable during workspace replacement."));
    }
    if (cameraCommandInProgress_) {
        return OperationResult::failure(
            QStringLiteral("A camera-pose command is already in progress."));
    }
    if (state_.phase() != WorkspacePhase::Ready) {
        return OperationResult::failure(QStringLiteral(
            "Camera pose changes require an idle ready workspace."));
    }
    if (workspaceUuid_.isEmpty()) {
        return OperationResult::failure(
            QStringLiteral("No UID was found in this workspace."));
    }
    return OperationResult::success();
}

void WorkspaceController::createColorService()
{
    Q_ASSERT(repository_);
    colorService_.reset(new MeshColorService(
        *repository_, comparer_, state_, renderer_));
    connect(
        colorService_.get(),
        &MeshColorService::analysisProgress,
        this,
        [this](
            quint64 generation,
            quint64 batchSerial,
            MeshId meshId,
            int percent,
            const QString& message) {
            QScopedValueRollback<bool> callbackGuard(handlingCallback_, true);
            publishOverlayUpdates(
                {{meshId, QStringLiteral("%1%").arg(qBound(0, percent, 100))}});
            emit analysisProgress(
                generation,
                batchSerial,
                meshId,
                percent,
                message);
        });
    connect(
        colorService_.get(),
        &MeshColorService::analysisFinished,
        this,
        [this](AnalysisBatchResult result) {
            QScopedValueRollback<bool> callbackGuard(handlingCallback_, true);
            publishOverlayUpdates(committedAnalysisOverlays());
            if (!result.result.ok)
                publishStatusMessage(result.result.error);
            publishWorkspaceChanged();
            emit analysisFinished(std::move(result));
        });
}

void WorkspaceController::publishStatusMessage(const QString& message)
{
    QScopedValueRollback<bool> callbackGuard(handlingCallback_, true);
    emit statusMessage(message);
}

void WorkspaceController::publishWorkspaceChanged()
{
    QScopedValueRollback<bool> callbackGuard(handlingCallback_, true);
    emit workspaceChanged();
}

OperationResult WorkspaceController::publishAnalysisStartFailure(
    SurfaceComparisonMetric metric,
    OperationResult result)
{
    QScopedValueRollback<bool> callbackGuard(handlingCallback_, true);
    emit analysisStartFailed(metric, result.error);
    return result;
}

void WorkspaceController::publishCameraSaveFinished(
    const OperationResult& result)
{
    QScopedValueRollback<bool> callbackGuard(handlingCallback_, true);
    emit cameraSaveFinished(result.ok, result.error);
}

void WorkspaceController::publishCameraApplyFinished(
    const OperationResult& result)
{
    QScopedValueRollback<bool> callbackGuard(handlingCallback_, true);
    emit cameraApplyFinished(result.ok, result.error);
}

void WorkspaceController::publishOverlayUpdates(
    const QVector<MeshAnalysisOverlayUpdate>& updates)
{
    if (updates.isEmpty())
        return;
    const OperationResult published = renderer_.setAnalysisOverlays(updates);
    if (!published.ok)
        publishStatusMessage(published.error);
}

QVector<MeshAnalysisOverlayUpdate>
WorkspaceController::committedAnalysisOverlays() const
{
    QVector<MeshAnalysisOverlayUpdate> overlays;
    overlays.reserve(state_.meshes().size());
    for (const MeshEntry& mesh : state_.meshes())
        overlays.append({mesh.id, analysisScoreLabel(mesh)});
    return overlays;
}
