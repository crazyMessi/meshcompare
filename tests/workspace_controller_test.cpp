#include <QtTest>

#include <initializer_list>
#include <memory>
#include <type_traits>

#include <QSignalSpy>
#include <QWidget>

#include <common/ml_document/mesh_model.h>
#include <vcg/complex/allocate.h>

#include "app/application_startup.h"
#include "app/coloring_commands.h"
#include "app/workspace_controller.h"
#include "fakes/fake_camera_pose_store.h"
#include "fakes/fake_mesh_import_service.h"
#include "fakes/fake_renderer_adapter.h"
#include "fakes/fake_surface_comparer.h"

namespace
{
MeshEntry entry(MeshId id, const QString& name = QString())
{
    const QString displayName = name.isEmpty()
                                    ? QStringLiteral("mesh-%1.obj").arg(id)
                                    : name;
    return {id,
            0,
            QStringLiteral("/tmp/%1").arg(displayName),
            displayName,
            {},
            false};
}

StagedWorkspace staged(
    std::initializer_list<MeshEntry> sourceEntries,
    SceneLayoutMode layoutMode = SceneLayoutMode::ComparisonGrid)
{
    std::unique_ptr<MeshLabMeshRepository> repository(new MeshLabMeshRepository);
    QVector<MeshEntry> entries;
    for (MeshEntry sourceEntry : sourceEntries) {
        MeshModel* mesh = repository->allocateMesh(
            sourceEntry.sourcePath,
            sourceEntry.displayName);
        CMeshO::VertexIterator vertex =
            vcg::tri::Allocator<CMeshO>::AddVertices(mesh->cm, 3);
        CVertexO* vertices[3];
        for (int index = 0; index < 3; ++index, ++vertex) {
            vertices[index] = &*vertex;
            vertex->N() = CMeshO::CoordType(0, 0, 1);
        }
        vertices[0]->P() = CMeshO::CoordType(0, 0, 0);
        vertices[1]->P() = CMeshO::CoordType(1, 0, 0);
        vertices[2]->P() = CMeshO::CoordType(0, 1, 0);
        CMeshO::FaceIterator face =
            vcg::tri::Allocator<CMeshO>::AddFaces(mesh->cm, 1);
        face->V(0) = vertices[0];
        face->V(1) = vertices[1];
        face->V(2) = vertices[2];
        mesh->updateDataMask();
        sourceEntry.resourceId = repository->resourceIdFor(*mesh);
        entries.append(sourceEntry);
    }
    return {OperationResult::success(),
            std::move(repository),
            entries,
            {},
            layoutMode};
}

StagedWorkspace failedStage(
    const QString& error,
    QVector<QPair<QString, QString>> fileErrors = {})
{
    return {OperationResult::failure(error), nullptr, {}, std::move(fileErrors)};
}

OperationResult installPresentations(
    WorkspaceState& state,
    FakeRendererAdapter& renderer,
    const QVector<MeshColorStateUpdate>& stateUpdates)
{
    PreparedColorStateUpdate prepared;
    const OperationResult preparedResult =
        state.prepareColorUpdates(stateUpdates, &prepared);
    if (!preparedResult.ok)
        return preparedResult;

    QVector<MeshColorPresentationUpdate> rendererUpdates;
    rendererUpdates.reserve(stateUpdates.size());
    for (const MeshColorStateUpdate& update : stateUpdates)
        rendererUpdates.append({update.meshId, update.presentation});
    const OperationResult rendered =
        renderer.setColorPresentations(rendererUpdates);
    if (!rendered.ok)
        return rendered;

    state.commitPreparedColorUpdates(std::move(prepared));
    return OperationResult::success();
}

ColorPresentation analyticalPresentation(ColorMode mode, const QColor& color)
{
    ColorPresentation presentation;
    presentation.mode = mode;
    presentation.faceColors = {color};
    return presentation;
}

ColorPresentation uniformPresentation(const QColor& color)
{
    ColorPresentation presentation;
    presentation.mode = ColorMode::UniformColor;
    presentation.uniformColor = color;
    return presentation;
}
} // namespace

static_assert(
    std::is_base_of<IColoringCommands, WorkspaceController>::value,
    "WorkspaceController must implement the renderer-free coloring command contract.");

class WorkspaceControllerTest : public QObject
{
    Q_OBJECT

private slots:
    void failedRendererPreparationPreservesTheOldWorkspace()
    {
        WorkspaceState state;
        FakeMeshImportService importer;
        importer.enqueue(staged({entry(1), entry(2)}));
        importer.enqueue(staged({entry(3), entry(4)}));
        FakeRendererAdapter renderer;
        FakeSurfaceComparer comparer;
        WorkspaceController controller(
            state, importer, renderer, comparer, cameraStore_);
        QVERIFY(controller.importMeshes({QStringLiteral("old-a.obj"),
                                         QStringLiteral("old-b.obj")})
                    .result.ok);
        QVERIFY(controller.selectMesh(2).ok);
        renderer.failNextPrepare(QStringLiteral("GPU upload failed"));

        const WorkspaceImportOutcome outcome = controller.importMeshes(
            {QStringLiteral("new-a.obj"), QStringLiteral("new-b.obj")});

        QVERIFY(!outcome.result.ok);
        QCOMPARE(outcome.result.error, QStringLiteral("GPU upload failed"));
        QCOMPARE(state.phase(), WorkspacePhase::Ready);
        QCOMPARE(state.generation(), quint64(1));
        QCOMPARE(state.meshes().size(), 2);
        QCOMPARE(state.meshes().front().id, MeshId(1));
        QCOMPARE(state.referenceId(), MeshId(1));
        QCOMPARE(state.selectedMeshId(), MeshId(2));
        QCOMPARE(renderer.committedGeneration(), quint64(1));
        QCOMPARE(renderer.referenceMeshId(), MeshId(1));
        QCOMPARE(renderer.selectedMeshId(), MeshId(2));
        QCOMPARE(renderer.discardCount(), 1);
    }

    void failedStagingPreservesTheOldWorkspaceAndReturnsFileErrors()
    {
        WorkspaceState state;
        FakeMeshImportService importer;
        importer.enqueue(staged({entry(1), entry(2)}));
        const QVector<QPair<QString, QString>> errors = {
            qMakePair(QStringLiteral("bad-a.obj"), QStringLiteral("unsupported format")),
            qMakePair(QStringLiteral("bad-b.obj"), QStringLiteral("file not found"))};
        importer.enqueue(failedStage(
            QStringLiteral("One or more meshes could not be loaded."),
            errors));
        FakeRendererAdapter renderer;
        FakeSurfaceComparer comparer;
        WorkspaceController controller(
            state, importer, renderer, comparer, cameraStore_);
        QVERIFY(controller.importMeshes({QStringLiteral("old-a.obj"),
                                         QStringLiteral("old-b.obj")})
                    .result.ok);
        const int prepareCount = renderer.prepareCount();

        const WorkspaceImportOutcome outcome = controller.importMeshes(
            {QStringLiteral("bad-a.obj"), QStringLiteral("bad-b.obj")});

        QVERIFY(!outcome.result.ok);
        QCOMPARE(outcome.fileErrors, errors);
        QCOMPARE(state.phase(), WorkspacePhase::Ready);
        QCOMPARE(state.generation(), quint64(1));
        QCOMPARE(state.meshes().front().id, MeshId(1));
        QCOMPARE(renderer.committedGeneration(), quint64(1));
        QCOMPARE(renderer.prepareCount(), prepareCount);
    }

