#include <QtTest>

#include <initializer_list>
#include <memory>
#include <type_traits>

#include <common/ml_document/mesh_model.h>
#include <vcg/complex/allocate.h>

#include "app/camera_commands.h"
#include "app/workspace_controller.h"
#include "fakes/fake_camera_pose_store.h"
#include "fakes/fake_mesh_import_service.h"
#include "fakes/fake_renderer_adapter.h"
#include "fakes/fake_surface_comparer.h"

namespace
{
const QString UuidA = QStringLiteral("00112233445566778899aabbccddeeff");
const QString UuidB = QStringLiteral("ffeeddccbbaa99887766554433221100");
const QString UuidC = QStringLiteral("0123456789abcdef0123456789abcdef");
const QString NoUuidNotice =
    QStringLiteral("No UID was found in this workspace.");

CameraPose pose(const QString& value)
{
    return {value};
}

MeshEntry entry(
    MeshId id,
    const QString& name,
    const QStringList& uuidCandidates = {})
{
    return {id,
            0,
            QStringLiteral("/tmp/%1").arg(name),
            name,
            uuidCandidates,
            false};
}

StagedWorkspace staged(std::initializer_list<MeshEntry> sourceEntries)
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
    return {OperationResult::success(), std::move(repository), entries, {}};
}

WorkspaceImportOutcome importWorkspace(WorkspaceController& controller)
{
    return controller.importMeshes(
        {QStringLiteral("reference.obj"), QStringLiteral("candidate.obj")});
}
} // namespace

static_assert(
    std::is_base_of<ICameraCommands, WorkspaceController>::value,
    "WorkspaceController must implement the renderer-free camera command contract.");

class CameraWorkflowTest final : public QObject
{
    Q_OBJECT

private slots:
    void uuidResolverPrioritizesReferenceThenImportOrderAndNormalizesCandidates()
    {
        const QVector<MeshEntry> meshes = {
            entry(1, QStringLiteral("first.obj"), {QString(),
                                                   UuidA}),
            entry(2, QStringLiteral("reference.obj"),
                  {QStringLiteral("{FFEEDDCC-BBAA-9988-7766-554433221100}"),
                   UuidC}),
            entry(3, QStringLiteral("third.obj"), {UuidC})};

        QCOMPARE(resolveWorkspaceUuid(meshes, 2), UuidB);
        QCOMPARE(resolveWorkspaceUuid(meshes, 99), UuidA);

        QVector<MeshEntry> referenceWithoutUuid = meshes;
        referenceWithoutUuid[1].uuidCandidates =
            QStringList{QString()};
        QCOMPARE(resolveWorkspaceUuid(referenceWithoutUuid, 2), UuidA);

        for (MeshEntry& mesh : referenceWithoutUuid)
            mesh.uuidCandidates = QStringList{QString()};
        QVERIFY(resolveWorkspaceUuid(referenceWithoutUuid, 2).isEmpty());

        referenceWithoutUuid[0].uuidCandidates =
            QStringList{QStringLiteral("Run-42")};
        QCOMPARE(
            resolveWorkspaceUuid(referenceWithoutUuid, 2),
            QStringLiteral("run-42"));
    }

