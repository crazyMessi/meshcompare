#include <QtTest>

#include <algorithm>
#include <cmath>

#include <QApplication>
#include <QByteArray>
#include <QDir>
#include <QFileInfo>
#include <QImage>

#include <common/ml_document/mesh_document.h>
#include <common/ml_shared_data_context/ml_scene_gl_shared_data_context.h>
#include <common/parameters/rich_parameter_list.h>
#include <wrap/qt/qt_thread_safe_memory_info.h>

#include "renderer/meshlab/mesh_lab_viewport.h"
#include "fakes/fake_viewport_callbacks.h"

namespace
{
constexpr int ImageWidth = 1000;
constexpr int ImageHeight = 600;
constexpr int MaximumChannelDifference = 3;
constexpr double RequiredEqualPixelRatio = 0.995;

// The viewport label is deliberately excluded from the raster contract because
// system-font rasterization is not a renderer regression.  The stored golden
// remains unmasked so that it can still be inspected as a complete viewport.
const QRect ViewportLabelMask(0, 0, 240, 96);

class FixedRenderScene
{
public:
    FixedRenderScene()
        : memoryInfo_(0), sharedContext_(document_, memoryInfo_, false, 1)
    {
    }

    MeshDocument& document() { return document_; }
    MLSceneGLSharedDataContext& sharedContext() { return sharedContext_; }
    RichParameterList& settings() { return settings_; }

private:
    MeshDocument document_;
    vcg::QtThreadSafeMemoryInfo memoryInfo_;
    MLSceneGLSharedDataContext sharedContext_;
    RichParameterList settings_;
};

struct ImageDiff
{
    double equalPixelRatio = 0.0;
    qsizetype comparedPixels = 0;
    qsizetype differentPixels = 0;
    int worstChannelDifference = 0;
    QSize actualSize;
    QSize referenceSize;

