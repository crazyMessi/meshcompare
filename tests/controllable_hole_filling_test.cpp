#include <QtTest>

#include <QComboBox>
#include <QDoubleSpinBox>
#include <QPushButton>
#include <QSpinBox>

#include "core/workspace_state.h"
#include "plugins/controllable_hole_filling/controllable_hole_filling.h"
#include "plugins/controllable_hole_filling/controllable_hole_filling_panel.h"

namespace
{

using namespace controllable_hole_filling;

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
    mesh.sourcePath = QStringLiteral("/tmp/mesh-%1.obj").arg(id);
    mesh.displayName = QStringLiteral("mesh-%1.obj").arg(id);
    return mesh;
}

class RecordingCommands final : public ICommands
{
public:
    OperationResult fillHoles(
        const FillRequest& request,
        FillSummary* summary) override
    {
        ++callCount;
        lastRequest = request;
        if (summary != nullptr) {
            summary->vertexCount = 1000;
            summary->faceCount = 2000;
            summary->activeCellCount = 900;
            summary->elapsedMilliseconds = 125;
            summary->outputLayerName =
                QStringLiteral("mesh-1.obj Reconstructed");
        }
        return result;
    }

    FillRequest lastRequest;
    OperationResult result = OperationResult::success();
    int callCount = 0;
};

} // namespace

class ControllableHoleFillingTest : public QObject
{
    Q_OBJECT

private slots:
    void releasePipelineClosesAnOpenCube()
    {
        const MemoryGeometry geometry = cubeWithoutTop();
        FillConfig config;
        config.resolution = 32;
        config.cclIterations = 2;
        config.epsFactor = 2.0;

        const FillResult result = Engine().fill(geometry, config);

        QVERIFY2(result.result.ok, qPrintable(result.result.error));
        QVERIFY(result.candidateCellCount > 0);
        QVERIFY(result.activeCellCount > 0);
        QVERIFY(!result.mesh.vertices.isEmpty());
        QVERIFY(!result.mesh.faces.isEmpty());
        for (const std::array<int, 3>& face : result.mesh.faces) {
            QVERIFY(face[0] >= 0 && face[0] < result.mesh.vertices.size());
            QVERIFY(face[1] >= 0 && face[1] < result.mesh.vertices.size());
            QVERIFY(face[2] >= 0 && face[2] < result.mesh.vertices.size());
            QVERIFY(face[0] != face[1]);
            QVERIFY(face[1] != face[2]);
            QVERIFY(face[2] != face[0]);
        }
    }

    void rejectsInvalidReleaseParameters()
    {
        FillConfig config;
        config.resolution = 8;

        const FillResult result = Engine().fill(cubeWithoutTop(), config);

        QVERIFY(!result.result.ok);
        QVERIFY(result.result.error.contains(QStringLiteral("Resolution")));
    }

    void panelExposesReleaseParametersAndMapsARequest()
    {
        WorkspaceState state;
        QVERIFY(state.commitWorkspace({entry(1), entry(2)}, 1).ok);
        QVERIFY(state.setSelectedMesh(2).ok);
        RecordingCommands commands;
        Panel panel(state, commands);

        auto* target = panel.findChild<QComboBox*>(
            QStringLiteral("holeFillingMeshCombo"));
        auto* resolution = panel.findChild<QSpinBox*>(
            QStringLiteral("holeFillingResolutionSpin"));
        auto* cclIterations = panel.findChild<QSpinBox*>(
            QStringLiteral("holeFillingCclIterationsSpin"));
        auto* epsFactor = panel.findChild<QDoubleSpinBox*>(
            QStringLiteral("holeFillingEpsFactorSpin"));
        auto* output = panel.findChild<QComboBox*>(
            QStringLiteral("holeFillingOutputCombo"));
        auto* apply = panel.findChild<QPushButton*>(
            QStringLiteral("generateHolePatchButton"));

        QVERIFY(target);
        QVERIFY(resolution);
        QVERIFY(cclIterations);
        QVERIFY(epsFactor);
        QVERIFY(output);
        QVERIFY(apply);
        QCOMPARE(target->currentData().toULongLong(), MeshId(2));
        QCOMPARE(resolution->value(), 512);
        QCOMPARE(cclIterations->value(), 3);
        QCOMPARE(epsFactor->value(), 2.0);
        QCOMPARE(
            output->currentData().toInt(),
            static_cast<int>(OutputMode::NewLayer));

        QTest::mouseClick(apply, Qt::LeftButton);

        QCOMPARE(commands.callCount, 1);
        QCOMPARE(commands.lastRequest.meshId, MeshId(2));
        QCOMPARE(commands.lastRequest.config.resolution, 512);
        QCOMPARE(commands.lastRequest.config.cclIterations, 3);
        QCOMPARE(commands.lastRequest.config.epsFactor, 2.0);
        QCOMPARE(commands.lastRequest.outputMode, OutputMode::NewLayer);
    }
};

QTEST_MAIN(ControllableHoleFillingTest)
#include "controllable_hole_filling_test.moc"
