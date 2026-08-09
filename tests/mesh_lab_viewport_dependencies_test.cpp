#include <QtTest>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>

#include <QApplication>
#include <QByteArray>
#include <QDomDocument>
#include <QFontMetrics>
#include <QImage>
#include <QMouseEvent>
#include <QRegularExpression>
#include <QWheelEvent>

#include <common/ml_document/mesh_document.h>
#include <common/ml_shared_data_context/ml_scene_gl_shared_data_context.h>
#include <common/parameters/rich_parameter_list.h>
#include <common/parameters/rich_parameter/rich_color.h>
#include <wrap/qt/qt_thread_safe_memory_info.h>

#include "renderer/meshlab/glarea_setting.h"
#include "renderer/meshlab/mesh_lab_viewport.h"
#include "fakes/fake_viewport_callbacks.h"

class TestRenderScene
{
public:
    TestRenderScene()
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

CameraPose withViewSettings(
    const CameraPose& pose,
    const QString& scale,
    const QString& nearPlane,
    const QString& farPlane)
{
    QString viewStateXml = pose.viewStateXml;
    const auto replaceAttribute = [&viewStateXml](const QString& name, const QString& value) {
        const QRegularExpression expression(
            QStringLiteral("%1=\"[^\"]*\"").arg(name));
        const QRegularExpressionMatch match = expression.match(viewStateXml);
        Q_ASSERT(match.hasMatch());
        viewStateXml.replace(
            match.capturedStart(),
            match.capturedLength(),
            QStringLiteral("%1=\"%2\"").arg(name, value));
    };

    replaceAttribute(QStringLiteral("TrackScale"), scale);
    replaceAttribute(QStringLiteral("NearPlane"), nearPlane);
    replaceAttribute(QStringLiteral("FarPlane"), farPlane);
    return {viewStateXml};
}

QString cameraAttribute(const CameraPose& pose, const QString& name)
{
    QDomDocument document;
    if (!document.setContent(pose.viewStateXml))
        return {};
    return document.documentElement()
        .firstChildElement(QStringLiteral("VCGCamera"))
        .attribute(name);
}

QString viewSettingsAttribute(const CameraPose& pose, const QString& name)
{
    QDomDocument document;
    if (!document.setContent(pose.viewStateXml))
        return {};
    return document.documentElement()
        .firstChildElement(QStringLiteral("ViewSettings"))
        .attribute(name);
}

double maximumCameraAttributeDifference(
    const CameraPose& left,
    const CameraPose& right,
    const QString& name)
{
    const QStringList leftValues =
        cameraAttribute(left, name).split(QLatin1Char(' '), Qt::SkipEmptyParts);
    const QStringList rightValues =
        cameraAttribute(right, name).split(QLatin1Char(' '), Qt::SkipEmptyParts);
    if (leftValues.size() != rightValues.size() || leftValues.isEmpty())
        return std::numeric_limits<double>::infinity();

    double maximum = 0.0;
    for (int index = 0; index < leftValues.size(); ++index) {
        bool leftValid = false;
        bool rightValid = false;
        const double leftValue = leftValues.at(index).toDouble(&leftValid);
        const double rightValue = rightValues.at(index).toDouble(&rightValid);
        if (!leftValid || !rightValid)
            return std::numeric_limits<double>::infinity();
        maximum = std::max(maximum, std::abs(leftValue - rightValue));
    }
    return maximum;
}

template <typename Value, std::size_t Count>
QString encodeBase64(const Value (&values)[Count])
{
    return QString::fromLatin1(
        QByteArray(
            reinterpret_cast<const char*>(values),
            static_cast<int>(sizeof(values)))
            .toBase64());
}

CameraPose binaryVcgCameraPoseWith(
    const QString& attributeName,
    const QString& attributeValue)
{
    const Scalarm translation[] = {0.0f, 0.0f, 0.0f};
    const Scalarm rotation[] = {
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f};
    const Scalarm focalMm[] = {12.0f};
    const int viewport[] = {640, 480};
    const Scalarm center[] = {320.0f, 240.0f};
    const Scalarm pixelSizeMm[] = {0.1f, 0.1f};
    const Scalarm lensDistortion[] = {0.0f, 0.0f};

    QDomDocument document;
    QDomElement root = document.createElement(QStringLiteral("project"));
    document.appendChild(root);
    QDomElement camera = document.createElement(QStringLiteral("VCGCamera"));
    camera.setAttribute(QStringLiteral("BinaryData"), QStringLiteral("1"));
    camera.setAttribute(QStringLiteral("TranslationVector"), encodeBase64(translation));
    camera.setAttribute(QStringLiteral("RotationMatrix"), encodeBase64(rotation));
    camera.setAttribute(QStringLiteral("CameraType"), QStringLiteral("0"));
    camera.setAttribute(QStringLiteral("FocalMm"), encodeBase64(focalMm));
    camera.setAttribute(QStringLiteral("ViewportPx"), encodeBase64(viewport));
    camera.setAttribute(QStringLiteral("CenterPx"), encodeBase64(center));
    camera.setAttribute(QStringLiteral("PixelSizeMm"), encodeBase64(pixelSizeMm));
    camera.setAttribute(QStringLiteral("LensDistortion"), encodeBase64(lensDistortion));
    camera.setAttribute(attributeName, attributeValue);
    root.appendChild(camera);

    QDomElement settings = document.createElement(QStringLiteral("ViewSettings"));
    settings.setAttribute(QStringLiteral("TrackScale"), QStringLiteral("1"));
    settings.setAttribute(QStringLiteral("NearPlane"), QStringLiteral("0.1"));
    settings.setAttribute(QStringLiteral("FarPlane"), QStringLiteral("500"));
    root.appendChild(settings);
    return {document.toString()};
}

CameraPose binaryOrthographicVcgCameraPose()
{
    const Scalarm focalMm[] = {0.0f};
    CameraPose pose = binaryVcgCameraPoseWith(
        QStringLiteral("CameraType"),
        QStringLiteral("1"));
    pose.viewStateXml.replace(
        QRegularExpression(QStringLiteral("FocalMm=\"[^\"]*\"")),
        QStringLiteral("FocalMm=\"%1\"").arg(encodeBase64(focalMm)));
    return pose;
}

CameraPose validLegacyCamParamPose()
{
    return {QStringLiteral(
        "<project><CamParam SimTra=\"0 0 0\" "
        "SimRot=\"1 0 0 0 0 1 0 0 0 0 1 0 0 0 0 1\" "
        "Focal=\"12\" Viewport=\"640 480\" Center=\"320 240\" "
        "ScaleF=\"0.1 0.1\" LensDist=\"0 0\" ScaleCorr=\"1\"/>"
        "<ViewSettings TrackScale=\"1\" NearPlane=\"0.1\" FarPlane=\"500\"/>"
        "</project>")};
}

CameraPose validLegacyCamParamPoseWithoutScaleCorrection()
{
    return {QStringLiteral(
        "<project><CamParam SimTra=\"0 0 0\" "
        "SimRot=\"1 0 0 0 0 1 0 0 0 0 1 0 0 0 0 1\" "
        "Focal=\"12\" Viewport=\"640 480\" Center=\"320 240\" "
        "ScaleF=\"0.1 0.1\" LensDist=\"0 0\"/>"
        "<ViewSettings TrackScale=\"1\" NearPlane=\"0.1\" FarPlane=\"500\"/>"
        "</project>")};
}

class MeshLabViewportDependenciesTest : public QObject
{
    Q_OBJECT

private slots:
    void cameraChangesUseInjectedCallbacks()
    {
        TestRenderScene scene;
        FakeViewportCallbacks callbacks;
        ViewportDependencies deps{
            scene.document(),
            scene.sharedContext(),
            scene.settings(),
            callbacks,
            1,
            1,
            1,
            1,
            QStringLiteral("GT"),
            true,
            QStringLiteral("0.93")};
        MeshLabViewport viewport(nullptr, deps);

        viewport.notifyCameraChangedForTest();

        QCOMPARE(callbacks.cameraChangeCount(), 1);
        QCOMPARE(callbacks.lastCameraViewportId(), 1);
        QVERIFY(!callbacks.lastCameraPose().viewStateXml.isEmpty());
    }

