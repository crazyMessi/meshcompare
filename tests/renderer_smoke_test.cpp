#include <QtTest>

#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QWidget>

#include <common/globals.h>
#include <common/plugins/plugin_manager.h>

#include "infrastructure/meshlab/meshlab_mesh_loader.h"
#include "renderer/meshlab/mesh_lab_renderer_adapter.h"
#include "renderer/meshlab/mesh_lab_viewport.h"
#include "services/mesh_import_service.h"

namespace
{
SceneDescriptor sceneFor(const StagedWorkspace& staged)
{
    SceneDescriptor scene;
    scene.generation = 1;
    scene.layoutMode = staged.layoutMode;
    for (int index = 0; index < staged.entries.size(); ++index) {
        const MeshEntry& entry = staged.entries.at(index);
        scene.meshes.append(
            {entry.id,
             entry.resourceId,
             entry.displayName,
             index == 0});
    }
    scene.referenceId = scene.meshes.front().id;
    return scene;
}
} // namespace

class RendererSmokeTest : public QObject
{
    Q_OBJECT

private slots:
    void twoImportedMeshesRenderWithLinkedCameras()
    {
        PluginManager& plugins = meshlab::pluginManagerInstance();
        if (plugins.numberIOPlugins() == 0)
            plugins.loadPlugins();

        const QString triangle = QFINDTESTDATA("fixtures/triangle_a.obj");
        const QString groundTruth = QFINDTESTDATA("fixtures/triangle_gt.obj");
        QVERIFY(QFileInfo::exists(triangle));
        QVERIFY(QFileInfo::exists(groundTruth));

        MeshLabMeshLoader loader;
        MeshImportService importService(loader);
        StagedWorkspace staged = importService.stage({groundTruth, triangle});
        QVERIFY2(staged.result.ok, qPrintable(staged.result.error));
        QCOMPARE(staged.entries.size(), 2);
        QVERIFY(staged.repository != nullptr);

        const SceneDescriptor scene = sceneFor(staged);

        QWidget host;
        host.resize(1000, 600);
        host.show();
        QVERIFY(QTest::qWaitForWindowExposed(&host));

        MeshLabRendererAdapter adapter;
        QVERIFY(adapter.mount(&host).ok);
        const OperationResult prepared = adapter.prepareScene(scene, *staged.repository);
        QVERIFY2(prepared.ok, qPrintable(prepared.error));
        adapter.commitPreparedScene();

        QTRY_COMPARE_WITH_TIMEOUT(adapter.validViewportCount(), 2, 5000);
        MeshLabViewport* first = adapter.viewportForTest(0);
        MeshLabViewport* second = adapter.viewportForTest(1);
        QVERIFY(first != nullptr);
        QVERIFY(second != nullptr);
        QTRY_VERIFY_WITH_TIMEOUT(first->isVisibleTo(&host), 2000);
        QTRY_VERIFY_WITH_TIMEOUT(second->isVisibleTo(&host), 2000);
        QTRY_COMPARE_WITH_TIMEOUT(second->size(), first->size(), 2000);
        QVERIFY(first->width() > 100);
        QVERIFY(first->height() > 30);
        const QString cameraBefore = first->captureCamera().viewStateXml;

        first->trackballStep(QStringLiteral("Horizontal +"));

        QTRY_COMPARE_WITH_TIMEOUT(
            second->captureCamera().viewStateXml,
            first->captureCamera().viewStateXml,
            2000);
        QVERIFY(first->captureCamera().viewStateXml != cameraBefore);
    }

    void meshLabProjectRendersAllLayersInOneViewport()
    {
        PluginManager& plugins = meshlab::pluginManagerInstance();
        if (plugins.numberIOPlugins() == 0)
            plugins.loadPlugins();

        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString firstMesh = directory.filePath(QStringLiteral("first.obj"));
        const QString secondMesh = directory.filePath(QStringLiteral("second.obj"));
        QVERIFY(QFile::copy(QFINDTESTDATA("fixtures/triangle_a.obj"), firstMesh));
        QVERIFY(QFile::copy(QFINDTESTDATA("fixtures/triangle_gt.obj"), secondMesh));

        const QString projectPath = directory.filePath(QStringLiteral("comparison.mlp"));
        QFile project(projectPath);
        QVERIFY(project.open(QIODevice::WriteOnly | QIODevice::Text));
        const QByteArray projectXml = QByteArrayLiteral(
            "<!DOCTYPE MeshLabDocument>\n"
            "<MeshLabProject><MeshGroup>\n"
            "  <MLMesh label=\"Candidate\" filename=\"first.obj\">\n"
            "    <MLMatrix44>1 0 0 0  0 1 0 0  0 0 1 0  0 0 0 1</MLMatrix44>\n"
            "  </MLMesh>\n"
            "  <MLMesh label=\"Ground Truth\" filename=\"second.obj\">\n"
            "    <MLMatrix44>1 0 0 0  0 1 0 0  0 0 1 0  0 0 0 1</MLMatrix44>\n"
            "  </MLMesh>\n"
            "</MeshGroup></MeshLabProject>\n");
        QCOMPARE(project.write(projectXml), projectXml.size());
        project.close();

        MeshLabMeshLoader loader;
        MeshImportService importService(loader);
        StagedWorkspace staged = importService.stage({projectPath});
        QVERIFY2(staged.result.ok, qPrintable(staged.result.error));
        QCOMPARE(staged.layoutMode, SceneLayoutMode::Overlay);

        QWidget host;
        host.resize(1000, 600);
        host.show();
        QVERIFY(QTest::qWaitForWindowExposed(&host));

        MeshLabRendererAdapter adapter;
        QVERIFY(adapter.mount(&host).ok);
        const OperationResult prepared = adapter.prepareScene(
            sceneFor(staged), *staged.repository);
        QVERIFY2(prepared.ok, qPrintable(prepared.error));
        adapter.commitPreparedScene();

        QTRY_COMPARE_WITH_TIMEOUT(adapter.validViewportCount(), 1, 5000);
        MeshLabViewport* overlay = adapter.viewportForTest(0);
        QVERIFY(overlay != nullptr);
        QCOMPARE(overlay->assignedMeshCountForTest(), 2);
        QTRY_VERIFY_WITH_TIMEOUT(overlay->isVisibleTo(&host), 2000);
    }
};

QTEST_MAIN(RendererSmokeTest)
#include "renderer_smoke_test.moc"
