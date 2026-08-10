#include <QtTest>

#include <QComboBox>
#include <QDoubleSpinBox>
#include <QPushButton>
#include <QSpinBox>

#include "core/workspace_state.h"
#include "plugins/controllable_hole_filling/python_patch.h"
#include "plugins/controllable_hole_filling/python_patch_panel.h"

namespace
{

using namespace python_hole_filling;

class MemoryGeometry final : public IMeshGeometryView
{
public:
    QVector<MeshPoint3D> vertices;
    QVector<std::array<int, 3>> faces;

    int vertexCount() const override { return vertices.size(); }
    MeshPoint3D vertexPosition(int index) const override
    {
        return vertices.at(index);
    }
    MeshPoint3D vertexNormal(int) const override
    {
        return MeshPoint3D{{0.0, 0.0, 1.0}};
    }
    int faceCount() const override { return faces.size(); }
    std::array<int, 3> faceVertexIndices(int index) const override
    {
        return faces.at(index);
    }
};

MemoryGeometry cubeWithoutTop()
{
    MemoryGeometry geometry;
    geometry.vertices = {
        MeshPoint3D{{0.0, 0.0, 0.0}},
        MeshPoint3D{{1.0, 0.0, 0.0}},
        MeshPoint3D{{1.0, 1.0, 0.0}},
        MeshPoint3D{{0.0, 1.0, 0.0}},
        MeshPoint3D{{0.0, 0.0, 1.0}},
        MeshPoint3D{{1.0, 0.0, 1.0}},
        MeshPoint3D{{1.0, 1.0, 1.0}},
        MeshPoint3D{{0.0, 1.0, 1.0}},
    };
    geometry.faces = {
        std::array<int, 3>{{0, 3, 2}},
        std::array<int, 3>{{0, 2, 1}},
        std::array<int, 3>{{0, 1, 5}},
        std::array<int, 3>{{0, 5, 4}},
        std::array<int, 3>{{1, 2, 6}},
        std::array<int, 3>{{1, 6, 5}},
        std::array<int, 3>{{2, 3, 7}},
        std::array<int, 3>{{2, 7, 6}},
        std::array<int, 3>{{3, 0, 4}},
        std::array<int, 3>{{3, 4, 7}},
    };
    return geometry;
}

MeshEntry entry(MeshId id)
{
    MeshEntry mesh;
    mesh.id = id;
    mesh.resourceId = id + 100;
    mesh.displayName = QStringLiteral("mesh-%1.obj").arg(id);
    return mesh;
}

class RecordingCommands final : public ICommands
{
public:
    OperationResult fillPythonHoles(
        const FillRequest& request,
        FillSummary* summary) override
    {
        lastRequest = request;
        ++callCount;
        if (summary != nullptr) {
            summary->patchedHoleCount = 1;
            summary->patchVertexCount = 512;
            summary->patchFaceCount = 1020;
        }
        return OperationResult::success();
    }

    FillRequest lastRequest;
    int callCount = 0;
};

} // namespace

class PythonHoleFillingTest : public QObject
{
    Q_OBJECT

private slots:
    void planarPatchClosesTheDetectedBoundary()
    {
        FillConfig config;
        config.method = PatchMethod::Planar;
        config.scope = HoleScope::All;
        config.minLoopVertices = 3;

        const FillResult result =
            Engine().fill(cubeWithoutTop(), config);

        QVERIFY2(result.result.ok, qPrintable(result.result.error));
        QCOMPARE(result.boundaryEdgeCount, 4);
        QCOMPARE(result.nonManifoldEdgeCount, 0);
        QCOMPARE(result.closedBoundaryLoopCount, 1);
        QCOMPARE(result.patchedHoleCount, 1);
        QCOMPARE(result.patch.vertices.size(), 4);
        QCOMPARE(result.patch.faces.size(), 2);
        QCOMPARE(result.merged.vertices.size(), 8);
        QCOMPARE(result.merged.faces.size(), 12);
    }

    void hybridPatchHonorsTheRequestedResolution()
    {
        FillConfig config;
        config.method = PatchMethod::Hybrid;
        config.scope = HoleScope::Largest;
        config.minLoopVertices = 3;
        config.targetVertices = 16;
        config.normalWeight = 0.0;
        config.maxTangentRms = 1.0;

        const FillResult result =
            Engine().fill(cubeWithoutTop(), config);

        QVERIFY2(result.result.ok, qPrintable(result.result.error));
        QCOMPARE(result.patch.vertices.size(), 16);
        QCOMPARE(result.patch.faces.size(), 26);
        QCOMPARE(result.merged.vertices.size(), 20);
        QCOMPARE(result.merged.faces.size(), 36);
        QVERIFY(result.holes.front().tangentRmsAfter <= 1.0);
    }

    void rejectsMeshesWithoutAnEligibleBoundary()
    {
        FillConfig config;
        config.method = PatchMethod::Planar;
        config.minLoopVertices = 6;

        const FillResult result =
            Engine().fill(cubeWithoutTop(), config);

        QVERIFY(!result.result.ok);
        QCOMPARE(
            result.result.error,
            QStringLiteral("No eligible manifold boundary loops were found."));
    }

    void panelMapsTheApprovedDefaults()
    {
        WorkspaceState state;
        QVERIFY(state.commitWorkspace({entry(1), entry(2)}, 1).ok);
        QVERIFY(state.setSelectedMesh(2).ok);
        RecordingCommands commands;
        Panel panel(state, commands);

        auto* target = panel.findChild<QComboBox*>(
            QStringLiteral("holeFillingMeshCombo"));
        auto* resolution = panel.findChild<QSpinBox*>(
            QStringLiteral("holeFillingTargetVerticesSpin"));
        auto* normalWeight = panel.findChild<QDoubleSpinBox*>(
            QStringLiteral("holeFillingNormalWeightSpin"));
        auto* anchorWeight = panel.findChild<QDoubleSpinBox*>(
            QStringLiteral("holeFillingAnchorWeightSpin"));
        auto* tangentRms = panel.findChild<QDoubleSpinBox*>(
            QStringLiteral("holeFillingMaxTangentRmsSpin"));
        auto* apply = panel.findChild<QPushButton*>(
            QStringLiteral("generateHolePatchButton"));

        QVERIFY(target);
        QVERIFY(resolution);
        QVERIFY(normalWeight);
        QVERIFY(anchorWeight);
        QVERIFY(tangentRms);
        QVERIFY(apply);
        QCOMPARE(target->currentData().toULongLong(), MeshId(2));
        QCOMPARE(resolution->value(), 512);
        QCOMPARE(normalWeight->value(), 40.0);
        QCOMPARE(anchorWeight->value(), 0.002);
        QCOMPARE(tangentRms->value(), 0.15);

        QTest::mouseClick(apply, Qt::LeftButton);

        QCOMPARE(commands.callCount, 1);
        QCOMPARE(commands.lastRequest.meshId, MeshId(2));
        QCOMPARE(commands.lastRequest.config.targetVertices, 512);
        QCOMPARE(commands.lastRequest.config.maxHoles, 8);
        QCOMPARE(commands.lastRequest.config.normalWeight, 40.0);
        QCOMPARE(commands.lastRequest.config.anchorWeight, 0.002);
        QCOMPARE(commands.lastRequest.config.maxTangentRms, 0.15);
        QCOMPARE(commands.lastRequest.outputMode, OutputMode::NewLayer);
    }
};

QTEST_MAIN(PythonHoleFillingTest)
#include "python_hole_filling_test.moc"
