#pragma once

#include <QString>
#include <QVector>

#include "../core/meshcompare_types.h"

struct CameraPoseSummary
{
    QString viewId;
    QString savedAtUtc;
};

struct CameraPanelSnapshot
{
    OperationResult result;
    QString workspaceUuid;
    QString suggestedWorkspaceUid;
    QVector<CameraPoseSummary> poses;
};

class ICameraCommands
{
public:
    virtual ~ICameraCommands() = default;

    virtual CameraPanelSnapshot cameraPanelSnapshot() const = 0;
    virtual OperationResult setCameraPoseUid(const QString& uid) = 0;
    virtual OperationResult saveCurrentCameraPose(
        QString* savedViewId = nullptr) = 0;
    virtual OperationResult applyCameraPose(const QString& viewId) = 0;
    virtual OperationResult deleteCameraPose(const QString& viewId) = 0;
};
