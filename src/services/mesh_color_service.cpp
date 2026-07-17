#include "mesh_color_service.h"

#include <cmath>
#include <exception>
#include <utility>

#include <QMetaObject>
#include <QSet>
#include <QThread>

#include "core/renderer_adapter.h"
#include "core/workspace_state.h"

namespace
{
struct TargetSnapshot
{
    MeshId meshId = 0;
    SurfaceMeshSnapshot surface;
};

OperationResult copySurfaceSnapshot(
    const IMeshGeometryView& geometry,
    SurfaceMeshSnapshot* snapshot)
{
    const int vertexCount = geometry.vertexCount();
    const int faceCount = geometry.faceCount();
    if (vertexCount <= 0 || faceCount <= 0) {
        return OperationResult::failure(
            QStringLiteral("Analysis geometry must contain vertices and faces."));
    }

    SurfaceMeshSnapshot candidate;
    candidate.vertices.reserve(vertexCount);
    candidate.vertexNormals.reserve(vertexCount);
    for (int index = 0; index < vertexCount; ++index) {
        const MeshPoint3D position = geometry.vertexPosition(index);
        const MeshPoint3D normal = geometry.vertexNormal(index);
        for (int component = 0; component < 3; ++component) {
            if (!std::isfinite(position[component]) ||
                !std::isfinite(normal[component])) {
                return OperationResult::failure(
                    QStringLiteral("Analysis geometry must contain finite coordinates and normals."));
            }
        }
        candidate.vertices.append(position);
        candidate.vertexNormals.append(normal);
    }

    candidate.faces.reserve(faceCount);
    for (int index = 0; index < faceCount; ++index) {
        const std::array<int, 3> face = geometry.faceVertexIndices(index);
        for (int vertexIndex : face) {
            if (vertexIndex < 0 || vertexIndex >= vertexCount) {
                return OperationResult::failure(
                    QStringLiteral("Analysis geometry contains an invalid face index."));
            }
        }
        candidate.faces.append(face);
    }
    *snapshot = std::move(candidate);
    return OperationResult::success();
}

AnalysisBatchResult failedBatch(
    quint64 generation,
    quint64 batchSerial,
    SurfaceComparisonMetric metric,
    const QString& error)
{
    AnalysisBatchResult result;
    result.result = OperationResult::failure(error);
    result.generation = generation;
    result.batchSerial = batchSerial;
    result.metric = metric;
    return result;
}
} // namespace

