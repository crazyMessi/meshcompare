#include "mesh_lab_viewport.h"

#include <algorithm>
#include <cstddef>
#include <cmath>
#include <cstring>
#include <exception>
#include <limits>
#include <utility>

#include <QByteArray>
#include <QCursor>
#include <QDomDocument>
#include <QFontMetrics>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QPen>
#include <QStringList>
#include <QWheelEvent>

#include <common/GLExtensionsManager.h>
#include <common/ml_document/mesh_document.h>
#include <common/ml_shared_data_context/ml_scene_gl_shared_data_context.h>
#include <common/mlexception.h>
#include <wrap/gl/picking.h>
#include <wrap/qt/device_to_logical.h>
#include <wrap/qt/shot_qt.h>
#include <wrap/qt/trackball.h>

#include "../../core/analysis_color_map.h"

#ifdef __APPLE__
#include <OpenGL/glu.h>
#else
#include <GL/glu.h>
#endif

namespace
{
QString openGlString(GLenum name)
{
    const GLubyte* value = glGetString(name);
    return value == nullptr
               ? QString()
               : QString::fromLatin1(reinterpret_cast<const char*>(value));
}

bool splitLegacyCameraValues(
    const QString& value,
    bool allowTrailingSpace,
    QStringList& values)
{
    if (value.isEmpty() || value.startsWith(QLatin1Char(' ')))
        return false;

    QString normalized = value;
    if (normalized.endsWith(QLatin1Char(' '))) {
        if (!allowTrailingSpace)
            return false;
        normalized.chop(1);
    }
    if (normalized.isEmpty() || normalized.endsWith(QLatin1Char(' ')) ||
        normalized.contains(QStringLiteral("  ")))
        return false;

    for (const QChar character : normalized) {
        if (character.isSpace() && character != QLatin1Char(' '))
            return false;
    }

    values = normalized.split(QLatin1Char(' '), Qt::KeepEmptyParts);
    for (const QString& token : values) {
        if (token.isEmpty())
            return false;
    }
    return true;
}

bool hasFiniteScalarmValues(const QStringList& values)
{
    const double maximum = static_cast<double>(std::numeric_limits<Scalarm>::max());
    for (const QString& value : values) {
        bool valid = false;
        const double number = value.toDouble(&valid);
        if (!valid || !std::isfinite(number) || number < -maximum || number > maximum)
            return false;
    }
    return true;
}

bool hasPositiveScalarmValues(const QStringList& values)
{
    if (!hasFiniteScalarmValues(values))
        return false;

    for (const QString& value : values) {
        if (value.toDouble() <= 0.0)
            return false;
    }
    return true;
}

bool hasFiniteScalarmAttribute(
    const QDomElement& camera,
    const QString& name,
    int valueCount,
    bool allowTrailingSpace = false)
{
    if (!camera.hasAttribute(name))
        return false;

    QStringList values;
    return splitLegacyCameraValues(camera.attribute(name), allowTrailingSpace, values) &&
           values.size() == valueCount && hasFiniteScalarmValues(values);
}

bool hasPositiveScalarmAttribute(
    const QDomElement& camera,
    const QString& name,
    int valueCount,
    bool allowTrailingSpace = false)
{
    if (!camera.hasAttribute(name))
        return false;

    QStringList values;
    return splitLegacyCameraValues(camera.attribute(name), allowTrailingSpace, values) &&
           values.size() == valueCount && hasPositiveScalarmValues(values);
}

bool hasValidTranslationVector(const QDomElement& camera)
{
    const QString name = QStringLiteral("TranslationVector");
    if (!camera.hasAttribute(name))
        return false;

    QStringList values;
    if (!splitLegacyCameraValues(camera.attribute(name), false, values) ||
        (values.size() != 3 && values.size() != 4) ||
        !hasFiniteScalarmValues(values)) {
        return false;
    }

    if (values.size() == 4) {
        bool valid = false;
        const double homogeneousCoordinate = values.at(3).toDouble(&valid);
        if (!valid || homogeneousCoordinate != 1.0)
            return false;
    }
    return true;
}

bool hasValidIntegerAttribute(
    const QDomElement& camera,
    const QString& name,
    int valueCount,
    bool requirePositive)
{
    if (!camera.hasAttribute(name))
        return false;

    QStringList values;
    if (!splitLegacyCameraValues(camera.attribute(name), false, values) ||
        values.size() != valueCount) {
        return false;
    }

    for (const QString& value : values) {
        bool valid = false;
        const int number = value.toInt(&valid);
        if (!valid || (requirePositive && number <= 0))
            return false;
    }
    return true;
}

bool hasValidCameraType(const QDomElement& camera)
{
    const QString name = QStringLiteral("CameraType");
    if (!camera.hasAttribute(name))
        return true;

    QStringList values;
    if (!splitLegacyCameraValues(camera.attribute(name), false, values) || values.size() != 1)
        return false;

    bool valid = false;
    const int type = values.front().toInt(&valid);
    return valid && type >= vcg::Camera<Scalarm>::PERSPECTIVE &&
           type <= vcg::Camera<Scalarm>::CAVALIERI;
}

bool decodeExactBase64Attribute(
    const QDomElement& camera,
    const QString& name,
    int expectedByteCount,
    QByteArray& decodedData)
{
    if (!camera.hasAttribute(name))
        return false;

    const QByteArray::FromBase64Result decoded = QByteArray::fromBase64Encoding(
        camera.attribute(name).toLatin1(),
        QByteArray::Base64Encoding | QByteArray::AbortOnBase64DecodingErrors);
    if (!decoded || decoded.decoded.size() != expectedByteCount)
        return false;

    decodedData = decoded.decoded;
    return true;
}

template <typename Value, std::size_t Count>
bool decodeBinaryValues(
    const QDomElement& camera,
    const QString& name,
    Value (&values)[Count])
{
    QByteArray decodedData;
    if (!decodeExactBase64Attribute(
            camera,
            name,
            static_cast<int>(sizeof(values)),
            decodedData)) {
        return false;
    }

    std::memcpy(values, decodedData.constData(), sizeof(values));
    return true;
}

bool hasFiniteScalarmValues(const Scalarm* values, std::size_t count)
{
    for (std::size_t index = 0; index < count; ++index) {
        if (!std::isfinite(values[index]))
            return false;
    }
    return true;
}

bool hasPositiveScalarmValues(const Scalarm* values, std::size_t count)
{
    if (!hasFiniteScalarmValues(values, count))
        return false;

    for (std::size_t index = 0; index < count; ++index) {
        if (values[index] <= 0.0)
            return false;
    }
    return true;
}

bool hasValidTextVcgCamera(const QDomElement& camera)
{
    return hasValidTranslationVector(camera) &&
           hasFiniteScalarmAttribute(camera, QStringLiteral("RotationMatrix"), 16, true) &&
           hasFiniteScalarmAttribute(camera, QStringLiteral("FocalMm"), 1) &&
           hasValidIntegerAttribute(camera, QStringLiteral("ViewportPx"), 2, true) &&
           hasFiniteScalarmAttribute(camera, QStringLiteral("CenterPx"), 2) &&
           hasFiniteScalarmAttribute(camera, QStringLiteral("PixelSizeMm"), 2) &&
           hasFiniteScalarmAttribute(camera, QStringLiteral("LensDistortion"), 2);
}

bool hasValidBinaryVcgCamera(const QDomElement& camera)
{
    Scalarm translation[3];
    Scalarm rotation[16];
    Scalarm focalMm[1];
    int viewport[2];
    Scalarm center[2];
    Scalarm pixelSizeMm[2];
    Scalarm lensDistortion[2];

    return decodeBinaryValues(camera, QStringLiteral("TranslationVector"), translation) &&
           hasFiniteScalarmValues(translation, 3) &&
           decodeBinaryValues(camera, QStringLiteral("RotationMatrix"), rotation) &&
           hasFiniteScalarmValues(rotation, 16) &&
           decodeBinaryValues(camera, QStringLiteral("FocalMm"), focalMm) &&
           hasFiniteScalarmValues(focalMm, 1) &&
           decodeBinaryValues(camera, QStringLiteral("ViewportPx"), viewport) &&
           viewport[0] > 0 && viewport[1] > 0 &&
           decodeBinaryValues(camera, QStringLiteral("CenterPx"), center) &&
           hasFiniteScalarmValues(center, 2) &&
           decodeBinaryValues(camera, QStringLiteral("PixelSizeMm"), pixelSizeMm) &&
           hasPositiveScalarmValues(pixelSizeMm, 2) &&
           decodeBinaryValues(camera, QStringLiteral("LensDistortion"), lensDistortion) &&
           hasFiniteScalarmValues(lensDistortion, 2);
}

bool hasValidVcgCameraStructure(const QDomNode& node)
{
    const QDomElement camera = node.toElement();
    if (camera.isNull() || camera.tagName() != QStringLiteral("VCGCamera") ||
        !hasValidCameraType(camera)) {
        return false;
    }

    bool validBinaryData = false;
    if (camera.hasAttribute(QStringLiteral("BinaryData"))) {
        bool valid = false;
        const int binaryData = camera.attribute(QStringLiteral("BinaryData")).toInt(&valid);
        if (!valid || (binaryData != 0 && binaryData != 1))
            return false;
        validBinaryData = binaryData == 1;
    }

    return validBinaryData ? hasValidBinaryVcgCamera(camera) : hasValidTextVcgCamera(camera);
}

bool hasValidLegacyCamParamStructure(const QDomNode& node)
{
    const QDomElement camera = node.toElement();
    if (camera.isNull() || camera.tagName() != QStringLiteral("CamParam"))
        return false;

    return hasFiniteScalarmAttribute(camera, QStringLiteral("SimTra"), 3) &&
           hasFiniteScalarmAttribute(camera, QStringLiteral("SimRot"), 16, true) &&
           hasPositiveScalarmAttribute(camera, QStringLiteral("Focal"), 1) &&
           hasValidIntegerAttribute(camera, QStringLiteral("Viewport"), 2, true) &&
           hasValidIntegerAttribute(camera, QStringLiteral("Center"), 2, false) &&
           hasPositiveScalarmAttribute(camera, QStringLiteral("ScaleF"), 2) &&
           hasFiniteScalarmAttribute(camera, QStringLiteral("LensDist"), 2) &&
           (!camera.hasAttribute(QStringLiteral("ScaleCorr")) ||
            hasFiniteScalarmAttribute(camera, QStringLiteral("ScaleCorr"), 1));
}

struct TrackballTransform
{
    vcg::Quaternionf rotation;
    vcg::Point3f translation;
};

TrackballTransform trackballTransformFromShot(
    const Shotm& shot,
    float cameraDistance,
    float scale)
{
    vcg::Quaternion<Scalarm> rotation;
    rotation.FromMatrix(shot.Extrinsics.Rot());

    TrackballTransform transform{
        vcg::Quaternionf::Construct(rotation),
        vcg::Point3f::Construct(-shot.Extrinsics.Tra())};
    transform.translation +=
        vcg::Point3f::Construct(
            transform.rotation.Inverse().Rotate(vcg::Point3f(0, 0, cameraDistance))) *
        (1.0f / scale);
    return transform;
}

bool hasFiniteTrackballTransform(const TrackballTransform& transform)
{
    for (int index = 0; index < 4; ++index) {
        if (!std::isfinite(transform.rotation.V(index)))
            return false;
    }
    for (int index = 0; index < 3; ++index) {
        if (!std::isfinite(transform.translation[index]))
            return false;
    }
    return true;
}

void setGlColor(const vcg::Color4b& color)
{
    glColor4ub(color[0], color[1], color[2], color[3]);
}

void setLightColor(GLenum light, GLenum property, const vcg::Color4b& color)
{
    const GLfloat values[] = {
        color[0] / 255.0f,
        color[1] / 255.0f,
        color[2] / 255.0f,
        color[3] / 255.0f};
    glLightfv(light, property, values);
}

QString distanceMappingLabel(DistanceColorMapping mapping)
{
    switch (mapping) {
    case DistanceColorMapping::Linear:
        return QStringLiteral("Linear");
    case DistanceColorMapping::SquareRoot:
        return QStringLiteral("Square root");
    default:
        return {};
    }
}

QString legendNumber(double value)
{
    return QString::number(value, 'g', 4);
}
} // namespace