    void mousePressActivatesInjectedViewport()
    {
        TestRenderScene scene;
        FakeViewportCallbacks callbacks;
        ViewportDependencies deps{
            scene.document(),
            scene.sharedContext(),
            scene.settings(),
            callbacks,
            7,
            1,
            1,
            1,
            QStringLiteral("GT"),
            true,
            QString()};
        MeshLabViewport viewport(nullptr, deps);
        QMouseEvent press(
            QEvent::MouseButtonPress,
            QPointF(10, 10),
            Qt::LeftButton,
            Qt::LeftButton,
            Qt::NoModifier);

        QApplication::sendEvent(&viewport, &press);

        QCOMPARE(callbacks.viewportActivationCount(), 1);
        QCOMPARE(callbacks.lastActivatedViewportId(), 7);
    }

    void referenceBadgeStateCanBeUpdatedWithoutReconstructingTheViewport()
    {
        TestRenderScene scene;
        FakeViewportCallbacks callbacks;
        ViewportDependencies deps{
            scene.document(),
            scene.sharedContext(),
            scene.settings(),
            callbacks,
            1,
            1,
            1,
            2,
            QStringLiteral("GT"),
            true,
            QString(),
            true};
        MeshLabViewport viewport(nullptr, deps);
        QVERIFY(viewport.isReference());

        viewport.setReference(false);

        QVERIFY(!viewport.isReference());
    }

    void analysisOverlayLabelCanChangeBetweenProgressScoreAndEmpty()
    {
        TestRenderScene scene;
        FakeViewportCallbacks callbacks;
        ViewportDependencies deps{
            scene.document(),
            scene.sharedContext(),
            scene.settings(),
            callbacks,
            1,
            1,
            1,
            2,
            QStringLiteral("candidate"),
            false,
            QString()};
        MeshLabViewport viewport(nullptr, deps);

        viewport.setScoreLabel(QStringLiteral("64%"));
        QCOMPARE(viewport.scoreLabelForTest(), QStringLiteral("64%"));

        viewport.setScoreLabel(QStringLiteral("P 0.942"));
        QCOMPARE(viewport.scoreLabelForTest(), QStringLiteral("P 0.942"));

        viewport.setScoreLabel(QStringLiteral("N 0.917"));
        QCOMPARE(viewport.scoreLabelForTest(), QStringLiteral("N 0.917"));

        viewport.setScoreLabel(QString());
        QCOMPARE(viewport.scoreLabelForTest(), QString());
    }

    void analysisOverlayTextIsVisibleInTheRenderedBadge()
    {
        TestRenderScene scene;
        MeshModel* mesh = scene.document().addNewMesh(
            QString(),
            QStringLiteral("overlay-probe"),
            false);
        vcg::tri::Allocator<CMeshO>::AddVertices(mesh->cm, 3);
        mesh->cm.vert[0].P() = CMeshO::CoordType(0, 0, 0);
        mesh->cm.vert[1].P() = CMeshO::CoordType(1, 0, 0);
        mesh->cm.vert[2].P() = CMeshO::CoordType(0, 1, 0);
        vcg::tri::Allocator<CMeshO>::AddFaces(mesh->cm, 1);
        mesh->cm.face[0].V(0) = &mesh->cm.vert[0];
        mesh->cm.face[0].V(1) = &mesh->cm.vert[1];
        mesh->cm.face[0].V(2) = &mesh->cm.vert[2];
        mesh->updateBoxAndNormals();

        FakeViewportCallbacks callbacks;
        const QString scoreLabel = QStringLiteral("P 0.942");
        ViewportDependencies deps{
            scene.document(),
            scene.sharedContext(),
            scene.settings(),
            callbacks,
            2,
            mesh->id(),
            1,
            1,
            QStringLiteral("candidate"),
            false,
            scoreLabel};
        MeshLabViewport viewport(nullptr, deps);
        viewport.resize(640, 480);
        viewport.show();
        QTest::qWait(20);
        const OperationResult initialized =
            viewport.initializeForScenePreparation();
        QVERIFY2(initialized.ok, qPrintable(initialized.error));
        viewport.repaint();
        QApplication::processEvents();

        QImage frame;
        const OperationResult captured = viewport.captureImage(frame);
        QVERIFY2(captured.ok, qPrintable(captured.error));
        QVERIFY(!frame.isNull());
        QFont font = viewport.font();
        font.setPointSizeF(qMax(9.0, font.pointSizeF()));
        font.setWeight(QFont::DemiBold);
        const QFontMetrics metrics(font);
        const int logicalWidth = metrics.horizontalAdvance(scoreLabel) + 16;
        const QRect logicalBadge(
            viewport.width() - logicalWidth - 12,
            12,
            logicalWidth,
            metrics.height() + 16);
        const qreal scaleX = qreal(frame.width()) / viewport.width();
        const qreal scaleY = qreal(frame.height()) / viewport.height();
        const QRect pixelBadge(
            qRound(logicalBadge.x() * scaleX),
            qRound(logicalBadge.y() * scaleY),
            qRound(logicalBadge.width() * scaleX),
            qRound(logicalBadge.height() * scaleY));
        int badgePixels = 0;
        int nearWhitePixels = 0;
        for (int y = pixelBadge.top(); y <= pixelBadge.bottom(); ++y) {
            for (int x = pixelBadge.left(); x <= pixelBadge.right(); ++x) {
                const QColor pixel = frame.pixelColor(x, y);
                if (pixel.blue() >= 80 && pixel.blue() <= 190 &&
                    pixel.blue() >= pixel.green() + 15 &&
                    pixel.green() >= pixel.red() + 25 && pixel.red() < 90) {
                    ++badgePixels;
                }
                if (pixel.red() >= 220 && pixel.green() >= 220 &&
                    pixel.blue() >= 220) {
                    ++nearWhitePixels;
                }
            }
        }

        QVERIFY2(
            badgePixels >= 100,
            qPrintable(QStringLiteral(
                "Expected blue score-badge pixels; found %1.")
                           .arg(badgePixels)));
        QVERIFY2(
            nearWhitePixels >= 8,
            qPrintable(QStringLiteral(
                "Expected white score text pixels in the analysis badge; found %1.")
                           .arg(nearWhitePixels)));
    }