class MeshColorAnalysisThread final : public QThread
{
public:
    MeshColorAnalysisThread(
        MeshColorService& owner,
        ISurfaceComparer& comparer,
        AnalysisRequest request,
        quint64 batchSerial,
        SurfaceMeshSnapshot reference,
        QVector<TargetSnapshot> targets,
        std::shared_ptr<std::atomic_bool> cancellation)
        : owner_(&owner),
          comparer_(comparer),
          request_(std::move(request)),
          batchSerial_(batchSerial),
          reference_(std::move(reference)),
          targets_(std::move(targets)),
          cancellation_(std::move(cancellation))
    {
    }

protected:
    void run() override
    {
        try {
            runBatch();
        }
        catch (const std::exception& exception) {
            owner_->queueCompletion(failedBatch(
                request_.generation,
                batchSerial_,
                request_.metric,
                QString::fromLocal8Bit(exception.what())));
        }
        catch (...) {
            owner_->queueCompletion(failedBatch(
                request_.generation,
                batchSerial_,
                request_.metric,
                QStringLiteral("Surface comparison failed with an unknown exception.")));
        }
    }

private:
    void runBatch()
    {
        QVector<MeshAnalysisResult> staged;
        staged.reserve(targets_.size());
        for (const TargetSnapshot& target : targets_) {
            if (cancellation_->load(std::memory_order_acquire)) {
                owner_->queueCompletion(failedBatch(
                    request_.generation,
                    batchSerial_,
                    request_.metric,
                    QStringLiteral("Analysis cancelled.")));
                return;
            }

            const AnalysisProgress progress =
                [this, meshId = target.meshId](int percent, const QString& message) {
                    if (cancellation_->load(std::memory_order_acquire))
                        return false;
                    owner_->queueProgress(
                        request_.generation,
                        batchSerial_,
                        meshId,
                        percent,
                        message);
                    return !cancellation_->load(std::memory_order_acquire);
                };
            const SurfaceComparisonOutcome outcome = comparer_.compare(
                target.surface,
                reference_,
                request_.metric,
                request_.options,
                progress,
                [this] {
                    return cancellation_->load(std::memory_order_acquire);
                });
            if (!outcome.result.ok) {
                owner_->queueCompletion(failedBatch(
                    request_.generation,
                    batchSerial_,
                    request_.metric,
                    outcome.result.error));
                return;
            }
            bool validScores =
                outcome.comparison.faceScores.size() == target.surface.faces.size() &&
                outcome.comparison.coloredFaceCount == target.surface.faces.size() &&
                std::isfinite(outcome.comparison.globalScore) &&
                outcome.comparison.globalScore >= 0.0 &&
                outcome.comparison.globalScore <= 1.0;
            for (double faceScore : outcome.comparison.faceScores) {
                if (!std::isfinite(faceScore) || faceScore < 0.0 || faceScore > 1.0) {
                    validScores = false;
                    break;
                }
            }
            if (!validScores) {
                owner_->queueCompletion(failedBatch(
                    request_.generation,
                    batchSerial_,
                    request_.metric,
                    QStringLiteral("Analysis returned invalid comparison scores.")));
                return;
            }

            MeshAnalysisResult result;
            result.result = OperationResult::success();
            result.meshId = target.meshId;
            result.globalScore = outcome.comparison.globalScore;
            result.faceColors = surfaceScoreColors(outcome.comparison.faceScores);
            staged.append(std::move(result));
        }

        AnalysisBatchResult result;
        result.result = OperationResult::success();
        result.generation = request_.generation;
        result.batchSerial = batchSerial_;
        result.metric = request_.metric;
        result.meshes = std::move(staged);
        owner_->queueCompletion(std::move(result));
    }

    // MeshColorService always joins this one-shot thread in its destructor,
    // so the owner outlives run() and a cross-thread QPointer is unnecessary.
    MeshColorService* owner_ = nullptr;
    ISurfaceComparer& comparer_;
    AnalysisRequest request_;
    quint64 batchSerial_ = 0;
    SurfaceMeshSnapshot reference_;
    QVector<TargetSnapshot> targets_;
    std::shared_ptr<std::atomic_bool> cancellation_;
};

MeshColorService::MeshColorService(
    const IMeshResourceProvider& resources,
    ISurfaceComparer& comparer,
    WorkspaceState& state,
    IRendererAdapter& renderer,
    QObject* parent)
    : QObject(parent),
      resources_(resources),
      comparer_(comparer),
      state_(state),
      renderer_(renderer)
{
    qRegisterMetaType<AnalysisBatchResult>("AnalysisBatchResult");
}

MeshColorService::~MeshColorService()
{
    shuttingDown_ = true;
    stopAnalysis(false);
}

