#pragma once

#include <functional>

#include <QImage>

#include "mesh_resource_provider.h"

class QWidget;

struct RendererEvents {
    std::function<void(MeshId)> selectedMeshChanged;
    std::function<void(const CameraPose&)> cameraChanged;
    std::function<void(MeshId, int)> resourceProgress;
    std::function<void(const QString&)> rendererError;
};

class IRendererAdapter {
public:
    virtual ~IRendererAdapter() = default;
    virtual OperationResult mount(QWidget* viewportHost) = 0;
    virtual void setEvents(RendererEvents events) = 0;
    virtual OperationResult prepareScene(
        const SceneDescriptor& scene,
        const IMeshResourceProvider& resources) = 0;
    virtual void commitPreparedScene() = 0;
    virtual void discardPreparedScene() = 0;
    virtual void clearScene() = 0;
    virtual void setSelectedMesh(MeshId meshId) = 0;
    virtual void setReferenceMesh(MeshId meshId) = 0;
    virtual OperationResult setMeshVisible(MeshId meshId, bool visible) = 0;
    virtual OperationResult setColorPresentations(
        const QVector<MeshColorPresentationUpdate>& updates) = 0;
    virtual OperationResult setAnalysisOverlays(
        const QVector<MeshAnalysisOverlayUpdate>& updates) = 0;
    virtual CameraPose captureCamera() const = 0;
    virtual OperationResult captureImage(QImage& image) = 0;
    virtual OperationResult restoreCamera(const CameraPose& pose) = 0;
    virtual void resetCamera() = 0;
    virtual void setDiagnostic(DiagnosticFlag flag, bool enabled) = 0;
    virtual RendererDiagnostics diagnostics() const = 0;
};
