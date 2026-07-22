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
    int vertexCount = 0;
    int faceCount = 0;
    SurfaceMeshSnapshot surface;
};

QVector<QColor> sourceOrFallbackVertexColors(
    const IMeshGeometryView& geometry)
{
    QVector<QColor> colors;
    colors.reserve(geometry.vertexCount());
    if (geometry.hasVertexColors()) {
        bool valid = true;
        for (int index = 0; index < geometry.vertexCount(); ++index) {
            const QColor color = geometry.vertexColor(index);
            if (!color.isValid()) {
                valid = false;
                break;
            }
            colors.append(color);
        }
        if (valid && colors.size() == geometry.vertexCount())
            return colors;
        colors.clear();
    }

    colors.fill(QColor(180, 180, 180, 255), geometry.vertexCount());
    return colors;
}

bool doubleLayerCacheOptionsMatch(
    const SurfaceComparisonOptions& left,
    const SurfaceComparisonOptions& right)
{
    return left.sampleCount == right.sampleCount &&
           left.nearestNeighborCount == right.nearestNeighborCount &&
           left.oppositeNormalAngleDegrees ==
               right.oppositeNormalAngleDegrees &&
           left.doubleLayerRandomSeed == right.doubleLayerRandomSeed;
}

AnalysisSummary distanceSummary(
    const SurfaceComparisonResult& comparison,
    double threshold)
{
    const DistanceToReferenceStatistics& statistics =
        comparison.distanceStatistics;
    AnalysisSummary summary;
    summary.kind = AnalysisKind::DistanceToReference;
    summary.distance.vertexCount = statistics.vertexCount;
    summary.distance.finiteVertexCount = statistics.finiteVertexCount;
    summary.distance.meanDistance = statistics.meanDistance;
    summary.distance.percentile99Distance =
        statistics.percentile99Distance;
    summary.distance.maxDistance = statistics.maxDistance;
    int aboveThresholdCount = 0;
    for (double distance : comparison.vertexDistances) {
        if (distance > threshold)
            ++aboveThresholdCount;
    }
    summary.distance.aboveThresholdVertexFraction =
        comparison.vertexDistances.isEmpty()
        ? 0.0
        : double(aboveThresholdCount)
            / double(comparison.vertexDistances.size());
    return summary;
}

AnalysisSummary doubleLayerSummary(
    const DoubleLayerStatistics& statistics)
{
    AnalysisSummary summary;
    summary.kind = AnalysisKind::DoubleLayer;
    summary.doubleLayer.sampleCount = statistics.sampleCount;
    summary.doubleLayer.meanSampleScore = statistics.meanSampleScore;
    summary.doubleLayer.affectedSampleFraction =
        statistics.affectedSampleFraction;
    summary.doubleLayer.affectedFaceFraction =
        statistics.affectedFaceFraction;
    summary.doubleLayer.affectedVertexFraction =
        statistics.affectedVertexFraction;
    return summary;
}

bool validUnitValue(double value)
{
    return std::isfinite(value) && value >= 0.0 && value <= 1.0;
}

bool validDistanceComparison(
    const SurfaceComparisonResult& comparison,
    int vertexCount)
{
    if (comparison.vertexDistances.size() != vertexCount ||
        comparison.distanceStatistics.vertexCount != vertexCount ||
        comparison.distanceStatistics.finiteVertexCount != vertexCount ||
        !std::isfinite(comparison.distanceStatistics.meanDistance) ||
        !std::isfinite(comparison.distanceStatistics.percentile99Distance) ||
        !std::isfinite(comparison.distanceStatistics.maxDistance) ||
        comparison.distanceStatistics.meanDistance < 0.0 ||
        comparison.distanceStatistics.percentile99Distance < 0.0 ||
        comparison.distanceStatistics.maxDistance < 0.0 ||
        comparison.distanceStatistics.meanDistance >
            comparison.distanceStatistics.maxDistance ||
        comparison.distanceStatistics.percentile99Distance >
            comparison.distanceStatistics.maxDistance) {
        return false;
    }
    for (double distance : comparison.vertexDistances) {
        if (!std::isfinite(distance) || distance < 0.0)
            return false;
    }
    return true;
}