    void capturedCameraUsesMeshLabViewStateXml()
    {
        TestRenderScene scene;
        FakeViewportCallbacks callbacks;
        ViewportDependencies deps{
            scene.document(),
            scene.sharedContext(),
            scene.settings(),
            callbacks,
            1,
            1,
            1,
            1,
            QStringLiteral("GT"),
            true,
            QString()};
        MeshLabViewport viewport(nullptr, deps);
        const CameraPose pose = viewport.captureCamera();
        QDomDocument document;

        QVERIFY(document.setContent(pose.viewStateXml));
        QCOMPARE(document.documentElement().tagName(), QStringLiteral("project"));
        QVERIFY(!document.documentElement().firstChildElement(QStringLiteral("VCGCamera")).isNull());
        QVERIFY(!document.documentElement().firstChildElement(QStringLiteral("ViewSettings")).isNull());
    }

    void orthographicDiagnosticIsCapturedAndCanBeDisabledAfterRestore()
    {
        TestRenderScene scene;
        FakeViewportCallbacks callbacks;
        ViewportDependencies deps{
            scene.document(),
            scene.sharedContext(),
            scene.settings(),
            callbacks,
            1,
            1,
            1,
            1,
            QStringLiteral("GT"),
            true,
            QString()};
        MeshLabViewport viewport(nullptr, deps);
        viewport.resize(640, 480);

        const CameraPose perspective = viewport.captureCamera();
        QCOMPARE(
            cameraAttribute(perspective, QStringLiteral("CameraType")),
            QStringLiteral("0"));
        viewport.setDiagnostic(DiagnosticFlag::Orthographic, true);
        const CameraPose orthographic = viewport.captureCamera();
        QCOMPARE(
            cameraAttribute(orthographic, QStringLiteral("CameraType")),
            QStringLiteral("1"));
        QVERIFY(maximumCameraAttributeDifference(
                    perspective,
                    orthographic,
                    QStringLiteral("TranslationVector")) <= 1e-4);
        QVERIFY(maximumCameraAttributeDifference(
                    perspective,
                    orthographic,
                    QStringLiteral("RotationMatrix")) <= 1e-4);

        viewport.setDiagnostic(DiagnosticFlag::Orthographic, false);
        const CameraPose perspectiveAgain = viewport.captureCamera();
        QCOMPARE(
            cameraAttribute(perspectiveAgain, QStringLiteral("CameraType")),
            QStringLiteral("0"));
        QVERIFY(maximumCameraAttributeDifference(
                    orthographic,
                    perspectiveAgain,
                    QStringLiteral("TranslationVector")) <= 1e-4);

        QVERIFY(viewport.restoreCamera(orthographic).ok);
        const CameraPose restoredOrthographic = viewport.captureCamera();
        QCOMPARE(
            cameraAttribute(restoredOrthographic, QStringLiteral("CameraType")),
            QStringLiteral("1"));
        viewport.setDiagnostic(DiagnosticFlag::Orthographic, false);
        const CameraPose restoredPerspective = viewport.captureCamera();
        QCOMPARE(
            cameraAttribute(restoredPerspective, QStringLiteral("CameraType")),
            QStringLiteral("0"));
        QVERIFY(maximumCameraAttributeDifference(
                    restoredOrthographic,
                    restoredPerspective,
                    QStringLiteral("TranslationVector")) <= 1e-4);
        QVERIFY(maximumCameraAttributeDifference(
                    restoredOrthographic,
                    restoredPerspective,
                    QStringLiteral("RotationMatrix")) <= 1e-4);
    }

    void doubleSidedRenderingOverridesAndRestoresMeshOptions()
    {
        TestRenderScene scene;
        FakeViewportCallbacks callbacks;
        ViewportDependencies deps{
            scene.document(),
            scene.sharedContext(),
            scene.settings(),
            callbacks,
            1,
            1,
            1,
            1,
            QStringLiteral("GT"),
            true,
            QString()};
        MeshLabViewport viewport(nullptr, deps);

        QVERIFY(viewport.usesBackFaceCulling(true));
        QVERIFY(!viewport.usesBackFaceCulling(false));
        QVERIFY(!viewport.usesDoubleSidedLighting(false));
        QVERIFY(viewport.usesDoubleSidedLighting(true));

        viewport.setDiagnostic(DiagnosticFlag::DoubleSided, true);

        QVERIFY(!viewport.usesBackFaceCulling(true));
        QVERIFY(!viewport.usesBackFaceCulling(false));
        QVERIFY(viewport.usesDoubleSidedLighting(false));
        QVERIFY(viewport.usesDoubleSidedLighting(true));

        viewport.setDiagnostic(DiagnosticFlag::DoubleSided, false);

        QVERIFY(viewport.usesBackFaceCulling(true));
        QVERIFY(!viewport.usesDoubleSidedLighting(false));
    }

