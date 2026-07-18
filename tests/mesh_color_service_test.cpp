#include <QtTest>

#include <array>
#include <limits>
#include <memory>
#include <stdexcept>

#include <QElapsedTimer>
#include <QHash>
#include <QMutex>
#include <QMutexLocker>
#include <QSignalSpy>
#include <QThread>

#include "core/workspace_state.h"
#include "services/mesh_color_service.h"
#include "fakes/fake_renderer_adapter.h"
#include "fakes/fake_surface_comparer.h"

namespace
{
class GeometryAccessRecorder
{
public:
    void record() const
    {
        QMutexLocker lock(&mutex_);
        threads_.append(QThread::currentThread());
    }

    QVector<QThread*> threads() const
    {
        QMutexLocker lock(&mutex_);
        return threads_;
    }

private:
    mutable QMutex mutex_;
    mutable QVector<QThread*> threads_;
};

class OwningGeometry final : public IMeshGeometryView
{
public:
    explicit OwningGeometry(
        std::shared_ptr<GeometryAccessRecorder> recorder =
            std::make_shared<GeometryAccessRecorder>())
        : recorder_(std::move(recorder))
    {
    }

    QVector<MeshPoint3D> positions = {
        MeshPoint3D{{0.0, 0.0, 0.0}},
        MeshPoint3D{{1.0, 0.0, 0.0}},
        MeshPoint3D{{0.0, 1.0, 0.0}}};
    QVector<MeshPoint3D> normals = {
        MeshPoint3D{{0.0, 0.0, 1.0}},
        MeshPoint3D{{0.0, 0.0, 1.0}},
        MeshPoint3D{{0.0, 0.0, 1.0}}};
    QVector<std::array<int, 3>> faces = {{{{0, 1, 2}}}};
    QVector<QColor> colors = {
        QColor(11, 22, 33, 44),
        QColor(55, 66, 77, 88),
        QColor(99, 111, 122, 133)};

    int vertexCount() const override
    {
        recorder_->record();
        return positions.size();
    }
    MeshPoint3D vertexPosition(int index) const override
    {
        recorder_->record();
        if (surfaceAccessBlocked)
            throw std::runtime_error("surface snapshot access was blocked");
        return positions.at(index);
    }
    MeshPoint3D vertexNormal(int index) const override
    {
        recorder_->record();
        if (surfaceAccessBlocked)
            throw std::runtime_error("surface snapshot access was blocked");
        return normals.at(index);
    }
    bool hasVertexColors() const override { return true; }
    QColor vertexColor(int index) const override
    {
        recorder_->record();
        return colors.at(index);
    }
    int faceCount() const override
    {
        recorder_->record();
        if (surfaceAccessBlocked)
            throw std::runtime_error("surface snapshot access was blocked");
        return faces.size();
    }
    std::array<int, 3> faceVertexIndices(int index) const override
    {
        recorder_->record();
        if (surfaceAccessBlocked)
            throw std::runtime_error("surface snapshot access was blocked");
        return faces.at(index);
    }
    bool surfaceAccessBlocked = false;

private:
    std::shared_ptr<GeometryAccessRecorder> recorder_;
};

class OwningResourceProvider final : public IMeshResourceProvider
{
public:
    OwningResourceProvider()
        : recorder_(std::make_shared<GeometryAccessRecorder>())
    {
        geometries_.insert(101, OwningGeometry(recorder_));
        geometries_.insert(102, OwningGeometry(recorder_));
        geometries_.insert(103, OwningGeometry(recorder_));
    }

    const IMeshGeometryView* geometry(MeshResourceId id) const override
    {
        QMutexLocker lock(&mutex_);
        recorder_->record();
        const auto found = geometries_.constFind(id);
        return found == geometries_.cend() ? nullptr : &found.value();
    }

    void remove(MeshResourceId id)
    {
        QMutexLocker lock(&mutex_);
        geometries_.remove(id);
    }

    void setFirstX(MeshResourceId id, double value)
    {
        QMutexLocker lock(&mutex_);
        geometries_[id].positions[0][0] = value;
    }

    void blockSurfaceSnapshotAccess(MeshResourceId id)
    {
        QMutexLocker lock(&mutex_);
        geometries_[id].surfaceAccessBlocked = true;
    }

    QVector<MeshPoint3D> positions(MeshResourceId id) const
    {
        QMutexLocker lock(&mutex_);
        return geometries_.value(id).positions;
    }

    QVector<MeshPoint3D> normals(MeshResourceId id) const
    {
        QMutexLocker lock(&mutex_);
        return geometries_.value(id).normals;
    }

    QVector<std::array<int, 3>> faces(MeshResourceId id) const
    {
        QMutexLocker lock(&mutex_);
        return geometries_.value(id).faces;
    }