    void invalidStagedWorkspaceIsRejectedBeforeRendererPreparation()
    {
        WorkspaceState state;
        FakeMeshImportService importer;
        importer.enqueue(staged({entry(1), entry(2)}));
        importer.enqueue(staged({entry(3), entry(3)}));
        FakeRendererAdapter renderer;
        FakeSurfaceComparer comparer;
        WorkspaceController controller(
            state, importer, renderer, comparer, cameraStore_);
        QVERIFY(controller.importMeshes({QStringLiteral("old-a.obj"),
                                         QStringLiteral("old-b.obj")})
                    .result.ok);
        const int prepareCount = renderer.prepareCount();

        const WorkspaceImportOutcome outcome = controller.importMeshes(
            {QStringLiteral("duplicate-a.obj"), QStringLiteral("duplicate-b.obj")});

        QVERIFY(!outcome.result.ok);
        QCOMPARE(outcome.result.error,
                 QStringLiteral("Workspace mesh IDs must be unique."));
        QCOMPARE(renderer.prepareCount(), prepareCount);
        QCOMPARE(renderer.committedGeneration(), quint64(1));
        QCOMPARE(state.phase(), WorkspacePhase::Ready);
        QCOMPARE(state.generation(), quint64(1));
        QCOMPARE(state.meshes().front().id, MeshId(1));
    }

    void successfulImportCommitsRendererBeforeStateAndThenRefreshesRendererState()
    {
        WorkspaceState state;
        FakeMeshImportService importer(staged({entry(7, QStringLiteral("scene_gt.obj")),
                                               entry(8, QStringLiteral("candidate.obj"))}));
        FakeRendererAdapter renderer;
        FakeSurfaceComparer comparer;
        QStringList sequence;
        renderer.setTrace(&sequence);
        WorkspaceController controller(
            state, importer, renderer, comparer, cameraStore_);
        sequence.clear();
        renderer.setPrepareObserver([&] {
            QCOMPARE(state.phase(), WorkspacePhase::Loading);
            QCOMPARE(state.generation(), quint64(0));
        });
        renderer.setCommitObserver([&] {
            QCOMPARE(state.phase(), WorkspacePhase::Loading);
            QCOMPARE(state.generation(), quint64(0));
        });
        renderer.setReferenceObserver([&] {
            QCOMPARE(state.phase(), WorkspacePhase::Ready);
            QCOMPARE(state.generation(), quint64(1));
            sequence.insert(sequence.size() - 1, QStringLiteral("state-commit"));
        });
        renderer.setSelectionObserver([&] {
            QCOMPARE(state.selectedMeshId(), MeshId(7));
        });

        const WorkspaceImportOutcome outcome = controller.importMeshes(
            {QStringLiteral("scene_gt.obj"), QStringLiteral("candidate.obj")});

        QVERIFY2(outcome.result.ok, qPrintable(outcome.result.error));
        QCOMPARE(sequence,
                 QStringList({QStringLiteral("prepare"),
                              QStringLiteral("renderer-commit"),
                              QStringLiteral("state-commit"),
                              QStringLiteral("reference-update"),
                              QStringLiteral("selection-update")}));
        QCOMPARE(renderer.lastPreparedScene().referenceId, MeshId(7));
        QVERIFY(renderer.lastPreparedScene().meshes.front().isReference);
    }

    void projectImportForwardsOverlayLayoutToTheRenderer()
    {
        WorkspaceState state;
        FakeMeshImportService importer(staged(
            {entry(1, QStringLiteral("candidate.obj")),
             entry(2, QStringLiteral("scene_gt.obj"))},
            SceneLayoutMode::Overlay));
        FakeRendererAdapter renderer;
        FakeSurfaceComparer comparer;
        WorkspaceController controller(
            state, importer, renderer, comparer, cameraStore_);

        const WorkspaceImportOutcome outcome = controller.importMeshes(
            {QStringLiteral("comparison.mlp")});

        QVERIFY2(outcome.result.ok, qPrintable(outcome.result.error));
        QCOMPARE(
            renderer.lastPreparedScene().layoutMode,
            SceneLayoutMode::Overlay);
    }

    void rendererSelectionEventUpdatesValidatedMeshIdInStateAndRenderer()
    {
        WorkspaceState state;
        FakeMeshImportService importer(staged({entry(11), entry(22)}));
        FakeRendererAdapter renderer;
        FakeSurfaceComparer comparer;
        WorkspaceController controller(
            state, importer, renderer, comparer, cameraStore_);
        QVERIFY(controller.importMeshes({QStringLiteral("a.obj"), QStringLiteral("b.obj")})
                    .result.ok);

        renderer.emitSelectedMeshChanged(22);

        QCOMPARE(state.selectedMeshId(), MeshId(22));
        QCOMPARE(renderer.selectedMeshId(), MeshId(22));
        renderer.emitSelectedMeshChanged(999);
        QCOMPARE(state.selectedMeshId(), MeshId(22));
        QCOMPARE(renderer.selectedMeshId(), MeshId(22));
    }

    void rendererErrorsBecomeNonBlockingControllerStatusEvents()
    {
        WorkspaceState state;
        FakeMeshImportService importer;
        FakeRendererAdapter renderer;
        FakeSurfaceComparer comparer;
        WorkspaceController controller(
            state, importer, renderer, comparer, cameraStore_);
        QSignalSpy statusSpy(&controller, &WorkspaceController::statusMessage);

        renderer.emitRendererError(QStringLiteral("OpenGL context was lost"));

        QCOMPARE(statusSpy.size(), 1);
        QCOMPARE(statusSpy.takeFirst().front().toString(),
                 QStringLiteral("OpenGL context was lost"));
    }

    void successfulImportPreservesReferenceResolverNotice()
    {
        WorkspaceState state;
        FakeMeshImportService importer(staged({entry(1), entry(2)}));
        FakeRendererAdapter renderer;
        FakeSurfaceComparer comparer;
        WorkspaceController controller(
            state, importer, renderer, comparer, cameraStore_);

        const WorkspaceImportOutcome outcome = controller.importMeshes(
            {QStringLiteral("a.obj"), QStringLiteral("b.obj")});

        QVERIFY(outcome.result.ok);
        QCOMPARE(outcome.notice,
                 QStringLiteral(
                     "No gt mesh found; using the first import.\n"
                     "No UUID was found in this workspace."));
        QVERIFY(outcome.fileErrors.isEmpty());
    }