MeshLabViewport::MeshLabViewport(QWidget* parent, ViewportDependencies dependencies)
    : QGLWidget(parent, &dependencies.sharedContext),
      document_(dependencies.document),
      sharedContext_(dependencies.sharedContext),
      settings_(dependencies.settings),
      callbacks_(dependencies.callbacks),
      viewportId_(dependencies.viewportId),
      meshModelIds_({dependencies.meshModelId}),
      viewportIndex_(dependencies.viewportIndex),
      viewportCount_(dependencies.viewportCount),
      label_(dependencies.label),
      selected_(dependencies.selected),
      scoreLabel_(dependencies.scoreLabel),
      reference_(dependencies.reference),
      colorLegend_(dependencies.colorLegend)
{
    meshModelIds_ += dependencies.additionalMeshModelIds;
    trackball_.center = vcg::Point3f(0, 0, 0);
    trackball_.radius = 1.0f;
    trackballLight_.center = vcg::Point3f(0, 0, 0);
    trackballLight_.radius = 1.0f;
    updateRenderSettings();
    resetCamera();
}

MeshLabViewport::~MeshLabViewport()
{
    if (openGLInitializationState_ == OpenGLInitializationState::Ready &&
        hasUsableOpenGLContexts()) {
        sharedContext_.removeView(context());
    }
}

