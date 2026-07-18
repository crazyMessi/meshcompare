#pragma once

#include <memory>

#include "../../core/meshcompare_types.h"
#include "viewport_dependencies.h"

class QWidget;

// The adapter owns a renderer-private viewport handle.  It deliberately
// exposes only product-level operations so application code never needs a
// MeshLab widget, document, or GL context.
class IViewport
{
public:
    virtual ~IViewport() = default;

    virtual QWidget* widget() const = 0;
    virtual OperationResult initializeForScenePreparation() = 0;
    virtual CameraPose captureCamera() const = 0;
    virtual OperationResult restoreCamera(const CameraPose& pose) = 0;
    virtual void resetCamera() = 0;
    virtual void setLabel(QString label) = 0;
    virtual void setSelected(bool selected) = 0;
    virtual void setReference(bool reference) = 0;
    virtual void setMeshVisible(int meshModelId, bool visible) = 0;
    virtual void setScoreLabel(QString label) = 0;
    virtual void setColorLegend(ColorLegendSpec legend) = 0;
    virtual void setDiagnostic(DiagnosticFlag flag, bool enabled) = 0;
    virtual void requestRepaint() = 0;
};

class IViewportFactory
{
public:
    virtual ~IViewportFactory() = default;

    virtual OperationResult createViewport(
        QWidget* parent,
        const ViewportDependencies& dependencies,
        std::unique_ptr<IViewport>& viewport) = 0;
};

class MeshLabViewportFactory final : public IViewportFactory
{
public:
    OperationResult createViewport(
        QWidget* parent,
        const ViewportDependencies& dependencies,
        std::unique_ptr<IViewport>& viewport) override;
};
