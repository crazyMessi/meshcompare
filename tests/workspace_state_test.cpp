#include <QtTest>

#include <type_traits>

#include "core/reference_resolver.h"
#include "core/workspace_state.h"

class WorkspaceStateTest : public QObject
{
    Q_OBJECT

private:
    static MeshEntry makeEntry(MeshId id)
    {
        return {id,
                id + 100,
                QStringLiteral("/tmp/mesh-%1.obj").arg(id),
                QStringLiteral("mesh-%1.obj").arg(id),
                {},
                false};
    }

private slots:
    void gtWinsCaseInsensitively()
    {
        QVector<MeshEntry> meshes = {
            {1, 101, "/tmp/result.obj", "result.obj", {}, false},
            {2, 102, "/tmp/Scene_GT.ply", "Scene_GT.ply", {}, false}
        };

        const ReferenceResolution result = resolveReference(meshes);

        QCOMPARE(result.referenceId, MeshId(2));
        QCOMPARE(result.notice, QString());
    }

    void firstMeshIsFallbackWithNotice()
    {
        QVector<MeshEntry> meshes = {
            {1, 101, "/tmp/a.obj", "a.obj", {}, false},
            {2, 102, "/tmp/b.obj", "b.obj", {}, false}
        };

        const ReferenceResolution result = resolveReference(meshes);

        QCOMPARE(result.referenceId, MeshId(1));
        QCOMPARE(result.notice, QStringLiteral("No gt mesh found; using the first import."));
    }

    void firstGtMatchWinsWhenMultipleMatchesExist()
    {
        QVector<MeshEntry> meshes = {
            {1, 101, "/tmp/GT-a.obj", "GT-a.obj", {}, false},
            {2, 102, "/tmp/gt-b.obj", "gt-b.obj", {}, false}
        };

        const ReferenceResolution result = resolveReference(meshes);

        QCOMPARE(result.referenceId, MeshId(1));
        QCOMPARE(result.notice,
                 QStringLiteral("Multiple gt meshes found; using the first import."));
    }

    void gtReferenceResolutionIgnoresGtInAnotherMeshParentDirectory()
    {
        QVector<MeshEntry> meshes = {
            {1,
             101,
             "/cache/normalized/s2_norm/model_s2.ply",
             "s2",
             {},
             false},
            {2,
             102,
             "/cache/normalized/hy_norm/model_hy.ply",
             "hy",
             {},
             false},
            {3,
             103,
             "/cache/normalized/gt_norm/model_gt_norm.ply",
             "gt",
             {},
             false},
            {4,
             104,
             "/cache/output/model_gt_norm/mesh.ply",
             "current_current",
             {},
             false},
        };

        const ReferenceResolution result = resolveReference(meshes);

        QCOMPARE(result.referenceId, MeshId(3));
        QCOMPARE(result.notice, QString());
    }

    void emptyReferenceResolutionHasNoReferenceOrNotice()
    {
        const ReferenceResolution result = resolveReference({});

        QCOMPARE(result.referenceId, MeshId(0));
        QCOMPARE(result.notice, QString());
    }

    void replacingWorkspaceIncrementsGeneration()
    {
        WorkspaceState state;
        state.beginLoading();
        QCOMPARE(state.commitWorkspace({makeEntry(1), makeEntry(2)}, 1).ok, true);
        QCOMPARE(state.phase(), WorkspacePhase::Ready);
        QCOMPARE(state.generation(), quint64(1));

        state.beginLoading();
        QCOMPARE(state.commitWorkspace({makeEntry(3), makeEntry(4)}, 3).ok, true);
        QCOMPARE(state.generation(), quint64(2));
        QCOMPARE(state.selectedMeshId(), MeshId(3));
    }

