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

    void highDpiCaptureIsNormalizedToRequestedPngPixels()
    {
        QImage highDpiCapture(800, 450, QImage::Format_RGB32);
        highDpiCapture.setDevicePixelRatio(2.0);

        const QImage normalized = normalizeGridRenderImage(
            highDpiCapture, QSize(400, 225));

        QCOMPARE(normalized.size(), QSize(400, 225));
        QCOMPARE(normalized.devicePixelRatio(), 1.0);
    }
};

QTEST_APPLESS_MAIN(GridRendererTest)
#include "grid_renderer_test.moc"