    QVector<QThread*> geometryThreads() const
    {
        return recorder_->threads();
    }

private:
    mutable QMutex mutex_;
    std::shared_ptr<GeometryAccessRecorder> recorder_;
    QHash<MeshResourceId, OwningGeometry> geometries_;
};

class ThrowingSurfaceComparer final : public ISurfaceComparer
{
public:
    SurfaceComparisonOutcome compare(
        const SurfaceMeshSnapshot&,
        const SurfaceMeshSnapshot&,
        SurfaceComparisonMetric,
        const SurfaceComparisonOptions&,
        AnalysisProgress,
        AnalysisCancellation) const override
    {
        throw std::runtime_error("scripted comparer exception");
    }
};

QVector<MeshEntry> entries()
{
    return {
        {1, 101, QStringLiteral("/tmp/gt.obj"), QStringLiteral("gt.obj"), {}, true},
        {2, 102, QStringLiteral("/tmp/a.obj"), QStringLiteral("a.obj"), {}, false},
        {3, 103, QStringLiteral("/tmp/b.obj"), QStringLiteral("b.obj"), {}, false}};
}

void makeReady(WorkspaceState& state)
{
    state.beginLoading();
    QVERIFY(state.commitWorkspace(entries(), 1).ok);
}

void commitFakeScene(
    FakeRendererAdapter& renderer,
    const WorkspaceState& state,
    const IMeshResourceProvider& resources)
{
    SceneDescriptor scene;
    scene.generation = state.generation();
    scene.referenceId = state.referenceId();
    for (const MeshEntry& mesh : state.meshes()) {
        scene.meshes.append(
            {mesh.id, mesh.resourceId, mesh.displayName, mesh.isReference});
    }
    QVERIFY(renderer.prepareScene(scene, resources).ok);
    renderer.commitPreparedScene();
}

AnalysisRequest request(const WorkspaceState& state)
{
    AnalysisRequest value;
    value.generation = state.generation();
    value.referenceId = state.referenceId();
    value.targetIds = {2, 3};
    value.metric = SurfaceComparisonMetric::PrecisionAtThreshold;
    value.options.sampleCount = 10;
    value.options.randomSeed = 17;
    return value;
}

AnalysisRequest distanceRequest(const WorkspaceState& state)
{
    AnalysisRequest value;
    value.generation = state.generation();
    value.referenceId = state.referenceId();
    value.targetIds = {2, 3};
    value.metric = SurfaceComparisonMetric::DistanceToReference;
    value.options.distanceDisplayThreshold = 0.04;
    return value;
}

AnalysisRequest doubleLayerRequest(const WorkspaceState& state)
{
    AnalysisRequest value;
    value.generation = state.generation();
    value.referenceId = state.referenceId();
    value.targetIds = {1, 2, 3};
    value.metric = SurfaceComparisonMetric::DoubleLayer;
    value.options.sampleCount = 500000;
    value.options.nearestNeighborCount = 20;
    value.options.oppositeNormalAngleDegrees = 170.0;
    value.options.doubleLayerRandomSeed = 0;
    return value;
}

AnalysisBatchResult takeBatch(QSignalSpy& spy)
{
    if (spy.isEmpty())
        spy.wait(5000);
    if (spy.isEmpty()) {
        QTest::qFail(
            "Timed out waiting for an analysis batch result.",
            __FILE__,
            __LINE__);
        return {};
    }
    return qvariant_cast<AnalysisBatchResult>(spy.takeFirst().at(0));
}
} // namespace

class MeshColorServiceTest : public QObject
{
    Q_OBJECT

private slots:
    void distanceAnalysisColorsTargetsAndReferenceByVertex()
    {
        WorkspaceState state;
        makeReady(state);
        OwningResourceProvider resources;
        FakeRendererAdapter renderer;
        commitFakeScene(renderer, state, resources);
        FakeSurfaceComparer comparer;
        comparer.enqueueDistanceSuccess({0.0, 0.01, 0.04});
        comparer.enqueueDistanceSuccess({0.0025, 0.0225, 0.08});
        MeshColorService service(resources, comparer, state, renderer);
        QSignalSpy finished(&service, &MeshColorService::analysisFinished);

        QVERIFY(service.startAnalysis(distanceRequest(state)).ok);
        const AnalysisBatchResult batch = takeBatch(finished);

        QVERIFY2(batch.result.ok, qPrintable(batch.result.error));
        QCOMPARE(comparer.callCount(), 2);
        QCOMPARE(renderer.presentationUpdateCount(), 1);
        QCOMPARE(renderer.lastPresentationBatch().size(), 3);
        QCOMPARE(state.mesh(1)->presentation.mode, ColorMode::VertexColor);
        QCOMPARE(
            state.mesh(1)->presentation.vertexColors,
            QVector<QColor>({
                QColor(11, 22, 33, 44),
                QColor(55, 66, 77, 88),
                QColor(99, 111, 122, 133)}));
        QCOMPARE(state.mesh(1)->analysisSummary.kind, AnalysisKind::None);
        QCOMPARE(state.mesh(2)->presentation.mode, ColorMode::VertexColor);
        QCOMPARE(
            state.mesh(2)->presentation.vertexColors,
            QVector<QColor>({
                QColor(68, 1, 84, 255),
                QColor(33, 145, 140, 255),
                QColor(253, 231, 37, 255)}));
        QCOMPARE(
            state.mesh(2)->analysisSummary.kind,
            AnalysisKind::DistanceToReference);
        QCOMPARE(
            state.mesh(2)->analysisSummary.distance.vertexCount,
            3);
        QCOMPARE(
            state.mesh(2)
                ->analysisSummary.distance.aboveThresholdVertexFraction,
            0.0);
        QCOMPARE(
            state.mesh(3)
                ->analysisSummary.distance.aboveThresholdVertexFraction,
            1.0 / 3.0);
        QCOMPARE(
            state.mesh(2)->presentation.colorLegend.kind,
            ColorLegendKind::Distance);
        QCOMPARE(
            state.mesh(2)->presentation.colorLegend.maximum,
            0.04);
        QCOMPARE(
            state.mesh(2)->presentation.colorLegend.distanceMapping,
            DistanceColorMapping::SquareRoot);
        QVERIFY(!state.mesh(2)->hasScore);
    }