    void committingWorkspaceMarksOnlyReferenceAndResetsScores()
    {
        MeshEntry first = makeEntry(1);
        first.presentation.mode = ColorMode::PrecisionResult;
        first.score = 0.91;
        first.hasScore = true;
        MeshEntry second = makeEntry(2);
        second.presentation.mode = ColorMode::UniformColor;
        second.score = 0.77;
        second.hasScore = true;

        WorkspaceState state;
        state.beginLoading();
        QCOMPARE(state.commitWorkspace({first, second}, 2).ok, true);

        QCOMPARE(state.referenceId(), MeshId(2));
        QCOMPARE(state.selectedMeshId(), MeshId(1));
        QCOMPARE(state.meshes().at(0).isReference, false);
        QCOMPARE(state.meshes().at(1).isReference, true);
        QCOMPARE(state.meshes().at(0).score, 0.0);
        QCOMPARE(state.meshes().at(1).score, 0.0);
        QVERIFY(!state.meshes().at(0).hasScore);
        QVERIFY(!state.meshes().at(1).hasScore);
    }

    void committingWorkspaceRetainsUuidCandidates()
    {
        MeshEntry first = makeEntry(1);
        first.uuidCandidates = QStringList({QStringLiteral("uuid-a"), QStringLiteral("uuid-b")});

        WorkspaceState state;
        state.beginLoading();
        QVERIFY(state.commitWorkspace({first, makeEntry(2)}, 1).ok);

        QCOMPARE(state.meshes().at(0).uuidCandidates,
                 QStringList({QStringLiteral("uuid-a"), QStringLiteral("uuid-b")}));
    }

    void commitRejectsTooFewOrTooManyMeshes()
    {
        WorkspaceState state;
        state.beginLoading();

        const OperationResult tooFew = state.commitWorkspace({makeEntry(1)}, 1);
        QVERIFY(!tooFew.ok);
        QVERIFY(!tooFew.error.isEmpty());
        QCOMPARE(state.generation(), quint64(0));
        QCOMPARE(state.meshes().size(), 0);

        QVector<MeshEntry> tooMany;
        for (MeshId id = 1; id <= 9; ++id)
            tooMany.push_back(makeEntry(id));
        const OperationResult tooManyResult = state.commitWorkspace(tooMany, 1);
        QVERIFY(!tooManyResult.ok);
        QVERIFY(!tooManyResult.error.isEmpty());
        QCOMPARE(state.generation(), quint64(0));
        QCOMPARE(state.meshes().size(), 0);
    }

    void commitRejectsDuplicateMeshIdsWithoutChangingWorkspace()
    {
        WorkspaceState state;
        state.beginLoading();
        QVERIFY(state.commitWorkspace({makeEntry(1), makeEntry(2)}, 1).ok);

        state.beginLoading();
        const OperationResult result = state.commitWorkspace({makeEntry(3), makeEntry(3)}, 3);

        QVERIFY(!result.ok);
        QVERIFY(!result.error.isEmpty());
        QCOMPARE(state.phase(), WorkspacePhase::Loading);
        QCOMPARE(state.generation(), quint64(1));
        QCOMPARE(state.referenceId(), MeshId(1));
        QCOMPARE(state.selectedMeshId(), MeshId(1));
        QCOMPARE(state.meshes().size(), 2);
        QCOMPARE(state.meshes().at(0).id, MeshId(1));
        QCOMPARE(state.meshes().at(1).id, MeshId(2));
    }

    void sharedValidationIsNonMutatingAndMatchesCommitRules()
    {
        WorkspaceState state;
        state.beginLoading();
        QVERIFY(state.commitWorkspace({makeEntry(1), makeEntry(2)}, 1).ok);

        const OperationResult result =
            state.validateWorkspace({makeEntry(3), makeEntry(3)}, 3);

        QVERIFY(!result.ok);
        QCOMPARE(result.error, QStringLiteral("Workspace mesh IDs must be unique."));
        QCOMPARE(state.phase(), WorkspacePhase::Ready);
        QCOMPARE(state.generation(), quint64(1));
        QCOMPARE(state.meshes().front().id, MeshId(1));
    }

