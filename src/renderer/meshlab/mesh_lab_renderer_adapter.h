#pragma once

#include <array>
#include <memory>

#include <QPointer>

#include "../../core/renderer_adapter.h"

class IViewportFactory;
class MeshLabViewport;
class QWidget;

class MeshLabRendererAdapter final : public IRendererAdapter
{
public:
    MeshLabRendererAdapter();
    explicit MeshLabRendererAdapter(IViewportFactory& viewportFactory);
    ~MeshLabRendererAdapter() override;

    OperationResult mount(QWidget* viewportHost) override;
    void setEvents(RendererEvents events) override;
    OperationResult prepareScene(
        const SceneDescriptor& scene,
        const IMeshResourceProvider& resources) override;
    void commitPreparedScene() override;
    void discardPreparedScene() override;
    void clearScene() override;
    void setSelectedMesh(MeshId meshId) override;
    void setReferenceMesh(MeshId meshId) override;
    OperationResult setColorPresentations(
        const QVector<MeshColorPresentationUpdate>& updates) override;
    OperationResult setAnalysisOverlays(
        const QVector<MeshAnalysisOverlayUpdate>& updates) override;
    CameraPose captureCamera() const override;
    OperationResult restoreCamera(const CameraPose& pose) override;
    void resetCamera() override;
    void setDiagnostic(DiagnosticFlag flag, bool enabled) override;
    RendererDiagnostics diagnostics() const override;

    quint64 preparedGeneration() const;
    quint64 committedGeneration() const;
    int validViewportCount() const;
    MeshLabViewport* viewportForTest(int index) const;

private:
    class AdapterViewportCallbacks;
    class SceneBundle;

    void destroyBundle(std::unique_ptr<SceneBundle>& bundle);
    void viewportActivated(int viewportId);
    void viewportCameraChanged(int viewportId, const CameraPose& pose);
    void viewportRendererError(int viewportId, const QString& message);

    std::unique_ptr<IViewportFactory> ownedViewportFactory_;
    IViewportFactory* viewportFactory_ = nullptr;
    std::unique_ptr<AdapterViewportCallbacks> callbacks_;
    std::unique_ptr<SceneBundle> prepared_;
    std::unique_ptr<SceneBundle> committed_;
    QPointer<QWidget> viewportHost_;
    RendererEvents events_;
    MeshId selectedMeshId_ = 0;
    MeshId referenceMeshId_ = 0;
    std::array<bool, 3> diagnosticEnabled_{{false, false, false}};
};