OperationResult MeshLabViewport::initializeForScenePreparation()
{
    if (openGLInitializationState_ == OpenGLInitializationState::Ready)
        return OperationResult::success();
    if (openGLInitializationState_ == OpenGLInitializationState::Failed)
        return openGLInitializationResult();

    if (!hasUsableOpenGLContexts()) {
        markOpenGLInitializationFailed(
            QStringLiteral("A valid OpenGL context is required to prepare the MeshLab viewport."));
        return openGLInitializationResult();
    }

    QGLContext* previousContext = const_cast<QGLContext*>(QGLContext::currentContext());
    try {
        // updateGL() is Qt's synchronous initialization entry point when the
        // child is already mapped.  A staged child is intentionally hidden,
        // so glInit() provides the same guarded initialization path without
        // exposing it before the scene is committed.
        updateGL();
        if (openGLInitializationState_ == OpenGLInitializationState::NotStarted)
            glInit();
    }
    catch (const MLException& exception) {
        markOpenGLInitializationFailed(QString::fromLocal8Bit(exception.what()));
    }
    catch (const std::exception& exception) {
        markOpenGLInitializationFailed(QString::fromLocal8Bit(exception.what()));
    }
    catch (...) {
        markOpenGLInitializationFailed(
            QStringLiteral("The MeshLab viewport OpenGL initialization failed."));
    }

    if (openGLInitializationState_ == OpenGLInitializationState::NotStarted) {
        markOpenGLInitializationFailed(
            QStringLiteral("The MeshLab viewport OpenGL initialization did not complete."));
    }
    if (openGLInitializationState_ != OpenGLInitializationState::Ready &&
        QGLContext::currentContext() == context()) {
        doneCurrent();
        if (previousContext != nullptr && previousContext != context())
            previousContext->makeCurrent();
    }
    return openGLInitializationResult();
}

void MeshLabViewport::initializeGL()
{
    if (openGLInitializationState_ != OpenGLInitializationState::NotStarted)
        return;
    if (!hasUsableOpenGLContexts()) {
        markOpenGLInitializationFailed(
            QStringLiteral("A valid OpenGL context is required to initialize the MeshLab viewport."));
        return;
    }

    makeCurrent();
    if (QGLContext::currentContext() != context()) {
        markOpenGLInitializationFailed(
            QStringLiteral("The MeshLab viewport could not make its OpenGL context current."));
        return;
    }
    if (!QGLContext::areSharing(context(), sharedContext_.context())) {
        markOpenGLInitializationFailed(
            QStringLiteral("The MeshLab viewport OpenGL context is not sharing with its scene context."));
        return;
    }

    try {
        glShadeModel(GL_SMOOTH);
        glPixelStorei(GL_PACK_ROW_LENGTH, 0);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glEnable(GL_DEPTH_TEST);
        glEnable(GL_NORMALIZE);
        const GLfloat diffuseColor[] = {1.0f, 1.0f, 1.0f, 1.0f};
        glEnable(GL_LIGHT0);
        glDisable(GL_LIGHT1);
        glLightfv(GL_LIGHT1, GL_DIFFUSE, diffuseColor);
        GLExtensionsManager::initializeGLextensions();
        openGlVendor_ = openGlString(GL_VENDOR);
        openGlRenderer_ = openGlString(GL_RENDERER);
        openGlVersion_ = openGlString(GL_VERSION);

        registerAssignedMesh();
        MLRenderingData defaultRendering;
        sharedContext_.addView(context(), defaultRendering);

        for (int meshModelId : meshModelIds_) {
            MLRenderingData renderingData;
            sharedContext_.getRenderInfoPerMeshView(
                meshModelId, context(), renderingData);
            MLPerViewGLOptions options;
            if (!renderingData.get(options)) {
                markOpenGLInitializationFailed(
                    QStringLiteral("The shared renderer did not register view data for the MeshLab viewport."));
                return;
            }
        }
        openGLInitializationState_ = OpenGLInitializationState::Ready;
        if (wireframeDiagnostic_)
            applyWireframeDiagnostic();
    }
    catch (const MLException& exception) {
        markOpenGLInitializationFailed(QString::fromLocal8Bit(exception.what()));
    }
    catch (const std::exception& exception) {
        markOpenGLInitializationFailed(QString::fromLocal8Bit(exception.what()));
    }
    catch (...) {
        markOpenGLInitializationFailed(
            QStringLiteral("The MeshLab viewport OpenGL initialization failed."));
    }
}

bool MeshLabViewport::hasUsableOpenGLContexts() const
{
    QGLContext* viewportContext = context();
    QGLContext* sharedContext = sharedContext_.context();
    return viewportContext != nullptr && sharedContext != nullptr && isValid() &&
           viewportContext->isValid() && sharedContext_.isValid() &&
           sharedContext->isValid();
}

void MeshLabViewport::markOpenGLInitializationFailed(const QString& error)
{
    if (openGLInitializationState_ == OpenGLInitializationState::Ready)
        return;
    openGLInitializationState_ = OpenGLInitializationState::Failed;
    openGLInitializationError_ = error;
}

OperationResult MeshLabViewport::openGLInitializationResult() const
{
    return openGLInitializationState_ == OpenGLInitializationState::Ready
               ? OperationResult::success()
               : OperationResult::failure(openGLInitializationError_);
}

void MeshLabViewport::paintEvent(QPaintEvent* event)
{
    Q_UNUSED(event);

    QPainter painter(this);
    painter.beginNativePainting();
    makeCurrent();

    if (!isValid() || openGLInitializationState_ != OpenGLInitializationState::Ready ||
        QGLContext::currentContext() != context()) {
        painter.endNativePainting();
        return;
    }

    updateRenderSettings();
    glClearColor(1.0f, 1.0f, 1.0f, 0.0f);
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    setView();
    drawGradient();
    drawLight();

    glPushMatrix();
    trackball_.GetView();
    trackball_.Apply();
    drawAssignedMesh();
    glPopMatrix();

    if (renderSettings_.startupShowTrackball)
        trackball_.DrawPostApply();

    // Match GLArea's double-click space: the mesh depth buffer is complete,
    // while glPopMatrix above has restored the modelview from before the
    // trackball. Unprojecting here gives the translation to apply to the
    // trackball itself.
    bool cameraRecentered = false;
    if (hasPendingRecenter_) {
        hasPendingRecenter_ = false;
        vcg::Point3f pickedPoint;
        if (vcg::Pick<vcg::Point3f>(
                pendingRecenterPoint_[0],
                pendingRecenterPoint_[1],
                pickedPoint)) {
            trackball_.MouseUp(
                pendingRecenterPoint_[0],
                pendingRecenterPoint_[1],
                vcg::Trackball::BUTTON_NONE);
            trackball_.Translate(-pickedPoint);
            trackball_.Scale(1.25f);
            QCursor::setPos(mapToGlobal(QPoint(width() / 2 + 2, height() / 2 + 2)));
            cameraRecentered = true;
        }
    }

    // QPainter's overlay primitives share a single plane. Leaving the native
    // depth test enabled lets each badge background occlude its own text.
    glDisable(GL_DEPTH_TEST);
    painter.endNativePainting();
    drawViewportOverlay(painter);

    if (cameraRecentered)
        notifyCameraChanged();
}

