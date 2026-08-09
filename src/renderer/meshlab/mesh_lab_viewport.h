#pragma once

#include <GL/glew.h>

#include <QGLWidget>
#include <QPair>
#include <QSet>
#include <QSize>
#include <QVector>

#include <common/ml_document/base_types.h>
#include <wrap/gui/trackball.h>

#include "glarea_setting.h"
#include "viewport_dependencies.h"

class QDomDocument;
class QPaintEvent;
class QPainter;
class MeshModel;
struct MLPerViewGLOptions;

class MeshLabViewport : public QGLWidget
{
public:
    explicit MeshLabViewport(QWidget* parent, ViewportDependencies dependencies);
    ~MeshLabViewport() override;

    CameraPose captureCamera() const;
    OperationResult captureImage(QImage& image);
    OperationResult restoreCamera(const CameraPose& pose);
    QString viewToText() const;
    void resetCamera();
    void setLabel(QString label);
    void notifyCameraChangedForTest();
    void trackballStep(const QString& direction);
    OperationResult initializeForScenePreparation();
    bool isSelected() const { return selected_; }
    void setSelected(bool selected);
    bool isReference() const { return reference_; }
    void setReference(bool reference);
    void setMeshVisible(int meshModelId, bool visible);
    void setScoreLabel(QString label);
    void setColorLegend(ColorLegendSpec legend);
    void setDiagnostic(DiagnosticFlag flag, bool enabled);
    RendererDiagnostics rendererDiagnostics() const;
    const QString& scoreLabelForTest() const { return scoreLabel_; }
    int assignedMeshCountForTest() const { return meshModelIds_.size(); }
    Matrix44m meshRenderTransformForTest(int meshModelId) const;
    bool usesBackFaceCulling(bool meshOptionEnabled) const;
    bool usesDoubleSidedLighting(bool meshOptionEnabled) const;

protected:
    void initializeGL() override;
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;

private:
    void updateRenderSettings();
    void setView();
    void drawScene();
    void drawGradient();
    void drawLight();
    void drawAssignedMesh();
    void drawNormals(const MeshModel& mesh, const Matrix44m& transform);
    Matrix44m meshRenderTransform(const MeshModel& mesh) const;
    void registerAssignedMesh();
    void applyWireframeDiagnostic();
    void drawViewportOverlay(QPainter& painter) const;
    void setLightingColors(const MLPerViewGLOptions& options);
    QPair<Shotm, float> shotFromTrackball() const;
    void loadShot(const QPair<Shotm, float>& shotAndScale);
    OperationResult loadViewState(const QDomDocument& document);
    void initializeShot(Shotm& shot) const;
    Shotm track2ShotCpu(Shotm referenceCamera) const;
    void shotToTrack(const Shotm& shot);
    bool usesOrthographicProjection() const;
    float cameraDistance() const;
    void notifyCameraChanged();
    bool hasUsableOpenGLContexts() const;
    void markOpenGLInitializationFailed(const QString& error);
    OperationResult openGLInitializationResult() const;

    enum class OpenGLInitializationState { NotStarted, Ready, Failed };

    MeshDocument& document_;
    MLSceneGLSharedDataContext& sharedContext_;
    RichParameterList& settings_;
    IViewportCallbacks& callbacks_;
    int viewportId_;
    QVector<int> meshModelIds_;
    QSet<int> hiddenMeshModelIds_;
    int viewportIndex_;
    int viewportCount_;
    QString label_;
    bool selected_;
    QString scoreLabel_;
    bool reference_ = false;
    bool normalizeMesh_ = false;
    bool doubleSidedRendering_ = false;
    ColorLegendSpec colorLegend_;
    bool cameraOrthographic_ = false;
    bool orthographicDiagnostic_ = false;
    bool wireframeDiagnostic_ = false;
    bool normalsDiagnostic_ = false;
    QString openGlVendor_;
    QString openGlRenderer_;
    QString openGlVersion_;
    GLAreaSetting renderSettings_;
    vcg::Trackball trackball_;
    vcg::Trackball trackballLight_;
    float fov_ = 60.0f;
    float perspectiveFov_ = 60.0f;
    float nearPlane_ = 0.1f;
    float farPlane_ = 500.0f;
    float clipRatioNear_ = 0.1f;
    float clipRatioFar_ = 500.0f;
    Scalarm restoredFocalMm_ = 0;
    QSize restoredFocalViewportSize_;
    bool hasRestoredFocalMm_ = false;
    bool activeDefaultTrackball_ = true;
    bool hasPendingRecenter_ = false;
    vcg::Point2i pendingRecenterPoint_ = vcg::Point2i(0, 0);
    OpenGLInitializationState openGLInitializationState_ = OpenGLInitializationState::NotStarted;
    QString openGLInitializationError_;
};
