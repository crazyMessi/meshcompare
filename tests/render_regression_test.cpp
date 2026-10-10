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
#include "renderer/meshlab/render_scene_context.h"
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

class TexturedTriangle final : public IMeshGeometryView, public IMeshResourceProvider
{
public:
    const IMeshGeometryView* geometry(MeshResourceId id) const override
    {
        return id == 1 ? this : nullptr;
    }
    int vertexCount() const override { return 3; }
    MeshPoint3D vertexPosition(int index) const override
    {
        static const MeshPoint3D points[] = {{{-1, -1, 0}}, {{1, -1, 0}}, {{0, 1, 0}}};
        return points[index];
    }
    MeshPoint3D vertexNormal(int) const override { return {{0, 0, 1}}; }
    bool hasVertexColors() const override { return true; }
    QColor vertexColor(int) const override { return QColor(128, 255, 255); }
    int faceCount() const override { return 1; }
    std::array<int, 3> faceVertexIndices(int) const override { return {{0, 1, 2}}; }
    int textureCount() const override { return 1; }
    bool hasFaceTexCoords() const override { return true; }
    std::array<MeshTexCoord, 3> faceTexCoords(int) const override
    {
        return {{{0, 0, 0}, {1, 0, 0}, {0.5, 1, 0}}};
    }
    QImage textureImage(int) const override
    {
        QImage image(16, 16, QImage::Format_ARGB32);
        for (int y = 0; y < image.height(); ++y)
            for (int x = 0; x < image.width(); ++x)
                image.setPixelColor(x, y, x < 8 ? Qt::red : Qt::green);
        return image;
    }
    bool isUnlit() const override { return true; }
};