    void importRestoresLatestPoseAfterRendererAndStateCommits()
    {
        WorkspaceState state;
        FakeMeshImportService importer(staged(
            {entry(1, QStringLiteral("reference_gt.obj"), {UuidA}),
             entry(2, QStringLiteral("candidate.obj"), {UuidB})}));
        FakeRendererAdapter renderer;
        FakeSurfaceComparer comparer;
        FakeCameraPoseStore store;
        // Deliberately shuffled: timestamp wins first, then view ID.
        store.add(UuidA, QStringLiteral("view_010"), pose(QStringLiteral("tie-winner")),
                  QStringLiteral("2026-07-15T02:00:00Z"));
        store.add(UuidA, QStringLiteral("view_999"), pose(QStringLiteral("older")),
                  QStringLiteral("2026-07-15T01:00:00Z"));
        store.add(UuidA, QStringLiteral("view_002"), pose(QStringLiteral("tie-loser")),
                  QStringLiteral("2026-07-15T02:00:00Z"));
        store.add(UuidB, QStringLiteral("view_900"), pose(QStringLiteral("wrong-uuid")),
                  QStringLiteral("2026-07-15T03:00:00Z"));

        QStringList sequence;
        renderer.setTrace(&sequence);
        store.setTrace(&sequence);
        WorkspaceController controller(
            state, importer, renderer, comparer, store);
        QSignalSpy applySpy(
            &controller,
            &WorkspaceController::cameraApplyFinished);
        sequence.clear();
        bool workspaceChangeSawRestoredCamera = false;
        connect(
            &controller,
            &WorkspaceController::workspaceChanged,
            &controller,
            [&] {
                workspaceChangeSawRestoredCamera =
                    renderer.camera().viewStateXml ==
                    QStringLiteral("tie-winner");
            });
        store.setListObserver([&] {
            QCOMPARE(state.phase(), WorkspacePhase::Ready);
            QCOMPARE(state.generation(), quint64(1));
        });
        store.setLoadObserver([&] {
            QCOMPARE(state.phase(), WorkspacePhase::Ready);
            QCOMPARE(state.referenceId(), MeshId(1));
        });
        renderer.setRestoreObserver([&] {
            QCOMPARE(state.phase(), WorkspacePhase::Ready);
            QCOMPARE(state.generation(), quint64(1));
        });

        const WorkspaceImportOutcome outcome = importWorkspace(controller);

        QVERIFY2(outcome.result.ok, qPrintable(outcome.result.error));
        QCOMPARE(renderer.camera().viewStateXml, QStringLiteral("tie-winner"));
        QCOMPARE(store.lastListUuid(), UuidA);
        QCOMPARE(store.lastLoadUuid(), UuidA);
        QCOMPARE(store.lastLoadViewId(), QStringLiteral("view_010"));
        QCOMPARE(applySpy.size(), 1);
        QCOMPARE(applySpy.at(0).at(0).toBool(), true);
        QVERIFY(applySpy.at(0).at(1).toString().isEmpty());
        QVERIFY(workspaceChangeSawRestoredCamera);
        QCOMPARE(sequence,
                 QStringList({QStringLiteral("prepare"),
                              QStringLiteral("renderer-commit"),
                              QStringLiteral("reference-update"),
                              QStringLiteral("selection-update"),
                              QStringLiteral("store-list"),
                              QStringLiteral("store-load:view_010"),
                              QStringLiteral("restore:tie-winner")}));
    }

    void noUuidSkipsStoreAndRestoreWithoutFailingImport()
    {
        WorkspaceState state;
        FakeMeshImportService importer(staged(
            {entry(1, QStringLiteral("reference_gt.obj")),
             entry(2, QStringLiteral("candidate.obj"))}));
        FakeRendererAdapter renderer;
        renderer.setCamera(pose(QStringLiteral("default-camera")));
        FakeSurfaceComparer comparer;
        FakeCameraPoseStore store;
        WorkspaceController controller(state, importer, renderer, comparer, store);

        const WorkspaceImportOutcome outcome = importWorkspace(controller);

        QVERIFY2(outcome.result.ok, qPrintable(outcome.result.error));
        QVERIFY(outcome.notice.contains(NoUuidNotice));
        QCOMPARE(state.phase(), WorkspacePhase::Ready);
        QCOMPARE(store.listCount(), 0);
        QCOMPARE(store.loadCount(), 0);
        QCOMPARE(renderer.restoreCount(), 0);
        QCOMPARE(renderer.camera().viewStateXml, QStringLiteral("default-camera"));

        const CameraPanelSnapshot snapshot = controller.cameraPanelSnapshot();
        QVERIFY(snapshot.result.ok);
        QVERIFY(snapshot.workspaceUuid.isEmpty());
        QVERIFY(snapshot.poses.isEmpty());
        QCOMPARE(store.listCount(), 0);
        QVERIFY(!controller.saveCurrentCameraPose().ok);
    }

