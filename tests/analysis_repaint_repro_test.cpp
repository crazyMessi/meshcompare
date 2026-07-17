#include <QtTest>

#include <QApplication>
#include <QDir>
#include <QFileInfo>
#include <QSignalSpy>
#include <QWidget>

#include <common/globals.h>
#include <common/plugins/plugin_manager.h>

#include "app/workspace_controller.h"
#include "core/workspace_state.h"
#include "infrastructure/meshlab/meshlab_mesh_loader.h"
#include "renderer/meshlab/mesh_lab_renderer_adapter.h"
#include "renderer/meshlab/mesh_lab_viewport.h"
#include "services/mesh_import_service.h"
#include "services/surface_comparison.h"
#include "fakes/fake_camera_pose_store.h"

namespace
{
QStringList reproMeshPaths()
{
    const QDir directory(QString::fromLocal8Bit(qgetenv("MESHCOMPARE_REPRO_DIR")));
    QStringList names;
    const QString configured = QString::fromLocal8Bit(
        qgetenv("MESHCOMPARE_REPRO_MESHES"));
    if (!configured.isEmpty()) {
        names = configured.split(QLatin1Char(','), Qt::SkipEmptyParts);
    }
    else {
        names = QStringList{
            QStringLiteral("reference_gt.ply"),
            QStringLiteral("our.ply"),
            QStringLiteral("udf_close.ply"),
            QStringLiteral("linf_odc.ply"),
            QStringLiteral("linf_mc.ply"),
            QStringLiteral("hy.ply")};
    }
    for (QString& name : names)
        name = directory.filePath(name.trimmed());
    return names;
}

int reproSampleCount()
{
    bool ok = false;
    const int configured = QString::fromLocal8Bit(
                               qgetenv("MESHCOMPARE_REPRO_SAMPLE_COUNT"))
                               .toInt(&ok);
    return ok && configured > 0 ? configured : 1;
}

QString reproSequence()
{
    const QString configured = QString::fromLocal8Bit(
                                   qgetenv("MESHCOMPARE_REPRO_SEQUENCE"))
                                   .trimmed()
                                   .toLower();
    return configured.isEmpty() ? QStringLiteral("both") : configured;
}

AnalysisBatchResult waitForAnalysis(QSignalSpy& finished)
{
    if (finished.isEmpty())
        finished.wait(30 * 60 * 1000);
    if (finished.isEmpty()) {
        QTest::qFail(
            "Timed out waiting for the analysis batch.",
            __FILE__,
            __LINE__);
        return {};
    }
    return qvariant_cast<AnalysisBatchResult>(finished.takeFirst().at(0));
}
} // namespace

class AnalysisRepaintReproTest final : public QObject
{
    Q_OBJECT

private slots:
    void consecutiveAnalysisRepaintsUnderGpuPressure()
    {
        if (qEnvironmentVariableIsEmpty("MESHCOMPARE_REPRO_DIR")) {
            QSKIP(
                "Set MESHCOMPARE_REPRO_DIR to run the large-mesh GPU-pressure "
                "regression.");
        }

        const QStringList paths = reproMeshPaths();
        for (const QString& path : paths) {
            QVERIFY2(
                QFileInfo::exists(path),
                qPrintable(QStringLiteral("Missing repro mesh: %1").arg(path)));
        }

        PluginManager& plugins = meshlab::pluginManagerInstance();
        if (plugins.numberIOPlugins() == 0)
            plugins.loadPlugins();

        QWidget host;
        host.resize(1440, 780);
        host.show();
        QVERIFY(QTest::qWaitForWindowExposed(&host, 5000));

        WorkspaceState state;
        MeshLabMeshLoader loader;
        MeshImportService importer(loader);
        MeshLabRendererAdapter renderer;
        SurfaceComparer comparer;
        FakeCameraPoseStore cameraStore;

        QVERIFY2(renderer.mount(&host).ok, "Renderer mount failed.");
        WorkspaceController controller(
            state, importer, renderer, comparer, cameraStore);
        QSignalSpy finished(&controller, &WorkspaceController::analysisFinished);

        const WorkspaceImportOutcome imported = controller.importMeshes(paths);
        QVERIFY2(imported.result.ok, qPrintable(imported.result.error));
        QTRY_COMPARE_WITH_TIMEOUT(
            renderer.validViewportCount(),
            paths.size(),
            10000);
        QApplication::processEvents();

        SurfaceComparisonOptions options;
        options.sampleCount = reproSampleCount();

        const QString sequence = reproSequence();
        QVERIFY2(
            sequence == QStringLiteral("both") ||
                sequence == QStringLiteral("precision") ||
                sequence == QStringLiteral("normal") ||
                sequence == QStringLiteral("none"),
            qPrintable(QStringLiteral("Unknown repro sequence: %1").arg(sequence)));

        if (sequence == QStringLiteral("both") ||
            sequence == QStringLiteral("precision")) {
            QVERIFY2(
                controller.startAnalysis(
                              SurfaceComparisonMetric::PrecisionAtThreshold,
                              options)
                    .ok,
                "Precision analysis did not start.");
            const AnalysisBatchResult precision = waitForAnalysis(finished);
            QVERIFY2(precision.result.ok, qPrintable(precision.result.error));
            QApplication::processEvents();
        }

        if (sequence == QStringLiteral("both") ||
            sequence == QStringLiteral("normal")) {
            QVERIFY2(
                controller.startAnalysis(
                              SurfaceComparisonMetric::NormalAgreement,
                              options)
                    .ok,
                "Normal-agreement analysis did not start.");
            const AnalysisBatchResult normalAgreement = waitForAnalysis(finished);
            QVERIFY2(
                normalAgreement.result.ok,
                qPrintable(normalAgreement.result.error));
        }

        if (sequence != QStringLiteral("none")) {
            const QColor missingAnalysisColor(128, 128, 128, 255);
            for (const MeshEntry& mesh : state.meshes()) {
                if (mesh.isReference)
                    continue;
                QVERIFY(!mesh.presentation.faceColors.isEmpty());
                for (const QColor& color : mesh.presentation.faceColors)
                    QVERIFY(color != missingAnalysisColor);
            }
        }

        for (int index = 0; index < renderer.validViewportCount(); ++index) {
            MeshLabViewport* viewport = renderer.viewportForTest(index);
            QVERIFY(viewport != nullptr);
            viewport->repaint();
        }
        QApplication::processEvents();
        QTest::qWait(250);
        QApplication::processEvents();
    }
};

QTEST_MAIN(AnalysisRepaintReproTest)
#include "analysis_repaint_repro_test.moc"