    void cancellingLoadingRestoresThePreviousUsablePhase()
    {
        WorkspaceState emptyState;
        emptyState.beginLoading();
        emptyState.cancelLoading();
        QCOMPARE(emptyState.phase(), WorkspacePhase::Empty);

        WorkspaceState readyState;
        readyState.beginLoading();
        QVERIFY(readyState.commitWorkspace({makeEntry(1), makeEntry(2)}, 1).ok);
        readyState.beginLoading();
        readyState.cancelLoading();
        QCOMPARE(readyState.phase(), WorkspacePhase::Ready);
        QCOMPARE(readyState.generation(), quint64(1));
    }

    void selectedMeshMutationRejectsUnknownIds()
    {
        WorkspaceState state;
        state.beginLoading();
        QVERIFY(state.commitWorkspace({makeEntry(1), makeEntry(2)}, 1).ok);

        QVERIFY(state.setSelectedMesh(2).ok);
        QCOMPARE(state.selectedMeshId(), MeshId(2));
        const OperationResult unknown = state.setSelectedMesh(99);
        QVERIFY(!unknown.ok);
        QCOMPARE(state.selectedMeshId(), MeshId(2));
    }

    void layerVisibilityKeepsAtLeastOneMeshVisible()
    {
        WorkspaceState state;
        state.beginLoading();
        QVERIFY(state
                    .commitWorkspace(
                        {makeEntry(1), makeEntry(2), makeEntry(3)},
                        1,
                        SceneLayoutMode::Overlay)
                    .ok);
        for (const MeshEntry& mesh : state.meshes())
            QVERIFY(mesh.visible);

        QVERIFY(state.setMeshVisible(2, false).ok);
        QVERIFY(state.setMeshVisible(3, false).ok);
        QVERIFY(!state.mesh(2)->visible);
        QVERIFY(!state.mesh(3)->visible);

        const OperationResult lastVisible =
            state.setMeshVisible(1, false);
        QVERIFY(!lastVisible.ok);
        QVERIFY(state.mesh(1)->visible);
        QVERIFY(!state.setMeshVisible(99, false).ok);
        QVERIFY(state.setMeshVisible(2, true).ok);
        QVERIFY(state.mesh(2)->visible);
    }

    void fatalTransitionEntersFatalPhase()
    {
        WorkspaceState state;

        state.enterFatalError();

        QCOMPARE(state.phase(), WorkspacePhase::FatalError);
    }

    void changingReferencePreservesUniformColorAndClearsAnalyticalModes()
    {
        MeshEntry first = makeEntry(1);
        first.presentation.mode = ColorMode::UniformColor;
        first.presentation.uniformColor = Qt::red;
        first.score = 0.1;
        first.hasScore = true;
        MeshEntry second = makeEntry(2);
        second.presentation.mode = ColorMode::PrecisionResult;
        second.presentation.faceColors = {Qt::green};
        second.score = 0.2;
        second.hasScore = true;
        MeshEntry third = makeEntry(3);
        third.presentation.mode = ColorMode::NormalAgreementResult;
        third.presentation.faceColors = {Qt::blue};
        third.score = 0.3;
        third.hasScore = true;

        WorkspaceState state;
        state.beginLoading();
        QVERIFY(state.commitWorkspace({first, second, third}, 1).ok);

        QCOMPARE(state.setReference(2).ok, true);

        QCOMPARE(state.referenceId(), MeshId(2));
        QCOMPARE(state.meshes().at(0).presentation.mode, ColorMode::UniformColor);
        QCOMPARE(state.meshes().at(0).presentation.uniformColor, QColor(Qt::red));
        QCOMPARE(state.meshes().at(1).presentation.mode, ColorMode::Default);
        QCOMPARE(state.meshes().at(2).presentation.mode, ColorMode::Default);
        QCOMPARE(state.meshes().at(0).score, 0.0);
        QCOMPARE(state.meshes().at(1).score, 0.0);
        QCOMPARE(state.meshes().at(2).score, 0.0);
        QVERIFY(!state.meshes().at(0).hasScore);
        QVERIFY(!state.meshes().at(1).hasScore);
        QVERIFY(!state.meshes().at(2).hasScore);
    }