    void distanceRawFieldCacheRemapsWithoutRecomparison()
    {
        WorkspaceState state;
        makeReady(state);
        OwningResourceProvider resources;
        FakeRendererAdapter renderer;
        commitFakeScene(renderer, state, resources);
        FakeSurfaceComparer comparer;
        comparer.enqueueDistanceSuccess({0.0, 0.01, 0.04});
        comparer.enqueueDistanceSuccess({0.0, 0.01, 0.04});
        MeshColorService service(resources, comparer, state, renderer);
        QSignalSpy finished(&service, &MeshColorService::analysisFinished);

        AnalysisRequest first = distanceRequest(state);
        QVERIFY(service.startAnalysis(first).ok);
        QVERIFY(takeBatch(finished).result.ok);
        QCOMPARE(comparer.callCount(), 2);
        const QColor firstMiddle =
            state.mesh(2)->presentation.vertexColors.at(1);
        resources.blockSurfaceSnapshotAccess(102);
        resources.blockSurfaceSnapshotAccess(103);

        AnalysisRequest remapped = distanceRequest(state);
        remapped.options.distanceColorMapping =
            DistanceColorMapping::Linear;
        QVERIFY(service.startAnalysis(remapped).ok);
        QVERIFY(takeBatch(finished).result.ok);

        QCOMPARE(comparer.callCount(), 2);
        QCOMPARE(firstMiddle, QColor(33, 145, 140, 255));
        QCOMPARE(
            state.mesh(2)->presentation.vertexColors.at(1),
            QColor(59, 82, 139, 255));
        QCOMPARE(
            state.mesh(2)->presentation.colorLegend.distanceMapping,
            DistanceColorMapping::Linear);
        QCOMPARE(
            state.mesh(2)->presentation.colorLegend.maximum,
            0.04);
    }

    void distanceRawFieldCacheRecomputesThresholdFractionWithoutRecomparison()
    {
        WorkspaceState state;
        makeReady(state);
        OwningResourceProvider resources;
        FakeRendererAdapter renderer;
        commitFakeScene(renderer, state, resources);
        FakeSurfaceComparer comparer;
        comparer.enqueueDistanceSuccess({0.0, 0.01, 0.04});
        comparer.enqueueDistanceSuccess({0.0, 0.01, 0.04});
        MeshColorService service(resources, comparer, state, renderer);
        QSignalSpy finished(&service, &MeshColorService::analysisFinished);

        QVERIFY(service.startAnalysis(distanceRequest(state)).ok);
        QVERIFY(takeBatch(finished).result.ok);
        QCOMPARE(comparer.callCount(), 2);
        QCOMPARE(
            state.mesh(2)
                ->analysisSummary.distance.aboveThresholdVertexFraction,
            0.0);
        resources.blockSurfaceSnapshotAccess(102);
        resources.blockSurfaceSnapshotAccess(103);

        AnalysisRequest thresholdChanged = distanceRequest(state);
        thresholdChanged.options.distanceDisplayThreshold = 0.009;
        QVERIFY(service.startAnalysis(thresholdChanged).ok);
        const AnalysisBatchResult cachedBatch = takeBatch(finished);

        QVERIFY2(cachedBatch.result.ok, qPrintable(cachedBatch.result.error));
        QCOMPARE(comparer.callCount(), 2);
        QCOMPARE(cachedBatch.meshes.size(), 2);
        for (const MeshAnalysisResult& mesh : cachedBatch.meshes)
            QVERIFY(mesh.reusedRawComparison);
        QCOMPARE(
            state.mesh(2)
                ->analysisSummary.distance.aboveThresholdVertexFraction,
            2.0 / 3.0);
        QCOMPARE(
            state.mesh(2)->presentation.colorLegend.maximum,
            0.009);
    }