    void fatalRendererInitializationMountsOnceAndPreservesTheOriginalError()
    {
        WorkspaceState state;
        FakeRendererAdapter renderer;
        renderer.failNextMount(QStringLiteral("OpenGL 2.1 is unavailable"));
        QWidget host;

        const OperationResult result = initializeRenderer(state, renderer, &host);

        QVERIFY(!result.ok);
        QCOMPARE(result.error, QStringLiteral("OpenGL 2.1 is unavailable"));
        QCOMPARE(renderer.mountCount(), 1);
        QCOMPARE(state.phase(), WorkspacePhase::FatalError);
    }

    void migrationFailureNoticeIsMergedIntoDeferredStartupImportOutcome()
    {
        const QString migrationNotice = QStringLiteral(
            "Camera pose migration was skipped: legacy store is unreadable");
        WorkspaceImportOutcome succeeded{
            OperationResult::success(),
            {},
            QStringLiteral("No gt mesh found; using the first import.")};
        WorkspaceImportOutcome failed{
            OperationResult::failure(QStringLiteral("Mesh import failed.")),
            {},
            {}};

        mergeStartupNotice(succeeded, migrationNotice);
        mergeStartupNotice(failed, migrationNotice);

        QCOMPARE(
            succeeded.notice,
            QStringLiteral(
                "No gt mesh found; using the first import.\n"
                "Camera pose migration was skipped: legacy store is unreadable"));
        QCOMPARE(
            failed.result.error,
            QStringLiteral(
                "Mesh import failed.\n"
                "Camera pose migration was skipped: legacy store is unreadable"));
    }

    void controllerDisconnectsEventsAndClearsRendererBeforeRepositoryDestruction()
    {
        WorkspaceState state;
        FakeMeshImportService importer(staged({entry(1), entry(2)}));
        FakeRendererAdapter renderer;
        FakeSurfaceComparer comparer;
        QStringList lifecycle;
        renderer.setTrace(&lifecycle);
        renderer.setClearProbeResource(1);
        {
            WorkspaceController controller(
                state, importer, renderer, comparer, cameraStore_);
            QVERIFY(controller.importMeshes(
                                  {QStringLiteral("a.obj"), QStringLiteral("b.obj")})
                        .result.ok);
            lifecycle.clear();
        }

        QCOMPARE(lifecycle,
                 QStringList({QStringLiteral("events-disconnect"),
                              QStringLiteral("clear")}));
        QCOMPARE(renderer.eventDisconnectCount(), 1);
        QCOMPARE(renderer.clearCount(), 1);
        QVERIFY(renderer.resourceAvailableDuringClear());
    }

    void pausedAnalysisIsJoinedBeforeFailedStagingAndOldServiceRemainsUsable()
    {
        WorkspaceState state;
        FakeMeshImportService importer;
        importer.enqueue(staged({entry(1), entry(2)}));
        importer.enqueue(failedStage(QStringLiteral("staging failed")));
        FakeRendererAdapter renderer;
        FakeSurfaceComparer comparer;
        comparer.enqueueSuccess(0.9);
        comparer.pauseNextComparison();
        WorkspaceController controller(
            state, importer, renderer, comparer, cameraStore_);
        QVERIFY(controller.importMeshes({QStringLiteral("a.obj"), QStringLiteral("b.obj")})
                    .result.ok);
        SurfaceComparisonOptions options;
        options.sampleCount = 10;
        QVERIFY(controller.startAnalysis(
                    SurfaceComparisonMetric::PrecisionAtThreshold, options).ok);
        QVERIFY(comparer.waitUntilEntered());

        const WorkspaceImportOutcome failed = controller.importMeshes(
            {QStringLiteral("bad-a.obj"), QStringLiteral("bad-b.obj")});

        QVERIFY(!failed.result.ok);
        QVERIFY(comparer.waitUntilReturned());
        QCOMPARE(state.phase(), WorkspacePhase::Ready);
        QCOMPARE(state.generation(), quint64(1));
        QCOMPARE(renderer.committedGeneration(), quint64(1));

        comparer.enqueueSuccess(0.8);
        QSignalSpy finished(&controller, &WorkspaceController::analysisFinished);
        QVERIFY(controller.startAnalysis(
                    SurfaceComparisonMetric::NormalAgreement, options).ok);
        QVERIFY(finished.wait(5000));
        const AnalysisBatchResult result =
            qvariant_cast<AnalysisBatchResult>(finished.takeFirst().front());
        QVERIFY2(result.result.ok, qPrintable(result.result.error));
        QCOMPARE(state.phase(), WorkspacePhase::Ready);
    }

    void pausedAnalysisIsJoinedBeforeFailedRendererPreparationAndOldServiceRemainsUsable()
    {
        WorkspaceState state;
        FakeMeshImportService importer;
        importer.enqueue(staged({entry(1), entry(2)}));
        importer.enqueue(staged({entry(3), entry(4)}));
        FakeRendererAdapter renderer;
        FakeSurfaceComparer comparer;
        comparer.enqueueSuccess(0.9);
        comparer.pauseNextComparison();
        WorkspaceController controller(
            state, importer, renderer, comparer, cameraStore_);
        QVERIFY(controller.importMeshes({QStringLiteral("a.obj"), QStringLiteral("b.obj")})
                    .result.ok);
        SurfaceComparisonOptions options;
        options.sampleCount = 10;
        QVERIFY(controller.startAnalysis(
                    SurfaceComparisonMetric::PrecisionAtThreshold, options).ok);
        QVERIFY(comparer.waitUntilEntered());
        renderer.failNextPrepare(QStringLiteral("GPU preparation failed"));

        const WorkspaceImportOutcome failed = controller.importMeshes(
            {QStringLiteral("new-a.obj"), QStringLiteral("new-b.obj")});

        QVERIFY(!failed.result.ok);
        QVERIFY(comparer.waitUntilReturned());
        QCOMPARE(state.phase(), WorkspacePhase::Ready);
        QCOMPARE(state.generation(), quint64(1));
        QCOMPARE(renderer.committedGeneration(), quint64(1));

        comparer.enqueueSuccess(0.8);
        QSignalSpy finished(&controller, &WorkspaceController::analysisFinished);
        QVERIFY(controller.startAnalysis(
                    SurfaceComparisonMetric::PrecisionAtThreshold, options).ok);
        QVERIFY(finished.wait(5000));
        QVERIFY(qvariant_cast<AnalysisBatchResult>(
                    finished.takeFirst().front()).result.ok);
    }

