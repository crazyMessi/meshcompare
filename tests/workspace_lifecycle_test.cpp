#include <QtTest>

#include <functional>
#include <memory>
#include <string>
#include <utility>

#include <common/ml_document/mesh_model.h>
#include <vcg/complex/allocate.h>

#include "app/workspace_controller.h"
#include "core/renderer_adapter.h"
#include "core/workspace_state.h"
#include "infrastructure/meshlab/meshlab_mesh_repository.h"
#include "services/mesh_import_service.h"
#include "fakes/fake_camera_pose_store.h"
#include "fakes/fake_surface_comparer.h"

namespace
{
const QString AnalysisJoin = QStringLiteral("analysis:join");
const QString CallbacksDisconnect = QStringLiteral("callbacks:disconnect");
const QString ViewportsDestroy = QStringLiteral("viewports:destroy");
const QString GpuRelease = QStringLiteral("gpu:release");
const QString RepositoryRelease = QStringLiteral("repository:release");

struct LifecycleCycle
{
    QString name;
    QStringList events;
    bool providerLiveDuringViewportDestruction = false;
    bool providerLiveDuringGpuRelease = false;
};

struct RepositoryLifetimeToken
{
    int serial = 0;
};

class LifecycleRecorder final
{
public:
    void beginCycle(const QString& name)
    {
        cycles_.append({name, {}, false, false});
        activeCycle_ = cycles_.size() - 1;
    }

    void append(const QString& event)
    {
        Q_ASSERT(activeCycle_ >= 0);
        cycles_[activeCycle_].events.append(event);
    }

    void recordViewportProviderLiveness(bool live)
    {
        Q_ASSERT(activeCycle_ >= 0);
        cycles_[activeCycle_].providerLiveDuringViewportDestruction = live;
    }

    void recordGpuProviderLiveness(bool live)
    {
        Q_ASSERT(activeCycle_ >= 0);
        cycles_[activeCycle_].providerLiveDuringGpuRelease = live;
    }

    // Callers first verify that the repository-owned weak lifetime token has
    // expired. The two provider probes immediately before that observation
    // prove renderer teardown happened while the same repository was live.
    void recordRepositoryReleaseBoundary()
    {
        append(RepositoryRelease);
        activeCycle_ = -1;
    }

    const QVector<LifecycleCycle>& cycles() const { return cycles_; }

private:
    QVector<LifecycleCycle> cycles_;
    int activeCycle_ = -1;
};

StagedWorkspace stagedWorkspace(
    int serial,
    std::weak_ptr<RepositoryLifetimeToken>* repositoryLifetime)
{
    std::unique_ptr<MeshLabMeshRepository> repository(new MeshLabMeshRepository);
    QVector<MeshEntry> entries;
    for (int index = 0; index < 2; ++index) {
        MeshEntry entry;
        entry.id = MeshId(serial * 10 + index + 1);
        entry.sourcePath = QStringLiteral("/tmp/lifecycle-%1-%2.obj")
                               .arg(serial)
                               .arg(index);
        entry.displayName = index == 0
            ? QStringLiteral("lifecycle_%1_gt.obj").arg(serial)
            : QStringLiteral("lifecycle_%1_candidate.obj").arg(serial);

        MeshModel* mesh = repository->allocateMesh(
            entry.sourcePath, entry.displayName);
        CMeshO::VertexIterator vertex =
            vcg::tri::Allocator<CMeshO>::AddVertices(mesh->cm, 3);
        CVertexO* vertices[3];
        for (int vertexIndex = 0; vertexIndex < 3;
             ++vertexIndex, ++vertex) {
            vertices[vertexIndex] = &*vertex;
            vertex->N() = CMeshO::CoordType(0, 0, 1);
        }
        vertices[0]->P() = CMeshO::CoordType(0, 0, 0);
        vertices[1]->P() = CMeshO::CoordType(1, 0, 0);
        vertices[2]->P() = CMeshO::CoordType(0, 1, 0);
        if (index == 0) {
            const std::shared_ptr<RepositoryLifetimeToken> lifetime(
                new RepositoryLifetimeToken{serial});
            CMeshO::PerVertexAttributeHandle<
                std::shared_ptr<RepositoryLifetimeToken>> lifetimeAttribute =
                vcg::tri::Allocator<CMeshO>::AddPerVertexAttribute<
                    std::shared_ptr<RepositoryLifetimeToken>>(
                    mesh->cm, std::string("meshcompare_lifecycle_token"));
            lifetimeAttribute[0] = lifetime;
            if (repositoryLifetime != nullptr)
                *repositoryLifetime = lifetime;
        }
        CMeshO::FaceIterator face =
            vcg::tri::Allocator<CMeshO>::AddFaces(mesh->cm, 1);
        face->V(0) = vertices[0];
        face->V(1) = vertices[1];
        face->V(2) = vertices[2];
        mesh->updateDataMask();
        entry.resourceId = repository->resourceIdFor(*mesh);
        entries.append(entry);
    }
    return {OperationResult::success(), std::move(repository), entries, {}};
}

class LifecycleImportService final : public IMeshImportService
{
public:
    void setBeforeStage(std::function<void(int)> callback)
    {
        beforeStage_ = std::move(callback);
    }