bool validDoubleLayerComparison(
    const SurfaceComparisonResult& comparison,
    int vertexCount)
{
    const DoubleLayerStatistics& statistics =
        comparison.doubleLayerStatistics;
    if (comparison.vertexScores.size() != vertexCount ||
        statistics.sampleCount <= 0 ||
        !validUnitValue(statistics.meanSampleScore) ||
        !validUnitValue(statistics.affectedSampleFraction) ||
        !validUnitValue(statistics.affectedFaceFraction) ||
        !validUnitValue(statistics.affectedVertexFraction)) {
        return false;
    }
    for (double score : comparison.vertexScores) {
        if (!validUnitValue(score))
            return false;
    }
    return true;
}

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
        QHash<MeshId, SurfaceComparisonResult> reusableComparisons,
        std::shared_ptr<std::atomic_bool> cancellation)
        : owner_(&owner),
          comparer_(comparer),
          request_(std::move(request)),
          batchSerial_(batchSerial),
          reference_(std::move(reference)),
          targets_(std::move(targets)),
          reusableComparisons_(std::move(reusableComparisons)),
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
            SurfaceComparisonOutcome outcome;
            bool reusedRawComparison = false;
            const auto reusable =
                reusableComparisons_.constFind(target.meshId);
            if (reusable != reusableComparisons_.cend()) {
                outcome.result = OperationResult::success();
                outcome.comparison = reusable.value();
                reusedRawComparison = true;
                owner_->queueProgress(
                    request_.generation,
                    batchSerial_,
                    target.meshId,
                    100,
                    QStringLiteral("Reusing cached analysis field..."));
            }
            else {
                outcome = comparer_.compare(
                    target.surface,
                    reference_,
                    request_.metric,
                    request_.options,
                    progress,
                    [this] {
                        return cancellation_->load(
                            std::memory_order_acquire);
                    });
            }
            if (!outcome.result.ok) {
                owner_->queueCompletion(failedBatch(
                    request_.generation,
                    batchSerial_,
                    request_.metric,
                    outcome.result.error));
                return;
            }
            MeshAnalysisResult result;
            result.result = OperationResult::success();
            result.meshId = target.meshId;
            result.rawComparison = outcome.comparison;
            result.reusedRawComparison = reusedRawComparison;
            if (request_.metric ==
                SurfaceComparisonMetric::DistanceToReference) {
                if (!validDistanceComparison(
                        outcome.comparison,
                        target.vertexCount)) {
                    owner_->queueCompletion(failedBatch(
                        request_.generation,
                        batchSerial_,
                        request_.metric,
                        QStringLiteral(
                            "Distance analysis returned an invalid vertex field.")));
                    return;
                }
                result.vertexColors = distanceToVertexColors(
                    outcome.comparison.vertexDistances,
                    request_.options.distanceDisplayThreshold,
                    request_.options.distanceColorMapping);
                result.analysisSummary = distanceSummary(
                    outcome.comparison,
                    request_.options.distanceDisplayThreshold);
            }
            else if (request_.metric ==
                     SurfaceComparisonMetric::DoubleLayer) {
                if (!validDoubleLayerComparison(
                        outcome.comparison,
                        target.vertexCount)) {
                    owner_->queueCompletion(failedBatch(
                        request_.generation,
                        batchSerial_,
                        request_.metric,
                        QStringLiteral(
                            "Double-layer analysis returned an invalid vertex field.")));
                    return;
                }
                result.vertexColors = doubleLayerVertexColors(
                    outcome.comparison.vertexScores);
                result.analysisSummary =
                    doubleLayerSummary(
                        outcome.comparison.doubleLayerStatistics);
            }
            else {
                const double minimumScore =
                    request_.metric == SurfaceComparisonMetric::NormalAgreement &&
                        !request_.options.useAbsoluteNormalDot
                    ? -1.0
                    : 0.0;
                bool validScores =
                    outcome.comparison.faceScores.size() ==
                        target.faceCount &&
                    outcome.comparison.coloredFaceCount ==
                        target.faceCount &&
                    std::isfinite(outcome.comparison.globalScore) &&
                    outcome.comparison.globalScore >= minimumScore &&
                    outcome.comparison.globalScore <= 1.0;
                for (double faceScore : outcome.comparison.faceScores) {
                    if (!std::isfinite(faceScore) ||
                        faceScore < minimumScore || faceScore > 1.0) {
                        validScores = false;
                        break;
                    }
                }
                if (!validScores) {
                    owner_->queueCompletion(failedBatch(
                        request_.generation,
                        batchSerial_,
                        request_.metric,
                        QStringLiteral(
                            "Analysis returned invalid comparison scores.")));
                    return;
                }
                result.globalScore = outcome.comparison.globalScore;
                result.faceColors = surfaceScoreColors(
                    outcome.comparison.faceScores,
                    minimumScore);
            }
            staged.append(std::move(result));
        }

        AnalysisBatchResult result;
        result.result = OperationResult::success();
        result.generation = request_.generation;
        result.batchSerial = batchSerial_;
        result.metric = request_.metric;
        result.referenceId = request_.referenceId;
        result.options = request_.options;
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
    QHash<MeshId, SurfaceComparisonResult> reusableComparisons_;
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

    if (cacheGeneration_ != request.generation) {
        distanceCache_.clear();
        doubleLayerCache_.clear();
        cacheGeneration_ = request.generation;
    }

    QSet<MeshId> expectedTargets;
    for (const MeshEntry& mesh : state_.meshes()) {
        if (request.metric == SurfaceComparisonMetric::DoubleLayer ||
            mesh.id != state_.referenceId()) {
            expectedTargets.insert(mesh.id);
        }
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
        return OperationResult::failure(
            request.metric == SurfaceComparisonMetric::DoubleLayer
                ? QStringLiteral(
                      "Double-layer targets must contain every mesh exactly once.")
                : QStringLiteral(
                      "Analysis targets must contain every non-Reference mesh exactly once."));
    }

    QHash<MeshId, SurfaceComparisonResult> reusableComparisons;
    if (request.metric ==
        SurfaceComparisonMetric::DistanceToReference) {
        for (MeshId targetId : request.targetIds) {
            const auto cached = distanceCache_.constFind(targetId);
            if (cached != distanceCache_.cend() &&
                cached->generation == request.generation &&
                cached->referenceId == request.referenceId) {
                reusableComparisons.insert(
                    targetId, cached->comparison);
            }
        }
    }
    else if (request.metric == SurfaceComparisonMetric::DoubleLayer) {
        for (MeshId targetId : request.targetIds) {
            const auto cached = doubleLayerCache_.constFind(targetId);
            if (cached != doubleLayerCache_.cend() &&
                cached->generation == request.generation &&
                doubleLayerCacheOptionsMatch(
                    cached->options, request.options)) {
                reusableComparisons.insert(
                    targetId, cached->comparison);
            }
        }
    }

    SurfaceMeshSnapshot referenceSnapshot;
    QVector<TargetSnapshot> targetSnapshots;
    QVector<QColor> referenceVertexColors;
    try {
        OperationResult copied;
        if (request.metric != SurfaceComparisonMetric::DoubleLayer) {
            const MeshEntry* referenceEntry =
                state_.mesh(request.referenceId);
            const IMeshGeometryView* referenceGeometry =
                resources_.geometry(referenceEntry->resourceId);
            if (referenceGeometry == nullptr) {
                return OperationResult::failure(
                    QStringLiteral(
                        "Reference mesh geometry is unavailable."));
            }
            if (request.metric ==
                SurfaceComparisonMetric::DistanceToReference) {
                referenceVertexColors =
                    sourceOrFallbackVertexColors(*referenceGeometry);
            }
            const bool needsReferenceSurface =
                request.metric !=
                    SurfaceComparisonMetric::DistanceToReference ||
                reusableComparisons.size() != request.targetIds.size();
            if (needsReferenceSurface) {
                copied = copySurfaceSnapshot(
                    *referenceGeometry, &referenceSnapshot);
                if (!copied.ok)
                    return copied;
            }
        }

        targetSnapshots.reserve(request.targetIds.size());
        for (MeshId targetId : request.targetIds) {
            TargetSnapshot target;
            target.meshId = targetId;
            const auto reusable =
                reusableComparisons.constFind(targetId);
            if (reusable != reusableComparisons.cend()) {
                target.vertexCount =
                    request.metric ==
                            SurfaceComparisonMetric::DistanceToReference
                    ? reusable->vertexDistances.size()
                    : reusable->vertexScores.size();
                target.faceCount = reusable->faceScores.size();
                targetSnapshots.append(std::move(target));
                continue;
            }

            const MeshEntry* entry = state_.mesh(targetId);
            Q_ASSERT(entry != nullptr);
            const IMeshGeometryView* geometry = resources_.geometry(entry->resourceId);
            if (geometry == nullptr) {
                return OperationResult::failure(
                    QStringLiteral("Target mesh geometry is unavailable."));
            }
            copied = copySurfaceSnapshot(*geometry, &target.surface);
            if (!copied.ok)
                return copied;
            target.vertexCount = target.surface.vertices.size();
            target.faceCount = target.surface.faces.size();
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
            std::move(reusableComparisons),
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
    activeReferenceId_ = request.referenceId;
    activeReferenceVertexColors_ = std::move(referenceVertexColors);
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
    activeReferenceId_ = 0;
    activeReferenceVertexColors_.clear();
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
        result.generation != state_.generation() ||
        (result.result.ok &&
         result.referenceId != activeReferenceId_)) {
        result.result = OperationResult::failure(
            QStringLiteral("Analysis result belongs to a stale workspace generation."));
    }

    if (result.result.ok) {
        const bool distanceMetric =
            result.metric ==
            SurfaceComparisonMetric::DistanceToReference;
        const bool doubleLayerMetric =
            result.metric == SurfaceComparisonMetric::DoubleLayer;
        const bool vertexMetric = distanceMetric || doubleLayerMetric;
        QVector<MeshColorStateUpdate> stateUpdates;
        QVector<MeshColorPresentationUpdate> rendererUpdates;
        const int referenceUpdateCount = distanceMetric ? 1 : 0;
        stateUpdates.reserve(
            result.meshes.size() + referenceUpdateCount);
        rendererUpdates.reserve(
            result.meshes.size() + referenceUpdateCount);
        QSet<MeshId> resultIds;
        for (const MeshAnalysisResult& meshResult : result.meshes) {
            bool valid = !resultIds.contains(meshResult.meshId) &&
                         state_.mesh(meshResult.meshId) != nullptr;
            if (vertexMetric) {
                const AnalysisKind expectedKind = distanceMetric
                    ? AnalysisKind::DistanceToReference
                    : AnalysisKind::DoubleLayer;
                valid = valid && !meshResult.vertexColors.isEmpty() &&
                        meshResult.faceColors.isEmpty() &&
                        meshResult.analysisSummary.kind == expectedKind;
            }
            else {
                valid = valid && meshResult.vertexColors.isEmpty() &&
                        meshResult.faceColors.size() > 0 &&
                        std::isfinite(meshResult.globalScore);
            }
            if (!valid) {
                result.result = OperationResult::failure(
                    QStringLiteral("Analysis returned an invalid target result batch."));
                break;
            }
            resultIds.insert(meshResult.meshId);
            ColorPresentation presentation;
            if (vertexMetric) {
                presentation.mode = ColorMode::VertexColor;
                presentation.vertexColors = meshResult.vertexColors;
                presentation.referenceDependent = distanceMetric;
                if (distanceMetric) {
                    presentation.colorLegend.kind =
                        ColorLegendKind::Distance;
                    presentation.colorLegend.distanceMapping =
                        result.options.distanceColorMapping;
                    presentation.colorLegend.minimum = 0.0;
                    presentation.colorLegend.maximum =
                        result.options.distanceDisplayThreshold;
                }
                stateUpdates.append(
                    {meshResult.meshId,
                     presentation,
                     0.0,
                     false,
                     meshResult.analysisSummary});
            }
            else {
                presentation.mode =
                    result.metric ==
                            SurfaceComparisonMetric::PrecisionAtThreshold
                    ? ColorMode::PrecisionResult
                    : ColorMode::NormalAgreementResult;
                presentation.faceColors = meshResult.faceColors;
                stateUpdates.append(
                    {meshResult.meshId,
                     presentation,
                     meshResult.globalScore,
                     true});
            }
            rendererUpdates.append({meshResult.meshId, presentation});
        }

        if (result.result.ok) {
            QSet<MeshId> expectedIds;
            for (const MeshEntry& mesh : state_.meshes()) {
                if (doubleLayerMetric || !mesh.isReference)
                    expectedIds.insert(mesh.id);
            }
            if (resultIds != expectedIds) {
                result.result = OperationResult::failure(
                    QStringLiteral("Analysis did not return every target mesh."));
            }
        }

        if (result.result.ok && distanceMetric) {
            const MeshEntry* reference =
                state_.mesh(activeReferenceId_);
            if (reference == nullptr ||
                activeReferenceVertexColors_.isEmpty()) {
                result.result = OperationResult::failure(
                    QStringLiteral(
                        "Distance analysis reference colors are unavailable."));
            }
            else {
                ColorPresentation presentation;
                presentation.mode = ColorMode::VertexColor;
                presentation.vertexColors =
                    activeReferenceVertexColors_;
                presentation.referenceDependent = true;
                stateUpdates.append(
                    {reference->id,
                     presentation,
                     0.0,
                     false,
                     AnalysisSummary{}});
                rendererUpdates.append(
                    {reference->id, presentation});
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
                    if (distanceMetric) {
                        for (const MeshAnalysisResult& meshResult :
                             result.meshes) {
                            DistanceCacheEntry cached;
                            cached.generation = result.generation;
                            cached.referenceId = result.referenceId;
                            cached.comparison =
                                meshResult.rawComparison;
                            distanceCache_.insert(
                                meshResult.meshId,
                                std::move(cached));
                        }
                    }
                    else if (doubleLayerMetric) {
                        for (const MeshAnalysisResult& meshResult :
                             result.meshes) {
                            DoubleLayerCacheEntry cached;
                            cached.generation = result.generation;
                            cached.options = result.options;
                            cached.comparison =
                                meshResult.rawComparison;
                            doubleLayerCache_.insert(
                                meshResult.meshId,
                                std::move(cached));
                        }
                    }
                }
            }
        }
    }

    active_ = false;
    activeGeneration_ = 0;
    activeReferenceId_ = 0;
    activeReferenceVertexColors_.clear();
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