    QString summary() const
    {
        if (actualSize != referenceSize) {
            return QStringLiteral("Image sizes differ: actual %1x%2, reference %3x%4.")
                .arg(actualSize.width())
                .arg(actualSize.height())
                .arg(referenceSize.width())
                .arg(referenceSize.height());
        }
        return QStringLiteral(
                   "Equal-pixel ratio %1 (required %2); %3 of %4 pixels differ; "
                   "worst channel delta %5 (allowed %6).")
            .arg(equalPixelRatio, 0, 'f', 6)
            .arg(RequiredEqualPixelRatio, 0, 'f', 6)
            .arg(differentPixels)
            .arg(comparedPixels)
            .arg(worstChannelDifference)
            .arg(MaximumChannelDifference);
    }
};

MeshModel* addFixedTriangle(FixedRenderScene& scene)
{
    MeshModel* mesh = scene.document().addNewMesh(
        QString(),
        QStringLiteral("triangle"),
        false);
    if (mesh == nullptr)
        return nullptr;

    vcg::tri::Allocator<CMeshO>::AddVertices(mesh->cm, 3);
    mesh->cm.vert[0].P() = CMeshO::CoordType(0, 0, 0);
    mesh->cm.vert[1].P() = CMeshO::CoordType(1, 0, 0);
    mesh->cm.vert[2].P() = CMeshO::CoordType(0, 1, 0);
    vcg::tri::Allocator<CMeshO>::AddFaces(mesh->cm, 1);
    mesh->cm.face[0].V(0) = &mesh->cm.vert[0];
    mesh->cm.face[0].V(1) = &mesh->cm.vert[1];
    mesh->cm.face[0].V(2) = &mesh->cm.vert[2];
    mesh->updateBoxAndNormals();
    return mesh;
}

QImage maskedForComparison(const QImage& source)
{
    QImage result = source.convertToFormat(QImage::Format_ARGB32);
    const QRect mask = ViewportLabelMask.intersected(result.rect());
    for (int y = mask.top(); y <= mask.bottom(); ++y) {
        QRgb* pixels = reinterpret_cast<QRgb*>(result.scanLine(y));
        std::fill(pixels + mask.left(), pixels + mask.right() + 1, qRgb(0, 0, 0));
    }
    return result;
}

ImageDiff compareImages(
    const QImage& actualSource,
    const QImage& referenceSource,
    int maximumChannelDifference)
{
    ImageDiff diff;
    diff.actualSize = actualSource.size();
    diff.referenceSize = referenceSource.size();
    if (actualSource.isNull() || referenceSource.isNull() ||
        diff.actualSize != diff.referenceSize) {
        return diff;
    }

    const QImage actual = maskedForComparison(actualSource);
    const QImage reference = maskedForComparison(referenceSource);
    diff.comparedPixels = qsizetype(actual.width()) * actual.height();

    for (int y = 0; y < actual.height(); ++y) {
        const QRgb* actualPixels =
            reinterpret_cast<const QRgb*>(actual.constScanLine(y));
        const QRgb* referencePixels =
            reinterpret_cast<const QRgb*>(reference.constScanLine(y));
        for (int x = 0; x < actual.width(); ++x) {
            const QRgb actualPixel = actualPixels[x];
            const QRgb referencePixel = referencePixels[x];
            const int channelDifference = std::max({
                std::abs(qRed(actualPixel) - qRed(referencePixel)),
                std::abs(qGreen(actualPixel) - qGreen(referencePixel)),
                std::abs(qBlue(actualPixel) - qBlue(referencePixel)),
                std::abs(qAlpha(actualPixel) - qAlpha(referencePixel))});
            diff.worstChannelDifference =
                std::max(diff.worstChannelDifference, channelDifference);
            if (channelDifference > maximumChannelDifference)
                ++diff.differentPixels;
        }
    }

    diff.equalPixelRatio = diff.comparedPixels == 0
        ? 0.0
        : 1.0 - (double(diff.differentPixels) / double(diff.comparedPixels));
    return diff;
}

QString referenceImagePath()
{
    const QFileInfo sourceFile(QString::fromUtf8(__FILE__));
    if (sourceFile.isAbsolute()) {
        return QDir(sourceFile.absolutePath())
            .filePath(QStringLiteral("fixtures/render_reference.png"));
    }

    const QString located = QFINDTESTDATA("fixtures/render_reference.png");
    if (!located.isEmpty())
        return located;

    // CMake places test executables three directories below the worktree in
    // the current build layout.  This fallback keeps explicit golden
    // generation usable with a relative __FILE__ while the reference does not
    // exist yet.
    return QDir(QCoreApplication::applicationDirPath())
        .absoluteFilePath(
            QStringLiteral("../../../src/meshcompare/tests/fixtures/render_reference.png"));
}

bool goldenUpdateRequested()
{
    return qgetenv("MESHCOMPARE_UPDATE_GOLDEN") == QByteArray("1");
}
} // namespace

class RenderRegressionTest : public QObject
{
    Q_OBJECT

private slots:
    void comparisonContractUsesThreeLevelsAnd995Percent()
    {
        QImage actual(20, 100, QImage::Format_ARGB32);
        actual.fill(qRgb(40, 50, 60));
        QImage reference = actual;

        for (int x = 0; x < 10; ++x)
            reference.setPixel(x, 99, qRgb(43, 47, 63));
        ImageDiff diff = compareImages(actual, reference, MaximumChannelDifference);
        QCOMPARE(diff.equalPixelRatio, 1.0);

        for (int x = 0; x < 10; ++x)
            reference.setPixel(x, 99, qRgb(44, 50, 60));
        diff = compareImages(actual, reference, MaximumChannelDifference);
        QCOMPARE(diff.differentPixels, qsizetype(10));
        QCOMPARE(diff.equalPixelRatio, RequiredEqualPixelRatio);

        reference.setPixel(10, 99, qRgb(40, 54, 60));
        diff = compareImages(actual, reference, MaximumChannelDifference);
        QVERIFY(diff.equalPixelRatio < RequiredEqualPixelRatio);

        // Differences inside the viewport-label mask never affect the ratio.
        reference.setPixel(0, 0, qRgb(255, 255, 255));
        diff = compareImages(actual, reference, MaximumChannelDifference);
        QVERIFY(diff.equalPixelRatio < RequiredEqualPixelRatio);
        for (int x = 0; x <= 10; ++x)
            reference.setPixel(x, 99, actual.pixel(x, 99));
        diff = compareImages(actual, reference, MaximumChannelDifference);
        QCOMPARE(diff.equalPixelRatio, 1.0);
    }