    void changingReferenceClearsDistanceButPreservesDoubleLayer()
    {
        MeshEntry distance = makeEntry(1);
        distance.presentation.mode = ColorMode::VertexColor;
        distance.presentation.vertexColors = {
            QColor(Qt::red), QColor(Qt::green), QColor(Qt::blue)};
        distance.presentation.referenceDependent = true;
        distance.analysisSummary.kind = AnalysisKind::DistanceToReference;
        distance.analysisSummary.distance.meanDistance = 0.001;
        distance.analysisSummary.distance.percentile99Distance = 0.004;
        distance.analysisSummary.distance.maxDistance = 0.02;

        MeshEntry doubleLayer = makeEntry(2);
        doubleLayer.presentation.mode = ColorMode::VertexColor;
        doubleLayer.presentation.vertexColors = {
            QColor(Qt::gray), QColor(Qt::yellow), QColor(Qt::red)};
        doubleLayer.analysisSummary.kind = AnalysisKind::DoubleLayer;
        doubleLayer.analysisSummary.doubleLayer.sampleCount = 10;
        doubleLayer.analysisSummary.doubleLayer.meanSampleScore = 0.1;
        doubleLayer.analysisSummary.doubleLayer.affectedSampleFraction = 0.2;
        doubleLayer.analysisSummary.doubleLayer.affectedFaceFraction = 0.3;
        doubleLayer.analysisSummary.doubleLayer.affectedVertexFraction = 0.4;

        WorkspaceState state;
        state.beginLoading();
        QVERIFY(state.commitWorkspace({distance, doubleLayer}, 1).ok);

        QVERIFY(state.setReference(2).ok);

        QCOMPARE(state.mesh(1)->presentation.mode, ColorMode::Default);
        QCOMPARE(state.mesh(1)->analysisSummary.kind, AnalysisKind::None);
        QCOMPARE(state.mesh(2)->presentation.mode, ColorMode::VertexColor);
        QCOMPARE(
            state.mesh(2)->analysisSummary.kind,
            AnalysisKind::DoubleLayer);
        QCOMPARE(
            state.mesh(2)->analysisSummary.doubleLayer.affectedFaceFraction,
            0.3);
    }

    void changingReferenceRejectsUnknownMesh()
    {
        WorkspaceState state;
        state.beginLoading();
        QVERIFY(state.commitWorkspace({makeEntry(1), makeEntry(2)}, 1).ok);

        const OperationResult result = state.setReference(3);

        QVERIFY(!result.ok);
        QCOMPARE(result.error,
                 QStringLiteral("Reference mesh does not exist in this workspace."));
        QCOMPARE(state.referenceId(), MeshId(1));
    }

    void analysisTransitionsRequireAReadyWorkspace()
    {
        WorkspaceState state;

        const OperationResult empty = state.beginAnalysis();
        QVERIFY(!empty.ok);
        QCOMPARE(state.phase(), WorkspacePhase::Empty);

        state.beginLoading();
        QVERIFY(state.commitWorkspace({makeEntry(1), makeEntry(2)}, 1).ok);
        QVERIFY(state.beginAnalysis().ok);
        QCOMPARE(state.phase(), WorkspacePhase::Analyzing);

        const OperationResult duplicate = state.beginAnalysis();
        QVERIFY(!duplicate.ok);
        QCOMPARE(state.phase(), WorkspacePhase::Analyzing);

        state.finishAnalysis();
        QCOMPARE(state.phase(), WorkspacePhase::Ready);
    }

    void meshLookupIsConstAndUnknownIdsReturnNull()
    {
        WorkspaceState state;
        state.beginLoading();
        QVERIFY(state.commitWorkspace({makeEntry(1), makeEntry(2)}, 1).ok);

        const WorkspaceState& constState = state;
        QVERIFY(constState.mesh(2) != nullptr);
        QCOMPARE(constState.mesh(2)->resourceId, MeshResourceId(102));
        QVERIFY(constState.mesh(99) == nullptr);
    }