    void doubleLayerAnalyzesEveryMeshAndCachesItsRawField()
    {
        WorkspaceState state;
        makeReady(state);
        OwningResourceProvider resources;
        FakeRendererAdapter renderer;
        commitFakeScene(renderer, state, resources);
        FakeSurfaceComparer comparer;
        DoubleLayerStatistics statistics;
        statistics.sampleCount = 500000;
        statistics.meanSampleScore = 0.1;
        statistics.affectedSampleFraction = 0.2;
        statistics.affectedFaceFraction = 0.3;
        statistics.affectedVertexFraction = 2.0 / 3.0;
        for (int index = 0; index < 3; ++index) {
            comparer.enqueueDoubleLayerSuccess(
                {0.0, 0.25, 1.0},
                statistics);
        }
        MeshColorService service(resources, comparer, state, renderer);
        QSignalSpy finished(&service, &MeshColorService::analysisFinished);

        const AnalysisRequest value = doubleLayerRequest(state);
        QVERIFY(service.startAnalysis(value).ok);
        QVERIFY(takeBatch(finished).result.ok);

        QCOMPARE(comparer.callCount(), 3);
        QCOMPARE(renderer.lastPresentationBatch().size(), 3);
        for (MeshId id : {MeshId(1), MeshId(2), MeshId(3)}) {
            QCOMPARE(
                state.mesh(id)->presentation.mode,
                ColorMode::VertexColor);
            QCOMPARE(
                state.mesh(id)->presentation.vertexColors,
                QVector<QColor>({
                    QColor(180, 180, 180, 255),
                    QColor(255, 105, 24, 255),
                    QColor(255, 0, 48, 255)}));
            QCOMPARE(
                state.mesh(id)->analysisSummary.kind,
                AnalysisKind::DoubleLayer);
            QCOMPARE(
                state.mesh(id)
                    ->analysisSummary.doubleLayer.affectedFaceFraction,
                0.3);
        }

        resources.blockSurfaceSnapshotAccess(101);
        resources.blockSurfaceSnapshotAccess(102);
        resources.blockSurfaceSnapshotAccess(103);
        QVERIFY(service.startAnalysis(value).ok);
        QVERIFY(takeBatch(finished).result.ok);
        QCOMPARE(comparer.callCount(), 3);
    }

    void oneTargetFailureCommitsNeitherRendererNorState()
    {
        WorkspaceState state;
        makeReady(state);
        OwningResourceProvider resources;
        FakeRendererAdapter renderer;
        commitFakeScene(renderer, state, resources);
        FakeSurfaceComparer comparer;
        comparer.enqueueSuccess(0.9);
        comparer.enqueueFailure(QStringLiteral("target has no valid faces"));
        MeshColorService service(resources, comparer, state, renderer);
        QSignalSpy finished(&service, &MeshColorService::analysisFinished);

        QVERIFY(service.startAnalysis(request(state)).ok);
        const AnalysisBatchResult batch = takeBatch(finished);

        QVERIFY(!batch.result.ok);
        QVERIFY(batch.batchSerial > 0);
        QCOMPARE(renderer.presentationUpdateCount(), 0);
        QCOMPARE(state.mesh(2)->presentation.mode, ColorMode::Default);
        QCOMPARE(state.mesh(3)->presentation.mode, ColorMode::Default);
        QCOMPARE(state.phase(), WorkspacePhase::Ready);
    }

    void everyTargetCommitsInExactlyOneRendererCall()
    {
        WorkspaceState state;
        makeReady(state);
        OwningResourceProvider resources;
        FakeRendererAdapter renderer;
        commitFakeScene(renderer, state, resources);
        FakeSurfaceComparer comparer;
        comparer.enqueueSuccess(0.9);
        comparer.enqueueSuccess(0.8);
        MeshColorService service(resources, comparer, state, renderer);
        QSignalSpy finished(&service, &MeshColorService::analysisFinished);

        QVERIFY(service.startAnalysis(request(state)).ok);
        const AnalysisBatchResult batch = takeBatch(finished);

        QVERIFY2(batch.result.ok, qPrintable(batch.result.error));
        QCOMPARE(batch.meshes.size(), 2);
        QCOMPARE(renderer.presentationUpdateCount(), 1);
        QCOMPARE(renderer.lastPresentationBatch().size(), 2);
        QCOMPARE(state.mesh(2)->score, 0.9);
        QCOMPARE(state.mesh(3)->score, 0.8);
        QCOMPARE(state.mesh(2)->presentation.mode, ColorMode::PrecisionResult);
        QCOMPARE(state.phase(), WorkspacePhase::Ready);
    }

    void rendererBatchFailureLeavesRendererAndStateUnchanged()
    {
        WorkspaceState state;
        makeReady(state);
        OwningResourceProvider resources;
        FakeRendererAdapter renderer;
        commitFakeScene(renderer, state, resources);
        FakeSurfaceComparer comparer;
        comparer.enqueueSuccess(0.9);
        comparer.enqueueSuccess(0.8);
        renderer.failNextPresentationBatch(QStringLiteral("GPU allocation failed"));
        renderer.setPresentationObserver([&] {
            QCOMPARE(state.mesh(2)->presentation.mode, ColorMode::Default);
            QCOMPARE(state.mesh(3)->presentation.mode, ColorMode::Default);
        });
        MeshColorService service(resources, comparer, state, renderer);
        QSignalSpy finished(&service, &MeshColorService::analysisFinished);

        QVERIFY(service.startAnalysis(request(state)).ok);
        const AnalysisBatchResult batch = takeBatch(finished);

        QVERIFY(!batch.result.ok);
        QCOMPARE(renderer.presentationAttemptCount(), 1);
        QCOMPARE(renderer.presentationUpdateCount(), 0);
        QCOMPARE(renderer.presentation(2).mode, ColorMode::Default);
        QCOMPARE(state.mesh(2)->presentation.mode, ColorMode::Default);
        QCOMPARE(state.phase(), WorkspacePhase::Ready);
    }

