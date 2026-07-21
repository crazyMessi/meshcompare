#include <QtTest>

#include <QImage>
#include <QTemporaryDir>

#include "app/grid_renderer.h"

class GridRendererTest : public QObject
{
    Q_OBJECT

private slots:
    void defaultDimensionsAre2048By1152()
    {
        const GridRenderRequest request;

        QCOMPARE(request.outputSize, QSize(2048, 1152));
    }

    void emptyProjectFailsBeforeCreatingAnOpenGLHost()
    {
        QTemporaryDir outputDirectory;
        QVERIFY(outputDirectory.isValid());
        const GridRenderRequest request{
            {}, outputDirectory.path(), QSize(2048, 1152)};

        const OperationResult result = renderComparisonGrid(request);

        QVERIFY(!result.ok);
        QCOMPARE(
            result.error,
            QStringLiteral("A MeshLab project path is required for grid rendering."));
    }

    void invalidDimensionsFailBeforeCreatingAnOpenGLHost()
    {
        QTemporaryDir outputDirectory;
        QVERIFY(outputDirectory.isValid());
        const GridRenderRequest request{
            QStringLiteral("comparison.mlp"), outputDirectory.path(), QSize(0, 1152)};

        const OperationResult result = renderComparisonGrid(request);

        QVERIFY(!result.ok);
        QCOMPARE(
            result.error,
            QStringLiteral("Grid render dimensions must be positive."));
    }

    void excessiveDimensionsFailBeforeCreatingAnOpenGLHost()
    {
        QTemporaryDir outputDirectory;
        QVERIFY(outputDirectory.isValid());
        const GridRenderRequest request{
            QStringLiteral("comparison.mlp"),
            outputDirectory.path(),
            QSize(16384, 16384)};

        const OperationResult result = renderComparisonGrid(request);

        QVERIFY(!result.ok);
        QCOMPARE(
            result.error,
            QStringLiteral(
                "Grid render dimensions must be at most 16384 per side and no more than 67108864 pixels."));
    }

    void invalidCameraFailsBeforeCreatingAnOpenGLHost()
    {
        QTemporaryDir outputDirectory;
        QVERIFY(outputDirectory.isValid());
        GridRenderRequest request;
        request.projectPath = QStringLiteral("comparison.mlp");
        request.outputDirectory = outputDirectory.path();
        request.camera.enabled = true;
        request.camera.position = {1.0, 2.0, 3.0};
        request.camera.target = {1.0, 2.0, 3.0};

        const OperationResult result = renderComparisonGrid(request);

        QVERIFY(!result.ok);
        QCOMPARE(
            result.error,
            QStringLiteral("Grid render camera parameters do not define a valid perspective view."));
    }

    void cameraClipPlanesScaleForRemoteCoordinates()
    {
        GridRenderCamera camera;
        camera.enabled = true;
        camera.position = {0.0, 0.0, 1000.0};
        camera.target = {0.0, 0.0, 0.0};

        const GridRenderCameraClipPlanes clipPlanes =
            gridRenderCameraClipPlanes(camera);

        QCOMPARE(clipPlanes.nearPlane, 0.1);
        QCOMPARE(clipPlanes.farPlane, 100000.0);
    }

    void cameraClipPlanesSupportCloseCoordinates()
    {
        GridRenderCamera camera;
        camera.enabled = true;
        camera.position = {0.0, 0.0, 0.01};
        camera.target = {0.0, 0.0, 0.0};

        const GridRenderCameraClipPlanes clipPlanes =
            gridRenderCameraClipPlanes(camera);

        QCOMPARE(clipPlanes.nearPlane, 0.00001);
        QCOMPARE(clipPlanes.farPlane, 500.0);
    }

    void cameraRejectsCoordinatesOutsideMeshLabTransformRange()
    {
        GridRenderCamera camera;
        camera.enabled = true;
        camera.position = {1e100, 0.0, 1.0};
        camera.target = {0.0, 0.0, 0.0};

        QVERIFY(!isValidGridRenderCamera(camera));
    }

    void cameraRejectsAViewThatCollapsesAtMeshLabPrecision()
    {
        GridRenderCamera camera;
        camera.enabled = true;
        camera.position = {1e12, 0.0, 0.0};
        camera.target = {1e12 - 1.0, 0.0, 0.0};

        QVERIFY(!isValidGridRenderCamera(camera));
    }

    void cameraRejectsAViewWhoseMeshLabBasisWouldOverflow()
    {
        GridRenderCamera camera;
        camera.enabled = true;
        camera.position = {0.0, 0.0, 1e12};
        camera.target = {0.0, 0.0, 0.0};

        QVERIFY(!isValidGridRenderCamera(camera));
    }

    void highDpiCaptureIsNormalizedToRequestedPngPixels()
    {
        QImage highDpiCapture(800, 450, QImage::Format_RGB32);
        highDpiCapture.setDevicePixelRatio(2.0);

        const QImage normalized = normalizeGridRenderImage(
            highDpiCapture, QSize(400, 225));

        QCOMPARE(normalized.size(), QSize(400, 225));
        QCOMPARE(normalized.devicePixelRatio(), 1.0);
    }

    void highDpiHostUsesLogicalDimensionsThatBoundFramebufferPixels()
    {
        QCOMPARE(
            gridRenderHostSizeForPixelOutput(QSize(3840, 2160), 2.0),
            QSize(1920, 1080));
        QCOMPARE(
            gridRenderHostSizeForPixelOutput(QSize(3840, 2160), 3.0),
            QSize(1280, 720));
        QVERIFY(gridRenderHostSizeForPixelOutput(QSize(3840, 2160), 0.0).isEmpty());
    }
};

QTEST_APPLESS_MAIN(GridRendererTest)
#include "grid_renderer_test.moc"