    void missingUidSuggestsMlpFilenameAndAcceptsManualUidForSaving()
    {
        WorkspaceState state;
        FakeMeshImportService importer(staged(
            {entry(1, QStringLiteral("reference_gt.obj")),
             entry(2, QStringLiteral("candidate.obj"))}));
        FakeRendererAdapter renderer;
        renderer.setCamera(pose(QStringLiteral("manual-camera")));
        FakeSurfaceComparer comparer;
        FakeCameraPoseStore store;
        WorkspaceController controller(state, importer, renderer, comparer, store);

        const WorkspaceImportOutcome outcome = controller.importMeshes(
            {QStringLiteral("/tmp/Comparison_Run-42.mlp")});

        QVERIFY2(outcome.result.ok, qPrintable(outcome.result.error));
        const CameraPanelSnapshot before = controller.cameraPanelSnapshot();
        QVERIFY(before.workspaceUuid.isEmpty());
        QCOMPARE(
            before.suggestedWorkspaceUid,
            QStringLiteral("Comparison_Run-42"));

        const OperationResult selected =
            controller.setCameraPoseUid(QStringLiteral("  Manual-2048  "));
        QVERIFY2(selected.ok, qPrintable(selected.error));
        const OperationResult saved = controller.saveCurrentCameraPose();

        QVERIFY2(saved.ok, qPrintable(saved.error));
        QCOMPARE(
            controller.cameraPanelSnapshot().workspaceUuid,
            QStringLiteral("manual-2048"));
        QCOMPARE(store.lastSaveUuid(), QStringLiteral("manual-2048"));
        QCOMPARE(
            store.lastSavedPose().viewStateXml,
            QStringLiteral("manual-camera"));
    }

    void storeListFailureIsANonFatalImportNotice()
    {
        WorkspaceState state;
        FakeMeshImportService importer(staged(
            {entry(1, QStringLiteral("reference_gt.obj"), {UuidA}),
             entry(2, QStringLiteral("candidate.obj"))}));
        FakeRendererAdapter renderer;
        renderer.setCamera(pose(QStringLiteral("default-camera")));
        FakeSurfaceComparer comparer;
        FakeCameraPoseStore store;
        store.failNextList(QStringLiteral("library unreadable"));
        WorkspaceController controller(state, importer, renderer, comparer, store);
        QSignalSpy applySpy(
            &controller,
            &WorkspaceController::cameraApplyFinished);

        const WorkspaceImportOutcome outcome = importWorkspace(controller);

        QVERIFY(outcome.result.ok);
        QVERIFY(outcome.notice.contains(QStringLiteral("library unreadable")));
        QCOMPARE(state.phase(), WorkspacePhase::Ready);
        QCOMPARE(renderer.camera().viewStateXml, QStringLiteral("default-camera"));
        QCOMPARE(renderer.restoreCount(), 0);
        QCOMPARE(applySpy.size(), 1);
        QCOMPARE(applySpy.at(0).at(0).toBool(), false);
        QVERIFY(applySpy.at(0).at(1).toString().contains(
            QStringLiteral("library unreadable")));
    }

    void storeLoadFailureIsANonFatalImportNotice()
    {
        WorkspaceState state;
        FakeMeshImportService importer(staged(
            {entry(1, QStringLiteral("reference_gt.obj"), {UuidA}),
             entry(2, QStringLiteral("candidate.obj"))}));
        FakeRendererAdapter renderer;
        renderer.setCamera(pose(QStringLiteral("default-camera")));
        FakeSurfaceComparer comparer;
        FakeCameraPoseStore store;
        store.add(UuidA, QStringLiteral("view_001"), pose(QStringLiteral("saved")),
                  QStringLiteral("2026-07-15T01:00:00Z"));
        store.failNextLoad(QStringLiteral("pose disappeared"));
        WorkspaceController controller(state, importer, renderer, comparer, store);
        QSignalSpy applySpy(
            &controller,
            &WorkspaceController::cameraApplyFinished);

        const WorkspaceImportOutcome outcome = importWorkspace(controller);

        QVERIFY(outcome.result.ok);
        QVERIFY(outcome.notice.contains(QStringLiteral("pose disappeared")));
        QCOMPARE(state.phase(), WorkspacePhase::Ready);
        QCOMPARE(renderer.camera().viewStateXml, QStringLiteral("default-camera"));
        QCOMPARE(renderer.restoreCount(), 0);
        QCOMPARE(applySpy.size(), 1);
        QCOMPARE(applySpy.at(0).at(0).toBool(), false);
        QVERIFY(applySpy.at(0).at(1).toString().contains(
            QStringLiteral("pose disappeared")));
    }