OperationResult MeshColorService::startAnalysis(const AnalysisRequest& request)
{
    if (QThread::currentThread() != thread()) {
        return OperationResult::failure(
            QStringLiteral("Analysis must be started on the color service owner thread."));
    }
    if (active_ || worker_) {
        return OperationResult::failure(
            QStringLiteral("An analysis batch is already running."));
    }
    if (state_.phase() != WorkspacePhase::Ready) {
        return OperationResult::failure(
            QStringLiteral("Analysis requires a ready workspace."));
    }
    if (request.generation != state_.generation()) {
        return OperationResult::failure(
            QStringLiteral("Analysis request generation is stale."));
    }
    if (request.referenceId != state_.referenceId() ||
        state_.mesh(request.referenceId) == nullptr) {
        return OperationResult::failure(
            QStringLiteral("Analysis request reference does not match the workspace."));
    }

    const OperationResult optionsValidation =
        validateSurfaceComparisonOptions(request.metric, request.options);
    if (!optionsValidation.ok)
        return optionsValidation;

    QSet<MeshId> expectedTargets;
    for (const MeshEntry& mesh : state_.meshes()) {
        if (mesh.id != state_.referenceId())
            expectedTargets.insert(mesh.id);
    }
    QSet<MeshId> requestedTargets;
    for (MeshId targetId : request.targetIds) {
        if (requestedTargets.contains(targetId)) {
            return OperationResult::failure(
                QStringLiteral("Analysis target mesh IDs must be unique."));
        }
        requestedTargets.insert(targetId);
    }
    if (requestedTargets != expectedTargets ||
        request.targetIds.size() != expectedTargets.size()) {
        return OperationResult::failure(QStringLiteral(
            "Analysis targets must contain every non-Reference mesh exactly once."));
    }

    SurfaceMeshSnapshot referenceSnapshot;
    QVector<TargetSnapshot> targetSnapshots;
    try {
        const MeshEntry* referenceEntry = state_.mesh(request.referenceId);
        const IMeshGeometryView* referenceGeometry =
            resources_.geometry(referenceEntry->resourceId);
        if (referenceGeometry == nullptr) {
            return OperationResult::failure(
                QStringLiteral("Reference mesh geometry is unavailable."));
        }
        OperationResult copied =
            copySurfaceSnapshot(*referenceGeometry, &referenceSnapshot);
        if (!copied.ok)
            return copied;

        targetSnapshots.reserve(request.targetIds.size());
        for (MeshId targetId : request.targetIds) {
            const MeshEntry* entry = state_.mesh(targetId);
            Q_ASSERT(entry != nullptr);
            const IMeshGeometryView* geometry = resources_.geometry(entry->resourceId);
            if (geometry == nullptr) {
                return OperationResult::failure(
                    QStringLiteral("Target mesh geometry is unavailable."));
            }
            TargetSnapshot target;
            target.meshId = targetId;
            copied = copySurfaceSnapshot(*geometry, &target.surface);
            if (!copied.ok)
                return copied;
            targetSnapshots.append(std::move(target));
        }
    }
    catch (const std::exception& exception) {
        return OperationResult::failure(QString::fromLocal8Bit(exception.what()));
    }
    catch (...) {
        return OperationResult::failure(
            QStringLiteral("Analysis geometry snapshotting failed."));
    }

    const quint64 candidateSerial = nextBatchSerial_ + 1;
    std::shared_ptr<std::atomic_bool> candidateCancellation;
    std::unique_ptr<MeshColorAnalysisThread> candidateWorker;
    try {
        candidateCancellation = std::make_shared<std::atomic_bool>(false);
        candidateWorker.reset(new MeshColorAnalysisThread(
            *this,
            comparer_,
            request,
            candidateSerial,
            std::move(referenceSnapshot),
            std::move(targetSnapshots),
            candidateCancellation));
    }
    catch (const std::exception& exception) {
        return OperationResult::failure(QString::fromLocal8Bit(exception.what()));
    }

    const OperationResult transition = state_.beginAnalysis();
    if (!transition.ok)
        return transition;

    active_ = true;
    activeGeneration_ = request.generation;
    activeMetric_ = request.metric;
    activeBatchSerial_ = candidateSerial;
    nextBatchSerial_ = candidateSerial;
    cancellation_ = std::move(candidateCancellation);
    worker_ = std::move(candidateWorker);
    worker_->start();
    return OperationResult::success();
}

void MeshColorService::cancelAnalysis()
{
    Q_ASSERT_X(
        QThread::currentThread() == thread(),
        "MeshColorService::cancelAnalysis",
        "analysis lifecycle must be controlled on the owner thread");
    if (QThread::currentThread() != thread())
        return;
    stopAnalysis(true);
}