    void successfulReplacementJoinsOldAnalysisBeforeCommitAndCreatesANewUsableService()
    {
        WorkspaceState state;
        FakeMeshImportService importer;
        importer.enqueue(staged({entry(1), entry(2)}));
        importer.enqueue(staged({entry(3), entry(4)}));
        FakeRendererAdapter renderer;
        FakeSurfaceComparer comparer;
        comparer.enqueueSuccess(0.9);
        comparer.pauseNextComparison();
        WorkspaceController controller(
            state, importer, renderer, comparer, cameraStore_);
        QVERIFY(controller.importMeshes({QStringLiteral("a.obj"), QStringLiteral("b.obj")})
                    .result.ok);
        SurfaceComparisonOptions options;
        options.sampleCount = 10;
        QVERIFY(controller.startAnalysis(
                    SurfaceComparisonMetric::PrecisionAtThreshold, options).ok);
        QVERIFY(comparer.waitUntilEntered());
        renderer.setCommitObserver([&] { QCOMPARE(comparer.returnedCount(), 1); });

        const WorkspaceImportOutcome replacement = controller.importMeshes(
            {QStringLiteral("new-a.obj"), QStringLiteral("new-b.obj")});

        QVERIFY2(replacement.result.ok, qPrintable(replacement.result.error));
        QCOMPARE(state.phase(), WorkspacePhase::Ready);
        QCOMPARE(state.generation(), quint64(2));
        QCOMPARE(state.meshes().front().id, MeshId(3));
        QCOMPARE(renderer.committedGeneration(), quint64(2));

        comparer.enqueueSuccess(0.77);
        QSignalSpy finished(&controller, &WorkspaceController::analysisFinished);
        QVERIFY(controller.startAnalysis(
                    SurfaceComparisonMetric::NormalAgreement, options).ok);
        QVERIFY(finished.wait(5000));
        const AnalysisBatchResult result =
            qvariant_cast<AnalysisBatchResult>(finished.takeFirst().front());
        QVERIFY2(result.result.ok, qPrintable(result.result.error));
        QCOMPARE(result.generation, quint64(2));
        QCOMPARE(state.mesh(4)->presentation.mode, ColorMode::NormalAgreementResult);
    }

    void replacementGuardRejectsDirectReentrantAnalysisFromCancellationSignal()
    {
        WorkspaceState state;
        FakeMeshImportService importer;
        importer.enqueue(staged({entry(1), entry(2)}));
        importer.enqueue(failedStage(QStringLiteral("replacement staging failed")));
        FakeRendererAdapter renderer;
        FakeSurfaceComparer comparer;
        comparer.enqueueSuccess(0.9);
        comparer.pauseNextComparison();
        WorkspaceController controller(
            state, importer, renderer, comparer, cameraStore_);
        QVERIFY(controller.importMeshes({QStringLiteral("a.obj"), QStringLiteral("b.obj")})
                    .result.ok);
        SurfaceComparisonOptions options;
        options.sampleCount = 10;
        QVERIFY(controller.startAnalysis(
                    SurfaceComparisonMetric::PrecisionAtThreshold, options).ok);
        QVERIFY(comparer.waitUntilEntered());
        OperationResult reentrantStart = OperationResult::success();
        connect(
            &controller,
            &WorkspaceController::analysisFinished,
            &controller,
            [&](const AnalysisBatchResult&) {
                reentrantStart = controller.startAnalysis(
                    SurfaceComparisonMetric::NormalAgreement, options);
            },
            Qt::DirectConnection);

        const WorkspaceImportOutcome replacement = controller.importMeshes(
            {QStringLiteral("bad-a.obj"), QStringLiteral("bad-b.obj")});

        QVERIFY(!replacement.result.ok);
        QVERIFY(!reentrantStart.ok);
        QVERIFY(
            reentrantStart.error.contains(
                QStringLiteral("replacement"), Qt::CaseInsensitive) ||
            reentrantStart.error.contains(
                QStringLiteral("callback"), Qt::CaseInsensitive));
        QCOMPARE(comparer.callCount(), 1);
        QCOMPARE(state.phase(), WorkspacePhase::Ready);
        QCOMPARE(state.generation(), quint64(1));
        QCOMPARE(renderer.committedGeneration(), quint64(1));
    }

    void rendererErrorCallbackRejectsReentrantReplacementUntilColorCommandReturns()
    {
        WorkspaceState state;
        FakeMeshImportService importer;
        importer.enqueue(staged({entry(1), entry(2)}));
        importer.enqueue(staged({entry(3), entry(4)}));
        FakeRendererAdapter renderer;
        FakeSurfaceComparer comparer;
        WorkspaceController controller(
            state, importer, renderer, comparer, cameraStore_);
        QVERIFY(controller.importMeshes({QStringLiteral("a.obj"), QStringLiteral("b.obj")})
                    .result.ok);
        WorkspaceImportOutcome reentrantImport;
        bool attempted = false;
        connect(
            &controller,
            &WorkspaceController::statusMessage,
            &controller,
            [&](const QString&) {
                if (attempted)
                    return;
                attempted = true;
                reentrantImport = controller.importMeshes(
                    {QStringLiteral("new-a.obj"), QStringLiteral("new-b.obj")});
            },
            Qt::DirectConnection);
        renderer.setPresentationObserver([&] {
            renderer.emitRendererError(QStringLiteral("scripted renderer warning"));
        });

        const OperationResult colorResult =
            controller.setUniformColor(2, QColor(QStringLiteral("#5aaa75")));

        QVERIFY2(colorResult.ok, qPrintable(colorResult.error));
        QVERIFY(attempted);
        QVERIFY(!reentrantImport.result.ok);
        QVERIFY(reentrantImport.result.error.contains(
            QStringLiteral("callback"), Qt::CaseInsensitive));
        QCOMPARE(state.generation(), quint64(1));
        QCOMPARE(renderer.committedGeneration(), quint64(1));
        QCOMPARE(state.mesh(2)->presentation.mode, ColorMode::UniformColor);
        QCOMPARE(renderer.presentation(2).mode, ColorMode::UniformColor);

        renderer.setPresentationObserver({});
        const WorkspaceImportOutcome replacement = controller.importMeshes(
            {QStringLiteral("new-a.obj"), QStringLiteral("new-b.obj")});
        QVERIFY2(replacement.result.ok, qPrintable(replacement.result.error));
        QCOMPARE(state.generation(), quint64(2));
    }

