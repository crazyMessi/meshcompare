#pragma once

#include <QString>
#include <QVector>

#include "../../core/meshcompare_types.h"

class MeshDocument;
class MLSceneGLSharedDataContext;
class RichParameterList;

class IViewportCallbacks
{
public:
    virtual ~IViewportCallbacks() = default;

    virtual void viewportActivated(int viewportId) = 0;
    virtual void cameraChanged(int viewportId, const CameraPose& pose) = 0;
    virtual void rendererError(int viewportId, const QString& message) = 0;
};

struct ViewportDependencies
{
    MeshDocument& document;
    MLSceneGLSharedDataContext& sharedContext;
    RichParameterList& settings;
    IViewportCallbacks& callbacks;
    int viewportId;
    int meshModelId;
    int viewportIndex;
    int viewportCount;
    QString label;
    bool selected;
    QString scoreLabel;
    bool reference = false;
    QVector<int> additionalMeshModelIds;
    ColorLegendSpec colorLegend;
    bool normalizeMesh = false;
};