void MeshLabViewport::updateRenderSettings()
{
    RichParameterList defaultSettings;
    GLAreaSetting::initGlobalParameterList(defaultSettings);
    for (const RichParameter& parameter : defaultSettings) {
        if (!settings_.hasParameter(parameter.name()))
            settings_.addParam(parameter);
    }
    renderSettings_.updateGlobalParameterSet(settings_);
}

void MeshLabViewport::setView()
{
    const int viewportWidth = qMax(1, width());
    const int viewportHeight = qMax(1, height());
    glViewport(
        0,
        0,
        QTLogicalToDevice(this, viewportWidth),
        QTLogicalToDevice(this, viewportHeight));

    const GLfloat aspect = static_cast<GLfloat>(viewportWidth) / viewportHeight;
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();

    vcg::Matrix44f translation;
    translation.SetTranslate(trackball_.center);
    vcg::Matrix44f scale;
    scale.SetScale(4.0f, 4.0f, 4.0f);
    const vcg::Matrix44f trackballMatrix =
        scale * translation * trackball_.Matrix() * (-translation);

    const bool orthographic = usesOrthographicProjection();
    const float cameraDist = cameraDistance();
    nearPlane_ = cameraDist * clipRatioNear_;
    farPlane_ = cameraDist * clipRatioFar_;

    const Box3m meshBounds = document_.bbox();
    if (!meshBounds.IsNull()) {
        Box3m transformedBounds;
        transformedBounds.Add(
            Matrix44m::Construct(trackballMatrix),
            meshBounds);
        const float maxFarPlane =
            cameraDist + std::max(1.75f, static_cast<float>(-transformedBounds.min[2]));
        farPlane_ = std::min(farPlane_, maxFarPlane);
    }

    if (farPlane_ < nearPlane_) {
        farPlane_ = nearPlane_ + 0.01f;
        clipRatioFar_ = farPlane_ / cameraDist;
    }

    if (orthographic)
        glOrtho(-1.75f * aspect, 1.75f * aspect, -1.75f, 1.75f, nearPlane_, farPlane_);
    else
        gluPerspective(fov_, aspect, nearPlane_, farPlane_);

    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    gluLookAt(0.0, 0.0, cameraDist, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0);
}

void MeshLabViewport::drawGradient()
{
    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glLoadIdentity();
    glOrtho(-1.0, 1.0, -1.0, 1.0, -1.0, 1.0);
    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadIdentity();
    glPushAttrib(GL_ENABLE_BIT | GL_CURRENT_BIT);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_LIGHTING);
    glDisable(GL_TEXTURE_2D);

    glBegin(GL_TRIANGLE_STRIP);
    setGlColor(renderSettings_.backgroundTopColor);
    glVertex2f(-1.0f, 1.0f);
    setGlColor(renderSettings_.backgroundBotColor);
    glVertex2f(-1.0f, -1.0f);
    setGlColor(renderSettings_.backgroundTopColor);
    glVertex2f(1.0f, 1.0f);
    setGlColor(renderSettings_.backgroundBotColor);
    glVertex2f(1.0f, -1.0f);
    glEnd();

    glPopAttrib();
    glPopMatrix();
    glMatrixMode(GL_PROJECTION);
    glPopMatrix();
    glMatrixMode(GL_MODELVIEW);
}

void MeshLabViewport::drawLight()
{
    glPushMatrix();
    trackballLight_.GetView();
    trackballLight_.Apply();

    const GLfloat frontLight[] = {0.0f, 0.0f, 1.0f, 0.0f};
    const GLfloat backLight[] = {0.0f, 0.0f, -1.0f, 0.0f};
    glLightfv(GL_LIGHT0, GL_POSITION, frontLight);
    glLightfv(GL_LIGHT1, GL_POSITION, backLight);
    glPopMatrix();
}

void MeshLabViewport::drawAssignedMesh()
{
    if (document_.isBusy())
        return;

    registerAssignedMesh();
    for (int meshModelId : meshModelIds_) {
        if (hiddenMeshModelIds_.contains(meshModelId))
            continue;
        MeshModel* mesh = document_.getMesh(meshModelId);
        if (mesh == nullptr)
            continue;

        MLRenderingData renderingData;
        sharedContext_.getRenderInfoPerMeshView(
            meshModelId, context(), renderingData);
        MLPerViewGLOptions options;
        if (!renderingData.get(options)) {
            callbacks_.rendererError(
                viewportId_,
                QStringLiteral("The shared renderer has no view data for an assigned mesh."));
            continue;
        }

        setLightingColors(options);
        if (options._back_face_cull)
            glEnable(GL_CULL_FACE);
        else
            glDisable(GL_CULL_FACE);

        sharedContext_.setMeshTransformationMatrix(meshModelId, mesh->cm.Tr);
        sharedContext_.draw(meshModelId, context());
        if (normalsDiagnostic_)
            drawNormals(*mesh);
    }
}

void MeshLabViewport::setLabel(QString label)
{
    label_ = std::move(label);
    update();
}

void MeshLabViewport::setMeshVisible(int meshModelId, bool visible)
{
    if (!meshModelIds_.contains(meshModelId))
        return;
    if (visible)
        hiddenMeshModelIds_.remove(meshModelId);
    else
        hiddenMeshModelIds_.insert(meshModelId);
    update();
}

void MeshLabViewport::drawNormals(const MeshModel& mesh)
{
    const Scalarm diagonal = mesh.cm.bbox.Diag();
    if (!std::isfinite(static_cast<double>(diagonal)) || diagonal <= 0)
        return;

    const Scalarm length = diagonal * Scalarm(0.025);
    glPushAttrib(GL_ENABLE_BIT | GL_CURRENT_BIT | GL_LINE_BIT);
    glDisable(GL_LIGHTING);
    glDisable(GL_TEXTURE_2D);
    glLineWidth(1.0f);
    glColor4ub(255, 196, 64, 230);
    glBegin(GL_LINES);
    for (const CVertexO& vertex : mesh.cm.vert) {
        if (vertex.IsD())
            continue;
        CMeshO::CoordType normal = vertex.N();
        const Scalarm norm = normal.Norm();
        if (!std::isfinite(static_cast<double>(norm)) || norm <= 0)
            continue;
        normal /= norm;
        const CMeshO::CoordType endpoint = vertex.P() + (normal * length);
        glVertex3d(
            static_cast<GLdouble>(vertex.P()[0]),
            static_cast<GLdouble>(vertex.P()[1]),
            static_cast<GLdouble>(vertex.P()[2]));
        glVertex3d(
            static_cast<GLdouble>(endpoint[0]),
            static_cast<GLdouble>(endpoint[1]),
            static_cast<GLdouble>(endpoint[2]));
    }
    glEnd();
    glPopAttrib();
}