    void colorUpdateBatchValidatesThenCommitsAtomically()
    {
        WorkspaceState state;
        state.beginLoading();
        QVERIFY(state.commitWorkspace({makeEntry(1), makeEntry(2), makeEntry(3)}, 1).ok);

        ColorPresentation precision;
        precision.mode = ColorMode::PrecisionResult;
        precision.faceColors = {QColor(Qt::green)};
        ColorPresentation normal;
        normal.mode = ColorMode::NormalAgreementResult;
        normal.faceColors = {QColor(Qt::yellow)};
        const QVector<MeshColorStateUpdate> updates = {
            {2, precision, 0.9, true},
            {3, normal, 0.8, true}};

        PreparedColorStateUpdate prepared;
        QVERIFY(state.prepareColorUpdates(updates, &prepared).ok);
        QVERIFY(prepared.isValid());
        state.commitPreparedColorUpdates(std::move(prepared));
        QCOMPARE(state.mesh(2)->presentation.mode, ColorMode::PrecisionResult);
        QCOMPARE(state.mesh(2)->score, 0.9);
        QVERIFY(state.mesh(2)->hasScore);
        QCOMPARE(state.mesh(3)->presentation.mode, ColorMode::NormalAgreementResult);

        const QVector<MeshColorStateUpdate> duplicate = {
            {2, ColorPresentation{}, 0.0, false},
            {2, ColorPresentation{}, 0.0, false}};
        QVERIFY(!state.validateColorUpdates(duplicate).ok);
        QCOMPARE(state.mesh(2)->presentation.mode, ColorMode::PrecisionResult);
        QCOMPARE(state.mesh(3)->presentation.mode, ColorMode::NormalAgreementResult);

        ColorPresentation invalidMode;
        invalidMode.mode = static_cast<ColorMode>(999);
        QVERIFY(!state.validateColorUpdates({{2, invalidMode, 0.0, false}}).ok);
        ColorPresentation strayDefault;
        strayDefault.faceColors = {QColor(Qt::red)};
        QVERIFY(!state.validateColorUpdates({{2, strayDefault, 0.0, false}}).ok);
        ColorPresentation strayAnalytical = precision;
        strayAnalytical.uniformColor = QColor(Qt::blue);
        QVERIFY(!state.validateColorUpdates({{2, strayAnalytical, 0.9, true}}).ok);
        QVERIFY(!state.validateColorUpdates({{2, precision, 1.01, true}}).ok);
    }

    void vertexColorUpdateValidatesAndCommitsAtomically()
    {
        WorkspaceState state;
        state.beginLoading();
        QVERIFY(state.commitWorkspace({makeEntry(1), makeEntry(2)}, 1).ok);

        ColorPresentation vertexAnalysis;
        vertexAnalysis.mode = ColorMode::PrecisionResult;
        vertexAnalysis.vertexColors = {
            QColor(Qt::red), QColor(Qt::green), QColor(Qt::blue)};
        PreparedColorStateUpdate prepared;
        QVERIFY(state
                    .prepareColorUpdates(
                        {{2, vertexAnalysis, 0.75, true}},
                        &prepared)
                    .ok);
        state.commitPreparedColorUpdates(std::move(prepared));
        QCOMPARE(
            state.mesh(2)->presentation.vertexColors,
            vertexAnalysis.vertexColors);

        ColorPresentation bothDomains = vertexAnalysis;
        bothDomains.faceColors = {QColor(Qt::yellow)};
        QVERIFY(!state
                     .validateColorUpdates(
                         {{2, bothDomains, 0.75, true}})
                     .ok);

        ColorPresentation invalidVertex = vertexAnalysis;
        invalidVertex.vertexColors[1] = QColor();
        QVERIFY(!state
                     .validateColorUpdates(
                         {{2, invalidVertex, 0.75, true}})
                     .ok);

        ColorPresentation strayDefault;
        strayDefault.vertexColors = {QColor(Qt::red)};
        QVERIFY(!state
                     .validateColorUpdates(
                         {{2, strayDefault, 0.0, false}})
                     .ok);

        ColorPresentation strayUniform;
        strayUniform.mode = ColorMode::UniformColor;
        strayUniform.uniformColor = QColor(Qt::white);
        strayUniform.vertexColors = {QColor(Qt::red)};
        QVERIFY(!state
                     .validateColorUpdates(
                         {{2, strayUniform, 0.0, false}})
                     .ok);
    }

