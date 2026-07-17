#pragma once

#include <functional>

#include <QObject>
#include <QString>
#include <QStringList>

#include "../core/meshcompare_types.h"

class IRendererAdapter;
class QCoreApplication;
class QEvent;
class QWidget;
class WorkspaceState;
struct WorkspaceImportOutcome;

class FileOpenEventBridge final : public QObject
{
public:
    using OpenHandler = std::function<void(const QStringList&)>;

    explicit FileOpenEventBridge(
        QCoreApplication& application,
        QObject* parent = nullptr);
    ~FileOpenEventBridge() override;

    void setOpenHandler(OpenHandler handler);
    void clearOpenHandler();
    void queueOpenPaths(const QStringList& paths);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void scheduleDispatch();
    void dispatch();

    QCoreApplication& application_;
    OpenHandler openHandler_;
    QStringList pendingPaths_;
    bool dispatchScheduled_ = false;
};

OperationResult initializeRenderer(
    WorkspaceState& state,
    IRendererAdapter& renderer,
    QWidget* viewportHost);

void mergeStartupNotice(
    WorkspaceImportOutcome& outcome,
    const QString& notice);

int presentFatalStartupError(
    const OperationResult& failure,
    QWidget* parent = nullptr);
