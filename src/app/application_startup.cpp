#include "application_startup.h"

#include <cstdlib>
#include <utility>

#include <QMessageBox>
#include <QCoreApplication>
#include <QFileOpenEvent>
#include <QObject>
#include <QTimer>
#include <QUrl>

#include "workspace_controller.h"
#include "../core/renderer_adapter.h"
#include "../core/workspace_state.h"

namespace
{
void appendNoticeLine(QString& destination, const QString& notice)
{
    if (notice.isEmpty())
        return;
    if (!destination.isEmpty())
        destination.append(QLatin1Char('\n'));
    destination.append(notice);
}
} // namespace

FileOpenEventBridge::FileOpenEventBridge(
    QCoreApplication& application,
    QObject* parent)
    : QObject(parent), application_(application)
{
    application_.installEventFilter(this);
}

FileOpenEventBridge::~FileOpenEventBridge()
{
    application_.removeEventFilter(this);
}

void FileOpenEventBridge::setOpenHandler(OpenHandler handler)
{
    openHandler_ = std::move(handler);
    scheduleDispatch();
}

void FileOpenEventBridge::clearOpenHandler()
{
    openHandler_ = {};
}

void FileOpenEventBridge::queueOpenPaths(const QStringList& paths)
{
    for (const QString& path : paths) {
        if (!path.isEmpty())
            pendingPaths_.append(path);
    }
    scheduleDispatch();
}

bool FileOpenEventBridge::eventFilter(QObject* watched, QEvent* event)
{
    if (watched != &application_ || event->type() != QEvent::FileOpen)
        return QObject::eventFilter(watched, event);

    auto* fileOpenEvent = static_cast<QFileOpenEvent*>(event);
    QString path = fileOpenEvent->file();
    if (path.isEmpty() && fileOpenEvent->url().isLocalFile())
        path = fileOpenEvent->url().toLocalFile();
    if (path.isEmpty())
        return QObject::eventFilter(watched, event);

    queueOpenPaths({path});
    event->accept();
    return true;
}

void FileOpenEventBridge::scheduleDispatch()
{
    if (dispatchScheduled_ || pendingPaths_.isEmpty() || !openHandler_)
        return;
    dispatchScheduled_ = true;
    QTimer::singleShot(0, this, [this] { dispatch(); });
}

void FileOpenEventBridge::dispatch()
{
    dispatchScheduled_ = false;
    if (pendingPaths_.isEmpty() || !openHandler_)
        return;

    QStringList paths;
    paths.swap(pendingPaths_);
    const OpenHandler handler = openHandler_;
    handler(paths);
    scheduleDispatch();
}

OperationResult initializeRenderer(
    WorkspaceState& state,
    IRendererAdapter& renderer,
    QWidget* viewportHost)
{
    const OperationResult mounted = renderer.mount(viewportHost);
    if (!mounted.ok)
        state.enterFatalError();
    return mounted;
}

void mergeStartupNotice(
    WorkspaceImportOutcome& outcome,
    const QString& notice)
{
    if (outcome.result.ok)
        appendNoticeLine(outcome.notice, notice);
    else
        appendNoticeLine(outcome.result.error, notice);
}

int presentFatalStartupError(const OperationResult& failure, QWidget* parent)
{
    QMessageBox box(
        QMessageBox::Critical,
        QStringLiteral("Mesh Compare Startup Error"),
        QStringLiteral("Mesh Compare could not start."),
        QMessageBox::Close,
        parent);
    box.setInformativeText(failure.error);
    box.setTextInteractionFlags(
        Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    box.exec();
    return EXIT_FAILURE;
}