    void lowFovPerspectiveRemainsPerspectiveAndPreservesClipping()
    {
        TestRenderScene scene;
        FakeViewportCallbacks callbacks;
        ViewportDependencies deps{
            scene.document(),
            scene.sharedContext(),
            scene.settings(),
            callbacks,
            1,
            1,
            1,
            1,
            QStringLiteral("GT"),
            true,
            QString()};
        MeshLabViewport viewport(nullptr, deps);
        viewport.resize(640, 480);
        CameraPose lowFov = withViewSettings(
            viewport.captureCamera(),
            QStringLiteral("1"),
            QStringLiteral("0.2"),
            QStringLiteral("200"));
        lowFov.viewStateXml.replace(
            QRegularExpression(QStringLiteral("FocalMm=\"[^\"]*\"")),
            QStringLiteral("FocalMm=\"1000\""));

        const OperationResult restored = viewport.restoreCamera(lowFov);

        QVERIFY2(restored.ok, qPrintable(restored.error));
        QCOMPARE(
            cameraAttribute(viewport.captureCamera(), QStringLiteral("CameraType")),
            QStringLiteral("0"));

        viewport.show();
        QTest::qWait(20);
        const OperationResult initialized =
            viewport.initializeForScenePreparation();
        QVERIFY2(initialized.ok, qPrintable(initialized.error));
        viewport.repaint();
        QApplication::processEvents();
        const CameraPose afterRender = viewport.captureCamera();
        QVERIFY(std::abs(
                    viewSettingsAttribute(
                        afterRender, QStringLiteral("NearPlane"))
                            .toDouble() -
                    0.2) <= 1e-5);
        QVERIFY(std::abs(
                    viewSettingsAttribute(
                        afterRender, QStringLiteral("FarPlane"))
                            .toDouble() -
                    200.0) <= 1e-4);
    }

    void capturedCameraCanBeRestored()
    {
        TestRenderScene scene;
        FakeViewportCallbacks callbacks;
        ViewportDependencies deps{
            scene.document(),
            scene.sharedContext(),
            scene.settings(),
            callbacks,
            1,
            1,
            1,
            1,
            QStringLiteral("GT"),
            true,
            QString()};
        MeshLabViewport viewport(nullptr, deps);
        const CameraPose pose = viewport.captureCamera();

        const OperationResult result = viewport.restoreCamera(pose);
        QVERIFY2(result.ok, qPrintable(result.error));
    }

    void restoredCameraPreservesFocalLengthWithoutRoundTripDrift()
    {
        TestRenderScene scene;
        FakeViewportCallbacks callbacks;
        ViewportDependencies deps{
            scene.document(),
            scene.sharedContext(),
            scene.settings(),
            callbacks,
            1,
            1,
            1,
            1,
            QStringLiteral("GT"),
            true,
            QString()};
        MeshLabViewport viewport(nullptr, deps);
        viewport.resize(500, 600);
        CameraPose restored = viewport.captureCamera();
        restored.viewStateXml.replace(
            QRegularExpression(QStringLiteral("FocalMm=\"[^\"]*\"")),
            QStringLiteral("FocalMm=\"12\""));

        const OperationResult result = viewport.restoreCamera(restored);

        QVERIFY2(result.ok, qPrintable(result.error));
        QCOMPARE(
            cameraAttribute(viewport.captureCamera(), QStringLiteral("FocalMm")),
            QStringLiteral("12"));

        viewport.resetCamera();

        QVERIFY(
            cameraAttribute(viewport.captureCamera(), QStringLiteral("FocalMm")) !=
            QStringLiteral("12"));
    }

    void restoreCameraRejectsInvalidViewSettings_data()
    {
        QTest::addColumn<QString>("scale");
        QTest::addColumn<QString>("nearPlane");
        QTest::addColumn<QString>("farPlane");

        QTest::newRow("negative-scale") << QStringLiteral("-1") << QStringLiteral("0.1")
                                          << QStringLiteral("500");
        QTest::newRow("zero-scale") << QStringLiteral("0") << QStringLiteral("0.1")
                                      << QStringLiteral("500");
        QTest::newRow("subnormal-positive-scale") << QStringLiteral("1e-40")
                                                    << QStringLiteral("0.1")
                                                    << QStringLiteral("500");
        QTest::newRow("negative-near-plane") << QStringLiteral("1") << QStringLiteral("-0.1")
                                              << QStringLiteral("500");
        QTest::newRow("zero-far-plane") << QStringLiteral("1") << QStringLiteral("0.1")
                                         << QStringLiteral("0");
        QTest::newRow("equal-planes") << QStringLiteral("1") << QStringLiteral("10")
                                       << QStringLiteral("10");
        QTest::newRow("inverted-planes") << QStringLiteral("1") << QStringLiteral("10")
                                          << QStringLiteral("1");
    }

    void restoreCameraRejectsInvalidViewSettings()
    {
        QFETCH(QString, scale);
        QFETCH(QString, nearPlane);
        QFETCH(QString, farPlane);

        TestRenderScene scene;
        FakeViewportCallbacks callbacks;
        ViewportDependencies deps{
            scene.document(),
            scene.sharedContext(),
            scene.settings(),
            callbacks,
            1,
            1,
            1,
            1,
            QStringLiteral("GT"),
            true,
            QString()};
        MeshLabViewport viewport(nullptr, deps);
        const CameraPose before = viewport.captureCamera();
        const CameraPose invalid = withViewSettings(before, scale, nearPlane, farPlane);

        const OperationResult result = viewport.restoreCamera(invalid);
        QVERIFY2(!result.ok, qPrintable(result.error));
        QVERIFY(!result.error.isEmpty());
        QCOMPARE(viewport.captureCamera().viewStateXml, before.viewStateXml);
    }

    void restoreCameraRejectsStructurallyInvalidVcgCamera_data()
    {
        QTest::addColumn<QString>("kind");

        QTest::newRow("incomplete") << QStringLiteral("incomplete");
        QTest::newRow("short-text-rotation") << QStringLiteral("short-text-rotation");
        QTest::newRow("non-numeric-text-focal-length") << QStringLiteral("non-numeric-text-focal-length");
        QTest::newRow("invalid-binary-base64") << QStringLiteral("invalid-binary-base64");
        QTest::newRow("short-binary-rotation") << QStringLiteral("short-binary-rotation");
        QTest::newRow("non-finite-binary-rotation") << QStringLiteral("non-finite-binary-rotation");
        QTest::newRow("infinite-binary-translation") << QStringLiteral("infinite-binary-translation");
        QTest::newRow("zero-binary-viewport") << QStringLiteral("zero-binary-viewport");
    }

