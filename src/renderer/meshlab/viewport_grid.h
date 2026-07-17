#pragma once

#include <functional>
#include <memory>
#include <vector>

#include <QObject>
#include <QHash>
#include <QPointer>

#include "viewport_factory.h"

class MeshDocument;
class MLSceneGLSharedDataContext;
class RichParameterList;
class QWidget;

struct ViewportGridSceneDependencies
{
    MeshDocument& document;
    MLSceneGLSharedDataContext& sharedContext;
    RichParameterList& settings;
    std::function<int(MeshId)> meshModelIdFor;
};

class ViewportGrid final : public QObject, private IViewportCallbacks
{
public:
    ViewportGrid(IViewportFactory& viewportFactory, IViewportCallbacks& eventSink);
    ~ViewportGrid() override;

    OperationResult create(
        QWidget* host,
        const SceneDescriptor& scene,
        const ViewportGridSceneDependencies& dependencies,
        MeshId selectedMeshId);
    void showCommitted();
    void setSelectedMesh(MeshId meshId);
    void setReferenceMesh(MeshId meshId);
    OperationResult setAnalysisOverlays(
        const QVector<MeshAnalysisOverlayUpdate>& updates);
    CameraPose captureCamera() const;
    OperationResult restoreCamera(const CameraPose& pose);
    void resetCamera();
    void setDiagnostic(DiagnosticFlag flag, bool enabled);
    void requestRepaint();

    int viewportCount() const;
    MeshId meshIdForViewport(int viewportId) const;
    IViewport* viewportAt(int index) const;

private:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void viewportActivated(int viewportId) override;
    void cameraChanged(int viewportId, const CameraPose& pose) override;
    void rendererError(int viewportId, const QString& message) override;
    void clear();
    void fillHost();

    IViewportFactory& viewportFactory_;
    IViewportCallbacks& eventSink_;
    QPointer<QWidget> host_;
    QPointer<QWidget> container_;
    std::vector<std::unique_ptr<IViewport>> viewports_;
    std::vector<MeshId> viewportMeshIds_;
    QVector<MeshId> sceneMeshIds_;
    QHash<MeshId, QString> meshLabels_;
    QHash<MeshId, QString> analysisLabels_;
    bool overlayMode_ = false;
    bool propagatingCamera_ = false;
};