    void analysisProgressCallbackRejectsReentrantWorkspaceReplacement()
    {
        WorkspaceState state;
        FakeMeshImportService importer;
        importer.enqueue(staged({entry(1), entry(2)}));
        importer.enqueue(staged({entry(3), entry(4)}));
        FakeRendererAdapter renderer;
        FakeSurfaceComparer comparer;
        comparer.enqueueSuccess(0.9);
        WorkspaceController controller(
            state, importer, renderer, comparer, cameraStore_);
        QVERIFY(controller.importMeshes({QStringLiteral("a.obj"), QStringLiteral("b.obj")})
                    .result.ok);

        WorkspaceImportOutcome reentrantImport;
        bool attempted = false;
        connect(
            &controller,
            &WorkspaceController::analysisProgress,
            &controller,
            [&](quint64, quint64, MeshId, int, const QString&) {
                if (attempted)
                    return;
                attempted = true;
                reentrantImport = controller.importMeshes(
                    {QStringLiteral("new-a.obj"), QStringLiteral("new-b.obj")});
            },
            Qt::DirectConnection);
        QSignalSpy finished(&controller, &WorkspaceController::analysisFinished);
        SurfaceComparisonOptions options;
        options.sampleCount = 10;

        QVERIFY(controller.startAnalysis(
                    SurfaceComparisonMetric::PrecisionAtThreshold, options).ok);
        QTRY_VERIFY(attempted);
        QTRY_COMPARE(finished.size(), 1);

        QVERIFY(!reentrantImport.result.ok);
        QVERIFY(reentrantImport.result.error.contains(
            QStringLiteral("callback"), Qt::CaseInsensitive));
        QVERIFY(qvariant_cast<AnalysisBatchResult>(
                    finished.takeFirst().front()).result.ok);
        QCOMPARE(state.generation(), quint64(1));
        QCOMPARE(state.phase(), WorkspacePhase::Ready);

        const WorkspaceImportOutcome replacement = controller.importMeshes(
            {QStringLiteral("new-a.obj"), QStringLiteral("new-b.obj")});
        QVERIFY2(replacement.result.ok, qPrintable(replacement.result.error));
        QCOMPARE(state.generation(), quint64(2));
    }

    void analysisFinishedCallbackRejectsReentrantWorkspaceReplacement()
    {
        WorkspaceState state;
        FakeMeshImportService importer;
        importer.enqueue(staged({entry(1), entry(2)}));
        importer.enqueue(staged({entry(3), entry(4)}));
        FakeRendererAdapter renderer;
        FakeSurfaceComparer comparer;
        comparer.enqueueSuccess(0.9);
        WorkspaceController controller(
            state, importer, renderer, comparer, cameraStore_);
        QVERIFY(controller.importMeshes({QStringLiteral("a.obj"), QStringLiteral("b.obj")})
                    .result.ok);

        WorkspaceImportOutcome reentrantImport;
        AnalysisBatchResult completed;
        bool attempted = false;
        connect(
            &controller,
            &WorkspaceController::analysisFinished,
            &controller,
            [&](const AnalysisBatchResult& result) {
                if (attempted)
                    return;
                attempted = true;
                completed = result;
                reentrantImport = controller.importMeshes(
                    {QStringLiteral("new-a.obj"), QStringLiteral("new-b.obj")});
            },
            Qt::DirectConnection);
        SurfaceComparisonOptions options;
        options.sampleCount = 10;

        QVERIFY(controller.startAnalysis(
                    SurfaceComparisonMetric::NormalAgreement, options).ok);
        QTRY_VERIFY(attempted);

        QVERIFY2(completed.result.ok, qPrintable(completed.result.error));
        QVERIFY(!reentrantImport.result.ok);
        QVERIFY(reentrantImport.result.error.contains(
            QStringLiteral("callback"), Qt::CaseInsensitive));
        QCOMPARE(state.generation(), quint64(1));
        QCOMPARE(state.phase(), WorkspacePhase::Ready);

        const WorkspaceImportOutcome replacement = controller.importMeshes(
            {QStringLiteral("new-a.obj"), QStringLiteral("new-b.obj")});
        QVERIFY2(replacement.result.ok, qPrintable(replacement.result.error));
        QCOMPARE(state.generation(), quint64(2));
    }

    void referenceChangeClearsAnalyticalPresentationsInOneBatchAndPreservesUniformColor()
    {
        WorkspaceState state;
        FakeMeshImportService importer(staged({entry(1), entry(2), entry(3)}));
        FakeRendererAdapter renderer;
        FakeSurfaceComparer comparer;
        WorkspaceController controller(
            state, importer, renderer, comparer, cameraStore_);
        QVERIFY(controller.importMeshes(
                              {QStringLiteral("a.obj"),
                               QStringLiteral("b.obj"),
                               QStringLiteral("c.obj")})
                    .result.ok);

        const QColor uniformColor(QStringLiteral("#5aaa75"));
        const ColorPresentation precision = analyticalPresentation(
            ColorMode::PrecisionResult, QColor(QStringLiteral("#42d66b")));
        const ColorPresentation uniform = uniformPresentation(uniformColor);
        const OperationResult installed = installPresentations(
            state,
            renderer,
            {{2, precision, 0.942, true}, {3, uniform, 0.0, false}});
        QVERIFY2(installed.ok, qPrintable(installed.error));
        QVERIFY(renderer.setAnalysisOverlays({{2, QStringLiteral("P 0.942")}}).ok);
        const int presentationAttempts = renderer.presentationAttemptCount();
        QSignalSpy workspaceChanged(
            &controller, &WorkspaceController::workspaceChanged);
        bool observedPrecommitState = false;
        renderer.setPresentationObserver([&] {
            observedPrecommitState = true;
            QCOMPARE(state.referenceId(), MeshId(1));
            QCOMPARE(state.mesh(2)->presentation.mode, ColorMode::PrecisionResult);
            QCOMPARE(state.mesh(3)->presentation.mode, ColorMode::UniformColor);
            QCOMPARE(renderer.referenceMeshId(), MeshId(1));
        });

        const OperationResult changed = controller.setReference(2);

        QVERIFY2(changed.ok, qPrintable(changed.error));
        QVERIFY(observedPrecommitState);
        QCOMPARE(renderer.presentationAttemptCount(), presentationAttempts + 1);
        QCOMPARE(renderer.lastPresentationBatch().size(), 1);
        QCOMPARE(renderer.lastPresentationBatch().front().meshId, MeshId(2));
        QCOMPARE(
            renderer.lastPresentationBatch().front().presentation.mode,
            ColorMode::Default);
        QCOMPARE(state.referenceId(), MeshId(2));
        QCOMPARE(renderer.referenceMeshId(), MeshId(2));
        QCOMPARE(state.mesh(2)->presentation.mode, ColorMode::Default);
        QVERIFY(!state.mesh(2)->hasScore);
        QCOMPARE(state.mesh(3)->presentation.mode, ColorMode::UniformColor);
        QCOMPARE(state.mesh(3)->presentation.uniformColor, uniformColor);
        QCOMPARE(renderer.presentation(2).mode, ColorMode::Default);
        QCOMPARE(renderer.presentation(3).mode, ColorMode::UniformColor);
        QCOMPARE(renderer.presentation(3).uniformColor, uniformColor);
        QCOMPARE(renderer.analysisOverlay(2), QString());
        QCOMPARE(workspaceChanged.size(), 1);
    }

