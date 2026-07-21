#include <QtTest>

#include <QTemporaryDir>

#include "app/grid_renderer.h"

class GridRendererTest : public QObject
{
    Q_OBJECT

private slots:
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
};

QTEST_APPLESS_MAIN(GridRendererTest)
#include "grid_renderer_test.moc"