    void invalidAndIncompleteRequestsFailBeforeWorkerWork()
    {
        WorkspaceState state;
        makeReady(state);
        OwningResourceProvider resources;
        FakeRendererAdapter renderer;
        commitFakeScene(renderer, state, resources);
        FakeSurfaceComparer comparer;
        MeshColorService service(resources, comparer, state, renderer);

        AnalysisRequest value = request(state);
        value.targetIds = {2};
        QVERIFY(!service.startAnalysis(value).ok);
        value = request(state);
        value.targetIds = {2, 2};
        QVERIFY(!service.startAnalysis(value).ok);
        value = request(state);
        value.targetIds = {1, 2, 3};
        QVERIFY(!service.startAnalysis(value).ok);
        value = request(state);
        value.targetIds = {2, 99};
        QVERIFY(!service.startAnalysis(value).ok);
        value = request(state);
        value.generation += 1;
        QVERIFY(!service.startAnalysis(value).ok);
        value = request(state);
        value.options.sampleCount = 0;
        QVERIFY(!service.startAnalysis(value).ok);
        value = request(state);
        value.options.distanceThreshold = std::numeric_limits<float>::quiet_NaN();
        QVERIFY(!service.startAnalysis(value).ok);
        value = request(state);
        value.metric = static_cast<SurfaceComparisonMetric>(99);
        QVERIFY(!service.startAnalysis(value).ok);

        QCOMPARE(comparer.callCount(), 0);
        QCOMPARE(resources.geometryThreads().size(), 0);
        QCOMPARE(state.phase(), WorkspacePhase::Ready);
    }

    void missingGeometryFailsBeforeWorkerWork()
    {
        WorkspaceState state;
        makeReady(state);
        OwningResourceProvider resources;
        resources.remove(103);
        FakeRendererAdapter renderer;
        commitFakeScene(renderer, state, resources);
        FakeSurfaceComparer comparer;
        MeshColorService service(resources, comparer, state, renderer);

        const OperationResult result = service.startAnalysis(request(state));

        QVERIFY(!result.ok);
        QCOMPARE(comparer.callCount(), 0);
        QCOMPARE(state.phase(), WorkspacePhase::Ready);
    }

    void busyCommandsRejectAndCancellationJoinsTheWorker()
    {
        WorkspaceState state;
        makeReady(state);
        OwningResourceProvider resources;
        FakeRendererAdapter renderer;
        commitFakeScene(renderer, state, resources);
        FakeSurfaceComparer comparer;
        comparer.enqueueSuccess(0.9);
        comparer.pauseNextComparison();
        MeshColorService service(resources, comparer, state, renderer);

        QVERIFY(service.startAnalysis(request(state)).ok);
        QVERIFY(comparer.waitUntilEntered());
        QVERIFY(!service.startAnalysis(request(state)).ok);
        QVERIFY(!service.setUniformColor(2, QColor(Qt::green)).ok);
        QVERIFY(!service.clearColoring().ok);

        service.cancelAnalysis();

        QVERIFY(comparer.waitUntilReturned());
        QCOMPARE(state.phase(), WorkspacePhase::Ready);
        QCOMPARE(renderer.presentationUpdateCount(), 0);
    }

    void cancellationUsesDedicatedCallbackIndependentlyOfProgress()
    {
        WorkspaceState state;
        makeReady(state);
        OwningResourceProvider resources;
        FakeRendererAdapter renderer;
        commitFakeScene(renderer, state, resources);
        FakeSurfaceComparer comparer;
        comparer.enqueueSuccess(0.9);
        comparer.requireDedicatedCancellationForNextComparison();
        MeshColorService service(resources, comparer, state, renderer);
        QSignalSpy finished(&service, &MeshColorService::analysisFinished);

        QVERIFY(service.startAnalysis(request(state)).ok);
        QVERIFY(comparer.waitUntilEntered(1, 1000));

        QElapsedTimer cancellationTimer;
        cancellationTimer.start();
        service.cancelAnalysis();

        QVERIFY2(
            cancellationTimer.elapsed() < 1000,
            "Dedicated cancellation did not release the analysis worker promptly.");
        QVERIFY(comparer.ignoredProgressCancellationCount() > 0);
        QVERIFY(comparer.dedicatedCancellationObservationCount() > 0);
        QCOMPARE(finished.size(), 1);
        const AnalysisBatchResult batch =
            qvariant_cast<AnalysisBatchResult>(finished.takeFirst().at(0));
        QVERIFY(!batch.result.ok);
        QCOMPARE(batch.result.error, QStringLiteral("Analysis cancelled."));
        QCOMPARE(renderer.presentationUpdateCount(), 0);
        QCOMPARE(state.phase(), WorkspacePhase::Ready);
    }