    void restoreCameraRejectsStructurallyInvalidVcgCamera()
    {
        QFETCH(QString, kind);

        TestRenderScene scene;
        FakeViewportCallbacks callbacks;
        ViewportDependencies deps{
            scene.document(),
            scene.sharedContext(),
            scene.settings(),
            callbacks,
            1,
            1,
            1,
            1,
            QStringLiteral("GT"),
            true,
            QString()};
        MeshLabViewport viewport(nullptr, deps);
        const CameraPose before = viewport.captureCamera();
        CameraPose invalid;
        if (kind == QStringLiteral("incomplete")) {
            invalid = {QStringLiteral(
                "<project><VCGCamera/><ViewSettings TrackScale=\"1\" NearPlane=\"0.1\" "
                "FarPlane=\"500\"/></project>")};
        }
        else if (kind == QStringLiteral("short-text-rotation")) {
            invalid = withViewSettings(before, QStringLiteral("1"), QStringLiteral("0.1"),
                                       QStringLiteral("500"));
            invalid.viewStateXml.replace(
                QRegularExpression(QStringLiteral("RotationMatrix=\"[^\"]*\"")),
                QStringLiteral("RotationMatrix=\"1 0 0\""));
        }
        else if (kind == QStringLiteral("non-numeric-text-focal-length")) {
            invalid = before;
            invalid.viewStateXml.replace(
                QRegularExpression(QStringLiteral("FocalMm=\"[^\"]*\"")),
                QStringLiteral("FocalMm=\"not-a-number\""));
        }
        else if (kind == QStringLiteral("invalid-binary-base64")) {
            invalid = binaryVcgCameraPoseWith(
                QStringLiteral("RotationMatrix"),
                QStringLiteral("not-base64!"));
        }
        else if (kind == QStringLiteral("short-binary-rotation")) {
            invalid = binaryVcgCameraPoseWith(
                QStringLiteral("RotationMatrix"),
                QStringLiteral("AAAAAA=="));
        }
        else if (kind == QStringLiteral("non-finite-binary-rotation")) {
            const Scalarm rotation[] = {
                std::numeric_limits<Scalarm>::quiet_NaN(), 0.0f, 0.0f, 0.0f,
                0.0f, 1.0f, 0.0f, 0.0f,
                0.0f, 0.0f, 1.0f, 0.0f,
                0.0f, 0.0f, 0.0f, 1.0f};
            invalid = binaryVcgCameraPoseWith(
                QStringLiteral("RotationMatrix"),
                encodeBase64(rotation));
        }
        else if (kind == QStringLiteral("infinite-binary-translation")) {
            const Scalarm translation[] = {
                std::numeric_limits<Scalarm>::infinity(), 0.0f, 0.0f};
            invalid = binaryVcgCameraPoseWith(
                QStringLiteral("TranslationVector"),
                encodeBase64(translation));
        }
        else {
            const int viewport[] = {0, 480};
            invalid = binaryVcgCameraPoseWith(
                QStringLiteral("ViewportPx"),
                encodeBase64(viewport));
        }

        const OperationResult result = viewport.restoreCamera(invalid);
        QVERIFY2(!result.ok, qPrintable(result.error));
        QVERIFY(result.error.contains(QStringLiteral("VCG camera"), Qt::CaseInsensitive));
        QCOMPARE(viewport.captureCamera().viewStateXml, before.viewStateXml);
    }

    void binaryVcgCameraCanBeRestored()
    {
        TestRenderScene scene;
        FakeViewportCallbacks callbacks;
        ViewportDependencies deps{
            scene.document(),
            scene.sharedContext(),
            scene.settings(),
            callbacks,
            1,
            1,
            1,
            1,
            QStringLiteral("GT"),
            true,
            QString()};
        MeshLabViewport viewport(nullptr, deps);

        const OperationResult result = viewport.restoreCamera(
            binaryVcgCameraPoseWith(QStringLiteral("CameraType"), QStringLiteral("0")));

        QVERIFY2(result.ok, qPrintable(result.error));
    }

    void binaryOrthographicVcgCameraCanBeRestored()
    {
        TestRenderScene scene;
        FakeViewportCallbacks callbacks;
        ViewportDependencies deps{
            scene.document(),
            scene.sharedContext(),
            scene.settings(),
            callbacks,
            1,
            1,
            1,
            1,
            QStringLiteral("GT"),
            true,
            QString()};
        MeshLabViewport viewport(nullptr, deps);

        const OperationResult result = viewport.restoreCamera(binaryOrthographicVcgCameraPose());

        QVERIFY2(result.ok, qPrintable(result.error));
    }

    void legacyCamParamCanBeRestored()
    {
        TestRenderScene scene;
        FakeViewportCallbacks callbacks;
        ViewportDependencies deps{
            scene.document(),
            scene.sharedContext(),
            scene.settings(),
            callbacks,
            1,
            1,
            1,
            1,
            QStringLiteral("GT"),
            true,
            QString()};
        MeshLabViewport viewport(nullptr, deps);

        const OperationResult result = viewport.restoreCamera(validLegacyCamParamPose());

        QVERIFY2(result.ok, qPrintable(result.error));
    }

    void legacyCamParamWithoutScaleCorrectionCanBeRestored()
    {
        TestRenderScene scene;
        FakeViewportCallbacks callbacks;
        ViewportDependencies deps{
            scene.document(),
            scene.sharedContext(),
            scene.settings(),
            callbacks,
            1,
            1,
            1,
            1,
            QStringLiteral("GT"),
            true,
            QString()};
        MeshLabViewport viewport(nullptr, deps);

        const OperationResult result = viewport.restoreCamera(
            validLegacyCamParamPoseWithoutScaleCorrection());

        QVERIFY2(result.ok, qPrintable(result.error));
    }

    void restoreCameraRejectsStructurallyInvalidLegacyCamParam_data()
    {
        QTest::addColumn<QString>("kind");

        QTest::newRow("incomplete") << QStringLiteral("incomplete");
        QTest::newRow("short-rotation") << QStringLiteral("short-rotation");
        QTest::newRow("non-numeric-focal") << QStringLiteral("non-numeric-focal");
    }

    void restoreCameraRejectsStructurallyInvalidLegacyCamParam()
    {
        QFETCH(QString, kind);

        TestRenderScene scene;
        FakeViewportCallbacks callbacks;
        ViewportDependencies deps{
            scene.document(),
            scene.sharedContext(),
            scene.settings(),
            callbacks,
            1,
            1,
            1,
            1,
            QStringLiteral("GT"),
            true,
            QString()};
        MeshLabViewport viewport(nullptr, deps);
        const CameraPose before = viewport.captureCamera();
        CameraPose invalid;
        if (kind == QStringLiteral("incomplete")) {
            invalid = {QStringLiteral(
                "<project><CamParam/><ViewSettings TrackScale=\"1\" NearPlane=\"0.1\" "
                "FarPlane=\"500\"/></project>")};
        }
        else if (kind == QStringLiteral("short-rotation")) {
            invalid = validLegacyCamParamPose();
            invalid.viewStateXml.replace(
                QRegularExpression(QStringLiteral("SimRot=\"[^\"]*\"")),
                QStringLiteral("SimRot=\"1 0 0\""));
        }
        else {
            invalid = validLegacyCamParamPose();
            invalid.viewStateXml.replace(
                QRegularExpression(QStringLiteral("Focal=\"[^\"]*\"")),
                QStringLiteral("Focal=\"not-a-number\""));
        }

        const OperationResult result = viewport.restoreCamera(invalid);
        QVERIFY2(!result.ok, qPrintable(result.error));
        QVERIFY(result.error.contains(QStringLiteral("legacy camera"), Qt::CaseInsensitive));
        QCOMPARE(viewport.captureCamera().viewStateXml, before.viewStateXml);
    }

