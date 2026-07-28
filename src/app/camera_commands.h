#pragma once

#include <QImage>
#include <QString>
#include <QStringList>
#include <QVector>

#include "../core/meshcompare_types.h"

struct CameraPoseSummary
{
    QString viewId;
    QString savedAtUtc;
    QStringList tags;
    QString screenshotPath;
    QString workspaceUuid;
};

struct CameraPanelSnapshot
{
    OperationResult result;
    QString workspaceUuid;
    QString suggestedWorkspaceUid;
    QVector<CameraPoseSummary> poses;
};

struct CameraScreenshotLibrarySnapshot
{
    OperationResult result;
    QVector<CameraPoseSummary> poses;
};

class ICameraCommands
{
public:
    virtual ~ICameraCommands() = default;

    virtual CameraPanelSnapshot cameraPanelSnapshot() const = 0;
    virtual QString cameraPoseLibraryPath() const { return {}; }
    virtual CameraScreenshotLibrarySnapshot cameraScreenshotLibrarySnapshot() const
    {
        const CameraPanelSnapshot currentWorkspace = cameraPanelSnapshot();
        return {currentWorkspace.result, currentWorkspace.poses};
    }
    virtual OperationResult setCameraPoseUid(const QString& uid) = 0;
    virtual OperationResult saveCurrentCameraPose(
        QString* savedViewId = nullptr,
        const QStringList& tags = {}) = 0;
    virtual OperationResult saveCurrentCameraPoseWithScreenshot(
        QImage& screenshot,
        QString* savedViewId = nullptr,
        const QStringList& tags = {}) = 0;
    virtual OperationResult applyCameraPose(const QString& viewId) = 0;
    virtual OperationResult setCameraPoseTags(
        const QString& viewId,
        const QStringList& tags) = 0;
    virtual OperationResult deleteCameraPose(const QString& viewId) = 0;
};
