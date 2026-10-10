#include <QtTest>

#include <initializer_list>
#include <memory>
#include <type_traits>

#include <QSet>
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
    const QVector<MeshEntry>& sourceEntries,
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

StagedWorkspace stagedOpenCube()
{
    std::unique_ptr<MeshLabMeshRepository> repository(
        new MeshLabMeshRepository);
    MeshModel* mesh = repository->allocateMesh(
        QStringLiteral("/tmp/open-cube.obj"),
        QStringLiteral("open-cube.obj"));
    const CMeshO::CoordType positions[] = {
        CMeshO::CoordType(0, 0, 0),
        CMeshO::CoordType(1, 0, 0),
        CMeshO::CoordType(1, 1, 0),
        CMeshO::CoordType(0, 1, 0),
        CMeshO::CoordType(0, 0, 1),
        CMeshO::CoordType(1, 0, 1),
        CMeshO::CoordType(1, 1, 1),
        CMeshO::CoordType(0, 1, 1),
    };
    std::vector<CMeshO::VertexPointer> vertices(8);
    CMeshO::VertexIterator vertex =
        vcg::tri::Allocator<CMeshO>::AddVertices(mesh->cm, 8);
    for (int index = 0; index < 8; ++index, ++vertex) {
        vertices[static_cast<std::size_t>(index)] = &*vertex;
        vertex->P() = positions[index];
    }
    const std::array<int, 3> faces[] = {
        {{0, 3, 2}},
        {{0, 2, 1}},
        {{0, 1, 5}},
        {{0, 5, 4}},
        {{1, 2, 6}},
        {{1, 6, 5}},
        {{2, 3, 7}},
        {{2, 7, 6}},
        {{3, 0, 4}},
        {{3, 4, 7}},
    };
    CMeshO::FaceIterator face =
        vcg::tri::Allocator<CMeshO>::AddFaces(mesh->cm, 10);
    for (const std::array<int, 3>& indices : faces) {
        face->V(0) = vertices[static_cast<std::size_t>(indices[0])];
        face->V(1) = vertices[static_cast<std::size_t>(indices[1])];
        face->V(2) = vertices[static_cast<std::size_t>(indices[2])];
        ++face;
    }
    mesh->updateDataMask();

    MeshEntry source = entry(1, QStringLiteral("open-cube.obj"));
    source.resourceId = repository->resourceIdFor(*mesh);
    return {OperationResult::success(),
            std::move(repository),
            {source},
            {},
            SceneLayoutMode::Overlay};
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

ColorPresentation vertexPresentation(const QColor& color)
{
    ColorPresentation presentation;
    presentation.mode = ColorMode::VertexColor;
    presentation.vertexColors = {color, color, color};
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
    void importsASingleMeshAsAReadyWorkspace()
    {
        WorkspaceState state;
        FakeMeshImportService importer(
            staged({entry(1, QStringLiteral("single.obj"))}));
        FakeRendererAdapter renderer;
        FakeSurfaceComparer comparer;
        WorkspaceController controller(
            state, importer, renderer, comparer, cameraStore_);

        const WorkspaceImportOutcome outcome =
            controller.importMeshes({QStringLiteral("single.obj")});

        QVERIFY2(outcome.result.ok, qPrintable(outcome.result.error));
        QCOMPARE(state.phase(), WorkspacePhase::Ready);
        QCOMPARE(state.meshes().size(), 1);
        QCOMPARE(state.referenceId(), MeshId(1));
        QCOMPARE(renderer.lastPreparedScene().meshes.size(), 1);
    }

    void repeatedImportsAppendLayersWithFreshIds()
    {
        WorkspaceState state;
        FakeMeshImportService importer;
        importer.enqueue(staged({entry(7), entry(42)}));
        importer.enqueue(staged({entry(1), entry(2)}));
        importer.enqueue(staged({entry(1)}));
        FakeRendererAdapter renderer;
        FakeSurfaceComparer comparer;
        WorkspaceController controller(
            state, importer, renderer, comparer, cameraStore_);
        QVERIFY(controller.importMeshes({QStringLiteral("original.obj")})
                    .result.ok);
        const MeshResourceId firstResource = state.mesh(7)->resourceId;
        const MeshResourceId secondResource = state.mesh(42)->resourceId;

        QVERIFY(controller.importMeshes({QStringLiteral("additional.obj")})
                    .result.ok);
        QVERIFY(controller.importMeshes({QStringLiteral("additional.obj")})
                    .result.ok);

        QCOMPARE(state.generation(), quint64(3));
        QCOMPARE(state.meshes().size(), 5);
        const QVector<MeshId> expectedIds = {7, 42, 43, 44, 45};
        QSet<MeshResourceId> resources;
        for (int index = 0; index < expectedIds.size(); ++index) {
            QCOMPARE(state.meshes().at(index).id, expectedIds.at(index));
            resources.insert(state.meshes().at(index).resourceId);
            QVERIFY(state.meshes().at(index).visible);
        }
        QCOMPARE(resources.size(), 5);
        QCOMPARE(state.mesh(7)->resourceId, firstResource);
        QCOMPARE(state.mesh(42)->resourceId, secondResource);
        QCOMPARE(state.mesh(43)->displayName, QStringLiteral("mesh-1.obj"));
        QCOMPARE(state.mesh(45)->displayName, QStringLiteral("mesh-1.obj"));
        QCOMPARE(renderer.lastPreparedScene().meshes.size(), 5);
        QCOMPARE(renderer.committedGeneration(), quint64(3));
    }

    void appendPreservesCurrentWorkspaceSettings_data()
    {
        QTest::addColumn<bool>("overlay");
        QTest::newRow("grid") << false;
        QTest::newRow("overlay") << true;
    }

    void appendPreservesCurrentWorkspaceSettings()
    {
        QFETCH(bool, overlay);
        const SceneLayoutMode layout = overlay
            ? SceneLayoutMode::Overlay : SceneLayoutMode::ComparisonGrid;
        MeshEntry reference = entry(2, QStringLiteral("reference_gt.obj"));
        reference.uuidCandidates = QStringList{QStringLiteral("original-run")};
        MeshEntry incoming = entry(1, QStringLiteral("new_gt.obj"));
        incoming.uuidCandidates = QStringList{QStringLiteral("different-run")};
        WorkspaceState state;
        FakeMeshImportService importer;
        importer.enqueue(staged({entry(1), reference, entry(3), entry(4)}, layout));
        importer.enqueue(staged(
            {incoming}, overlay
                ? SceneLayoutMode::ComparisonGrid : SceneLayoutMode::Overlay));
        FakeRendererAdapter renderer;
        FakeSurfaceComparer comparer;
        WorkspaceController controller(
            state, importer, renderer, comparer, cameraStore_);
        QVERIFY(controller.importMeshes({QStringLiteral("original.mlp")})
                    .result.ok);
        QVERIFY(controller.selectMesh(3).ok);
        QVERIFY(controller.setMeshVisible(1, false).ok);
        if (!overlay)
            QVERIFY(controller.setGridNormalizationEnabled(true).ok);
        QVERIFY(controller.setCameraPoseUid(QStringLiteral("chosen-run")).ok);
        const CameraPanelSnapshot cameraBefore = controller.cameraPanelSnapshot();
        const QColor uniformColor(QStringLiteral("#3d7eff"));
        QVERIFY(controller.setUniformColor(1, uniformColor).ok);
        const ColorPresentation analytical = analyticalPresentation(
            ColorMode::PrecisionResult, QColor(QStringLiteral("#42d66b")));
        ColorPresentation distance = vertexPresentation(
            QColor(QStringLiteral("#f79a31")));
        distance.referenceDependent = true;
        AnalysisSummary summary;
        summary.kind = AnalysisKind::DistanceToReference;
        summary.distance.vertexCount = 3;
        summary.distance.finiteVertexCount = 3;
        summary.distance.meanDistance = 0.125;
        summary.distance.maxDistance = 0.25;
        summary.distance.aboveThresholdVertexFraction = 0.25;
        const OperationResult installed = installPresentations(
            state, renderer,
            {{3, analytical, 0.942, true}, {4, distance, 0.0, false, summary}});
        QVERIFY2(installed.ok, qPrintable(installed.error));
        QVERIFY(renderer.setAnalysisOverlays(
                    {{4, QStringLiteral("> threshold 25.0%")}}).ok);
        renderer.setCamera({QStringLiteral("<current-camera/>")});
        const int restoreCount = renderer.restoreCount();
        const QVector<MeshEntry> before = state.meshes();

        const WorkspaceImportOutcome outcome =
            controller.importMeshes({QStringLiteral("new.mlp")});

        QVERIFY2(outcome.result.ok, qPrintable(outcome.result.error));
        QVERIFY(outcome.notice.isEmpty());
        QCOMPARE(state.meshes().size(), 5);
        QCOMPARE(state.referenceId(), MeshId(2));
        QCOMPARE(state.selectedMeshId(), MeshId(3));
        QCOMPARE(state.layoutMode(), layout);
        QCOMPARE(state.gridNormalizationEnabled(), !overlay);
        for (int index = 0; index < before.size(); ++index) {
            const MeshEntry& previous = before.at(index);
            const MeshEntry& current = state.meshes().at(index);
            QCOMPARE(current.id, previous.id);
            QCOMPARE(current.resourceId, previous.resourceId);
            QCOMPARE(current.sourcePath, previous.sourcePath);
            QCOMPARE(current.displayName, previous.displayName);
            QCOMPARE(current.uuidCandidates, previous.uuidCandidates);
            QCOMPARE(current.isReference, previous.isReference);
            QCOMPARE(current.visible, previous.visible);
            QCOMPARE(current.presentation.mode, previous.presentation.mode);
            QCOMPARE(current.presentation.uniformColor,
                     previous.presentation.uniformColor);
            QCOMPARE(current.presentation.faceColors,
                     previous.presentation.faceColors);
            QCOMPARE(current.presentation.vertexColors,
                     previous.presentation.vertexColors);
            QCOMPARE(current.presentation.referenceDependent,
                     previous.presentation.referenceDependent);
            QCOMPARE(current.score, previous.score);
            QCOMPARE(current.hasScore, previous.hasScore);
        }
        QCOMPARE(state.mesh(4)->analysisSummary.kind, summary.kind);
        QCOMPARE(state.mesh(4)->analysisSummary.distance.meanDistance, 0.125);
        QCOMPARE(renderer.analysisOverlay(4), QStringLiteral("> threshold 25.0%"));
        QCOMPARE(renderer.presentation(1).uniformColor, uniformColor);
        QCOMPARE(renderer.presentation(3).faceColors, analytical.faceColors);
        QCOMPARE(renderer.referenceMeshId(), MeshId(2));
        QCOMPARE(renderer.selectedMeshId(), MeshId(3));
        QCOMPARE(renderer.lastPreparedScene().layoutMode, layout);
        QCOMPARE(renderer.lastPreparedScene().normalizeGridMeshes, !overlay);
        QCOMPARE(renderer.lastPreparedScene().initialCamera.viewStateXml,
                 QStringLiteral("<current-camera/>"));
        QCOMPARE(renderer.camera().viewStateXml,
                 QStringLiteral("<current-camera/>"));
        QCOMPARE(renderer.restoreCount(), restoreCount);
        const CameraPanelSnapshot cameraAfter = controller.cameraPanelSnapshot();
        QCOMPARE(cameraAfter.workspaceUuid, cameraBefore.workspaceUuid);
        QCOMPARE(cameraAfter.suggestedWorkspaceUid,
                 cameraBefore.suggestedWorkspaceUid);
        QVERIFY(state.mesh(5)->visible);
        QVERIFY(!state.mesh(5)->isReference);
    }

    void appendFillsOnlyAvailableVisibleSlots_data()
    {
        QTest::addColumn<bool>("overlay");
        QTest::addColumn<int>("oldCount");
        QTest::addColumn<int>("oldVisibleCount");
        QTest::addColumn<int>("addedCount");
        for (bool overlay : {false, true}) {
            const QByteArray prefix = overlay ? "overlay-" : "grid-";
            QTest::newRow((prefix + "below-limit").constData())
                << overlay << 3 << 3 << 2;
            QTest::newRow((prefix + "crosses-limit").constData())
                << overlay << 7 << 7 << 4;
            QTest::newRow((prefix + "already-full").constData())
                << overlay << 8 << 8 << 2;
            QTest::newRow((prefix + "existing-hidden-layers").constData())
                << overlay << 12 << 6 << 4;
        }
    }

    void appendFillsOnlyAvailableVisibleSlots()
    {
        QFETCH(bool, overlay);
        QFETCH(int, oldCount);
        QFETCH(int, oldVisibleCount);
        QFETCH(int, addedCount);
        QVector<MeshEntry> existing;
        for (int index = 0; index < oldCount; ++index) {
            MeshEntry mesh = entry(index + 1);
            mesh.visible = index < oldVisibleCount;
            existing.append(mesh);
        }
        QVector<MeshEntry> incoming;
        for (int index = 0; index < addedCount; ++index)
            incoming.append(entry(index + 1));
        WorkspaceState state;
        FakeMeshImportService importer;
        importer.enqueue(staged(existing, overlay
            ? SceneLayoutMode::Overlay : SceneLayoutMode::ComparisonGrid));
        importer.enqueue(staged(incoming));
        FakeRendererAdapter renderer;
        FakeSurfaceComparer comparer;
        WorkspaceController controller(
            state, importer, renderer, comparer, cameraStore_);
        QVERIFY(controller.importMeshes({QStringLiteral("existing.glb")})
                    .result.ok);

        const WorkspaceImportOutcome outcome =
            controller.importMeshes({QStringLiteral("additional.glb")});

        QVERIFY2(outcome.result.ok, qPrintable(outcome.result.error));
        QCOMPARE(state.meshes().size(), oldCount + addedCount);
        QCOMPARE(renderer.lastPreparedScene().meshes.size(), oldCount + addedCount);
        for (int index = 0; index < oldCount; ++index) {
            QCOMPARE(state.meshes().at(index).visible, index < oldVisibleCount);
            QCOMPARE(renderer.meshVisible(index + 1), index < oldVisibleCount);
        }
        const int available = meshcompare::MaxVisibleMeshLayers - oldVisibleCount;
        for (int index = 0; index < addedCount; ++index) {
            const MeshId id = oldCount + index + 1;
            QCOMPARE(state.mesh(id)->visible, index < available);
            QCOMPARE(renderer.meshVisible(id), index < available);
        }
        if (addedCount > available) {
            const MeshId lastAdded = oldCount + addedCount;
            QVERIFY(!controller.setMeshVisible(lastAdded, true).ok);
            QVERIFY(controller.setMeshVisible(1, false).ok);
            QVERIFY(controller.setMeshVisible(lastAdded, true).ok);
        }
    }

    void holeFillingCanCreateANewReconstructedLayer()
    {
        WorkspaceState state;
        FakeMeshImportService importer(stagedOpenCube());
        FakeRendererAdapter renderer;
        FakeSurfaceComparer comparer;
        WorkspaceController controller(
            state, importer, renderer, comparer, cameraStore_);
        QVERIFY(controller.importMeshes({QStringLiteral("open-cube.obj")})
                    .result.ok);

        controllable_hole_filling::FillRequest request;
        request.meshId = 1;
        request.config.resolution = 32;
        request.config.cclIterations = 2;
        request.outputMode =
            controllable_hole_filling::OutputMode::NewLayer;
        controllable_hole_filling::FillSummary summary;

        const OperationResult result =
            controller.fillHoles(request, &summary);

        QVERIFY2(result.ok, qPrintable(result.error));
        QCOMPARE(state.generation(), quint64(2));
        QCOMPARE(state.meshes().size(), 2);
        QCOMPARE(state.selectedMeshId(), MeshId(2));
        QCOMPARE(state.meshes().back().displayName,
                 QStringLiteral("open-cube.obj Reconstructed"));
        QVERIFY(summary.vertexCount > 0);
        QVERIFY(summary.faceCount > 0);
        QVERIFY(summary.activeCellCount > 0);
        QCOMPARE(renderer.lastPreparedScene().generation, quint64(2));
        QCOMPARE(renderer.lastPreparedScene().meshes.size(), 2);
        QCOMPARE(renderer.selectedMeshId(), MeshId(2));
    }

    void holeFillingCanReplaceTheCurrentMeshResource()
    {
        WorkspaceState state;
        FakeMeshImportService importer(stagedOpenCube());
        FakeRendererAdapter renderer;
        FakeSurfaceComparer comparer;
        WorkspaceController controller(
            state, importer, renderer, comparer, cameraStore_);
        QVERIFY(controller.importMeshes({QStringLiteral("open-cube.obj")})
                    .result.ok);
        const MeshResourceId originalResource =
            state.meshes().front().resourceId;

        controllable_hole_filling::FillRequest request;
        request.meshId = 1;
        request.config.resolution = 32;
        request.config.cclIterations = 2;
        request.outputMode =
            controllable_hole_filling::OutputMode::CurrentMesh;

        const OperationResult result =
            controller.fillHoles(request);

        QVERIFY2(result.ok, qPrintable(result.error));
        QCOMPARE(state.generation(), quint64(2));
        QCOMPARE(state.meshes().size(), 1);
        QCOMPARE(state.selectedMeshId(), MeshId(1));
        QVERIFY(state.meshes().front().resourceId != originalResource);
        QCOMPARE(
            renderer.lastPreparedScene().meshes.front().resourceId,
            state.meshes().front().resourceId);
    }

    void failedHoleFillingScenePreparationPreservesTheCurrentMesh()
    {
        WorkspaceState state;
        FakeMeshImportService importer(stagedOpenCube());
        FakeRendererAdapter renderer;
        FakeSurfaceComparer comparer;
        WorkspaceController controller(
            state, importer, renderer, comparer, cameraStore_);
        QVERIFY(controller.importMeshes({QStringLiteral("open-cube.obj")})
                    .result.ok);
        const MeshResourceId originalResource =
            state.meshes().front().resourceId;
        renderer.failNextPrepare(QStringLiteral("GPU upload failed"));

        controllable_hole_filling::FillRequest request;
        request.meshId = 1;
        request.config.resolution = 32;
        request.config.cclIterations = 2;
        request.outputMode =
            controllable_hole_filling::OutputMode::CurrentMesh;

        const OperationResult result =
            controller.fillHoles(request);

        QVERIFY(!result.ok);
        QCOMPARE(result.error, QStringLiteral("GPU upload failed"));
        QCOMPARE(state.generation(), quint64(1));
        QCOMPARE(state.meshes().size(), 1);
        QCOMPARE(state.meshes().front().resourceId, originalResource);
        QCOMPARE(renderer.committedGeneration(), quint64(1));
    }

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
        const MeshResourceId oldResource = state.mesh(2)->resourceId;
        const QColor oldColor(QStringLiteral("#3d7eff"));
        QVERIFY(controller.setUniformColor(2, oldColor).ok);
        renderer.setCamera({QStringLiteral("<old-camera/>")});
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
        QCOMPARE(state.mesh(2)->resourceId, oldResource);
        QCOMPARE(state.mesh(2)->presentation.uniformColor, oldColor);
        QCOMPARE(renderer.presentation(2).uniformColor, oldColor);
        QCOMPARE(renderer.camera().viewStateXml, QStringLiteral("<old-camera/>"));
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
        QCOMPARE(state.layoutMode(), SceneLayoutMode::Overlay);
    }

    void switchesLayoutWithoutLosingCameraOrColorPresentation()
    {
        WorkspaceState state;
        FakeMeshImportService importer(staged(
            {entry(1), entry(2)}, SceneLayoutMode::Overlay));
        FakeRendererAdapter renderer;
        FakeSurfaceComparer comparer;
        WorkspaceController controller(
            state, importer, renderer, comparer, cameraStore_);
        QVERIFY(controller.importMeshes({QStringLiteral("comparison.mlp")})
                    .result.ok);
        const QColor color(QStringLiteral("#3d7eff"));
        QVERIFY(controller.setUniformColor(2, color).ok);
        renderer.setCamera({QStringLiteral("<camera-state/>")});
        const int previousPrepareCount = renderer.prepareCount();
        QSignalSpy changedSpy(
            &controller, &WorkspaceController::workspaceChanged);

        const OperationResult result =
            controller.setLayoutMode(SceneLayoutMode::ComparisonGrid);

        QVERIFY2(result.ok, qPrintable(result.error));
        QCOMPARE(renderer.prepareCount(), previousPrepareCount + 1);
        QCOMPARE(
            renderer.lastPreparedScene().layoutMode,
            SceneLayoutMode::ComparisonGrid);
        QCOMPARE(
            renderer.lastPreparedScene().initialCamera.viewStateXml,
            QStringLiteral("<camera-state/>"));
        QCOMPARE(
            renderer.lastPreparedScene().meshes.at(1).presentation.mode,
            ColorMode::UniformColor);
        QCOMPARE(
            renderer.lastPreparedScene().meshes.at(1).presentation.uniformColor,
            color);
        QCOMPARE(state.layoutMode(), SceneLayoutMode::ComparisonGrid);
        QCOMPARE(renderer.camera().viewStateXml, QStringLiteral("<camera-state/>"));
        QCOMPARE(changedSpy.size(), 1);
    }

    void failedLayoutPreparationKeepsTheCurrentLayout()
    {
        WorkspaceState state;
        FakeMeshImportService importer(staged(
            {entry(1), entry(2)}, SceneLayoutMode::Overlay));
        FakeRendererAdapter renderer;
        FakeSurfaceComparer comparer;
        WorkspaceController controller(
            state, importer, renderer, comparer, cameraStore_);
        QVERIFY(controller.importMeshes({QStringLiteral("comparison.mlp")})
                    .result.ok);
        renderer.failNextPrepare(QStringLiteral("layout preparation failed"));
        QSignalSpy changedSpy(
            &controller, &WorkspaceController::workspaceChanged);

        const OperationResult result =
            controller.setLayoutMode(SceneLayoutMode::ComparisonGrid);

        QVERIFY(!result.ok);
        QCOMPARE(result.error, QStringLiteral("layout preparation failed"));
        QCOMPARE(state.layoutMode(), SceneLayoutMode::Overlay);
        QCOMPARE(renderer.committedGeneration(), state.generation());
        QCOMPARE(changedSpy.size(), 0);
    }

    void gridNormalizationRebuildsAtomicallyAndPreservesTheCamera()
    {
        WorkspaceState state;
        FakeMeshImportService importer(staged({entry(1), entry(2)}));
        FakeRendererAdapter renderer;
        FakeSurfaceComparer comparer;
        WorkspaceController controller(
            state, importer, renderer, comparer, cameraStore_);
        QVERIFY(controller.importMeshes(
                    {QStringLiteral("a.obj"), QStringLiteral("b.obj")})
                    .result.ok);
        renderer.setCamera({QStringLiteral("<camera-state/>")});
        const int prepareCount = renderer.prepareCount();
        QSignalSpy changedSpy(
            &controller, &WorkspaceController::workspaceChanged);

        const OperationResult enabled =
            controller.setGridNormalizationEnabled(true);

        QVERIFY2(enabled.ok, qPrintable(enabled.error));
        QVERIFY(state.gridNormalizationEnabled());
        QCOMPARE(renderer.prepareCount(), prepareCount + 1);
        QVERIFY(renderer.lastPreparedScene().normalizeGridMeshes);
        QCOMPARE(
            renderer.lastPreparedScene().initialCamera.viewStateXml,
            QStringLiteral("<camera-state/>"));
        QCOMPARE(changedSpy.size(), 1);

        renderer.failNextPrepare(
            QStringLiteral("normalization preparation failed"));
        const OperationResult disabled =
            controller.setGridNormalizationEnabled(false);

        QVERIFY(!disabled.ok);
        QCOMPARE(
            disabled.error,
            QStringLiteral("normalization preparation failed"));
        QVERIFY(state.gridNormalizationEnabled());
        QVERIFY(renderer.lastPreparedScene().normalizeGridMeshes);
        QCOMPARE(changedSpy.size(), 1);
    }

    void gridNormalizationIsRejectedOutsideGridView()
    {
        WorkspaceState state;
        FakeMeshImportService importer(staged(
            {entry(1), entry(2)}, SceneLayoutMode::Overlay));
        FakeRendererAdapter renderer;
        FakeSurfaceComparer comparer;
        WorkspaceController controller(
            state, importer, renderer, comparer, cameraStore_);
        QVERIFY(controller.importMeshes({QStringLiteral("comparison.mlp")})
                    .result.ok);

        const OperationResult result =
            controller.setGridNormalizationEnabled(true);

        QVERIFY(!result.ok);
        QVERIFY(!state.gridNormalizationEnabled());
    }

    void largeWorkspaceVisibilityChanges_data()
    {
        QTest::addColumn<bool>("overlay");
        QTest::newRow("grid") << false;
        QTest::newRow("overlay") << true;
    }

    void largeWorkspaceVisibilityChanges()
    {
        QFETCH(bool, overlay);
        QVector<MeshEntry> entries;
        for (MeshId id = 1; id <= 12; ++id) {
            MeshEntry mesh = entry(id);
            mesh.visible = id <= 8;
            entries.append(mesh);
        }
        WorkspaceState state;
        FakeMeshImportService importer(staged(
            entries, overlay ? SceneLayoutMode::Overlay
                             : SceneLayoutMode::ComparisonGrid));
        FakeRendererAdapter renderer;
        FakeSurfaceComparer comparer;
        WorkspaceController controller(
            state, importer, renderer, comparer, cameraStore_);
        QVERIFY(controller.importMeshes({QStringLiteral("large.glb")}).result.ok);
        QCOMPARE(state.meshes().size(), 12);
        QCOMPARE(renderer.lastPreparedScene().meshes.size(), 12);
        QVERIFY(!state.mesh(12)->visible);
        const int prepares = renderer.prepareCount();
        const int updates = renderer.visibilityUpdateCount();

        QVERIFY(!controller.setMeshVisible(12, true).ok);
        QCOMPARE(renderer.prepareCount(), prepares);
        QCOMPARE(renderer.visibilityUpdateCount(), updates);
        QCOMPARE(state.phase(), WorkspacePhase::Ready);
        QVERIFY(!state.mesh(12)->visible);

        QVERIFY(controller.setMeshVisible(1, false).ok);
        QVERIFY(controller.setMeshVisible(12, true).ok);
        QVERIFY(!state.mesh(1)->visible);
        QVERIFY(state.mesh(12)->visible);
        QCOMPARE(state.referenceId(), MeshId(1));
        const SceneLayoutMode otherMode = overlay
            ? SceneLayoutMode::ComparisonGrid : SceneLayoutMode::Overlay;
        QVERIFY(controller.setLayoutMode(otherMode).ok);
        const SceneDescriptor& scene = renderer.lastPreparedScene();
        QCOMPARE(scene.meshes.size(), 12);
        QVERIFY(!scene.meshes.at(0).visible);
        QVERIFY(scene.meshes.at(11).visible);
        QCOMPARE(state.generation(), quint64(1));
    }

    void overlayVisibilityUpdatesRendererAndPersistsAcrossLayoutChanges()
    {
        WorkspaceState state;
        FakeMeshImportService importer(staged(
            {entry(1), entry(2), entry(3)}, SceneLayoutMode::Overlay));
        FakeRendererAdapter renderer;
        FakeSurfaceComparer comparer;
        WorkspaceController controller(
            state, importer, renderer, comparer, cameraStore_);
        QVERIFY(controller.importMeshes({QStringLiteral("comparison.mlp")})
                    .result.ok);

        QVERIFY(controller.setMeshVisible(2, false).ok);

        QVERIFY(!state.mesh(2)->visible);
        QVERIFY(!renderer.meshVisible(2));
        QCOMPARE(renderer.visibilityUpdateCount(), 1);
        QVERIFY(controller
                    .setLayoutMode(SceneLayoutMode::ComparisonGrid)
                    .ok);
        QVERIFY(!renderer.lastPreparedScene().meshes.at(1).visible);
        QVERIFY(controller.setLayoutMode(SceneLayoutMode::Overlay).ok);
        QVERIFY(!renderer.meshVisible(2));
        QVERIFY(!state.mesh(2)->visible);
    }

    void gridVisibilityRebuildsTheSceneAndKeepsTheHiddenReference()
    {
        WorkspaceState state;
        FakeMeshImportService importer(staged({entry(1), entry(2), entry(3)}));
        FakeRendererAdapter renderer;
        FakeSurfaceComparer comparer;
        WorkspaceController controller(
            state, importer, renderer, comparer, cameraStore_);
        QVERIFY(controller.importMeshes({QStringLiteral("comparison.mlp")})
                    .result.ok);
        renderer.setCamera({QStringLiteral("<grid-camera-state/>")});
        const int previousPrepareCount = renderer.prepareCount();
        QSignalSpy changedSpy(
            &controller, &WorkspaceController::workspaceChanged);

        const OperationResult result = controller.setMeshVisible(1, false);

        QVERIFY2(result.ok, qPrintable(result.error));
        QCOMPARE(renderer.prepareCount(), previousPrepareCount + 1);
        QCOMPARE(
            renderer.lastPreparedScene().initialCamera.viewStateXml,
            QStringLiteral("<grid-camera-state/>"));
        QVERIFY(!renderer.lastPreparedScene().meshes.at(0).visible);
        QVERIFY(renderer.lastPreparedScene().meshes.at(0).isReference);
        QVERIFY(!state.mesh(1)->visible);
        QCOMPARE(state.referenceId(), MeshId(1));
        QCOMPARE(renderer.referenceMeshId(), MeshId(1));
        QCOMPARE(renderer.visibilityUpdateCount(), 0);
        QCOMPARE(changedSpy.size(), 1);
    }

    void failedGridVisibilityPreparationPreservesCurrentScene()
    {
        WorkspaceState state;
        FakeMeshImportService importer(staged({entry(1), entry(2)}));
        FakeRendererAdapter renderer;
        FakeSurfaceComparer comparer;
        WorkspaceController controller(
            state, importer, renderer, comparer, cameraStore_);
        QVERIFY(controller.importMeshes({QStringLiteral("comparison.mlp")})
                    .result.ok);
        renderer.failNextPrepare(QStringLiteral("grid visibility preparation failed"));

        const OperationResult result = controller.setMeshVisible(2, false);

        QVERIFY(!result.ok);
        QCOMPARE(result.error, QStringLiteral("grid visibility preparation failed"));
        QVERIFY(state.mesh(2)->visible);
        QVERIFY(renderer.meshVisible(2));
        QCOMPARE(renderer.committedGeneration(), state.generation());
    }

    void visibilityFailureAndLastVisibleGuardPreserveState()
    {
        WorkspaceState state;
        FakeMeshImportService importer(staged(
            {entry(1), entry(2)}, SceneLayoutMode::Overlay));
        FakeRendererAdapter renderer;
        FakeSurfaceComparer comparer;
        WorkspaceController controller(
            state, importer, renderer, comparer, cameraStore_);
        QVERIFY(controller.importMeshes({QStringLiteral("comparison.mlp")})
                    .result.ok);
        renderer.failNextVisibilityUpdate(
            QStringLiteral("visibility update failed"));

        const OperationResult failed =
            controller.setMeshVisible(2, false);

        QVERIFY(!failed.ok);
        QVERIFY(state.mesh(2)->visible);
        QVERIFY(renderer.meshVisible(2));
        QVERIFY(controller.setMeshVisible(2, false).ok);
        const int attempts = renderer.visibilityAttemptCount();
        const OperationResult lastVisible =
            controller.setMeshVisible(1, false);
        QVERIFY(!lastVisible.ok);
        QVERIFY(state.mesh(1)->visible);
        QCOMPARE(renderer.visibilityAttemptCount(), attempts);
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
                     "No UID was found in this workspace."));
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
        FakeMeshImportService importer;
        importer.enqueue(staged({entry(1), entry(2)}));
        importer.enqueue(staged({entry(1)}));
        FakeRendererAdapter renderer;
        FakeSurfaceComparer comparer;
        QStringList lifecycle;
        renderer.setTrace(&lifecycle);
        {
            WorkspaceController controller(
                state, importer, renderer, comparer, cameraStore_);
            QVERIFY(controller.importMeshes(
                                  {QStringLiteral("a.obj"), QStringLiteral("b.obj")})
                        .result.ok);
            QVERIFY(controller.importMeshes({QStringLiteral("additional.obj")})
                        .result.ok);
            renderer.setClearProbeResource(state.mesh(3)->resourceId);
            lifecycle.clear();
        }

        QCOMPARE(lifecycle,
                 QStringList({QStringLiteral("events-disconnect"),
                              QStringLiteral("clear")}));
        QCOMPARE(renderer.eventDisconnectCount(), 2);
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

    void successfulAppendJoinsOldAnalysisBeforeCommitAndCreatesANewUsableService()
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

        const WorkspaceImportOutcome appended = controller.importMeshes(
            {QStringLiteral("new-a.obj"), QStringLiteral("new-b.obj")});

        QVERIFY2(appended.result.ok, qPrintable(appended.result.error));
        QCOMPARE(state.phase(), WorkspacePhase::Ready);
        QCOMPARE(state.generation(), quint64(2));
        QCOMPARE(state.meshes().size(), 4);
        QCOMPARE(state.meshes().front().id, MeshId(1));
        QCOMPARE(state.meshes().back().id, MeshId(4));
        QCOMPARE(renderer.committedGeneration(), quint64(2));

        comparer.enqueueSuccess(0.75);
        comparer.enqueueSuccess(0.76);
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

    void importGuardRejectsDirectReentrantAnalysisFromCancellationSignal()
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

        const WorkspaceImportOutcome appended = controller.importMeshes(
            {QStringLiteral("bad-a.obj"), QStringLiteral("bad-b.obj")});

        QVERIFY(!appended.result.ok);
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

    void rendererErrorCallbackRejectsReentrantImportUntilColorCommandReturns()
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
        const WorkspaceImportOutcome appended = controller.importMeshes(
            {QStringLiteral("new-a.obj"), QStringLiteral("new-b.obj")});
        QVERIFY2(appended.result.ok, qPrintable(appended.result.error));
        QCOMPARE(state.generation(), quint64(2));
    }

    void analysisProgressCallbackRejectsReentrantWorkspaceImport()
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

        const WorkspaceImportOutcome appended = controller.importMeshes(
            {QStringLiteral("new-a.obj"), QStringLiteral("new-b.obj")});
        QVERIFY2(appended.result.ok, qPrintable(appended.result.error));
        QCOMPARE(state.generation(), quint64(2));
    }

    void analysisFinishedCallbackRejectsReentrantWorkspaceImport()
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

        const WorkspaceImportOutcome appended = controller.importMeshes(
            {QStringLiteral("new-a.obj"), QStringLiteral("new-b.obj")});
        QVERIFY2(appended.result.ok, qPrintable(appended.result.error));
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

    void referenceChangeClearsDistanceAndPreservesDoubleLayerPresentation()
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

        AnalysisSummary distanceSummary;
        distanceSummary.kind = AnalysisKind::DistanceToReference;
        distanceSummary.distance.vertexCount = 3;
        distanceSummary.distance.finiteVertexCount = 3;
        distanceSummary.distance.percentile99Distance = 0.004;
        distanceSummary.distance.maxDistance = 0.004;
        distanceSummary.distance.aboveThresholdVertexFraction = 0.25;
        AnalysisSummary doubleLayerSummary;
        doubleLayerSummary.kind = AnalysisKind::DoubleLayer;
        doubleLayerSummary.doubleLayer.sampleCount = 500000;
        doubleLayerSummary.doubleLayer.affectedFaceFraction = 0.032;
        ColorPresentation distance =
            vertexPresentation(QColor(QStringLiteral("#21918c")));
        distance.referenceDependent = true;
        const ColorPresentation doubleLayer =
            vertexPresentation(QColor(QStringLiteral("#ff9030")));
        const OperationResult installed = installPresentations(
            state,
            renderer,
            {{2, distance, 0.0, false, distanceSummary},
             {3, doubleLayer, 0.0, false, doubleLayerSummary}});
        QVERIFY2(installed.ok, qPrintable(installed.error));
        const int presentationAttempts = renderer.presentationAttemptCount();

        const OperationResult changed = controller.setReference(2);

        QVERIFY2(changed.ok, qPrintable(changed.error));
        QCOMPARE(renderer.presentationAttemptCount(), presentationAttempts + 1);
        QCOMPARE(renderer.lastPresentationBatch().size(), 1);
        QCOMPARE(renderer.lastPresentationBatch().front().meshId, MeshId(2));
        QCOMPARE(
            renderer.lastPresentationBatch().front().presentation.mode,
            ColorMode::Default);
        QCOMPARE(state.mesh(2)->presentation.mode, ColorMode::Default);
        QCOMPARE(
            state.mesh(2)->analysisSummary.kind,
            AnalysisKind::None);
        QCOMPARE(
            state.mesh(3)->presentation.mode,
            ColorMode::VertexColor);
        QCOMPARE(
            state.mesh(3)->analysisSummary.kind,
            AnalysisKind::DoubleLayer);
        QCOMPARE(renderer.presentation(3).mode, ColorMode::VertexColor);
    }

    void analyticalSummariesProduceMetricSpecificViewportLabels()
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

        AnalysisSummary distanceSummary;
        distanceSummary.kind = AnalysisKind::DistanceToReference;
        distanceSummary.distance.vertexCount = 3;
        distanceSummary.distance.finiteVertexCount = 3;
        distanceSummary.distance.percentile99Distance = 0.004;
        distanceSummary.distance.maxDistance = 0.004;
        distanceSummary.distance.aboveThresholdVertexFraction = 0.25;
        AnalysisSummary doubleLayerSummary;
        doubleLayerSummary.kind = AnalysisKind::DoubleLayer;
        doubleLayerSummary.doubleLayer.sampleCount = 500000;
        doubleLayerSummary.doubleLayer.affectedFaceFraction = 0.032;
        ColorPresentation distance =
            vertexPresentation(QColor(QStringLiteral("#21918c")));
        distance.referenceDependent = true;
        const OperationResult installed = installPresentations(
            state,
            renderer,
            {{2,
              distance,
              0.0,
              false,
              distanceSummary},
             {3,
              vertexPresentation(QColor(QStringLiteral("#ff9030"))),
              0.0,
              false,
              doubleLayerSummary}});
        QVERIFY2(installed.ok, qPrintable(installed.error));

        const OperationResult switched =
            controller.setLayoutMode(SceneLayoutMode::Overlay);

        QVERIFY2(switched.ok, qPrintable(switched.error));
        QCOMPARE(
            renderer.lastPreparedScene().meshes.at(1).analysisLabel,
            QStringLiteral("> threshold 25.0%"));
        QCOMPARE(
            renderer.lastPreparedScene().meshes.at(2).analysisLabel,
            QStringLiteral("DL 3.2%"));
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

    void doubleLayerAnalysisTargetsEveryMeshIncludingTheReference()
    {
        WorkspaceState state;
        FakeMeshImportService importer(
            staged({entry(1), entry(2), entry(3)}));
        FakeRendererAdapter renderer;
        FakeSurfaceComparer comparer;
        comparer.enqueueSuccess(0.0);
        comparer.pauseNextComparison();
        WorkspaceController controller(
            state, importer, renderer, comparer, cameraStore_);
        QVERIFY(controller.importMeshes(
                              {QStringLiteral("a.obj"),
                               QStringLiteral("b.obj"),
                               QStringLiteral("c.obj")})
                    .result.ok);
        SurfaceComparisonOptions options;
        options.sampleCount = 10;

        const OperationResult started = controller.startAnalysis(
            SurfaceComparisonMetric::DoubleLayer, options);

        QVERIFY2(started.ok, qPrintable(started.error));
        const QVector<MeshAnalysisOverlayUpdate> targets =
            renderer.lastOverlayBatch();
        QCOMPARE(targets.size(), 3);
        QCOMPARE(targets.at(0).meshId, MeshId(1));
        QCOMPARE(targets.at(1).meshId, MeshId(2));
        QCOMPARE(targets.at(2).meshId, MeshId(3));
        controller.cancelAnalysis();
    }

    void distanceAnalysisTargetsOnlyNonReferenceMeshes()
    {
        WorkspaceState state;
        FakeMeshImportService importer(
            staged({entry(1), entry(2), entry(3)}));
        FakeRendererAdapter renderer;
        FakeSurfaceComparer comparer;
        comparer.enqueueSuccess(0.0);
        comparer.pauseNextComparison();
        WorkspaceController controller(
            state, importer, renderer, comparer, cameraStore_);
        QVERIFY(controller.importMeshes(
                              {QStringLiteral("a.obj"),
                               QStringLiteral("b.obj"),
                               QStringLiteral("c.obj")})
                    .result.ok);
        SurfaceComparisonOptions options;

        const OperationResult started = controller.startAnalysis(
            SurfaceComparisonMetric::DistanceToReference, options);

        QVERIFY2(started.ok, qPrintable(started.error));
        const QVector<MeshAnalysisOverlayUpdate> targets =
            renderer.lastOverlayBatch();
        QCOMPARE(targets.size(), 2);
        QCOMPARE(targets.at(0).meshId, MeshId(2));
        QCOMPARE(targets.at(1).meshId, MeshId(3));
        controller.cancelAnalysis();
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

    void workspaceChangedDirectSubscriberCannotReentrantlyAppendMeshes()
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
        const QStringList additionalPaths = {
            QStringLiteral("new-a.obj"), QStringLiteral("new-b.obj")};
        const QMetaObject::Connection connection = connect(
            &controller,
            &WorkspaceController::workspaceChanged,
            &controller,
            [&] {
                if (attempted)
                    return;
                attempted = true;
                reentrantImport = controller.importMeshes(additionalPaths);
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
        const WorkspaceImportOutcome appended = controller.importMeshes(
            {QStringLiteral("new-a.obj"), QStringLiteral("new-b.obj")});
        QVERIFY2(appended.result.ok, qPrintable(appended.result.error));
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