    StagedWorkspace stage(const QStringList&) override
    {
        ++stageCount_;
        if (beforeStage_)
            beforeStage_(stageCount_);
        std::weak_ptr<RepositoryLifetimeToken> lifetime;
        StagedWorkspace result = stagedWorkspace(stageCount_, &lifetime);
        repositoryLifetimes_.append(lifetime);
        return result;
    }

    int stageCount() const { return stageCount_; }
    bool repositoryReleased(int stageSerial) const
    {
        Q_ASSERT(stageSerial > 0 && stageSerial <= repositoryLifetimes_.size());
        return repositoryLifetimes_.at(stageSerial - 1).expired();
    }

private:
    std::function<void(int)> beforeStage_;
    QVector<std::weak_ptr<RepositoryLifetimeToken>> repositoryLifetimes_;
    int stageCount_ = 0;
};

class LifecycleRendererAdapter final : public IRendererAdapter
{
public:
    explicit LifecycleRendererAdapter(LifecycleRecorder& recorder)
        : recorder_(recorder)
    {
    }

    OperationResult mount(QWidget*) override
    {
        return OperationResult::success();
    }

    void setEvents(RendererEvents events) override
    {
        const bool connected = events.selectedMeshChanged ||
                               events.cameraChanged ||
                               events.resourceProgress ||
                               events.rendererError;
        if (eventsConnected_ && !connected) {
            recorder_.append(CallbacksDisconnect);
            ++eventDisconnectCount_;
        }
        eventsConnected_ = connected;
        events_ = std::move(events);
    }

    OperationResult prepareScene(
        const SceneDescriptor& scene,
        const IMeshResourceProvider& resources) override
    {
        if (scene.meshes.isEmpty()) {
            return OperationResult::failure(
                QStringLiteral("Lifecycle scene has no meshes."));
        }
        for (const SceneMesh& mesh : scene.meshes) {
            if (resources.geometry(mesh.resourceId) == nullptr) {
                return OperationResult::failure(
                    QStringLiteral("Lifecycle scene resource is unavailable."));
            }
        }
        preparedProvider_ = &resources;
        preparedScene_ = scene;
        return OperationResult::success();
    }

    void commitPreparedScene() override
    {
        if (preparedProvider_ == nullptr)
            return;
        if (committedProvider_ != nullptr)
            releaseCommittedScene(true);
        committedProvider_ = preparedProvider_;
        committedScene_ = preparedScene_;
        preparedProvider_ = nullptr;
        preparedScene_ = {};
    }

    void discardPreparedScene() override
    {
        preparedProvider_ = nullptr;
        preparedScene_ = {};
    }

    void clearScene() override
    {
        ++clearCount_;
        discardPreparedScene();
        if (committedProvider_ != nullptr)
            releaseCommittedScene(false);
    }

    void setSelectedMesh(MeshId meshId) override { selectedMeshId_ = meshId; }
    void setReferenceMesh(MeshId meshId) override { referenceMeshId_ = meshId; }
    OperationResult setMeshVisible(MeshId, bool) override
    {
        return committedProvider_ == nullptr
            ? OperationResult::failure(QStringLiteral("No committed lifecycle scene."))
            : OperationResult::success();
    }

    OperationResult setColorPresentations(
        const QVector<MeshColorPresentationUpdate>&) override
    {
        return committedProvider_ == nullptr
            ? OperationResult::failure(QStringLiteral("No committed lifecycle scene."))
            : OperationResult::success();
    }

    OperationResult setAnalysisOverlays(
        const QVector<MeshAnalysisOverlayUpdate>&) override
    {
        return committedProvider_ == nullptr
            ? OperationResult::failure(QStringLiteral("No committed lifecycle scene."))
            : OperationResult::success();
    }