    void rendererRestoreFailureIsANonFatalImportNoticeAndIsAtomic()
    {
        WorkspaceState state;
        FakeMeshImportService importer(staged(
            {entry(1, QStringLiteral("reference_gt.obj"), {UuidA}),
             entry(2, QStringLiteral("candidate.obj"))}));
        FakeRendererAdapter renderer;
        renderer.setCamera(pose(QStringLiteral("default-camera")));
        renderer.failNextRestore(QStringLiteral("invalid camera XML"));
        FakeSurfaceComparer comparer;
        FakeCameraPoseStore store;
        store.add(UuidA, QStringLiteral("view_001"), pose(QStringLiteral("saved")),
                  QStringLiteral("2026-07-15T01:00:00Z"));
        WorkspaceController controller(state, importer, renderer, comparer, store);
        QSignalSpy applySpy(
            &controller,
            &WorkspaceController::cameraApplyFinished);

        const WorkspaceImportOutcome outcome = importWorkspace(controller);

        QVERIFY(outcome.result.ok);
        QVERIFY(outcome.notice.contains(QStringLiteral("invalid camera XML")));
        QCOMPARE(state.phase(), WorkspacePhase::Ready);
        QCOMPARE(renderer.camera().viewStateXml, QStringLiteral("default-camera"));
        QCOMPARE(renderer.restoreCount(), 1);
        QCOMPARE(applySpy.size(), 1);
        QCOMPARE(applySpy.at(0).at(0).toBool(), false);
        QVERIFY(applySpy.at(0).at(1).toString().contains(
            QStringLiteral("invalid camera XML")));
    }

    void cameraCommandsAreUuidScopedAndPersistImmediately()
    {
        WorkspaceState state;
        FakeMeshImportService importer(staged(
            {entry(1, QStringLiteral("reference_gt.obj"), {UuidA}),
             entry(2, QStringLiteral("candidate.obj"), {UuidB})}));
        FakeRendererAdapter renderer;
        FakeSurfaceComparer comparer;
        FakeCameraPoseStore store;
        WorkspaceController controller(state, importer, renderer, comparer, store);
        QVERIFY(importWorkspace(controller).result.ok);
        store.add(UuidA, QStringLiteral("view_A"), pose(QStringLiteral("saved-A")),
                  QStringLiteral("2026-07-15T01:00:00Z"));
        store.add(UuidB, QStringLiteral("view_B"), pose(QStringLiteral("saved-B")),
                  QStringLiteral("2026-07-15T02:00:00Z"));
        store.resetObservations();

        renderer.setCamera(pose(QStringLiteral("captured-current")));
        QString savedViewId;
        const OperationResult saved = controller.saveCurrentCameraPose(&savedViewId);

        QVERIFY2(saved.ok, qPrintable(saved.error));
        QCOMPARE(savedViewId, QStringLiteral("view_001"));
        QCOMPARE(store.lastSaveUuid(), UuidA);
        QCOMPARE(store.lastSavedPose().viewStateXml,
                 QStringLiteral("captured-current"));

        const CameraPanelSnapshot snapshot = controller.cameraPanelSnapshot();
        QVERIFY2(snapshot.result.ok, qPrintable(snapshot.result.error));
        QCOMPARE(snapshot.workspaceUuid, UuidA);
        QCOMPARE(snapshot.poses.size(), 2);
        QCOMPARE(store.lastListUuid(), UuidA);
        for (const CameraPoseSummary& summary : snapshot.poses)
            QVERIFY(summary.viewId != QStringLiteral("view_B"));

        const OperationResult applied =
            controller.applyCameraPose(QStringLiteral("view_A"));
        QVERIFY2(applied.ok, qPrintable(applied.error));
        QCOMPARE(store.lastLoadUuid(), UuidA);
        QCOMPARE(store.lastLoadViewId(), QStringLiteral("view_A"));
        QCOMPARE(renderer.camera().viewStateXml, QStringLiteral("saved-A"));

        const OperationResult deleted = controller.deleteCameraPose(savedViewId);
        QVERIFY2(deleted.ok, qPrintable(deleted.error));
        QCOMPARE(store.lastRemoveUuid(), UuidA);
        QCOMPARE(store.lastRemoveViewId(), savedViewId);
        QCOMPARE(controller.cameraPanelSnapshot().poses.size(), 1);
    }