    void snapshotsAreCopiedOnOwnerThreadAndOutliveProviderChanges()
    {
        WorkspaceState state;
        makeReady(state);
        OwningResourceProvider resources;
        FakeRendererAdapter renderer;
        commitFakeScene(renderer, state, resources);
        FakeSurfaceComparer comparer;
        comparer.enqueueSuccess(0.9);
        comparer.enqueueSuccess(0.8);
        comparer.pauseNextComparison();
        MeshColorService service(resources, comparer, state, renderer);
        QSignalSpy finished(&service, &MeshColorService::analysisFinished);
        const QThread* ownerThread = QThread::currentThread();

        QVERIFY(service.startAnalysis(request(state)).ok);
        QVERIFY(comparer.waitUntilEntered());
        resources.setFirstX(102, 44.0);
        const FakeSurfaceComparer::Call firstCall = comparer.calls().front();
        QCOMPARE(firstCall.source.vertices.front()[0], 0.0);
        QVERIFY(firstCall.thread != ownerThread);
        for (QThread* thread : resources.geometryThreads())
            QCOMPARE(thread, ownerThread);
        comparer.resume();

        QVERIFY(takeBatch(finished).result.ok);
        QCOMPARE(resources.positions(102).front()[0], 44.0);
    }

    void staleGenerationNeverCommits()
    {
        WorkspaceState state;
        makeReady(state);
        OwningResourceProvider resources;
        FakeRendererAdapter renderer;
        commitFakeScene(renderer, state, resources);
        FakeSurfaceComparer comparer;
        comparer.enqueueSuccess(0.9);
        comparer.enqueueSuccess(0.8);
        comparer.pauseNextComparison();
        MeshColorService service(resources, comparer, state, renderer);
        QSignalSpy finished(&service, &MeshColorService::analysisFinished);

        QVERIFY(service.startAnalysis(request(state)).ok);
        QVERIFY(comparer.waitUntilEntered());
        state.beginLoading();
        QVERIFY(state.commitWorkspace(entries(), 1).ok);
        comparer.resume();
        const AnalysisBatchResult batch = takeBatch(finished);

        QVERIFY(!batch.result.ok);
        QCOMPARE(renderer.presentationUpdateCount(), 0);
        QCOMPARE(state.generation(), quint64(2));
        QCOMPARE(state.mesh(2)->presentation.mode, ColorMode::Default);
    }

    void cancelledQueuedSuccessAndQueuedProgressNeverCommit()
    {
        WorkspaceState state;
        makeReady(state);
        OwningResourceProvider resources;
        FakeRendererAdapter renderer;
        commitFakeScene(renderer, state, resources);
        FakeSurfaceComparer comparer;
        comparer.enqueueSuccess(0.9);
        comparer.enqueueSuccess(0.8);
        comparer.pauseNextComparison();
        MeshColorService service(resources, comparer, state, renderer);
        QSignalSpy finished(&service, &MeshColorService::analysisFinished);
        QSignalSpy progress(&service, &MeshColorService::analysisProgress);

        QVERIFY(service.startAnalysis(request(state)).ok);
        QVERIFY(comparer.waitUntilEntered());
        comparer.resume();
        QVERIFY(comparer.waitUntilReturned(2));
        service.cancelAnalysis();
        QCoreApplication::processEvents();

        QVERIFY(!finished.isEmpty());
        QCOMPARE(progress.size(), 0);
        QCOMPARE(renderer.presentationUpdateCount(), 0);
        QCOMPARE(state.mesh(2)->presentation.mode, ColorMode::Default);
        QCOMPARE(state.phase(), WorkspacePhase::Ready);
    }

    void destructionCancelsAndSynchronouslyJoins()
    {
        WorkspaceState state;
        makeReady(state);
        OwningResourceProvider resources;
        FakeRendererAdapter renderer;
        commitFakeScene(renderer, state, resources);
        FakeSurfaceComparer comparer;
        comparer.enqueueSuccess(0.9);
        comparer.pauseNextComparison();
        std::unique_ptr<MeshColorService> service(
            new MeshColorService(resources, comparer, state, renderer));
        QVERIFY(service->startAnalysis(request(state)).ok);
        QVERIFY(comparer.waitUntilEntered());

        service.reset();

        QVERIFY(comparer.waitUntilReturned());
        QCOMPARE(state.phase(), WorkspacePhase::Ready);
        QCOMPARE(renderer.presentationUpdateCount(), 0);
    }