    void wheelZoomNotifiesInjectedCameraCallback()
    {
        TestRenderScene scene;
        FakeViewportCallbacks callbacks;
        ViewportDependencies deps{
            scene.document(),
            scene.sharedContext(),
            scene.settings(),
            callbacks,
            4,
            1,
            1,
            1,
            QStringLiteral("GT"),
            true,
            QString()};
        MeshLabViewport viewport(nullptr, deps);
        viewport.resize(320, 240);
        const CameraPose before = viewport.captureCamera();
        QWheelEvent wheel(
            QPointF(10, 10),
            QPointF(10, 10),
            QPoint(),
            QPoint(0, 120),
            Qt::NoButton,
            Qt::NoModifier,
            Qt::NoScrollPhase,
            false);

        QApplication::sendEvent(&viewport, &wheel);

        QCOMPARE(callbacks.cameraChangeCount(), 1);
        QCOMPARE(callbacks.lastCameraViewportId(), 4);
        QVERIFY(callbacks.lastCameraPose().viewStateXml != before.viewStateXml);
    }

    void smoothWheelZoomUsesPixelDeltaWhenAngleDeltaIsEmpty()
    {
        TestRenderScene scene;
        FakeViewportCallbacks callbacks;
        ViewportDependencies deps{
            scene.document(),
            scene.sharedContext(),
            scene.settings(),
            callbacks,
            4,
            1,
            1,
            1,
            QStringLiteral("GT"),
            true,
            QString()};
        MeshLabViewport viewport(nullptr, deps);
        viewport.resize(320, 240);
        const CameraPose before = viewport.captureCamera();
        QWheelEvent smoothWheel(
            QPointF(10, 10),
            QPointF(10, 10),
            QPoint(0, 15),
            QPoint(),
            Qt::NoButton,
            Qt::NoModifier,
            Qt::ScrollUpdate,
            false);

        QApplication::sendEvent(&viewport, &smoothWheel);

        QCOMPARE(callbacks.cameraChangeCount(), 1);
        QCOMPARE(callbacks.lastCameraViewportId(), 4);
        const double scaleBefore =
            viewSettingsAttribute(before, QStringLiteral("TrackScale")).toDouble();
        const double scaleAfter = viewSettingsAttribute(
            callbacks.lastCameraPose(),
            QStringLiteral("TrackScale")).toDouble();
        QVERIFY(scaleAfter < scaleBefore);
    }

    void mouseDragNotifiesInjectedCameraCallback()
    {
        TestRenderScene scene;
        FakeViewportCallbacks callbacks;
        ViewportDependencies deps{
            scene.document(),
            scene.sharedContext(),
            scene.settings(),
            callbacks,
            5,
            1,
            1,
            1,
            QStringLiteral("GT"),
            true,
            QString()};
        MeshLabViewport viewport(nullptr, deps);
        viewport.resize(320, 240);
        const CameraPose before = viewport.captureCamera();
        QMouseEvent press(
            QEvent::MouseButtonPress,
            QPointF(20, 20),
            Qt::LeftButton,
            Qt::LeftButton,
            Qt::NoModifier);
        QMouseEvent move(
            QEvent::MouseMove,
            QPointF(120, 60),
            Qt::NoButton,
            Qt::LeftButton,
            Qt::NoModifier);
        QMouseEvent release(
            QEvent::MouseButtonRelease,
            QPointF(120, 60),
            Qt::LeftButton,
            Qt::NoButton,
            Qt::NoModifier);

        QApplication::sendEvent(&viewport, &press);
        QApplication::sendEvent(&viewport, &move);
        QApplication::sendEvent(&viewport, &release);

        QCOMPARE(callbacks.viewportActivationCount(), 1);
        QCOMPARE(callbacks.cameraChangeCount(), 1);
        QCOMPARE(callbacks.lastCameraViewportId(), 5);
        QVERIFY(callbacks.lastCameraPose().viewStateXml != before.viewStateXml);
    }

    void doubleClickRecentersOnThePickedSurfacePoint()
    {
        TestRenderScene scene;
        MeshModel* mesh = scene.document().addNewMesh(
            QString(),
            QStringLiteral("recenter-probe"),
            false);
        vcg::tri::Allocator<CMeshO>::AddVertices(mesh->cm, 3);
        mesh->cm.vert[0].P() = CMeshO::CoordType(0, 0, 0);
        mesh->cm.vert[1].P() = CMeshO::CoordType(1, 0, 0);
        mesh->cm.vert[2].P() = CMeshO::CoordType(0, 1, 0);
        vcg::tri::Allocator<CMeshO>::AddFaces(mesh->cm, 1);
        mesh->cm.face[0].V(0) = &mesh->cm.vert[0];
        mesh->cm.face[0].V(1) = &mesh->cm.vert[1];
        mesh->cm.face[0].V(2) = &mesh->cm.vert[2];
        mesh->updateBoxAndNormals();

        FakeViewportCallbacks callbacks;
        ViewportDependencies deps{
            scene.document(),
            scene.sharedContext(),
            scene.settings(),
            callbacks,
            6,
            mesh->id(),
            1,
            1,
            QStringLiteral("GT"),
            true,
            QString()};
        MeshLabViewport viewport(nullptr, deps);
        viewport.resize(640, 480);
        viewport.show();
        QTest::qWait(20);
        const OperationResult initialized =
            viewport.initializeForScenePreparation();
        QVERIFY2(initialized.ok, qPrintable(initialized.error));
        viewport.repaint();
        QApplication::processEvents();

        const CameraPose defaultCamera = viewport.captureCamera();
        QWheelEvent wheel(
            QPointF(320, 240),
            QPointF(320, 240),
            QPoint(),
            QPoint(0, 120),
            Qt::NoButton,
            Qt::NoModifier,
            Qt::NoScrollPhase,
            false);
        QApplication::sendEvent(&viewport, &wheel);
        QCOMPARE(callbacks.cameraChangeCount(), 1);
        viewport.repaint();
        QApplication::processEvents();

        const CameraPose baseline = viewport.captureCamera();
        QVERIFY(baseline.viewStateXml != defaultCamera.viewStateXml);
        const double baselineScale =
            viewSettingsAttribute(baseline, QStringLiteral("TrackScale")).toDouble();
        const auto sendDoubleClickAt = [&viewport](const QPoint& position) {
            QMouseEvent doubleClick(
                QEvent::MouseButtonDblClick,
                QPointF(position),
                Qt::LeftButton,
                Qt::LeftButton,
                Qt::NoModifier);
            QApplication::sendEvent(&viewport, &doubleClick);
        };
        const auto repaintViewport = [&viewport] {
            viewport.repaint();
            QApplication::processEvents();
        };

        // Both points are well inside the rendered triangle, on opposite sides
        // of its center. Re-centering must therefore produce distinct camera
        // translations instead of restoring one fixed default camera.
        sendDoubleClickAt(QPoint(230, 330));
        QCOMPARE(callbacks.cameraChangeCount(), 1);
        repaintViewport();
        QCOMPARE(callbacks.cameraChangeCount(), 2);
        QCOMPARE(callbacks.lastCameraViewportId(), 6);
        const CameraPose firstRecenter = callbacks.lastCameraPose();
        const double firstScale = viewSettingsAttribute(
                                      firstRecenter,
                                      QStringLiteral("TrackScale"))
                                      .toDouble();
        QVERIFY(std::abs(firstScale - (baselineScale * 1.25)) < 1e-4);
        QVERIFY(maximumCameraAttributeDifference(
                    firstRecenter,
                    baseline,
                    QStringLiteral("TranslationVector")) > 1e-3);

        const OperationResult restored = viewport.restoreCamera(baseline);
        QVERIFY2(restored.ok, qPrintable(restored.error));
        repaintViewport();

        sendDoubleClickAt(QPoint(350, 330));
        QCOMPARE(callbacks.cameraChangeCount(), 2);
        repaintViewport();
        QCOMPARE(callbacks.cameraChangeCount(), 3);
        const CameraPose secondRecenter = callbacks.lastCameraPose();
        const double secondScale = viewSettingsAttribute(
                                       secondRecenter,
                                       QStringLiteral("TrackScale"))
                                       .toDouble();
        QVERIFY(std::abs(secondScale - (baselineScale * 1.25)) < 1e-4);
        QVERIFY(maximumCameraAttributeDifference(
                    firstRecenter,
                    secondRecenter,
                    QStringLiteral("TranslationVector")) > 1e-3);
    }