    void saveWithScreenshotCapturesTheViewportAndPersistsTheCurrentPose()
    {
        WorkspaceState state;
        FakeMeshImportService importer(staged(
            {entry(1, QStringLiteral("reference_gt.obj"), {UuidA}),
             entry(2, QStringLiteral("candidate.obj"))}));
        FakeRendererAdapter renderer;
        FakeSurfaceComparer comparer;
        FakeCameraPoseStore store;
        WorkspaceController controller(state, importer, renderer, comparer, store);
        QVERIFY(importWorkspace(controller).result.ok);
        renderer.setCamera(pose(QStringLiteral("captured-with-screenshot")));
        QImage expected(5, 3, QImage::Format_RGB32);
        expected.fill(QColor(72, 140, 211));
        renderer.setCaptureImage(expected);

        QImage screenshot;
        QString savedViewId;
        const OperationResult saved =
            controller.saveCurrentCameraPoseWithScreenshot(
                screenshot,
                &savedViewId);

        QVERIFY2(saved.ok, qPrintable(saved.error));
        QCOMPARE(savedViewId, QStringLiteral("view_001"));
        QCOMPARE(renderer.captureImageCount(), 1);
        QCOMPARE(screenshot, expected);
        QCOMPARE(store.lastSaveUuid(), UuidA);
        QCOMPARE(
            store.lastSavedPose().viewStateXml,
            QStringLiteral("captured-with-screenshot"));
    }

    void screenshotCaptureFailureDoesNotPersistOrMutateOutputs()
    {
        WorkspaceState state;
        FakeMeshImportService importer(staged(
            {entry(1, QStringLiteral("reference_gt.obj"), {UuidA}),
             entry(2, QStringLiteral("candidate.obj"))}));
        FakeRendererAdapter renderer;
        FakeSurfaceComparer comparer;
        FakeCameraPoseStore store;
        WorkspaceController controller(state, importer, renderer, comparer, store);
        QVERIFY(importWorkspace(controller).result.ok);
        store.resetObservations();
        renderer.failNextCaptureImage(QStringLiteral("framebuffer unavailable"));
        QSignalSpy saveSpy(
            &controller,
            &WorkspaceController::cameraSaveFinished);
        QImage screenshot(2, 2, QImage::Format_RGB32);
        screenshot.fill(Qt::magenta);
        const QImage unchangedScreenshot = screenshot;
        QString savedViewId = QStringLiteral("unchanged");

        const OperationResult saved =
            controller.saveCurrentCameraPoseWithScreenshot(
                screenshot,
                &savedViewId);

        QVERIFY(!saved.ok);
        QCOMPARE(saved.error, QStringLiteral("framebuffer unavailable"));
        QCOMPARE(renderer.captureImageCount(), 1);
        QCOMPARE(store.saveCount(), 0);
        QCOMPARE(screenshot, unchangedScreenshot);
        QCOMPARE(savedViewId, QStringLiteral("unchanged"));
        QCOMPARE(saveSpy.size(), 1);
        QCOMPARE(saveSpy.at(0).at(0).toBool(), false);
        QCOMPARE(
            saveSpy.at(0).at(1).toString(),
            QStringLiteral("framebuffer unavailable"));
    }