void MeshLabViewport::registerAssignedMesh()
{
    for (int meshModelId : meshModelIds_) {
        if (document_.getMesh(meshModelId) != nullptr)
            sharedContext_.meshInserted(meshModelId);
    }
}

void MeshLabViewport::drawViewportOverlay(QPainter& painter) const
{
    QString title = label_;
    if (viewportCount_ > 1)
        title = QStringLiteral("%1 / %2  %3").arg(viewportIndex_).arg(viewportCount_).arg(label_);

    painter.save();
    QFont font = painter.font();
    font.setPointSizeF(qMax(9.0, font.pointSizeF()));
    font.setWeight(QFont::DemiBold);
    painter.setFont(font);
    const QFontMetrics metrics(font);
    const int padding = 8;
    const int titleWidth = metrics.horizontalAdvance(title);
    const QRect titleRect(12, 12, titleWidth + (padding * 2), metrics.height() + (padding * 2));

    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(12, 22, 36, 205));
    painter.drawRoundedRect(titleRect, 6.0, 6.0);
    painter.setPen(QColor(235, 242, 250));
    painter.drawText(titleRect, Qt::AlignCenter, title);

    if (reference_) {
        const QString referenceLabel = QStringLiteral("Reference");
        const int badgeWidth = metrics.horizontalAdvance(referenceLabel) + (padding * 2);
        const QRect badgeRect(
            titleRect.right() + 6,
            titleRect.top(),
            badgeWidth,
            titleRect.height());
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(15, 132, 95, 225));
        painter.drawRoundedRect(badgeRect, 6.0, 6.0);
        painter.setPen(Qt::white);
        painter.drawText(badgeRect, Qt::AlignCenter, referenceLabel);
    }

    if (!scoreLabel_.isEmpty()) {
        const int scoreWidth = metrics.horizontalAdvance(scoreLabel_) + (padding * 2);
        const QRect scoreRect(
            width() - scoreWidth - 12,
            12,
            scoreWidth,
            metrics.height() + (padding * 2));
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(28, 112, 150, 220));
        painter.drawRoundedRect(scoreRect, 6.0, 6.0);
        painter.setPen(Qt::white);
        painter.drawText(scoreRect, Qt::AlignCenter, scoreLabel_);
    }

    const QString mappingLabel =
        distanceMappingLabel(colorLegend_.distanceMapping);
    if (colorLegend_.kind == ColorLegendKind::Distance &&
        !mappingLabel.isEmpty() &&
        std::isfinite(colorLegend_.minimum) &&
        std::isfinite(colorLegend_.maximum) &&
        colorLegend_.minimum >= 0.0 &&
        colorLegend_.maximum >= colorLegend_.minimum &&
        width() >= 170 && height() >= 110) {
        const int legendWidth = qMin(280, width() - 24);
        const int legendHeight = 70;
        const QRect legendRect(
            12,
            height() - legendHeight - 12,
            legendWidth,
            legendHeight);
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(12, 22, 36, 220));
        painter.drawRoundedRect(legendRect, 7.0, 7.0);

        QFont legendTitleFont = font;
        legendTitleFont.setPointSizeF(qMax(8.5, font.pointSizeF() - 0.5));
        painter.setFont(legendTitleFont);
        painter.setPen(QColor(235, 242, 250));
        painter.drawText(
            legendRect.adjusted(10, 6, -10, -45),
            Qt::AlignLeft | Qt::AlignVCenter,
            QStringLiteral("Distance · %1").arg(mappingLabel));

        const QRect gradientRect(
            legendRect.left() + 10,
            legendRect.top() + 29,
            legendRect.width() - 20,
            12);
        for (int x = 0; x < gradientRect.width(); ++x) {
            const double position =
                gradientRect.width() <= 1
                    ? 0.0
                    : double(x) / double(gradientRect.width() - 1);
            const double distance =
                colorLegend_.minimum +
                ((colorLegend_.maximum - colorLegend_.minimum) * position);
            painter.fillRect(
                QRect(gradientRect.left() + x, gradientRect.top(), 1, gradientRect.height()),
                distanceColorForValue(
                    distance,
                    colorLegend_.maximum,
                    colorLegend_.distanceMapping));
        }
        painter.setPen(QColor(255, 255, 255, 85));
        painter.setBrush(Qt::NoBrush);
        painter.drawRect(gradientRect.adjusted(0, 0, -1, -1));

        QFont labelFont = painter.font();
        labelFont.setWeight(QFont::Normal);
        labelFont.setPointSizeF(qMax(8.0, labelFont.pointSizeF() - 1.0));
        painter.setFont(labelFont);
        painter.setPen(QColor(215, 225, 235));
        const QRect labelsRect(
            gradientRect.left(),
            gradientRect.bottom() + 3,
            gradientRect.width(),
            legendRect.bottom() - gradientRect.bottom() - 6);
        painter.drawText(
            labelsRect,
            Qt::AlignLeft | Qt::AlignVCenter,
            legendNumber(colorLegend_.minimum));
        painter.drawText(
            labelsRect,
            Qt::AlignRight | Qt::AlignVCenter,
            legendNumber(colorLegend_.maximum));
    }

    if (selected_) {
        QPen selectionPen(QColor(68, 209, 255), 2.0);
        painter.setPen(selectionPen);
        painter.setBrush(Qt::NoBrush);
        painter.drawRect(rect().adjusted(1, 1, -2, -2));
    }
    painter.restore();
}

void MeshLabViewport::setLightingColors(const MLPerViewGLOptions& options)
{
    if (options._double_side_lighting || options._fancy_lighting)
        glEnable(GL_LIGHT1);
    else
        glDisable(GL_LIGHT1);

    setLightColor(GL_LIGHT0, GL_AMBIENT, renderSettings_.baseLightAmbientColor);
    setLightColor(GL_LIGHT0, GL_DIFFUSE, renderSettings_.baseLightDiffuseColor);
    setLightColor(GL_LIGHT0, GL_SPECULAR, renderSettings_.baseLightSpecularColor);
    setLightColor(GL_LIGHT1, GL_AMBIENT, renderSettings_.baseLightAmbientColor);
    setLightColor(GL_LIGHT1, GL_DIFFUSE, renderSettings_.baseLightDiffuseColor);
    setLightColor(GL_LIGHT1, GL_SPECULAR, renderSettings_.baseLightSpecularColor);
    if (options._fancy_lighting) {
        setLightColor(GL_LIGHT0, GL_DIFFUSE, renderSettings_.fancyFLightDiffuseColor);
        setLightColor(GL_LIGHT1, GL_DIFFUSE, renderSettings_.fancyBLightDiffuseColor);
    }
}

