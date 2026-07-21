#include "grid_renderer.h"

#include <QCoreApplication>
#include <QDir>
#include <QDomDocument>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFileInfo>
#include <QImage>
#include <QThread>
#include <QWidget>

#include <common/ml_document/base_types.h>
#include <wrap/qt/shot_qt.h>

#include <cmath>

#include "core/reference_resolver.h"
#include "infrastructure/meshlab/meshlab_mesh_loader.h"
#include "renderer/meshlab/mesh_lab_renderer_adapter.h"
#include "services/mesh_import_service.h"

namespace
{
constexpr int ViewportReadyTimeoutMs = 10000;

SceneDescriptor gridSceneFor(
    const QVector<MeshEntry>& entries,
    MeshId referenceId,
    const GridRenderCamera& camera,
    const QString& initialCameraViewStateXml,
    const QSize& outputSize)
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
    if (!initialCameraViewStateXml.isEmpty()) {
        scene.initialCamera = {initialCameraViewStateXml};
    }
    else if (camera.enabled) {
        Shotm shot;
        shot.Intrinsics.cameraType = vcg::Camera<Scalarm>::PERSPECTIVE;
        shot.Intrinsics.PixelSizeMm[0] = 0.036916077f;
        shot.Intrinsics.PixelSizeMm[1] = 0.036916077f;
        shot.Intrinsics.ViewportPx[0] = outputSize.width();
        shot.Intrinsics.ViewportPx[1] = outputSize.height();
        shot.Intrinsics.CenterPx[0] = outputSize.width() / 2;
        shot.Intrinsics.CenterPx[1] = outputSize.height() / 2;
        const double viewportHeightMm =
            shot.Intrinsics.PixelSizeMm[1] * outputSize.height();
        shot.Intrinsics.FocalMm = static_cast<Scalarm>(
            viewportHeightMm /
            (2.0 * std::tan(camera.fieldOfViewDegrees * 3.14159265358979323846 / 360.0)));
        shot.SetViewPoint(Point3m(
            static_cast<Scalarm>(camera.position.x),
            static_cast<Scalarm>(camera.position.y),
            static_cast<Scalarm>(camera.position.z)));
        shot.LookAt(
            Point3m(
                static_cast<Scalarm>(camera.target.x),
                static_cast<Scalarm>(camera.target.y),
                static_cast<Scalarm>(camera.target.z)),
            Point3m(
                static_cast<Scalarm>(camera.up.x),
                static_cast<Scalarm>(camera.up.y),
                static_cast<Scalarm>(camera.up.z)));

        QDomDocument document(QStringLiteral("ViewState"));
        QDomElement root = document.createElement(QStringLiteral("project"));
        document.appendChild(root);
        root.appendChild(WriteShotToQDomNode(shot, document));
        QDomElement settings = document.createElement(QStringLiteral("ViewSettings"));
        const GridRenderCameraClipPlanes clipPlanes =
            gridRenderCameraClipPlanes(camera);
        settings.setAttribute(QStringLiteral("TrackScale"), 1.0);
        settings.setAttribute(QStringLiteral("NearPlane"), clipPlanes.nearPlane);
        settings.setAttribute(QStringLiteral("FarPlane"), clipPlanes.farPlane);
        root.appendChild(settings);
        scene.initialCamera = {document.toString()};
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
    if (!isSupportedGridRenderSize(request.outputSize)) {
        return OperationResult::failure(
            QStringLiteral(
                "Grid render dimensions must be at most 16384 per side and no more than 67108864 pixels."));
    }
    if (!isValidGridRenderCamera(request.camera)) {
        return OperationResult::failure(
            QStringLiteral("Grid render camera parameters do not define a valid perspective view."));
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

QImage normalizeGridRenderImage(QImage image, const QSize& outputSize)
{
    if (image.size() != outputSize) {
        image = image.scaled(
            outputSize,
            Qt::IgnoreAspectRatio,
            Qt::SmoothTransformation);
    }
    image.setDevicePixelRatio(1.0);
    return image;
}

QSize gridRenderHostSizeForPixelOutput(
    const QSize& outputSize,
    qreal devicePixelRatio)
{
    if (!std::isfinite(devicePixelRatio) || devicePixelRatio <= 0.0)
        return {};
    return QSize(
        qMax(1, static_cast<int>(std::ceil(outputSize.width() / devicePixelRatio))),
        qMax(1, static_cast<int>(std::ceil(outputSize.height() / devicePixelRatio))));
}

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
        staged.entries,
        reference.referenceId,
        request.camera,
        request.initialCameraViewStateXml,
        request.outputSize);
    QWidget viewportHost;
    // QGLWidget needs a shown parent to initialize its OpenGL drawable. This
    // keeps the native host off-screen; capture explicitly draws each frame.
    viewportHost.setAttribute(Qt::WA_DontShowOnScreen);
    viewportHost.resize(1, 1);
    viewportHost.show();
    QCoreApplication::processEvents();
    const QSize hostSize = gridRenderHostSizeForPixelOutput(
        request.outputSize, viewportHost.devicePixelRatioF());
    if (hostSize.isEmpty()) {
        return OperationResult::failure(
            QStringLiteral("The grid renderer could not determine a valid display scale."));
    }
    viewportHost.resize(hostSize);
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
    // The viewport is sized in Qt logical pixels, while the PNG contract is
    // physical pixels. Normalize high-DPI captures to the requested CLI size.
    image = normalizeGridRenderImage(std::move(image), request.outputSize);
    if (!image.save(resolvedOutputPath, "PNG")) {
        return OperationResult::failure(
            QStringLiteral("Could not save grid render: %1")
                .arg(resolvedOutputPath));
    }

    if (outputPath != nullptr)
        *outputPath = resolvedOutputPath;
    return OperationResult::success();
}