    void referenceRendererFailureLeavesStateMarkerPresentationsAndOverlaysUnchanged()
    {
        WorkspaceState state;
        FakeMeshImportService importer(staged({entry(1), entry(2), entry(3)}));
        FakeRendererAdapter renderer;
        FakeSurfaceComparer comparer;
        WorkspaceController controller(
            state, importer, renderer, comparer, cameraStore_);
        QVERIFY(controller.importMeshes(
                              {QStringLiteral("a.obj"),
                               QStringLiteral("b.obj"),
                               QStringLiteral("c.obj")})
                    .result.ok);

        const QColor uniformColor(QStringLiteral("#5aaa75"));
        const ColorPresentation precision = analyticalPresentation(
            ColorMode::PrecisionResult, QColor(QStringLiteral("#42d66b")));
        const ColorPresentation uniform = uniformPresentation(uniformColor);
        const OperationResult installed = installPresentations(
            state,
            renderer,
            {{2, precision, 0.942, true}, {3, uniform, 0.0, false}});
        QVERIFY2(installed.ok, qPrintable(installed.error));
        QVERIFY(renderer.setAnalysisOverlays(
                            {{2, QStringLiteral("P 0.942")},
                             {3, QStringLiteral("Uniform")}})
                    .ok);
        const int successfulPresentationUpdates = renderer.presentationUpdateCount();
        const int successfulOverlayUpdates = renderer.overlayUpdateCount();
        renderer.failNextPresentationBatch(QStringLiteral("presentation rejected"));
        QSignalSpy workspaceChanged(
            &controller, &WorkspaceController::workspaceChanged);

        const OperationResult changed = controller.setReference(2);

        QVERIFY(!changed.ok);
        QCOMPARE(changed.error, QStringLiteral("presentation rejected"));
        QCOMPARE(state.referenceId(), MeshId(1));
        QCOMPARE(renderer.referenceMeshId(), MeshId(1));
        QCOMPARE(state.mesh(2)->presentation.mode, ColorMode::PrecisionResult);
        QCOMPARE(state.mesh(2)->score, 0.942);
        QVERIFY(state.mesh(2)->hasScore);
        QCOMPARE(state.mesh(3)->presentation.mode, ColorMode::UniformColor);
        QCOMPARE(state.mesh(3)->presentation.uniformColor, uniformColor);
        QCOMPARE(renderer.presentation(2).mode, ColorMode::PrecisionResult);
        QCOMPARE(renderer.presentation(3).mode, ColorMode::UniformColor);
        QCOMPARE(renderer.analysisOverlay(2), QStringLiteral("P 0.942"));
        QCOMPARE(renderer.analysisOverlay(3), QStringLiteral("Uniform"));
        QCOMPARE(renderer.presentationUpdateCount(), successfulPresentationUpdates);
        QCOMPARE(renderer.overlayUpdateCount(), successfulOverlayUpdates);
        QCOMPARE(workspaceChanged.size(), 0);
    }

    void currentReferenceIsANonMutatingNoOp()
    {
        WorkspaceState state;
        FakeMeshImportService importer(staged({entry(1), entry(2)}));
        FakeRendererAdapter renderer;
        FakeSurfaceComparer comparer;
        WorkspaceController controller(
            state, importer, renderer, comparer, cameraStore_);
        QVERIFY(controller.importMeshes({QStringLiteral("a.obj"), QStringLiteral("b.obj")})
                    .result.ok);
        const ColorPresentation precision = analyticalPresentation(
            ColorMode::PrecisionResult, QColor(QStringLiteral("#42d66b")));
        const OperationResult installed = installPresentations(
            state, renderer, {{2, precision, 0.942, true}});
        QVERIFY2(installed.ok, qPrintable(installed.error));
        QVERIFY(renderer.setAnalysisOverlays({{2, QStringLiteral("P 0.942")}}).ok);
        const int presentationAttempts = renderer.presentationAttemptCount();
        const int overlayAttempts = renderer.overlayAttemptCount();
        QSignalSpy workspaceChanged(
            &controller, &WorkspaceController::workspaceChanged);

        const OperationResult unchanged = controller.setReference(1);

        QVERIFY2(unchanged.ok, qPrintable(unchanged.error));
        QCOMPARE(renderer.presentationAttemptCount(), presentationAttempts);
        QCOMPARE(renderer.overlayAttemptCount(), overlayAttempts);
        QCOMPARE(state.mesh(2)->presentation.mode, ColorMode::PrecisionResult);
        QCOMPARE(renderer.analysisOverlay(2), QStringLiteral("P 0.942"));
        QCOMPARE(workspaceChanged.size(), 0);
    }

    void unknownReferenceIsRejectedBeforeRendererMutation()
    {
        WorkspaceState state;
        FakeMeshImportService importer(staged({entry(1), entry(2)}));
        FakeRendererAdapter renderer;
        FakeSurfaceComparer comparer;
        WorkspaceController controller(
            state, importer, renderer, comparer, cameraStore_);
        QVERIFY(controller.importMeshes({QStringLiteral("a.obj"), QStringLiteral("b.obj")})
                    .result.ok);
        const int presentationAttempts = renderer.presentationAttemptCount();
        const int overlayAttempts = renderer.overlayAttemptCount();

        const OperationResult changed = controller.setReference(999);

        QVERIFY(!changed.ok);
        QCOMPARE(state.referenceId(), MeshId(1));
        QCOMPARE(renderer.referenceMeshId(), MeshId(1));
        QCOMPARE(renderer.presentationAttemptCount(), presentationAttempts);
        QCOMPARE(renderer.overlayAttemptCount(), overlayAttempts);
    }

    void referenceChangeIsRejectedWhileAnalyzing()
    {
        WorkspaceState state;
        FakeMeshImportService importer(staged({entry(1), entry(2)}));
        FakeRendererAdapter renderer;
        FakeSurfaceComparer comparer;
        WorkspaceController controller(
            state, importer, renderer, comparer, cameraStore_);
        QVERIFY(controller.importMeshes({QStringLiteral("a.obj"), QStringLiteral("b.obj")})
                    .result.ok);
        QVERIFY(state.beginAnalysis().ok);
        const int presentationAttempts = renderer.presentationAttemptCount();
        const int overlayAttempts = renderer.overlayAttemptCount();

        const OperationResult changed = controller.setReference(2);

        QVERIFY(!changed.ok);
        QCOMPARE(state.referenceId(), MeshId(1));
        QCOMPARE(renderer.referenceMeshId(), MeshId(1));
        QCOMPARE(renderer.presentationAttemptCount(), presentationAttempts);
        QCOMPARE(renderer.overlayAttemptCount(), overlayAttempts);
        state.finishAnalysis();
    }