    void cameraSaveAndApplyPublishDiagnosticsOutcomes()
    {
        WorkspaceState state;
        FakeMeshImportService importer(staged(
            {entry(1, QStringLiteral("reference_gt.obj"), {UuidA}),
             entry(2, QStringLiteral("candidate.obj"))}));
        FakeRendererAdapter renderer;
        FakeSurfaceComparer comparer;
        FakeCameraPoseStore store;
        WorkspaceController controller(state, importer, renderer, comparer, store);
        QVERIFY(importWorkspace(controller).result.ok);
        QSignalSpy saveSpy(
            &controller,
            &WorkspaceController::cameraSaveFinished);
        QSignalSpy applySpy(
            &controller,
            &WorkspaceController::cameraApplyFinished);

        renderer.setCamera(pose(QStringLiteral("captured")));
        QVERIFY(controller.saveCurrentCameraPose().ok);
        store.failNextLoad(QStringLiteral("saved pose is unreadable"));
        QVERIFY(!controller.applyCameraPose(QStringLiteral("view_001")).ok);

        QCOMPARE(saveSpy.size(), 1);
        QCOMPARE(saveSpy.at(0).at(0).toBool(), true);
        QVERIFY(saveSpy.at(0).at(1).toString().isEmpty());
        QCOMPARE(applySpy.size(), 1);
        QCOMPARE(applySpy.at(0).at(0).toBool(), false);
        QCOMPARE(
            applySpy.at(0).at(1).toString(),
            QStringLiteral("saved pose is unreadable"));
    }

    void cameraCommandFailuresDoNotClaimSuccessOrMutateOutputs()
    {
        WorkspaceState state;
        FakeMeshImportService importer(staged(
            {entry(1, QStringLiteral("reference_gt.obj"), {UuidA}),
             entry(2, QStringLiteral("candidate.obj"))}));
        FakeRendererAdapter renderer;
        FakeSurfaceComparer comparer;
        FakeCameraPoseStore store;
        WorkspaceController controller(state, importer, renderer, comparer, store);
        QVERIFY(importWorkspace(controller).result.ok);
        store.add(UuidA, QStringLiteral("view_001"), pose(QStringLiteral("saved")),
                  QStringLiteral("2026-07-15T01:00:00Z"));
        store.resetObservations();
        renderer.setCamera(pose(QStringLiteral("current")));

        store.failNextList(QStringLiteral("list failed"));
        const CameraPanelSnapshot snapshot = controller.cameraPanelSnapshot();
        QVERIFY(!snapshot.result.ok);
        QCOMPARE(snapshot.result.error, QStringLiteral("list failed"));
        QCOMPARE(snapshot.workspaceUuid, UuidA);
        QVERIFY(snapshot.poses.isEmpty());

        QString savedViewId = QStringLiteral("unchanged");
        store.failNextSave(QStringLiteral("save failed"));
        const OperationResult saved =
            controller.saveCurrentCameraPose(&savedViewId);
        QVERIFY(!saved.ok);
        QCOMPARE(saved.error, QStringLiteral("save failed"));
        QCOMPARE(savedViewId, QStringLiteral("unchanged"));

        store.failNextLoad(QStringLiteral("load failed"));
        const OperationResult loaded =
            controller.applyCameraPose(QStringLiteral("view_001"));
        QVERIFY(!loaded.ok);
        QCOMPARE(loaded.error, QStringLiteral("load failed"));
        QCOMPARE(renderer.camera().viewStateXml, QStringLiteral("current"));

        renderer.failNextRestore(QStringLiteral("restore failed"));
        const OperationResult restored =
            controller.applyCameraPose(QStringLiteral("view_001"));
        QVERIFY(!restored.ok);
        QCOMPARE(restored.error, QStringLiteral("restore failed"));
        QCOMPARE(renderer.camera().viewStateXml, QStringLiteral("current"));

        store.failNextRemove(QStringLiteral("delete failed"));
        const OperationResult removed =
            controller.deleteCameraPose(QStringLiteral("view_001"));
        QVERIFY(!removed.ok);
        QCOMPARE(removed.error, QStringLiteral("delete failed"));

        QVERIFY(state.beginAnalysis().ok);
        QVERIFY(!controller.saveCurrentCameraPose().ok);
        QVERIFY(!controller.applyCameraPose(QStringLiteral("view_001")).ok);
        QVERIFY(!controller.deleteCameraPose(QStringLiteral("view_001")).ok);
    }