    void doubleClickOnTheBackgroundLeavesTheCameraUnchanged()
    {
        TestRenderScene scene;
        MeshModel* mesh = scene.document().addNewMesh(
            QString(),
            QStringLiteral("recenter-miss-probe"),
            false);
        vcg::tri::Allocator<CMeshO>::AddVertices(mesh->cm, 3);
        mesh->cm.vert[0].P() = CMeshO::CoordType(0, 0, 0);
        mesh->cm.vert[1].P() = CMeshO::CoordType(1, 0, 0);
        mesh->cm.vert[2].P() = CMeshO::CoordType(0, 1, 0);
        vcg::tri::Allocator<CMeshO>::AddFaces(mesh->cm, 1);
        mesh->cm.face[0].V(0) = &mesh->cm.vert[0];
        mesh->cm.face[0].V(1) = &mesh->cm.vert[1];
        mesh->cm.face[0].V(2) = &mesh->cm.vert[2];
        mesh->updateBoxAndNormals();

        FakeViewportCallbacks callbacks;
        ViewportDependencies deps{
            scene.document(),
            scene.sharedContext(),
            scene.settings(),
            callbacks,
            6,
            mesh->id(),
            1,
            1,
            QStringLiteral("GT"),
            true,
            QString()};
        MeshLabViewport viewport(nullptr, deps);
        viewport.resize(640, 480);
        viewport.show();
        QTest::qWait(20);
        const OperationResult initialized =
            viewport.initializeForScenePreparation();
        QVERIFY2(initialized.ok, qPrintable(initialized.error));
        viewport.repaint();
        QApplication::processEvents();
        const CameraPose before = viewport.captureCamera();

        QMouseEvent doubleClick(
            QEvent::MouseButtonDblClick,
            QPointF(620, 460),
            Qt::LeftButton,
            Qt::LeftButton,
            Qt::NoModifier);
        QApplication::sendEvent(&viewport, &doubleClick);
        viewport.repaint();
        QApplication::processEvents();
        viewport.repaint();
        QApplication::processEvents();

        QCOMPARE(callbacks.cameraChangeCount(), 0);
        const CameraPose after = viewport.captureCamera();
        QVERIFY(maximumCameraAttributeDifference(
                    after,
                    before,
                    QStringLiteral("TranslationVector")) < 1e-5);
        QVERIFY(maximumCameraAttributeDifference(
                    after,
                    before,
                    QStringLiteral("RotationMatrix")) < 1e-5);
        QCOMPARE(
            viewSettingsAttribute(after, QStringLiteral("TrackScale")),
            viewSettingsAttribute(before, QStringLiteral("TrackScale")));
    }

    void emptySettingsReceiveDefaultRenderingParameters()
    {
        TestRenderScene scene;
        FakeViewportCallbacks callbacks;
        ViewportDependencies deps{
            scene.document(),
            scene.sharedContext(),
            scene.settings(),
            callbacks,
            8,
            1,
            1,
            2,
            QStringLiteral("analysis"),
            false,
            QStringLiteral("0.81")};

        MeshLabViewport viewport(nullptr, deps);

        QVERIFY(scene.settings().hasParameter(GLAreaSetting::backgroundTopColorParam()));
        QVERIFY(scene.settings().hasParameter(GLAreaSetting::baseLightDiffuseColorParam()));
    }

    void linkedViewportsResetFromTheSharedDocumentBounds()
    {
        TestRenderScene scene;
        MeshModel* assignedMesh =
            scene.document().addNewMesh(QString(), QStringLiteral("assigned"), false);
        assignedMesh->cm.bbox.Set(Point3m(0.0f, 0.0f, 0.0f));
        assignedMesh->cm.bbox.Add(Point3m(1.0f, 1.0f, 1.0f));
        MeshModel* distantMesh =
            scene.document().addNewMesh(QString(), QStringLiteral("distant"), false);
        distantMesh->cm.bbox.Set(Point3m(100.0f, 0.0f, 0.0f));
        distantMesh->cm.bbox.Add(Point3m(200.0f, 100.0f, 100.0f));

        FakeViewportCallbacks callbacks;
        ViewportDependencies assignedDependencies{
            scene.document(),
            scene.sharedContext(),
            scene.settings(),
            callbacks,
            9,
            assignedMesh->id(),
            1,
            2,
            QStringLiteral("assigned"),
            true,
            QString()};
        ViewportDependencies distantDependencies{
            scene.document(),
            scene.sharedContext(),
            scene.settings(),
            callbacks,
            10,
            distantMesh->id(),
            2,
            2,
            QStringLiteral("distant"),
            false,
            QString()};
        MeshLabViewport assignedViewport(nullptr, assignedDependencies);
        MeshLabViewport distantViewport(nullptr, distantDependencies);
        assignedViewport.resize(640, 480);
        distantViewport.resize(640, 480);
        QDomDocument document;

        QCOMPARE(
            assignedViewport.captureCamera().viewStateXml,
            distantViewport.captureCamera().viewStateXml);
        QVERIFY(document.setContent(assignedViewport.captureCamera().viewStateXml));
        const QDomElement settings =
            document.documentElement().firstChildElement(QStringLiteral("ViewSettings"));
        QVERIFY(settings.attribute(QStringLiteral("TrackScale")).toFloat() < 0.1f);
    }