    void uniformAndClearNeverMutateProviderGeometry()
    {
        WorkspaceState state;
        makeReady(state);
        OwningResourceProvider resources;
        const QVector<MeshPoint3D> beforePositions = resources.positions(102);
        const QVector<MeshPoint3D> beforeNormals = resources.normals(102);
        const QVector<std::array<int, 3>> beforeFaces = resources.faces(102);
        FakeRendererAdapter renderer;
        commitFakeScene(renderer, state, resources);
        FakeSurfaceComparer comparer;
        MeshColorService service(resources, comparer, state, renderer);

        comparer.enqueueSuccess(0.9);
        comparer.enqueueSuccess(0.8);
        QSignalSpy finished(&service, &MeshColorService::analysisFinished);
        QVERIFY(service.startAnalysis(request(state)).ok);
        QVERIFY(takeBatch(finished).result.ok);
        QVERIFY(state.mesh(3)->hasScore);

        QVERIFY(service.setUniformColor(2, QColor(QStringLiteral("#5aaa75"))).ok);
        QCOMPARE(state.mesh(2)->presentation.mode, ColorMode::UniformColor);
        QCOMPARE(renderer.presentation(2).mode, ColorMode::UniformColor);
        QVERIFY(!state.mesh(2)->hasScore);
        QCOMPARE(state.mesh(2)->score, 0.0);
        QVERIFY(state.mesh(3)->hasScore);
        QCOMPARE(state.mesh(3)->score, 0.8);
        QCOMPARE(state.mesh(3)->presentation.mode, ColorMode::PrecisionResult);
        QCOMPARE(renderer.presentation(3).mode, ColorMode::PrecisionResult);
        QCOMPARE(resources.positions(102), beforePositions);
        QCOMPARE(resources.normals(102), beforeNormals);
        QCOMPARE(resources.faces(102), beforeFaces);

        QVERIFY(service.clearColoring().ok);
        QCOMPARE(renderer.lastPresentationBatch().size(), state.meshes().size());
        QSet<MeshId> clearedIds;
        for (const MeshColorPresentationUpdate& update : renderer.lastPresentationBatch()) {
            clearedIds.insert(update.meshId);
            QCOMPARE(update.presentation.mode, ColorMode::Default);
        }
        QCOMPARE(clearedIds, QSet<MeshId>({1, 2, 3}));
        for (const MeshEntry& mesh : state.meshes()) {
            QCOMPARE(mesh.presentation.mode, ColorMode::Default);
            QVERIFY(!mesh.hasScore);
            QCOMPARE(renderer.presentation(mesh.id).mode, ColorMode::Default);
        }
        QCOMPARE(resources.positions(102), beforePositions);
        QCOMPARE(resources.normals(102), beforeNormals);
        QCOMPARE(resources.faces(102), beforeFaces);
        QCOMPARE(renderer.presentationUpdateCount(), 3);
    }

    void invalidComparerScoresFailTheBatchBeforeColorConversion()
    {
        WorkspaceState state;
        makeReady(state);
        OwningResourceProvider resources;
        FakeRendererAdapter renderer;
        commitFakeScene(renderer, state, resources);
        FakeSurfaceComparer comparer;
        comparer.enqueueSuccess(std::numeric_limits<double>::quiet_NaN());
        comparer.enqueueSuccess(0.8);
        MeshColorService service(resources, comparer, state, renderer);
        QSignalSpy finished(&service, &MeshColorService::analysisFinished);

        QVERIFY(service.startAnalysis(request(state)).ok);
        QVERIFY(!takeBatch(finished).result.ok);
        comparer.clearPendingOutcomes();
        QCOMPARE(renderer.presentationUpdateCount(), 0);
        QCOMPARE(state.mesh(2)->presentation.mode, ColorMode::Default);

        comparer.enqueueSuccess(0.9, {std::numeric_limits<double>::infinity()});
        comparer.enqueueSuccess(0.8);
        QVERIFY(service.startAnalysis(request(state)).ok);
        QVERIFY(!takeBatch(finished).result.ok);
        comparer.clearPendingOutcomes();
        QCOMPARE(renderer.presentationUpdateCount(), 0);

        comparer.enqueueSuccess(0.9, {1.01});
        comparer.enqueueSuccess(0.8);
        QVERIFY(service.startAnalysis(request(state)).ok);
        QVERIFY(!takeBatch(finished).result.ok);
        comparer.clearPendingOutcomes();
        QCOMPARE(renderer.presentationUpdateCount(), 0);

        comparer.enqueueSuccess(0.9, {-0.5});
        comparer.enqueueSuccess(0.8);
        QVERIFY(service.startAnalysis(request(state)).ok);
        QVERIFY(!takeBatch(finished).result.ok);
        comparer.clearPendingOutcomes();
        comparer.enqueueSuccess(0.9, {-1.1});
        comparer.enqueueSuccess(0.8);
        QVERIFY(service.startAnalysis(request(state)).ok);
        QVERIFY(!takeBatch(finished).result.ok);
        comparer.clearPendingOutcomes();

        comparer.enqueueSuccess(0.9, {-1.0});
        comparer.enqueueSuccess(0.8, {0.5});
        QVERIFY(service.startAnalysis(request(state)).ok);
        QVERIFY(!takeBatch(finished).result.ok);
        QCOMPARE(renderer.presentationUpdateCount(), 0);
    }

    void comparerExceptionBecomesABatchFailure()
    {
        WorkspaceState state;
        makeReady(state);
        OwningResourceProvider resources;
        FakeRendererAdapter renderer;
        commitFakeScene(renderer, state, resources);
        ThrowingSurfaceComparer comparer;
        MeshColorService service(resources, comparer, state, renderer);
        QSignalSpy finished(&service, &MeshColorService::analysisFinished);

        QVERIFY(service.startAnalysis(request(state)).ok);
        const AnalysisBatchResult batch = takeBatch(finished);

        QVERIFY(!batch.result.ok);
        QVERIFY(batch.result.error.contains(QStringLiteral("scripted comparer exception")));
        QCOMPARE(renderer.presentationUpdateCount(), 0);
        QCOMPARE(state.phase(), WorkspacePhase::Ready);
    }