    void referenceChangeRecomputesUuidWithoutAutomaticRestore()
    {
        WorkspaceState state;
        FakeMeshImportService importer(staged(
            {entry(1, QStringLiteral("reference_gt.obj"), {UuidA}),
             entry(2, QStringLiteral("candidate.obj"), {UuidB})}));
        FakeRendererAdapter renderer;
        FakeSurfaceComparer comparer;
        FakeCameraPoseStore store;
        store.add(UuidB, QStringLiteral("view_B"), pose(QStringLiteral("saved-B")),
                  QStringLiteral("2026-07-15T02:00:00Z"));
        WorkspaceController controller(state, importer, renderer, comparer, store);
        QVERIFY(importWorkspace(controller).result.ok);
        QCOMPARE(renderer.restoreCount(), 0);

        const OperationResult changed = controller.setReference(2);

        QVERIFY2(changed.ok, qPrintable(changed.error));
        QCOMPARE(renderer.restoreCount(), 0);
        const CameraPanelSnapshot snapshot = controller.cameraPanelSnapshot();
        QCOMPARE(snapshot.workspaceUuid, UuidB);
        QCOMPARE(snapshot.poses.size(), 1);
        QCOMPARE(snapshot.poses.front().viewId, QStringLiteral("view_B"));
    }

    void failedReplacementPreservesUuidAndSuccessfulNoUuidReplacementClearsIt()
    {
        WorkspaceState state;
        FakeMeshImportService importer;
        importer.enqueue(staged(
            {entry(1, QStringLiteral("reference_gt.obj"), {UuidA}),
             entry(2, QStringLiteral("candidate.obj"))}));
        importer.enqueue(staged(
            {entry(3, QStringLiteral("replacement_gt.obj")),
             entry(4, QStringLiteral("replacement.obj"))}));
        importer.enqueue(staged(
            {entry(5, QStringLiteral("replacement_gt.obj")),
             entry(6, QStringLiteral("replacement.obj"))}));
        FakeRendererAdapter renderer;
        FakeSurfaceComparer comparer;
        FakeCameraPoseStore store;
        WorkspaceController controller(state, importer, renderer, comparer, store);
        QVERIFY(importWorkspace(controller).result.ok);
        QCOMPARE(controller.cameraPanelSnapshot().workspaceUuid, UuidA);

        renderer.failNextPrepare(QStringLiteral("replacement failed"));
        const WorkspaceImportOutcome failed = importWorkspace(controller);

        QVERIFY(!failed.result.ok);
        QCOMPARE(controller.cameraPanelSnapshot().workspaceUuid, UuidA);

        const WorkspaceImportOutcome replaced = importWorkspace(controller);

        QVERIFY(replaced.result.ok);
        QVERIFY(replaced.notice.contains(NoUuidNotice));
        QVERIFY(controller.cameraPanelSnapshot().workspaceUuid.isEmpty());
    }

    void autoRestoreRejectsSnapshotReentryDuringStoreList()
    {
        WorkspaceState state;
        FakeMeshImportService importer(staged(
            {entry(1, QStringLiteral("reference_gt.obj"), {UuidA}),
             entry(2, QStringLiteral("candidate.obj"))}));
        FakeRendererAdapter renderer;
        FakeSurfaceComparer comparer;
        FakeCameraPoseStore store;
        store.add(UuidA, QStringLiteral("view_001"), pose(QStringLiteral("saved")),
                  QStringLiteral("2026-07-15T01:00:00Z"));
        WorkspaceController controller(state, importer, renderer, comparer, store);
        CameraPanelSnapshot nestedSnapshot;
        bool nesting = false;
        store.setListObserver([&] {
            if (nesting)
                return;
            nesting = true;
            nestedSnapshot = controller.cameraPanelSnapshot();
            nesting = false;
        });

        const WorkspaceImportOutcome outcome = importWorkspace(controller);

        QVERIFY(outcome.result.ok);
        QVERIFY(!nestedSnapshot.result.ok);
        QVERIFY(nestedSnapshot.result.error.contains(
            QStringLiteral("progress"), Qt::CaseInsensitive));
        QCOMPARE(store.listCount(), 1);
        QCOMPARE(renderer.camera().viewStateXml, QStringLiteral("saved"));
    }

