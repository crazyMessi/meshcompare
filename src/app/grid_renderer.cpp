#include "grid_renderer.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFileInfo>
#include <QImage>
#include <QThread>
#include <QWidget>

#include "core/reference_resolver.h"
#include "infrastructure/meshlab/meshlab_mesh_loader.h"
#include "renderer/meshlab/mesh_lab_renderer_adapter.h"
#include "services/mesh_import_service.h"

namespace
{
constexpr int ViewportReadyTimeoutMs = 10000;

SceneDescriptor gridSceneFor(
    const QVector<MeshEntry>& entries,
    MeshId referenceId)
{
    SceneDescriptor scene;
    scene.generation = 1;
    scene.referenceId = referenceId;
    scene.layoutMode = SceneLayoutMode::ComparisonGrid;
    scene.meshes.reserve(entries.size());
    for (const MeshEntry& entry : entries) {
        scene.meshes.append(
            {entry.id,
             entry.resourceId,
             entry.displayName,
             entry.id == referenceId,
             entry.presentation,
             {},
             entry.visible});
    }
    return scene;
}

OperationResult waitForViewports(
    MeshLabRendererAdapter& renderer,
    int expectedViewportCount)
{
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < ViewportReadyTimeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        if (renderer.validViewportCount() == expectedViewportCount)
            return OperationResult::success();
        QThread::msleep(10);
    }
    return OperationResult::failure(
        QStringLiteral("Timed out while preparing OpenGL viewports for grid rendering."));
}

OperationResult outputPathFor(
    const GridRenderRequest& request,
    QString* outputPath)
{
    if (request.projectPath.isEmpty()) {
        return OperationResult::failure(
            QStringLiteral("A MeshLab project path is required for grid rendering."));
    }
    if (request.outputDirectory.isEmpty()) {
        return OperationResult::failure(
            QStringLiteral("An output directory is required for grid rendering."));
    }
    if (request.outputSize.width() <= 0 || request.outputSize.height() <= 0) {
        return OperationResult::failure(
            QStringLiteral("Grid render dimensions must be positive."));
    }

    const QFileInfo projectInfo(request.projectPath);
    const QString outputBaseName = projectInfo.completeBaseName();
    if (outputBaseName.isEmpty()) {
        return OperationResult::failure(
            QStringLiteral("The MeshLab project must have a filename."));
    }
    if (!QDir().mkpath(request.outputDirectory)) {
        return OperationResult::failure(
            QStringLiteral("Could not create output directory: %1")
                .arg(request.outputDirectory));
    }

    if (outputPath != nullptr) {
        *outputPath = QDir(request.outputDirectory).absoluteFilePath(
            outputBaseName + QStringLiteral(".grid.png"));
    }
    return OperationResult::success();
}
} // namespace

OperationResult renderComparisonGrid(
    const GridRenderRequest& request,
    QString* outputPath)
{
    QString resolvedOutputPath;
    const OperationResult outputValidation = outputPathFor(
        request, &resolvedOutputPath);
    if (!outputValidation.ok)
        return outputValidation;

    MeshLabMeshLoader loader;
    MeshImportService importer(loader);
    StagedWorkspace staged = importer.stage({request.projectPath});
    if (!staged.result.ok)
        return staged.result;
    if (!staged.repository) {
        return OperationResult::failure(
            QStringLiteral("Grid rendering could not prepare a mesh repository."));
    }

    const ReferenceResolution reference = resolveReference(staged.entries);
    const SceneDescriptor scene = gridSceneFor(
        staged.entries, reference.referenceId);
    QWidget viewportHost;
    // QGLWidget needs a shown parent to initialize its OpenGL drawable. This
    // keeps the native host off-screen; capture explicitly draws each frame.
    viewportHost.setAttribute(Qt::WA_DontShowOnScreen);
    viewportHost.resize(request.outputSize);
    viewportHost.show();
    QCoreApplication::processEvents();

    MeshLabRendererAdapter renderer;
    const OperationResult mounted = renderer.mount(&viewportHost);
    if (!mounted.ok)
        return mounted;
    const OperationResult prepared = renderer.prepareScene(scene, *staged.repository);
    if (!prepared.ok) {
        renderer.discardPreparedScene();
        return prepared;
    }
    renderer.commitPreparedScene();
    renderer.setReferenceMesh(reference.referenceId);
    renderer.setSelectedMesh(reference.referenceId);

    const OperationResult ready = waitForViewports(renderer, scene.meshes.size());
    if (!ready.ok)
        return ready;

    QImage image;
    const OperationResult captured = renderer.captureImage(image);
    if (!captured.ok)
        return captured;
    if (image.isNull()) {
        return OperationResult::failure(
            QStringLiteral("Grid rendering produced an empty image."));
    }
    if (!image.save(resolvedOutputPath, "PNG")) {
        return OperationResult::failure(
            QStringLiteral("Could not save grid render: %1")
                .arg(resolvedOutputPath));
    }

    if (outputPath != nullptr)
        *outputPath = resolvedOutputPath;
    return OperationResult::success();
}