CameraPose MeshLabViewport::captureCamera() const
{
    return {viewToText()};
}

OperationResult MeshLabViewport::restoreCamera(const CameraPose& pose)
{
    QDomDocument document(QStringLiteral("ViewState"));
    if (!document.setContent(pose.viewStateXml))
        return OperationResult::failure(QStringLiteral("Camera state XML is invalid."));
    return loadViewState(document);
}

QString MeshLabViewport::viewToText() const
{
    const Shotm shot = shotFromTrackball().first;
    QDomDocument document(QStringLiteral("ViewState"));
    QDomElement root = document.createElement(QStringLiteral("project"));
    document.appendChild(root);
    root.appendChild(WriteShotToQDomNode(shot, document));

    QDomElement settings = document.createElement(QStringLiteral("ViewSettings"));
    settings.setAttribute(QStringLiteral("TrackScale"), trackball_.track.sca);
    settings.setAttribute(QStringLiteral("NearPlane"), nearPlane_);
    settings.setAttribute(QStringLiteral("FarPlane"), farPlane_);
    root.appendChild(settings);

    return document.toString();
}

void MeshLabViewport::resetCamera()
{
    trackball_.Reset();
    const Box3m bounds = document_.bbox();
    const float diagonal = bounds.Diag();
    if (std::isfinite(diagonal) && diagonal > 0.0f) {
        trackball_.track.sca = 3.0f / diagonal;
        trackball_.track.tra.Import(-bounds.Center());
    }
    fov_ = 60.0f;
    perspectiveFov_ = fov_;
    cameraOrthographic_ = false;
    clipRatioNear_ = 0.1f;
    clipRatioFar_ = 500.0f;
    nearPlane_ = 0.1f;
    farPlane_ = 500.0f;
    hasRestoredFocalMm_ = false;
    restoredFocalViewportSize_ = QSize();
    update();
}

void MeshLabViewport::notifyCameraChangedForTest()
{
    notifyCameraChanged();
}

void MeshLabViewport::trackballStep(const QString& direction)
{
    const float stepAngle = static_cast<float>(3.14159265358979323846 / 12.0);
    bool changed = true;
    if (direction == tr("Horizontal +")) {
        trackball_.track.rot =
            vcg::Quaternionf(-stepAngle, vcg::Point3f(0.0f, 1.0f, 0.0f)) *
            trackball_.track.rot;
    }
    else if (direction == tr("Horizontal -")) {
        trackball_.track.rot =
            vcg::Quaternionf(stepAngle, vcg::Point3f(0.0f, 1.0f, 0.0f)) *
            trackball_.track.rot;
    }
    else if (direction == tr("Vertical +")) {
        trackball_.track.rot =
            vcg::Quaternionf(-stepAngle, vcg::Point3f(1.0f, 0.0f, 0.0f)) *
            trackball_.track.rot;
    }
    else if (direction == tr("Vertical -")) {
        trackball_.track.rot =
            vcg::Quaternionf(stepAngle, vcg::Point3f(1.0f, 0.0f, 0.0f)) *
            trackball_.track.rot;
    }
    else if (direction == tr("Axial +")) {
        trackball_.track.rot =
            vcg::Quaternionf(-stepAngle, vcg::Point3f(0.0f, 0.0f, 1.0f)) *
            trackball_.track.rot;
    }
    else if (direction == tr("Axial -")) {
        trackball_.track.rot =
            vcg::Quaternionf(stepAngle, vcg::Point3f(0.0f, 0.0f, 1.0f)) *
            trackball_.track.rot;
    }
    else {
        changed = false;
    }

    if (changed)
        notifyCameraChanged();
}

void MeshLabViewport::setSelected(bool selected)
{
    if (selected_ == selected)
        return;
    selected_ = selected;
    update();
}

void MeshLabViewport::setReference(bool reference)
{
    if (reference_ == reference)
        return;
    reference_ = reference;
    update();
}

void MeshLabViewport::setScoreLabel(QString label)
{
    if (scoreLabel_ == label)
        return;
    scoreLabel_ = std::move(label);
    update();
}

void MeshLabViewport::setColorLegend(ColorLegendSpec legend)
{
    if (colorLegend_ == legend)
        return;
    colorLegend_ = std::move(legend);
    update();
}

void MeshLabViewport::setDiagnostic(DiagnosticFlag flag, bool enabled)
{
    switch (flag) {
    case DiagnosticFlag::Orthographic: {
        const bool wasOrthographic = usesOrthographicProjection();
        const bool cameraWasOrthographic = cameraOrthographic_;
        const QPair<Shotm, float> preservedCamera = shotFromTrackball();
        orthographicDiagnostic_ = enabled;
        if (!enabled && cameraWasOrthographic) {
            cameraOrthographic_ = false;
            fov_ = perspectiveFov_;
            hasRestoredFocalMm_ = false;
            restoredFocalViewportSize_ = QSize();
        }
        if (wasOrthographic != usesOrthographicProjection()) {
            const float distance = cameraDistance();
            if (std::isfinite(distance) && distance > 0.0f &&
                std::isfinite(nearPlane_) && std::isfinite(farPlane_) &&
                nearPlane_ > 0.0f && farPlane_ > nearPlane_) {
                clipRatioNear_ = nearPlane_ / distance;
                clipRatioFar_ = farPlane_ / distance;
            }
            trackball_.track.sca = preservedCamera.second;
            shotToTrack(preservedCamera.first);
        }
        break;
    }
    case DiagnosticFlag::Wireframe:
        wireframeDiagnostic_ = enabled;
        applyWireframeDiagnostic();
        break;
    case DiagnosticFlag::Normals:
        normalsDiagnostic_ = enabled;
        break;
    }
    update();
}

RendererDiagnostics MeshLabViewport::rendererDiagnostics() const
{
    RendererDiagnostics result;
    result.openGlVendor = openGlVendor_;
    result.openGlRenderer = openGlRenderer_;
    result.openGlVersion = openGlVersion_;
    return result;
}