    void cameraCommandsRejectReentryDuringRestoreAndStoreCalls()
    {
        WorkspaceState state;
        FakeMeshImportService importer(staged(
            {entry(1, QStringLiteral("reference_gt.obj"), {UuidA}),
             entry(2, QStringLiteral("candidate.obj"))}));
        FakeRendererAdapter renderer;
        FakeSurfaceComparer comparer;
        FakeCameraPoseStore store;
        store.add(UuidA, QStringLiteral("view_001"), pose(QStringLiteral("saved")),
                  QStringLiteral("2026-07-15T01:00:00Z"));
        WorkspaceController controller(state, importer, renderer, comparer, store);
        OperationResult nestedDuringRestore;
        renderer.setRestoreObserver([&] {
            nestedDuringRestore = controller.saveCurrentCameraPose();
        });

        QVERIFY(importWorkspace(controller).result.ok);
        QVERIFY(!nestedDuringRestore.ok);
        QVERIFY(nestedDuringRestore.error.contains(
            QStringLiteral("replacement"), Qt::CaseInsensitive));

        OperationResult nestedDuringSave;
        store.setSaveObserver([&] {
            nestedDuringSave =
                controller.deleteCameraPose(QStringLiteral("view_001"));
        });
        renderer.setCamera(pose(QStringLiteral("current")));
        const OperationResult saved = controller.saveCurrentCameraPose();

        QVERIFY2(saved.ok, qPrintable(saved.error));
        QVERIFY(!nestedDuringSave.ok);
        QVERIFY(nestedDuringSave.error.contains(
            QStringLiteral("progress"), Qt::CaseInsensitive));
    }

    void cameraStoreCallbacksCannotMutateOrReplaceTheWorkspace()
    {
        WorkspaceState state;
        FakeMeshImportService importer;
        importer.enqueue(staged(
            {entry(1, QStringLiteral("reference_gt.obj"), {UuidA}),
             entry(2, QStringLiteral("candidate.obj"), {UuidB})}));
        importer.enqueue(staged(
            {entry(3, QStringLiteral("replacement_gt.obj"), {UuidC}),
             entry(4, QStringLiteral("replacement.obj"))}));
        FakeRendererAdapter renderer;
        FakeSurfaceComparer comparer;
        FakeCameraPoseStore store;
        WorkspaceController controller(state, importer, renderer, comparer, store);
        QVERIFY(importWorkspace(controller).result.ok);
        renderer.setCamera(pose(QStringLiteral("captured")));
        WorkspaceImportOutcome nestedImport;
        store.setSaveObserver([&] {
            nestedImport = importWorkspace(controller);
        });

        const OperationResult saved = controller.saveCurrentCameraPose();

        QVERIFY2(saved.ok, qPrintable(saved.error));
        QVERIFY(!nestedImport.result.ok);
        QVERIFY(nestedImport.result.error.contains(
            QStringLiteral("camera"), Qt::CaseInsensitive));
        QCOMPARE(state.referenceId(), MeshId(1));
        QCOMPARE(controller.cameraPanelSnapshot().workspaceUuid, UuidA);
        QCOMPARE(store.lastSaveUuid(), UuidA);

        OperationResult nestedReference;
        store.setListObserver([&] {
            nestedReference = controller.setReference(2);
        });

        const CameraPanelSnapshot snapshot = controller.cameraPanelSnapshot();

        QVERIFY(snapshot.result.ok);
        QVERIFY(!nestedReference.ok);
        QVERIFY(nestedReference.error.contains(
            QStringLiteral("camera"), Qt::CaseInsensitive));
        QCOMPARE(state.referenceId(), MeshId(1));
        QCOMPARE(snapshot.workspaceUuid, UuidA);
    }
};

QTEST_MAIN(CameraWorkflowTest)
#include "camera_workflow_test.moc"