class MultipleTextureTriangles final : public IMeshGeometryView, public IMeshResourceProvider
{
public:
    explicit MultipleTextureTriangles(bool finalFaceIsPlain)
        : finalFaceIsPlain_(finalFaceIsPlain) {}
    const IMeshGeometryView* geometry(MeshResourceId id) const override
    {
        return id == 1 ? this : nullptr;
    }
    int vertexCount() const override { return 9; }
    MeshPoint3D vertexPosition(int index) const override
    {
        const double center = (index / 3 - 1) * 2.0;
        static const MeshPoint3D offsets[] = {{{-0.9, -0.9, 0}}, {{0.9, -0.9, 0}}, {{0, 0.9, 0}}};
        MeshPoint3D point = offsets[index % 3];
        point[0] += center;
        return point;
    }
    MeshPoint3D vertexNormal(int) const override { return {{0, 0, 1}}; }
    bool hasVertexColors() const override { return true; }
    QColor vertexColor(int index) const override
    {
        return textureIndex(index / 3) == -1 ? Qt::blue : Qt::white;
    }
    int faceCount() const override { return 3; }
    std::array<int, 3> faceVertexIndices(int index) const override
    {
        return {{index * 3, index * 3 + 1, index * 3 + 2}};
    }
    int textureCount() const override { return 2; }
    bool hasFaceTexCoords() const override { return true; }
    std::array<MeshTexCoord, 3> faceTexCoords(int index) const override
    {
        const int texture = textureIndex(index);
        return {{{0, 0, texture}, {1, 0, texture}, {0.5, 1, texture}}};
    }
    QImage textureImage(int index) const override
    {
        QImage image(2, 2, QImage::Format_ARGB32);
        image.fill(index == 0 ? Qt::red : Qt::green);
        return image;
    }
    bool isUnlit() const override { return true; }

private:
    int textureIndex(int face) const
    {
        if (face == 0)
            return 0;
        return face == (finalFaceIsPlain_ ? 2 : 1) ? -1 : 1;
    }
    bool finalFaceIsPlain_;
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

QImage captureRenderedFrame(MeshLabViewport& viewport)
{
    // Native QGLWidget painting/back-buffer publication completes through the
    // event loop on macOS. A bare repaint() can leave grabFrameBuffer reading
    // the previously displayed frame after a presentation update.
    viewport.update();
    QApplication::processEvents();
    QTest::qWait(30);
    return viewport.grabFrameBuffer(false);
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

    void sourceTextureIsRenderedWithVertexTintAndRestoredAfterColoring()
    {
        TexturedTriangle resources;
        RenderSceneContext scene;
        SceneDescriptor descriptor;
        descriptor.generation = 1;
        descriptor.referenceId = 1;
        descriptor.meshes.append({1, 1, QStringLiteral("textured triangle"), true});
        QVERIFY(scene.addResources(descriptor, resources).ok);
        RichParameterList settings;
        FakeViewportCallbacks callbacks;
        ViewportDependencies dependencies{
            scene.document(), scene.sharedContext(), settings, callbacks,
            1, scene.modelIdFor(1), 1, 1, QStringLiteral("textured triangle"),
            false, QString(), false};
        MeshLabViewport viewport(nullptr, dependencies);
        viewport.resize(500, 400);
        viewport.show();
        QVERIFY(QTest::qWaitForWindowExposed(&viewport, 5000));
        QVERIFY(viewport.initializeForScenePreparation().ok);
        QVERIFY(scene.setColorPresentations({{1, ColorPresentation{}}}).ok);
        viewport.resetCamera();
        viewport.repaint();
        QApplication::processEvents();
        const QImage original = captureRenderedFrame(viewport);
        QVERIFY(!original.isNull());
        int tintedRedPixels = 0;
        int greenPixels = 0;
        int untintedRedPixels = 0;
        QVector<QPoint> texturePixels;
        for (int y = 100; y < original.height(); ++y) {
            for (int x = 0; x < original.width(); ++x) {
                const QColor pixel = original.pixelColor(x, y);
                if (pixel.green() < 5 && pixel.blue() < 5) {
                    if (std::abs(pixel.red() - 128) <= 3) {
                        ++tintedRedPixels;
                        texturePixels.append(QPoint(x, y));
                    }
                    if (pixel.red() > 240)
                        ++untintedRedPixels;
                }
                if (pixel.green() > 240 && pixel.red() < 5 && pixel.blue() < 5) {
                    ++greenPixels;
                    texturePixels.append(QPoint(x, y));
                }
            }
        }
        QVERIFY2(tintedRedPixels > 500, qPrintable(QStringLiteral("Tinted red pixels: %1").arg(tintedRedPixels)));
        QVERIFY2(greenPixels > 500, qPrintable(QStringLiteral("Green pixels: %1").arg(greenPixels)));
        QCOMPARE(untintedRedPixels, 0);
        ColorPresentation uniform;
        uniform.mode = ColorMode::UniformColor;
        uniform.uniformColor = Qt::blue;
        QVERIFY(scene.setColorPresentations({{1, uniform}}).ok);
        viewport.repaint();
        const QImage colored = captureRenderedFrame(viewport);
        QCOMPARE(colored.size(), original.size());
        int uniformBluePixels = 0;
        for (const QPoint& position : texturePixels) {
            const QColor pixel = colored.pixelColor(position);
            if (pixel.blue() > pixel.red() + 40 && pixel.blue() > pixel.green() + 40)
                ++uniformBluePixels;
        }
        // Assert that the model itself changes, independent of how much of
        // the framebuffer the fitted triangle occupies on this platform.
        if (uniformBluePixels * 100 < texturePixels.size() * 95) {
            original.save(QDir::temp().filePath(QStringLiteral("meshcompare-source-original.png")));
            colored.save(QDir::temp().filePath(QStringLiteral("meshcompare-source-uniform.png")));
        }
        QVERIFY2(uniformBluePixels * 100 >= texturePixels.size() * 95,
            qPrintable(QStringLiteral("Uniform blue replaced %1 of %2 source texture pixels")
                .arg(uniformBluePixels).arg(texturePixels.size())));
        QVERIFY(scene.setColorPresentations({{1, ColorPresentation{}}}).ok);
        viewport.repaint();
        const QImage restored = captureRenderedFrame(viewport);
        QCOMPARE(restored.size(), original.size());
        int restoredTexturePixels = 0;
        for (const QPoint& position : texturePixels) {
            const QColor before = original.pixelColor(position);
            const QColor after = restored.pixelColor(position);
            if (std::abs(before.red() - after.red()) <= MaximumChannelDifference &&
                std::abs(before.green() - after.green()) <= MaximumChannelDifference &&
                std::abs(before.blue() - after.blue()) <= MaximumChannelDifference)
                ++restoredTexturePixels;
        }
        QVERIFY2(restoredTexturePixels * 100 >= texturePixels.size() * 99,
            qPrintable(QStringLiteral("Restored %1 of %2 source texture pixels")
                .arg(restoredTexturePixels).arg(texturePixels.size())));
        QCOMPARE(callbacks.rendererErrorCount(), 0);
    }

    void finalFaceTextureChangesDoNotLeakIntoPlainFaces_data()
    {
        QTest::addColumn<bool>("finalFaceIsPlain");
        QTest::addColumn<bool>("useBuffers");
        QTest::newRow("last-face-plain-buffered") << true << true;
        QTest::newRow("last-face-textured-buffered") << false << true;
        QTest::newRow("last-face-plain-immediate") << true << false;
        QTest::newRow("last-face-textured-immediate") << false << false;
    }

    void finalFaceTextureChangesDoNotLeakIntoPlainFaces()
    {
        QFETCH(bool, finalFaceIsPlain);
        QFETCH(bool, useBuffers);
        MultipleTextureTriangles resources(finalFaceIsPlain);
        RenderSceneSettings renderSettings;
        if (!useBuffers)
            renderSettings.gpuMemoryBytes = 0;
        RenderSceneContext scene(renderSettings);
        SceneDescriptor descriptor;
        descriptor.generation = 1;
        descriptor.referenceId = 1;
        descriptor.meshes.append({1, 1, QStringLiteral("texture groups"), true});
        QVERIFY(scene.addResources(descriptor, resources).ok);
        RichParameterList settings;
        FakeViewportCallbacks callbacks;
        ViewportDependencies dependencies{
            scene.document(), scene.sharedContext(), settings, callbacks,
            1, scene.modelIdFor(1), 1, 1, QStringLiteral("texture groups"),
            false, QString(), false};
        MeshLabViewport viewport(nullptr, dependencies);
        viewport.resize(600, 400);
        viewport.show();
        QVERIFY(QTest::qWaitForWindowExposed(&viewport, 5000));
        QVERIFY(viewport.initializeForScenePreparation().ok);
        QVERIFY(scene.setColorPresentations({{1, ColorPresentation{}}}).ok);
        QCOMPARE(scene.sharedContext().isBORenderingAvailable(scene.modelIdFor(1)), useBuffers);
        viewport.resetCamera();
        viewport.repaint();
        QApplication::processEvents();
        const QImage image = captureRenderedFrame(viewport);
        QVERIFY(!image.isNull());
        int red = 0, green = 0, blue = 0;
        QPointF redCenter, greenCenter, blueCenter;
        for (int y = 100; y < image.height(); ++y) {
            for (int x = 0; x < image.width(); ++x) {
                const QColor pixel = image.pixelColor(x, y);
                if (pixel.red() > 240 && pixel.green() < 5 && pixel.blue() < 5) {
                    ++red;
                    redCenter += QPointF(x, y);
                }
                if (pixel.green() > 240 && pixel.red() < 5 && pixel.blue() < 5) {
                    ++green;
                    greenCenter += QPointF(x, y);
                }
                if (pixel.blue() > 240 && pixel.red() < 5 && pixel.green() < 5) {
                    ++blue;
                    blueCenter += QPointF(x, y);
                }
            }
        }
        QVERIFY2(red > 300 && green > 300 && blue > 300,
            qPrintable(QStringLiteral("Expected three colored triangles, got red=%1, green=%2, blue=%3")
                .arg(red).arg(green).arg(blue)));
        // Color each original face differently. Texture batching can reorder
        // geometry; analysis colors must still refer to the original face IDs.
        const std::array<QPoint, 3> originalFaceCenters = {{
            (redCenter / red).toPoint(),
            (finalFaceIsPlain ? greenCenter / green : blueCenter / blue).toPoint(),
            (finalFaceIsPlain ? blueCenter / blue : greenCenter / green).toPoint()}};
        ColorPresentation analysis;
        analysis.mode = ColorMode::PrecisionResult;
        analysis.faceColors = {QColor(Qt::yellow), QColor(Qt::cyan), QColor(Qt::magenta)};
        QVERIFY(scene.setColorPresentations({{1, analysis}}).ok);
        viewport.repaint();
        const QImage analyzed = captureRenderedFrame(viewport);
        QVERIFY(!analyzed.isNull());
        const QColor first = analyzed.pixelColor(originalFaceCenters[0]);
        const QColor second = analyzed.pixelColor(originalFaceCenters[1]);
        const QColor third = analyzed.pixelColor(originalFaceCenters[2]);
        QVERIFY2(first.red() > first.blue() + 70 && first.green() > first.blue() + 70,
                 qPrintable(QStringLiteral("First face should be yellow, got %1").arg(first.name())));
        QVERIFY2(second.green() > second.red() + 70 && second.blue() > second.red() + 70,
                 qPrintable(QStringLiteral("Second face should be cyan, got %1").arg(second.name())));
        QVERIFY2(third.red() > third.green() + 70 && third.blue() > third.green() + 70,
                 qPrintable(QStringLiteral("Third face should be magenta, got %1").arg(third.name())));
        QVERIFY(scene.setColorPresentations({{1, ColorPresentation{}}}).ok);
        viewport.repaint();
        const ImageDiff restored = compareImages(captureRenderedFrame(viewport), image, MaximumChannelDifference);
        QVERIFY2(restored.equalPixelRatio >= RequiredEqualPixelRatio, qPrintable(restored.summary()));
        QCOMPARE(callbacks.rendererErrorCount(), 0);
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