void MeshLabViewport::applyWireframeDiagnostic()
{
    if (openGLInitializationState_ != OpenGLInitializationState::Ready ||
        context() == nullptr) {
        return;
    }

    for (int meshModelId : meshModelIds_) {
        if (document_.getMesh(meshModelId) == nullptr)
            continue;

        MLRenderingData renderingData;
        sharedContext_.getRenderInfoPerMeshView(
            meshModelId, context(), renderingData);
        if (wireframeDiagnostic_) {
            MLRenderingData::RendAtts solidAttributes;
            if (renderingData.get(
                    MLRenderingData::PR_SOLID,
                    solidAttributes)) {
                renderingData.set(
                    MLRenderingData::PR_WIREFRAME_TRIANGLES,
                    solidAttributes);
            }
            else {
                renderingData.set(
                    MLRenderingData::PR_WIREFRAME_TRIANGLES,
                    true);
            }
        }
        else {
            renderingData.set(
                MLRenderingData::PR_WIREFRAME_TRIANGLES,
                false);
        }

        MLPerViewGLOptions options;
        if (renderingData.get(options)) {
            options._peredge_wire_enabled = wireframeDiagnostic_;
            options._peredge_fauxwire_enabled = false;
            options._perwire_fixed_color_enabled = true;
            options._perwire_mesh_color_enabled = false;
            options._perwire_fixed_color =
                vcg::Color4b(vcg::Color4b::DarkGray);
            renderingData.set(options);
        }
        sharedContext_.setRenderingDataPerMeshView(
            meshModelId, context(), renderingData);
        sharedContext_.manageBuffers(meshModelId);
    }
}

void MeshLabViewport::mousePressEvent(QMouseEvent* event)
{
    event->accept();
    setFocus();
    callbacks_.viewportActivated(viewportId_);
    activeDefaultTrackball_ = !((event->modifiers() & Qt::ShiftModifier) &&
                                (event->modifiers() & Qt::ControlModifier) &&
                                event->button() == Qt::LeftButton);
    if (activeDefaultTrackball_) {
        trackball_.MouseDown(
            QT2VCG_X(this, event),
            QT2VCG_Y(this, event),
            QT2VCG(event->button(), event->modifiers()));
    }
    else {
        trackballLight_.MouseDown(
            QT2VCG_X(this, event),
            QT2VCG_Y(this, event),
            QT2VCG(event->button(), Qt::NoModifier));
    }
    update();
}

void MeshLabViewport::mouseMoveEvent(QMouseEvent* event)
{
    if (event->buttons() == Qt::NoButton)
        return;

    if (activeDefaultTrackball_) {
        trackball_.MouseMove(QT2VCG_X(this, event), QT2VCG_Y(this, event));
        notifyCameraChanged();
    }
    else {
        trackballLight_.MouseMove(QT2VCG_X(this, event), QT2VCG_Y(this, event));
        update();
    }
}

void MeshLabViewport::mouseReleaseEvent(QMouseEvent* event)
{
    if (activeDefaultTrackball_) {
        trackball_.MouseUp(
            QT2VCG_X(this, event),
            QT2VCG_Y(this, event),
            QT2VCG(event->button(), event->modifiers()));
    }
    else {
        trackballLight_.MouseUp(
            QT2VCG_X(this, event),
            QT2VCG_Y(this, event),
            QT2VCG(event->button(), event->modifiers()));
    }
    activeDefaultTrackball_ = true;
    update();
}

void MeshLabViewport::mouseDoubleClickEvent(QMouseEvent* event)
{
    event->accept();
    hasPendingRecenter_ = true;
    pendingRecenterPoint_ = vcg::Point2i(
        static_cast<int>(QT2VCG_X(this, event)),
        static_cast<int>(QT2VCG_Y(this, event)));
    update();
}

void MeshLabViewport::wheelEvent(QWheelEvent* event)
{
    setFocus();
    event->accept();
    const int angleDelta = event->angleDelta().y();
    const int pixelDelta = event->pixelDelta().y();
    float wheelDelta =
        angleDelta != 0
            ? angleDelta / 120.0f
            : pixelDelta / 120.0f;
    if (wheelDelta == 0.0f)
        return;
    if (renderSettings_.wheelDirection)
        wheelDelta *= -1.0f;
    trackball_.MouseWheel(wheelDelta);
    notifyCameraChanged();
}

QPair<Shotm, float> MeshLabViewport::shotFromTrackball() const
{
    Shotm shot;
    initializeShot(shot);

    const double viewportHeightMm = shot.Intrinsics.PixelSizeMm[1] * shot.Intrinsics.ViewportPx[1];
    if (hasRestoredFocalMm_ &&
        restoredFocalViewportSize_ == QSize(width(), height())) {
        shot.Intrinsics.FocalMm = restoredFocalMm_;
    }
    else {
        shot.Intrinsics.FocalMm =
            viewportHeightMm / (2 * std::tan(vcg::math::ToRad(fov_ / 2.0f)));
    }
    shot.Intrinsics.cameraType = usesOrthographicProjection()
                                     ? vcg::Camera<Scalarm>::ORTHO
                                     : vcg::Camera<Scalarm>::PERSPECTIVE;

    shot.Extrinsics.SetTra(
        shot.Extrinsics.Tra() +
        (vcg::Inverse(shot.Extrinsics.Rot()) * Point3m(0, 0, cameraDistance())));

    return qMakePair(track2ShotCpu(shot), trackball_.track.sca);
}

void MeshLabViewport::loadShot(const QPair<Shotm, float>& shotAndScale)
{
    const Shotm& shot = shotAndScale.first;
    if (shot.Intrinsics.cameraType == vcg::Camera<Scalarm>::PERSPECTIVE) {
        cameraOrthographic_ = false;
        fov_ = shot.GetFovFromFocal();
        perspectiveFov_ = fov_;
    }
    else {
        cameraOrthographic_ = true;
        fov_ = 5.0f;
    }
    trackball_.Reset();
    trackball_.track.sca = shotAndScale.second;
    shotToTrack(shot);
}