    CameraPose captureCamera() const override { return camera_; }
    OperationResult captureImage(QImage& image) override
    {
        Q_UNUSED(image);
        return OperationResult::failure(
            QStringLiteral("Lifecycle renderer image capture is unavailable."));
    }
    OperationResult restoreCamera(const CameraPose& pose) override
    {
        camera_ = pose;
        return OperationResult::success();
    }
    void resetCamera() override { camera_ = {}; }
    void setDiagnostic(DiagnosticFlag, bool) override {}
    RendererDiagnostics diagnostics() const override { return {}; }

    int replacementTeardownCount() const { return replacementTeardownCount_; }
    int clearCount() const { return clearCount_; }
    int eventDisconnectCount() const { return eventDisconnectCount_; }
    bool eventsConnected() const { return eventsConnected_; }
    MeshId selectedMeshId() const { return selectedMeshId_; }
    MeshId referenceMeshId() const { return referenceMeshId_; }

private:
    bool committedResourceIsLive() const
    {
        if (committedProvider_ == nullptr || committedScene_.meshes.isEmpty())
            return false;
        return committedProvider_->geometry(
                   committedScene_.meshes.front().resourceId) != nullptr;
    }

    void releaseCommittedScene(bool replacement)
    {
        if (replacement) {
            ++replacementTeardownCount_;
        }
        recorder_.append(ViewportsDestroy);
        recorder_.recordViewportProviderLiveness(committedResourceIsLive());
        recorder_.append(GpuRelease);
        recorder_.recordGpuProviderLiveness(committedResourceIsLive());
        committedProvider_ = nullptr;
        committedScene_ = {};
    }

    LifecycleRecorder& recorder_;
    RendererEvents events_;
    const IMeshResourceProvider* preparedProvider_ = nullptr;
    const IMeshResourceProvider* committedProvider_ = nullptr;
    SceneDescriptor preparedScene_;
    SceneDescriptor committedScene_;
    CameraPose camera_;
    MeshId selectedMeshId_ = 0;
    MeshId referenceMeshId_ = 0;
    int replacementTeardownCount_ = 0;
    int clearCount_ = 0;
    int eventDisconnectCount_ = 0;
    bool eventsConnected_ = false;
};

void verifyCycle(
    const LifecycleCycle& cycle,
    const QStringList& expectedEvents)
{
    QCOMPARE(cycle.events, expectedEvents);
    QVERIFY2(
        cycle.providerLiveDuringViewportDestruction,
        qPrintable(QStringLiteral(
            "%1 destroyed its repository before viewport teardown.")
                       .arg(cycle.name)));
    QVERIFY2(
        cycle.providerLiveDuringGpuRelease,
        qPrintable(QStringLiteral(
            "%1 destroyed its repository before GPU release.")
                       .arg(cycle.name)));
}

SurfaceComparisonOptions lifecycleAnalysisOptions()
{
    SurfaceComparisonOptions options;
    options.sampleCount = 10;
    return options;
}
} // namespace