    void analysisProgressUpdatesTheTargetOverlayBeforeForwardingTheSignal()
    {
        WorkspaceState state;
        FakeMeshImportService importer(staged({entry(1), entry(2)}));
        FakeRendererAdapter renderer;
        FakeSurfaceComparer comparer;
        comparer.enqueueSuccess(0.8);
        comparer.pauseNextComparison();
        WorkspaceController controller(
            state, importer, renderer, comparer, cameraStore_);
        QVERIFY(controller.importMeshes({QStringLiteral("a.obj"), QStringLiteral("b.obj")})
                    .result.ok);
        bool observedProgress = false;
        connect(
            &controller,
            &WorkspaceController::analysisProgress,
            &controller,
            [&](quint64, quint64, MeshId meshId, int percent, const QString&) {
                observedProgress = true;
                QCOMPARE(meshId, MeshId(2));
                QCOMPARE(
                    renderer.analysisOverlay(meshId),
                    QStringLiteral("%1%").arg(percent));
                QCOMPARE(renderer.analysisOverlay(1), QString());
            },
            Qt::DirectConnection);
        QSignalSpy finished(&controller, &WorkspaceController::analysisFinished);
        SurfaceComparisonOptions options;
        options.sampleCount = 10;

        QVERIFY(controller.startAnalysis(
                    SurfaceComparisonMetric::PrecisionAtThreshold, options).ok);
        QTRY_VERIFY(observedProgress);
        controller.cancelAnalysis();

        QCOMPARE(finished.size(), 1);
        QCOMPARE(state.phase(), WorkspacePhase::Ready);
        QCOMPARE(renderer.analysisOverlay(2), QString());
    }

    void analysisSuccessPublishesPrecisionScoreBadge()
    {
        WorkspaceState state;
        FakeMeshImportService importer(staged({entry(1), entry(2)}));
        FakeRendererAdapter renderer;
        FakeSurfaceComparer comparer;
        comparer.enqueueSuccess(0.942);
        WorkspaceController controller(
            state, importer, renderer, comparer, cameraStore_);
        QVERIFY(controller.importMeshes({QStringLiteral("a.obj"), QStringLiteral("b.obj")})
                    .result.ok);
        QSignalSpy finished(&controller, &WorkspaceController::analysisFinished);
        SurfaceComparisonOptions options;
        options.sampleCount = 10;

        QVERIFY(controller.startAnalysis(
                    SurfaceComparisonMetric::PrecisionAtThreshold, options).ok);
        QVERIFY(finished.wait(5000));

        const AnalysisBatchResult result =
            qvariant_cast<AnalysisBatchResult>(finished.takeFirst().front());
        QVERIFY2(result.result.ok, qPrintable(result.result.error));
        QCOMPARE(renderer.analysisOverlay(1), QString());
        QCOMPARE(renderer.analysisOverlay(2), QStringLiteral("P 0.942"));
    }

    void analysisSuccessPublishesNormalScoreBadge()
    {
        WorkspaceState state;
        FakeMeshImportService importer(staged({entry(1), entry(2)}));
        FakeRendererAdapter renderer;
        FakeSurfaceComparer comparer;
        comparer.enqueueSuccess(0.917);
        WorkspaceController controller(
            state, importer, renderer, comparer, cameraStore_);
        QVERIFY(controller.importMeshes({QStringLiteral("a.obj"), QStringLiteral("b.obj")})
                    .result.ok);
        QSignalSpy finished(&controller, &WorkspaceController::analysisFinished);
        SurfaceComparisonOptions options;
        options.sampleCount = 10;

        QVERIFY(controller.startAnalysis(
                    SurfaceComparisonMetric::NormalAgreement, options).ok);
        QVERIFY(finished.wait(5000));

        const AnalysisBatchResult result =
            qvariant_cast<AnalysisBatchResult>(finished.takeFirst().front());
        QVERIFY2(result.result.ok, qPrintable(result.result.error));
        QCOMPARE(renderer.analysisOverlay(1), QString());
        QCOMPARE(renderer.analysisOverlay(2), QStringLiteral("N 0.917"));
    }

    void failedAnalysisRestoresTheLastCommittedScoreBadgeAndReportsOneStatus()
    {
        WorkspaceState state;
        FakeMeshImportService importer(staged({entry(1), entry(2)}));
        FakeRendererAdapter renderer;
        FakeSurfaceComparer comparer;
        comparer.enqueueSuccess(0.942);
        comparer.enqueueFailure(QStringLiteral("comparison failed"));
        WorkspaceController controller(
            state, importer, renderer, comparer, cameraStore_);
        QVERIFY(controller.importMeshes({QStringLiteral("a.obj"), QStringLiteral("b.obj")})
                    .result.ok);
        SurfaceComparisonOptions options;
        options.sampleCount = 10;
        QSignalSpy finished(&controller, &WorkspaceController::analysisFinished);
        QVERIFY(controller.startAnalysis(
                    SurfaceComparisonMetric::PrecisionAtThreshold, options).ok);
        QVERIFY(finished.wait(5000));
        QVERIFY(qvariant_cast<AnalysisBatchResult>(
                    finished.takeFirst().front()).result.ok);
        QCOMPARE(renderer.analysisOverlay(2), QStringLiteral("P 0.942"));
        QSignalSpy status(&controller, &WorkspaceController::statusMessage);

        QVERIFY(controller.startAnalysis(
                    SurfaceComparisonMetric::NormalAgreement, options).ok);
        QVERIFY(finished.wait(5000));

        const AnalysisBatchResult result =
            qvariant_cast<AnalysisBatchResult>(finished.takeFirst().front());
        QVERIFY(!result.result.ok);
        QCOMPARE(result.result.error, QStringLiteral("comparison failed"));
        QCOMPARE(renderer.analysisOverlay(2), QStringLiteral("P 0.942"));
        QCOMPARE(status.size(), 1);
        QCOMPARE(status.takeFirst().front().toString(),
                 QStringLiteral("comparison failed"));
    }

    void cancelledAnalysisRestoresTheLastCommittedScoreBadge()
    {
        WorkspaceState state;
        FakeMeshImportService importer(staged({entry(1), entry(2)}));
        FakeRendererAdapter renderer;
        FakeSurfaceComparer comparer;
        comparer.enqueueSuccess(0.942);
        comparer.enqueueSuccess(0.8);
        WorkspaceController controller(
            state, importer, renderer, comparer, cameraStore_);
        QVERIFY(controller.importMeshes({QStringLiteral("a.obj"), QStringLiteral("b.obj")})
                    .result.ok);
        SurfaceComparisonOptions options;
        options.sampleCount = 10;
        QSignalSpy finished(&controller, &WorkspaceController::analysisFinished);
        QVERIFY(controller.startAnalysis(
                    SurfaceComparisonMetric::PrecisionAtThreshold, options).ok);
        QVERIFY(finished.wait(5000));
        QVERIFY(qvariant_cast<AnalysisBatchResult>(
                    finished.takeFirst().front()).result.ok);
        QCOMPARE(renderer.analysisOverlay(2), QStringLiteral("P 0.942"));

        comparer.pauseNextComparison();
        QVERIFY(controller.startAnalysis(
                    SurfaceComparisonMetric::NormalAgreement, options).ok);
        QVERIFY(comparer.waitUntilEntered(2));
        QTRY_VERIFY(renderer.analysisOverlay(2).endsWith(QLatin1Char('%')));
        controller.cancelAnalysis();

        QCOMPARE(finished.size(), 1);
        const AnalysisBatchResult result =
            qvariant_cast<AnalysisBatchResult>(finished.takeFirst().front());
        QVERIFY(!result.result.ok);
        QVERIFY(result.result.error.contains(
            QStringLiteral("cancel"), Qt::CaseInsensitive));
        QCOMPARE(renderer.analysisOverlay(2), QStringLiteral("P 0.942"));
    }