void MeshColorService::stopAnalysis(bool notify)
{
    if (!active_ && !worker_)
        return;

    const quint64 cancelledGeneration = activeGeneration_;
    const quint64 cancelledSerial = activeBatchSerial_;
    const SurfaceComparisonMetric cancelledMetric = activeMetric_;
    if (cancellation_)
        cancellation_->store(true, std::memory_order_release);
    activeBatchSerial_ = ++nextBatchSerial_;
    joinWorker();
    cancellation_.reset();
    const bool wasActive = active_;
    active_ = false;
    activeGeneration_ = 0;
    state_.finishAnalysis();

    if (notify && wasActive && !shuttingDown_) {
        emit analysisFinished(failedBatch(
            cancelledGeneration,
            cancelledSerial,
            cancelledMetric,
            QStringLiteral("Analysis cancelled.")));
    }
}

OperationResult MeshColorService::setUniformColor(MeshId meshId, const QColor& color)
{
    if (QThread::currentThread() != thread()) {
        return OperationResult::failure(
            QStringLiteral("Uniform coloring must run on the color service owner thread."));
    }
    if (state_.phase() != WorkspacePhase::Ready || active_ || worker_) {
        return OperationResult::failure(
            QStringLiteral("Uniform coloring requires an idle ready workspace."));
    }
    if (state_.mesh(meshId) == nullptr) {
        return OperationResult::failure(
            QStringLiteral("Uniform color target does not exist in this workspace."));
    }
    if (!color.isValid()) {
        return OperationResult::failure(
            QStringLiteral("Uniform color must be valid."));
    }

    ColorPresentation presentation;
    presentation.mode = ColorMode::UniformColor;
    presentation.uniformColor = color;
    const QVector<MeshColorStateUpdate> stateUpdates = {
        {meshId, presentation, 0.0, false}};
    PreparedColorStateUpdate preparedState;
    const OperationResult validation =
        state_.prepareColorUpdates(stateUpdates, &preparedState);
    if (!validation.ok)
        return validation;
    const OperationResult rendered = renderer_.setColorPresentations(
        {{meshId, presentation}});
    if (!rendered.ok)
        return rendered;
    state_.commitPreparedColorUpdates(std::move(preparedState));
    return OperationResult::success();
}

OperationResult MeshColorService::clearColoring()
{
    if (QThread::currentThread() != thread()) {
        return OperationResult::failure(
            QStringLiteral("Clearing coloring must run on the color service owner thread."));
    }
    if (state_.phase() != WorkspacePhase::Ready || active_ || worker_) {
        return OperationResult::failure(
            QStringLiteral("Clearing coloring requires an idle ready workspace."));
    }
    if (state_.meshes().isEmpty()) {
        return OperationResult::failure(
            QStringLiteral("There is no workspace coloring to clear."));
    }

    QVector<MeshColorStateUpdate> stateUpdates;
    QVector<MeshColorPresentationUpdate> rendererUpdates;
    stateUpdates.reserve(state_.meshes().size());
    rendererUpdates.reserve(state_.meshes().size());
    for (const MeshEntry& mesh : state_.meshes()) {
        stateUpdates.append({mesh.id, ColorPresentation{}, 0.0, false});
        rendererUpdates.append({mesh.id, ColorPresentation{}});
    }
    PreparedColorStateUpdate preparedState;
    const OperationResult validation =
        state_.prepareColorUpdates(stateUpdates, &preparedState);
    if (!validation.ok)
        return validation;
    const OperationResult rendered = renderer_.setColorPresentations(rendererUpdates);
    if (!rendered.ok)
        return rendered;
    state_.commitPreparedColorUpdates(std::move(preparedState));
    return OperationResult::success();
}