    void uniformAndClearRendererFailuresLeavePriorStateUnchanged()
    {
        WorkspaceState state;
        makeReady(state);
        OwningResourceProvider resources;
        FakeRendererAdapter renderer;
        commitFakeScene(renderer, state, resources);
        FakeSurfaceComparer comparer;
        MeshColorService service(resources, comparer, state, renderer);
        const QColor original(QStringLiteral("#5aaa75"));
        QVERIFY(service.setUniformColor(2, original).ok);
        QCOMPARE(renderer.presentationUpdateCount(), 1);

        renderer.failNextPresentationBatch(QStringLiteral("uniform failed"));
        QVERIFY(!service.setUniformColor(2, QColor(Qt::red)).ok);
        QCOMPARE(renderer.presentationUpdateCount(), 1);
        QCOMPARE(renderer.presentation(2).uniformColor, original);
        QCOMPARE(state.mesh(2)->presentation.uniformColor, original);

        renderer.failNextPresentationBatch(QStringLiteral("clear failed"));
        QVERIFY(!service.clearColoring().ok);
        QCOMPARE(renderer.presentationUpdateCount(), 1);
        QCOMPARE(renderer.presentation(2).mode, ColorMode::UniformColor);
        QCOMPARE(state.mesh(2)->presentation.mode, ColorMode::UniformColor);
    }

    void cancellationRetainsTheActiveMetric()
    {
        WorkspaceState state;
        makeReady(state);
        OwningResourceProvider resources;
        FakeRendererAdapter renderer;
        commitFakeScene(renderer, state, resources);
        FakeSurfaceComparer comparer;
        comparer.enqueueSuccess(0.9);
        comparer.pauseNextComparison();
        MeshColorService service(resources, comparer, state, renderer);
        QSignalSpy finished(&service, &MeshColorService::analysisFinished);
        AnalysisRequest value = request(state);
        value.metric = SurfaceComparisonMetric::NormalAgreement;
        QVERIFY(service.startAnalysis(value).ok);
        QVERIFY(comparer.waitUntilEntered());

        service.cancelAnalysis();

        const AnalysisBatchResult result = takeBatch(finished);
        QCOMPARE(result.metric, SurfaceComparisonMetric::NormalAgreement);
        QVERIFY(!result.result.ok);
    }

    void normalAgreementSuccessUsesNormalPresentationForEveryTarget()
    {
        WorkspaceState state;
        makeReady(state);
        OwningResourceProvider resources;
        FakeRendererAdapter renderer;
        commitFakeScene(renderer, state, resources);
        FakeSurfaceComparer comparer;
        comparer.enqueueSuccess(0.9);
        comparer.enqueueSuccess(0.8);
        MeshColorService service(resources, comparer, state, renderer);
        QSignalSpy finished(&service, &MeshColorService::analysisFinished);
        AnalysisRequest value = request(state);
        value.metric = SurfaceComparisonMetric::NormalAgreement;

        QVERIFY(service.startAnalysis(value).ok);
        const AnalysisBatchResult result = takeBatch(finished);

        QVERIFY2(result.result.ok, qPrintable(result.result.error));
        QCOMPARE(result.metric, SurfaceComparisonMetric::NormalAgreement);
        for (MeshId id : {MeshId(2), MeshId(3)}) {
            QCOMPARE(
                state.mesh(id)->presentation.mode,
                ColorMode::NormalAgreementResult);
            QCOMPARE(
                renderer.presentation(id).mode,
                ColorMode::NormalAgreementResult);
        }
    }

    void oldQueuedCompletionCannotFinishANewerActiveBatch()
    {
        WorkspaceState state;
        makeReady(state);
        OwningResourceProvider resources;
        FakeRendererAdapter renderer;
        commitFakeScene(renderer, state, resources);
        FakeSurfaceComparer comparer;
        comparer.enqueueSuccess(0.9);
        comparer.enqueueSuccess(0.8);
        comparer.pauseNextComparison();
        MeshColorService service(resources, comparer, state, renderer);
        QSignalSpy finished(&service, &MeshColorService::analysisFinished);

        QVERIFY(service.startAnalysis(request(state)).ok);
        QVERIFY(comparer.waitUntilEntered());
        comparer.resume();
        QVERIFY(comparer.waitUntilReturned(2));
        service.cancelAnalysis();
        finished.clear();

        comparer.enqueueSuccess(0.7);
        comparer.enqueueSuccess(0.6);
        QVERIFY(service.startAnalysis(request(state)).ok);
        QVERIFY(comparer.waitUntilReturned(4));
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 5000);

        const AnalysisBatchResult result =
            qvariant_cast<AnalysisBatchResult>(finished.takeFirst().front());
        QVERIFY(result.result.ok);
        QCOMPARE(state.phase(), WorkspacePhase::Ready);
        QCOMPARE(renderer.presentationUpdateCount(), 1);
        QCOMPARE(state.mesh(2)->score, 0.7);
        QCOMPARE(state.mesh(3)->score, 0.6);
    }
};

QTEST_GUILESS_MAIN(MeshColorServiceTest)
#include "mesh_color_service_test.moc"