    void uniformAndClearCommandsRemoveObsoleteScoreBadges()
    {
        WorkspaceState state;
        FakeMeshImportService importer(staged({entry(1), entry(2)}));
        FakeRendererAdapter renderer;
        FakeSurfaceComparer comparer;
        WorkspaceController controller(
            state, importer, renderer, comparer, cameraStore_);
        QVERIFY(controller.importMeshes({QStringLiteral("a.obj"), QStringLiteral("b.obj")})
                    .result.ok);
        const ColorPresentation precision = analyticalPresentation(
            ColorMode::PrecisionResult, QColor(QStringLiteral("#42d66b")));
        OperationResult installed = installPresentations(
            state, renderer, {{2, precision, 0.942, true}});
        QVERIFY2(installed.ok, qPrintable(installed.error));
        QVERIFY(renderer.setAnalysisOverlays({{2, QStringLiteral("P 0.942")}}).ok);

        const OperationResult uniform = controller.setUniformColor(
            2, QColor(QStringLiteral("#5aaa75")));

        QVERIFY2(uniform.ok, qPrintable(uniform.error));
        QCOMPARE(state.mesh(2)->presentation.mode, ColorMode::UniformColor);
        QCOMPARE(renderer.analysisOverlay(2), QString());

        installed = installPresentations(
            state, renderer, {{2, precision, 0.942, true}});
        QVERIFY2(installed.ok, qPrintable(installed.error));
        QVERIFY(renderer.setAnalysisOverlays({{2, QStringLiteral("P 0.942")}}).ok);

        const OperationResult cleared = controller.clearColoring();

        QVERIFY2(cleared.ok, qPrintable(cleared.error));
        QCOMPARE(state.mesh(2)->presentation.mode, ColorMode::Default);
        QCOMPARE(renderer.analysisOverlay(2), QString());
    }

    void workspaceChangedDirectSubscriberCannotReentrantlyReplaceWorkspace()
    {
        WorkspaceState state;
        FakeMeshImportService importer;
        importer.enqueue(staged({entry(1), entry(2)}));
        importer.enqueue(staged({entry(3), entry(4)}));
        FakeRendererAdapter renderer;
        FakeSurfaceComparer comparer;
        WorkspaceController controller(
            state, importer, renderer, comparer, cameraStore_);
        QVERIFY(controller.importMeshes({QStringLiteral("a.obj"), QStringLiteral("b.obj")})
                    .result.ok);
        WorkspaceImportOutcome reentrantImport;
        bool attempted = false;
        const QStringList replacementPaths = {
            QStringLiteral("new-a.obj"), QStringLiteral("new-b.obj")};
        const QMetaObject::Connection connection = connect(
            &controller,
            &WorkspaceController::workspaceChanged,
            &controller,
            [&] {
                if (attempted)
                    return;
                attempted = true;
                reentrantImport = controller.importMeshes(replacementPaths);
            },
            Qt::DirectConnection);

        const OperationResult colored = controller.setUniformColor(
            2, QColor(QStringLiteral("#5aaa75")));

        QVERIFY2(colored.ok, qPrintable(colored.error));
        QVERIFY(attempted);
        QVERIFY(!reentrantImport.result.ok);
        QVERIFY(reentrantImport.result.error.contains(
            QStringLiteral("callback"), Qt::CaseInsensitive));
        QCOMPARE(state.generation(), quint64(1));
        QCOMPARE(state.mesh(2)->presentation.mode, ColorMode::UniformColor);

        disconnect(connection);
        const WorkspaceImportOutcome replacement = controller.importMeshes(
            {QStringLiteral("new-a.obj"), QStringLiteral("new-b.obj")});
        QVERIFY2(replacement.result.ok, qPrintable(replacement.result.error));
        QCOMPARE(state.generation(), quint64(2));
    }

    void controllerDestructionCancelsAndJoinsBeforeRendererClear()
    {
        WorkspaceState state;
        FakeMeshImportService importer(staged({entry(1), entry(2)}));
        FakeRendererAdapter renderer;
        FakeSurfaceComparer comparer;
        comparer.enqueueSuccess(0.9);
        comparer.pauseNextComparison();
        renderer.setClearObserver([&] { QCOMPARE(comparer.returnedCount(), 1); });
        {
            WorkspaceController controller(
                state, importer, renderer, comparer, cameraStore_);
            QVERIFY(controller.importMeshes(
                                  {QStringLiteral("a.obj"), QStringLiteral("b.obj")})
                        .result.ok);
            SurfaceComparisonOptions options;
            options.sampleCount = 10;
            QVERIFY(controller.startAnalysis(
                        SurfaceComparisonMetric::PrecisionAtThreshold, options).ok);
            QVERIFY(comparer.waitUntilEntered());
        }

        QVERIFY(comparer.waitUntilReturned());
        QCOMPARE(renderer.clearCount(), 1);
    }

    void rejectedAnalysisPublishesItsMetricAndError()
    {
        WorkspaceState state;
        FakeMeshImportService importer;
        FakeRendererAdapter renderer;
        FakeSurfaceComparer comparer;
        WorkspaceController controller(
            state, importer, renderer, comparer, cameraStore_);
        bool published = false;
        SurfaceComparisonMetric publishedMetric =
            SurfaceComparisonMetric::NormalAgreement;
        QString publishedError;
        connect(
            &controller,
            &WorkspaceController::analysisStartFailed,
            &controller,
            [&](SurfaceComparisonMetric metric, const QString& error) {
                published = true;
                publishedMetric = metric;
                publishedError = error;
            },
            Qt::DirectConnection);

        SurfaceComparisonOptions options;
        const OperationResult result = controller.startAnalysis(
            SurfaceComparisonMetric::PrecisionAtThreshold,
            options);

        QVERIFY(!result.ok);
        QVERIFY(published);
        QCOMPARE(
            publishedMetric,
            SurfaceComparisonMetric::PrecisionAtThreshold);
        QCOMPARE(publishedError, result.error);
    }

private:
    FakeCameraPoseStore cameraStore_;
};

QTEST_MAIN(WorkspaceControllerTest)
#include "workspace_controller_test.moc"