OperationResult MeshLabViewport::loadViewState(const QDomDocument& document)
{
    const QDomElement root = document.documentElement();
    if (root.tagName() != QStringLiteral("project"))
        return OperationResult::failure(QStringLiteral("Camera state root must be a project element."));

    Shotm shot;
    bool hasShot = false;
    bool hasSettings = false;
    float scale = 1.0f;
    float nearPlane = 0.1f;
    float farPlane = 500.0f;

    for (QDomNode node = root.firstChild(); !node.isNull(); node = node.nextSibling()) {
        const QString name = node.nodeName();
        if (name == QStringLiteral("VCGCamera")) {
            if (!hasValidVcgCameraStructure(node)) {
                return OperationResult::failure(
                    QStringLiteral("Camera state contains an invalid VCG camera."));
            }
            if (!ReadShotFromQDomNode(shot, node))
                return OperationResult::failure(QStringLiteral("Camera state contains an invalid VCG camera."));
            hasShot = true;
        }
        else if (name == QStringLiteral("CamParam")) {
            if (!hasValidLegacyCamParamStructure(node)) {
                return OperationResult::failure(
                    QStringLiteral("Camera state contains an invalid legacy camera."));
            }
            if (!ReadShotFromOLDXML(shot, node))
                return OperationResult::failure(QStringLiteral("Camera state contains an invalid legacy camera."));
            hasShot = true;
        }
        else if (name == QStringLiteral("ViewSettings")) {
            const QDomElement settings = node.toElement();
            bool validScale = false;
            bool validNear = false;
            bool validFar = false;
            scale = settings.attribute(QStringLiteral("TrackScale")).toFloat(&validScale);
            nearPlane = settings.attribute(QStringLiteral("NearPlane")).toFloat(&validNear);
            farPlane = settings.attribute(QStringLiteral("FarPlane")).toFloat(&validFar);
            if (!validScale || !validNear || !validFar ||
                !std::isfinite(scale) || !std::isfinite(nearPlane) || !std::isfinite(farPlane)) {
                return OperationResult::failure(
                    QStringLiteral("Camera view settings must contain finite scale and clipping planes."));
            }
            if (scale <= 0.0f)
                return OperationResult::failure(QStringLiteral("Camera scale must be greater than zero."));
            if (nearPlane <= 0.0f || farPlane <= nearPlane) {
                return OperationResult::failure(
                    QStringLiteral("Camera clipping planes must satisfy 0 < near < far."));
            }
            hasSettings = true;
        }
    }

    if (!hasShot || !hasSettings)
        return OperationResult::failure(
            QStringLiteral("Camera state must contain a camera and view settings."));

    const float restoredFov = shot.Intrinsics.cameraType == vcg::Camera<Scalarm>::PERSPECTIVE
                                  ? shot.GetFovFromFocal()
                                  : 5.0f;
    if (!std::isfinite(restoredFov) || restoredFov <= 0.0f || restoredFov >= 180.0f) {
        return OperationResult::failure(QStringLiteral("Camera field of view is invalid."));
    }
    const bool restoredOrthographic =
        shot.Intrinsics.cameraType != vcg::Camera<Scalarm>::PERSPECTIVE;
    const float distance =
        (orthographicDiagnostic_ || restoredOrthographic)
            ? 8.0f
            : 1.75f / std::tan(vcg::math::ToRad(restoredFov * 0.5f));
    if (!std::isfinite(distance) || distance <= 0.0f)
        return OperationResult::failure(QStringLiteral("Camera distance is invalid."));

    if (!hasFiniteTrackballTransform(trackballTransformFromShot(shot, distance, scale))) {
        return OperationResult::failure(
            QStringLiteral("Camera state cannot produce a finite trackball transform."));
    }

    const float nearRatio = nearPlane / distance;
    const float farRatio = farPlane / distance;
    if (!std::isfinite(nearRatio) || !std::isfinite(farRatio))
        return OperationResult::failure(QStringLiteral("Camera clipping ratios are invalid."));

    fov_ = restoredFov;
    nearPlane_ = nearPlane;
    farPlane_ = farPlane;
    clipRatioNear_ = nearRatio;
    clipRatioFar_ = farRatio;
    loadShot(qMakePair(shot, scale));
    restoredFocalMm_ = shot.Intrinsics.FocalMm;
    restoredFocalViewportSize_ = QSize(
        shot.Intrinsics.ViewportPx[0],
        shot.Intrinsics.ViewportPx[1]);
    hasRestoredFocalMm_ = true;
    update();
    return OperationResult::success();
}

void MeshLabViewport::initializeShot(Shotm& shot) const
{
    const int viewportWidth = qMax(1, width());
    const int viewportHeight = qMax(1, height());
    shot.Intrinsics.PixelSizeMm[0] = 0.036916077f;
    shot.Intrinsics.PixelSizeMm[1] = 0.036916077f;
    shot.Intrinsics.DistorCenterPx[0] = viewportWidth / 2;
    shot.Intrinsics.DistorCenterPx[1] = viewportHeight / 2;
    shot.Intrinsics.CenterPx[0] = viewportWidth / 2;
    shot.Intrinsics.CenterPx[1] = viewportHeight / 2;
    shot.Intrinsics.ViewportPx[0] = viewportWidth;
    shot.Intrinsics.ViewportPx[1] = viewportHeight;

    const double viewportHeightMm = shot.Intrinsics.PixelSizeMm[1] * viewportHeight;
    shot.Intrinsics.FocalMm =
        viewportHeightMm / (2 * std::tan(vcg::math::ToRad(60.0f / 2.0f)));
    shot.Extrinsics.SetIdentity();
}

Shotm MeshLabViewport::track2ShotCpu(Shotm referenceCamera) const
{
    vcg::Matrix44<Scalarm> shotExtrinsics;
    referenceCamera.GetWorldToExtrinsicsMatrix().ToMatrix(shotExtrinsics);
    const vcg::Matrix44<Scalarm> modelWithTrackball =
        shotExtrinsics * vcg::Matrix44<Scalarm>::Construct(trackball_.Matrix());
    vcg::Matrix44<Scalarm> model;
    modelWithTrackball.ToMatrix(model);

    vcg::Point3<Scalarm> translation(
        model[0][3], model[1][3], model[2][3]);
    model[0][3] = 0;
    model[1][3] = 0;
    model[2][3] = 0;
    const double determinant = model.Determinant();
    if (determinant == 0.0)
        return referenceCamera;
    const double inverseScale = 1.0 / std::pow(determinant, 1.0 / 3.0);
    model *= inverseScale;
    model[3][3] = 1;
    referenceCamera.Extrinsics.SetRot(model);

    vcg::Matrix44<Scalarm> inverseModel = model;
    vcg::Transpose(inverseModel);
    translation = -(inverseModel * translation);
    translation *= inverseScale;
    referenceCamera.Extrinsics.SetTra(translation);
    return referenceCamera;
}

void MeshLabViewport::shotToTrack(const Shotm& shot)
{
    const TrackballTransform transform =
        trackballTransformFromShot(shot, cameraDistance(), trackball_.track.sca);
    trackball_.track.rot = transform.rotation;
    trackball_.track.tra = transform.translation;
}

float MeshLabViewport::cameraDistance() const
{
    if (usesOrthographicProjection())
        return 8.0f;
    return 1.75f / std::tan(vcg::math::ToRad(fov_ * 0.5f));
}

bool MeshLabViewport::usesOrthographicProjection() const
{
    return orthographicDiagnostic_ || cameraOrthographic_;
}

void MeshLabViewport::notifyCameraChanged()
{
    callbacks_.cameraChanged(viewportId_, captureCamera());
    update();
}