    void normalizedViewportMapsItsMeshToTheSharedDocumentBounds()
    {
        TestRenderScene scene;
        MeshModel* assignedMesh =
            scene.document().addNewMesh(QString(), QStringLiteral("assigned"), false);
        assignedMesh->cm.bbox.Set(Point3m(-1.0f, -2.0f, -3.0f));
        assignedMesh->cm.bbox.Add(Point3m(1.0f, 2.0f, 3.0f));
        assignedMesh->cm.Tr.SetTranslate(20.0f, 5.0f, -7.0f);
        MeshModel* distantMesh =
            scene.document().addNewMesh(QString(), QStringLiteral("distant"), false);
        distantMesh->cm.bbox.Set(Point3m(100.0f, 50.0f, 25.0f));
        distantMesh->cm.bbox.Add(Point3m(180.0f, 130.0f, 105.0f));

        FakeViewportCallbacks callbacks;
        ViewportDependencies dependencies{
            scene.document(),
            scene.sharedContext(),
            scene.settings(),
            callbacks,
            11,
            assignedMesh->id(),
            1,
            2,
            QStringLiteral("assigned"),
            true,
            QString(),
            false,
            {},
            {},
            true};
        MeshLabViewport viewport(nullptr, dependencies);

        const Matrix44m transform =
            viewport.meshRenderTransformForTest(assignedMesh->id());
        Box3m normalizedBounds;
        normalizedBounds.Add(transform, assignedMesh->cm.bbox);
        const Box3m sceneBounds = scene.document().bbox();

        QVERIFY(
            qAbs(normalizedBounds.Diag() - sceneBounds.Diag()) < 1e-3f);
        for (int axis = 0; axis < 3; ++axis) {
            QVERIFY(
                qAbs(
                    normalizedBounds.Center()[axis] -
                    sceneBounds.Center()[axis]) < 1e-3f);
        }
        QCOMPARE(assignedMesh->cm.Tr[0][3], Scalarm(20.0f));
        QCOMPARE(assignedMesh->cm.Tr[1][3], Scalarm(5.0f));
        QCOMPARE(assignedMesh->cm.Tr[2][3], Scalarm(-7.0f));
    }

    void selectionCanBeUpdatedWithoutParentLookup()
    {
        TestRenderScene scene;
        FakeViewportCallbacks callbacks;
        ViewportDependencies deps{
            scene.document(),
            scene.sharedContext(),
            scene.settings(),
            callbacks,
            10,
            1,
            1,
            1,
            QStringLiteral("candidate"),
            false,
            QString()};
        MeshLabViewport viewport(nullptr, deps);

        QVERIFY(!viewport.isSelected());
        viewport.setSelected(true);
        QVERIFY(viewport.isSelected());
    }

    void lateInsertedAssignedMeshRegistersBeforePaint()
    {
        TestRenderScene scene;
        MeshModel* mesh = scene.document().addNewMesh(
            QString(),
            QStringLiteral("late-assigned"),
            false);
        vcg::tri::Allocator<CMeshO>::AddVertices(mesh->cm, 3);
        mesh->cm.vert[0].P() = CMeshO::CoordType(0, 0, 0);
        mesh->cm.vert[1].P() = CMeshO::CoordType(1, 0, 0);
        mesh->cm.vert[2].P() = CMeshO::CoordType(0, 1, 0);
        vcg::tri::Allocator<CMeshO>::AddFaces(mesh->cm, 1);
        mesh->cm.face[0].V(0) = &mesh->cm.vert[0];
        mesh->cm.face[0].V(1) = &mesh->cm.vert[1];
        mesh->cm.face[0].V(2) = &mesh->cm.vert[2];
        mesh->updateBoxAndNormals();

        FakeViewportCallbacks callbacks;
        ViewportDependencies deps{
            scene.document(),
            scene.sharedContext(),
            scene.settings(),
            callbacks,
            11,
            mesh->id(),
            1,
            1,
            QStringLiteral("late-assigned"),
            true,
            QString()};
        MeshLabViewport viewport(nullptr, deps);
        viewport.resize(320, 240);
        viewport.show();
        QTest::qWait(20);
        viewport.repaint();
        QApplication::processEvents();

        QCOMPARE(callbacks.rendererErrorCount(), 0);
        MLRenderingData renderingData;
        MLPerViewGLOptions options;
        scene.sharedContext().getRenderInfoPerMeshView(mesh->id(), viewport.context(), renderingData);
        QVERIFY(renderingData.get(options));
        QVERIFY(renderingData.isPrimitiveActive(MLRenderingData::PR_SOLID));
    }

    void partialSettingsAreCompletedWithoutOverwritingInjectedValues()
    {
        TestRenderScene scene;
        const QColor injectedTopColor(17, 34, 51);
        scene.settings().addParam(
            RichColor(
                GLAreaSetting::backgroundTopColorParam(),
                injectedTopColor,
                QStringLiteral("Injected top color")));
        RichParameterList defaults;
        GLAreaSetting::initGlobalParameterList(defaults);

        FakeViewportCallbacks callbacks;
        ViewportDependencies deps{
            scene.document(),
            scene.sharedContext(),
            scene.settings(),
            callbacks,
            12,
            1,
            1,
            1,
            QStringLiteral("partial-settings"),
            false,
            QString()};
        MeshLabViewport viewport(nullptr, deps);

        QCOMPARE(scene.settings().getColor(GLAreaSetting::backgroundTopColorParam()), injectedTopColor);
        QVERIFY(scene.settings().hasParameter(GLAreaSetting::baseLightDiffuseColorParam()));
        QCOMPARE(scene.settings().size(), defaults.size());
    }
};

QTEST_MAIN(MeshLabViewportDependenciesTest)
#include "mesh_lab_viewport_dependencies_test.moc"