class WorkspaceLifecycleTest final : public QObject
{
    Q_OBJECT

private slots:
    void twentyFivePausedAnalysisReplacementsUseSafeReleaseOrder()
    {
        constexpr int ReplacementCount = 25;
        WorkspaceState state;
        LifecycleImportService importer;
        LifecycleRecorder recorder;
        LifecycleRendererAdapter renderer(recorder);
        FakeSurfaceComparer comparer;
        FakeCameraPoseStore cameraStore;
        std::unique_ptr<WorkspaceController> controller(new WorkspaceController(
            state, importer, renderer, comparer, cameraStore));

        const WorkspaceImportOutcome initial = controller->importMeshes(
            {QStringLiteral("initial-gt.obj"),
             QStringLiteral("initial-candidate.obj")});
        QVERIFY2(initial.result.ok, qPrintable(initial.result.error));
        QCOMPARE(state.phase(), WorkspacePhase::Ready);
        QCOMPARE(state.generation(), quint64(1));
        QVERIFY(!importer.repositoryReleased(1));

        importer.setBeforeStage([&](int stageCall) {
            // The initial import was stage 1. Every later stage must begin only
            // after the preceding paused analysis worker has returned.
            QCOMPARE(comparer.returnedCount(), stageCall - 1);
            QVERIFY(!importer.repositoryReleased(stageCall - 1));
            recorder.append(AnalysisJoin);
        });

        const SurfaceComparisonOptions options = lifecycleAnalysisOptions();
        for (int replacement = 0; replacement < ReplacementCount; ++replacement) {
            comparer.enqueueSuccess(0.9);
            comparer.pauseNextComparison();
            const OperationResult started = controller->startAnalysis(
                SurfaceComparisonMetric::PrecisionAtThreshold, options);
            QVERIFY2(started.ok, qPrintable(started.error));
            QVERIFY2(
                comparer.waitUntilEntered(replacement + 1),
                "The analysis worker did not enter before replacement.");

            recorder.beginCycle(
                QStringLiteral("replacement-%1").arg(replacement + 1));
            const WorkspaceImportOutcome outcome = controller->importMeshes(
                {QStringLiteral("replacement-gt.obj"),
                 QStringLiteral("replacement-candidate.obj")});
            QVERIFY2(outcome.result.ok, qPrintable(outcome.result.error));
            QVERIFY(importer.repositoryReleased(replacement + 1));
            recorder.recordRepositoryReleaseBoundary();

            QCOMPARE(comparer.returnedCount(), replacement + 1);
            QCOMPARE(state.phase(), WorkspacePhase::Ready);
            QCOMPARE(state.generation(), quint64(replacement + 2));
            QCOMPARE(renderer.referenceMeshId(), state.referenceId());
            QCOMPARE(renderer.selectedMeshId(), state.selectedMeshId());
        }

        recorder.beginCycle(QStringLiteral("idle-shutdown-after-stress"));
        controller.reset();
        QVERIFY(importer.repositoryReleased(importer.stageCount()));
        recorder.recordRepositoryReleaseBoundary();

        QCOMPARE(importer.stageCount(), ReplacementCount + 1);
        QCOMPARE(comparer.callCount(), ReplacementCount);
        QCOMPARE(comparer.returnedCount(), ReplacementCount);
        QCOMPARE(renderer.replacementTeardownCount(), ReplacementCount);
        QCOMPARE(
            renderer.eventDisconnectCount(),
            ReplacementCount + 1);
        QCOMPARE(renderer.clearCount(), 1);
        QVERIFY(!renderer.eventsConnected());

        const QVector<LifecycleCycle>& cycles = recorder.cycles();
        QCOMPARE(cycles.size(), ReplacementCount + 1);
        const QStringList replacementOrder = {
            AnalysisJoin,
            CallbacksDisconnect,
            ViewportsDestroy,
            GpuRelease,
            RepositoryRelease};
        for (int index = 0; index < ReplacementCount; ++index)
            verifyCycle(cycles.at(index), replacementOrder);
        verifyCycle(
            cycles.back(),
            {CallbacksDisconnect,
             ViewportsDestroy,
             GpuRelease,
             RepositoryRelease});
    }

    void idleReadyWorkspaceShutdownUsesSafeReleaseOrder()
    {
        WorkspaceState state;
        LifecycleImportService importer;
        LifecycleRecorder recorder;
        LifecycleRendererAdapter renderer(recorder);
        FakeSurfaceComparer comparer;
        FakeCameraPoseStore cameraStore;
        std::unique_ptr<WorkspaceController> controller(new WorkspaceController(
            state, importer, renderer, comparer, cameraStore));
        const WorkspaceImportOutcome imported = controller->importMeshes(
            {QStringLiteral("idle-gt.obj"), QStringLiteral("idle-result.obj")});
        QVERIFY2(imported.result.ok, qPrintable(imported.result.error));
        QCOMPARE(state.phase(), WorkspacePhase::Ready);
        QCOMPARE(comparer.callCount(), 0);
        QVERIFY(!importer.repositoryReleased(1));

        recorder.beginCycle(QStringLiteral("idle-ready-shutdown"));
        controller.reset();
        QVERIFY(importer.repositoryReleased(1));
        recorder.recordRepositoryReleaseBoundary();

        QCOMPARE(recorder.cycles().size(), 1);
        verifyCycle(
            recorder.cycles().front(),
            {CallbacksDisconnect,
             ViewportsDestroy,
             GpuRelease,
             RepositoryRelease});
        QCOMPARE(renderer.replacementTeardownCount(), 0);
        QCOMPARE(renderer.eventDisconnectCount(), 1);
        QCOMPARE(renderer.clearCount(), 1);
        QVERIFY(!renderer.eventsConnected());
    }
};

QTEST_MAIN(WorkspaceLifecycleTest)
#include "workspace_lifecycle_test.moc"