    void fixedTriangleMatchesMeshLabViewportGolden()
    {
        FixedRenderScene scene;
        MeshModel* mesh = addFixedTriangle(scene);
        QVERIFY(mesh != nullptr);

        FakeViewportCallbacks callbacks;
        ViewportDependencies dependencies{
            scene.document(),
            scene.sharedContext(),
            scene.settings(),
            callbacks,
            1,
            mesh->id(),
            1,
            1,
            QStringLiteral("triangle"),
            false,
            QString(),
            false};
        MeshLabViewport viewport(nullptr, dependencies);
        // QGLWidget uses native backing pixels on Retina even when Qt logical
        // high-DPI scaling is disabled.  Size the logical widget from its
        // backing scale so the renderer itself always produces 1000x600
        // physical pixels; never resize or resample the captured image.
        const qreal backingScale = viewport.devicePixelRatioF();
        QVERIFY2(
            std::isfinite(backingScale) && backingScale > 0.0,
            "The viewport reported an invalid device-pixel ratio.");
        const QSize logicalViewportSize(
            qRound(ImageWidth / backingScale),
            qRound(ImageHeight / backingScale));
        viewport.resize(logicalViewportSize);
        viewport.show();
        QVERIFY2(
            QTest::qWaitForWindowExposed(&viewport, 5000),
            "The fixed render viewport was not exposed within five seconds.");
        QCOMPARE(viewport.size(), logicalViewportSize);

        const OperationResult initialized = viewport.initializeForScenePreparation();
        QVERIFY2(initialized.ok, qPrintable(initialized.error));

        // Reset plus a fixed pair of 15-degree trackball steps is the camera
        // contract for this fixture.  It avoids accidental dependence on prior
        // input events while exercising MeshLab's default lighting response.
        viewport.resetCamera();
        viewport.trackballStep(QStringLiteral("Horizontal +"));
        viewport.trackballStep(QStringLiteral("Vertical -"));
        viewport.repaint();
        QApplication::processEvents();
        QTest::qWait(50);

        const QImage actual = viewport.grabFrameBuffer(false);
        QVERIFY2(!actual.isNull(), "The MeshLab viewport returned an empty frame.");
        QCOMPARE(actual.size(), QSize(ImageWidth, ImageHeight));
        QCOMPARE(callbacks.rendererErrorCount(), 0);

        const QString referencePath = referenceImagePath();
        QVERIFY2(
            !referencePath.isEmpty(),
            "The render-reference source path could not be resolved.");
        if (goldenUpdateRequested()) {
            QVERIFY2(
                QDir().mkpath(QFileInfo(referencePath).absolutePath()),
                qPrintable(QStringLiteral("Could not create golden directory: %1")
                               .arg(QFileInfo(referencePath).absolutePath())));
            QVERIFY2(
                actual.save(referencePath, "PNG"),
                qPrintable(QStringLiteral("Could not update render golden: %1")
                               .arg(referencePath)));
        }

        QVERIFY2(
            QFileInfo::exists(referencePath),
            qPrintable(QStringLiteral(
                           "Render golden is missing: %1. Generate it only with "
                           "MESHCOMPARE_UPDATE_GOLDEN=1.")
                           .arg(referencePath)));
        const QImage reference(referencePath);
        QVERIFY2(
            !reference.isNull(),
            qPrintable(QStringLiteral("Render golden is unreadable: %1")
                           .arg(referencePath)));
        QCOMPARE(reference.size(), QSize(ImageWidth, ImageHeight));

        const ImageDiff diff =
            compareImages(actual, reference, MaximumChannelDifference);
        QVERIFY2(
            diff.equalPixelRatio >= RequiredEqualPixelRatio,
            qPrintable(diff.summary()));
    }
};

int main(int argc, char** argv)
{
    QApplication application(argc, argv);
    RenderRegressionTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "render_regression_test.moc"