    void typedVertexAnalysisSummaryMustMatchItsPresentation()
    {
        WorkspaceState state;
        state.beginLoading();
        QVERIFY(state.commitWorkspace({makeEntry(1), makeEntry(2)}, 1).ok);

        ColorPresentation vertexPresentation;
        vertexPresentation.mode = ColorMode::VertexColor;
        vertexPresentation.vertexColors = {
            QColor(Qt::red), QColor(Qt::green), QColor(Qt::blue)};
        vertexPresentation.referenceDependent = true;
        vertexPresentation.colorLegend.kind =
            ColorLegendKind::Distance;
        vertexPresentation.colorLegend.distanceMapping =
            DistanceColorMapping::SquareRoot;
        vertexPresentation.colorLegend.minimum = 0.0;
        vertexPresentation.colorLegend.maximum = 0.04;
        AnalysisSummary distance;
        distance.kind = AnalysisKind::DistanceToReference;
        distance.distance.vertexCount = 3;
        distance.distance.finiteVertexCount = 3;
        distance.distance.meanDistance = 0.001;
        distance.distance.percentile99Distance = 0.004;
        distance.distance.maxDistance = 0.02;
        distance.distance.aboveThresholdVertexFraction = 0.25;

        PreparedColorStateUpdate prepared;
        QVERIFY(state
                    .prepareColorUpdates(
                        {{2,
                          vertexPresentation,
                          0.0,
                          false,
                          distance}},
                        &prepared)
                    .ok);
        state.commitPreparedColorUpdates(std::move(prepared));
        QCOMPARE(
            state.mesh(2)->analysisSummary.kind,
            AnalysisKind::DistanceToReference);
        QCOMPARE(
            state.mesh(2)->analysisSummary.distance.percentile99Distance,
            0.004);
        QCOMPARE(
            state.mesh(2)
                ->analysisSummary.distance.aboveThresholdVertexFraction,
            0.25);
        QCOMPARE(
            state.mesh(2)->presentation.colorLegend.maximum,
            0.04);

        ColorPresentation invalidLegend = vertexPresentation;
        invalidLegend.colorLegend.distanceMapping =
            static_cast<DistanceColorMapping>(99);
        QVERIFY(!state
                     .validateColorUpdates(
                         {{2,
                           invalidLegend,
                           0.0,
                           false,
                           distance}})
                     .ok);

        AnalysisSummary invalidDistance = distance;
        invalidDistance.distance.maxDistance = -1.0;
        QVERIFY(!state
                     .validateColorUpdates(
                         {{2,
                           vertexPresentation,
                           0.0,
                           false,
                           invalidDistance}})
                     .ok);

        invalidDistance = distance;
        invalidDistance.distance.aboveThresholdVertexFraction = 1.01;
        QVERIFY(!state
                     .validateColorUpdates(
                         {{2,
                           vertexPresentation,
                           0.0,
                           false,
                           invalidDistance}})
                     .ok);

        ColorPresentation uniform;
        uniform.mode = ColorMode::UniformColor;
        uniform.uniformColor = QColor(Qt::white);
        QVERIFY(!state
                     .validateColorUpdates(
                         {{2, uniform, 0.0, false, distance}})
                     .ok);

        AnalysisSummary doubleLayer;
        doubleLayer.kind = AnalysisKind::DoubleLayer;
        doubleLayer.doubleLayer.sampleCount = 10;
        doubleLayer.doubleLayer.affectedFaceFraction = 1.1;
        QVERIFY(!state
                     .validateColorUpdates(
                         {{2,
                           vertexPresentation,
                           0.0,
                           false,
                           doubleLayer}})
                     .ok);
    }