void MeshColorService::queueProgress(
    quint64 generation,
    quint64 batchSerial,
    MeshId meshId,
    int percent,
    const QString& message)
{
    QMetaObject::invokeMethod(
        this,
        [this, generation, batchSerial, meshId, percent, message] {
            publishProgress(
                generation,
                batchSerial,
                meshId,
                percent,
                message);
        },
        Qt::QueuedConnection);
}

void MeshColorService::queueCompletion(AnalysisBatchResult result)
{
    QMetaObject::invokeMethod(
        this,
        [this, result = std::move(result)]() mutable {
            completeAnalysis(std::move(result));
        },
        Qt::QueuedConnection);
}

void MeshColorService::publishProgress(
    quint64 generation,
    quint64 batchSerial,
    MeshId meshId,
    int percent,
    const QString& message)
{
    if (!active_ || generation != activeGeneration_ ||
        generation != state_.generation() || batchSerial != activeBatchSerial_ ||
        (cancellation_ && cancellation_->load(std::memory_order_acquire))) {
        return;
    }
    emit analysisProgress(generation, batchSerial, meshId, percent, message);
}

void MeshColorService::completeAnalysis(AnalysisBatchResult result)
{
    if (!active_ || result.batchSerial != activeBatchSerial_)
        return;

    joinWorker();
    cancellation_.reset();

    if (result.generation != activeGeneration_ ||
        result.generation != state_.generation()) {
        result.result = OperationResult::failure(
            QStringLiteral("Analysis result belongs to a stale workspace generation."));
    }

    if (result.result.ok) {
        QVector<MeshColorStateUpdate> stateUpdates;
        QVector<MeshColorPresentationUpdate> rendererUpdates;
        stateUpdates.reserve(result.meshes.size());
        rendererUpdates.reserve(result.meshes.size());
        QSet<MeshId> resultIds;
        const ColorMode mode = result.metric == SurfaceComparisonMetric::PrecisionAtThreshold
            ? ColorMode::PrecisionResult
            : ColorMode::NormalAgreementResult;
        for (const MeshAnalysisResult& meshResult : result.meshes) {
            if (resultIds.contains(meshResult.meshId) ||
                state_.mesh(meshResult.meshId) == nullptr ||
                meshResult.faceColors.isEmpty() ||
                !std::isfinite(meshResult.globalScore)) {
                result.result = OperationResult::failure(
                    QStringLiteral("Analysis returned an invalid target result batch."));
                break;
            }
            resultIds.insert(meshResult.meshId);
            ColorPresentation presentation;
            presentation.mode = mode;
            presentation.faceColors = meshResult.faceColors;
            stateUpdates.append(
                {meshResult.meshId, presentation, meshResult.globalScore, true});
            rendererUpdates.append({meshResult.meshId, presentation});
        }

        if (result.result.ok) {
            QSet<MeshId> expectedIds;
            for (const MeshEntry& mesh : state_.meshes()) {
                if (!mesh.isReference)
                    expectedIds.insert(mesh.id);
            }
            if (resultIds != expectedIds) {
                result.result = OperationResult::failure(
                    QStringLiteral("Analysis did not return every target mesh."));
            }
        }

        if (result.result.ok) {
            PreparedColorStateUpdate preparedState;
            const OperationResult stateValidation =
                state_.prepareColorUpdates(stateUpdates, &preparedState);
            if (!stateValidation.ok) {
                result.result = stateValidation;
            }
            else {
                const OperationResult rendered =
                    renderer_.setColorPresentations(rendererUpdates);
                if (!rendered.ok) {
                    result.result = rendered;
                }
                else {
                    state_.commitPreparedColorUpdates(std::move(preparedState));
                }
            }
        }
    }

    active_ = false;
    activeGeneration_ = 0;
    state_.finishAnalysis();
    emit analysisFinished(std::move(result));
}

void MeshColorService::joinWorker()
{
    if (!worker_)
        return;
    // ISurfaceComparer implementations are expected to honor the dedicated
    // cancellation check between expensive operations. An implementation
    // that ignores it can delay this synchronous lifecycle join.
    worker_->wait();
    worker_.reset();
}