    void clearingColoringResetsAllPresentationsAndScoresAtOnce()
    {
        WorkspaceState state;
        state.beginLoading();
        QVERIFY(state.commitWorkspace({makeEntry(1), makeEntry(2)}, 1).ok);
        ColorPresentation uniform;
        uniform.mode = ColorMode::UniformColor;
        uniform.uniformColor = QColor(Qt::red);
        ColorPresentation precision;
        precision.mode = ColorMode::PrecisionResult;
        precision.faceColors = {QColor(Qt::green)};
        const QVector<MeshColorStateUpdate> updates = {
            {1, uniform, 0.0, false},
            {2, precision, 0.9, true}};
        PreparedColorStateUpdate prepared;
        QVERIFY(state.prepareColorUpdates(updates, &prepared).ok);
        state.commitPreparedColorUpdates(std::move(prepared));
        QVERIFY(state.mesh(1)->presentation.mode != ColorMode::Default);
        QVERIFY(state.mesh(2)->hasScore);

        const QVector<MeshColorStateUpdate> clearUpdates = {
            {1, ColorPresentation{}, 0.0, false},
            {2, ColorPresentation{}, 0.0, false}};
        QVERIFY(state.prepareColorUpdates(clearUpdates, &prepared).ok);
        state.commitPreparedColorUpdates(std::move(prepared));

        for (const MeshEntry& mesh : state.meshes()) {
            QCOMPARE(mesh.presentation.mode, ColorMode::Default);
            QVERIFY(!mesh.hasScore);
            QCOMPARE(mesh.score, 0.0);
        }
    }

    void preparedColorUpdateIsSingleUseAndInvalidOrStaleCommitsAreNoOps()
    {
        WorkspaceState state;
        state.beginLoading();
        QVERIFY(state.commitWorkspace({makeEntry(1), makeEntry(2)}, 1).ok);
        ColorPresentation uniform;
        uniform.mode = ColorMode::UniformColor;
        uniform.uniformColor = QColor(Qt::red);

        PreparedColorStateUpdate prepared;
        QVERIFY(state.prepareColorUpdates({{2, uniform, 0.0, false}}, &prepared).ok);
        PreparedColorStateUpdate moved(std::move(prepared));
        QVERIFY(!prepared.isValid());
        QVERIFY(moved.isValid());

        state.commitPreparedColorUpdates(std::move(prepared));
        QCOMPARE(state.mesh(2)->presentation.mode, ColorMode::Default);
        state.commitPreparedColorUpdates(std::move(moved));
        QCOMPARE(state.mesh(2)->presentation.mode, ColorMode::UniformColor);
        QVERIFY(!moved.isValid());

        PreparedColorStateUpdate invalid;
        state.commitPreparedColorUpdates(std::move(invalid));
        QCOMPARE(state.mesh(2)->presentation.mode, ColorMode::UniformColor);

        PreparedColorStateUpdate stale;
        QVERIFY(state.prepareColorUpdates({{2, ColorPresentation{}, 0.0, false}}, &stale).ok);
        state.beginLoading();
        QVERIFY(state.commitWorkspace({makeEntry(3), makeEntry(4)}, 3).ok);
        state.commitPreparedColorUpdates(std::move(stale));
        QCOMPARE(state.generation(), quint64(2));
        QCOMPARE(state.meshes().size(), 2);
        QCOMPARE(state.meshes().front().id, MeshId(3));
        QCOMPARE(state.meshes().back().id, MeshId(4));
    }
};

static_assert(!std::is_copy_constructible<PreparedColorStateUpdate>::value, "prepared transaction must be move-only");
static_assert(!std::is_copy_assignable<PreparedColorStateUpdate>::value, "prepared transaction must be move-only");

QTEST_GUILESS_MAIN(WorkspaceStateTest)
#include "workspace_state_test.moc"
