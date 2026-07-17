#include "standalone_main_window.h"

#include "camera_commands.h"
#include "camera_panel.h"
#include "coloring_commands.h"
#include "coloring_panel.h"
#include "workspace_controller.h"
#include "../core/workspace_state.h"

#include <algorithm>

#include <QAbstractButton>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QMimeData>
#include <QPushButton>
#include <QResizeEvent>
#include <QSizePolicy>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>
#include <QWidget>

namespace
{
bool localFilePaths(const QMimeData& mimeData, QStringList* paths)
{
    const QList<QUrl> urls = mimeData.urls();
    if (urls.isEmpty() ||
        !std::all_of(
            urls.cbegin(),
            urls.cend(),
            [](const QUrl& url) { return url.isLocalFile(); })) {
        return false;
    }

    if (paths != nullptr) {
        paths->clear();
        paths->reserve(urls.size());
        for (const QUrl& url : urls)
            paths->append(url.toLocalFile());
    }
    return true;
}
} // namespace

StandaloneMainWindow::StandaloneMainWindow(WorkspaceState& state, QWidget* parent)
    : QMainWindow(parent), state_(state)
{
    setWindowTitle(QStringLiteral("Mesh Compare"));
    setAcceptDrops(true);

    auto* root = new QWidget(this);
    auto* layout = new QVBoxLayout(root);
    layout->setContentsMargins(8, 8, 8, 6);
    layout->setSpacing(6);

    commandBar_ = buildCommandBar(root);
    commandBar_->setObjectName("commandBar");
    viewportHost_ = new QWidget(root);
    viewportHost_->setObjectName("viewportHost");
    viewportHost_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    statusLabel_ = new QLabel(
        tr("Open a 2–8 layer MeshLab project or import 2–8 meshes to begin."),
        root);
    statusLabel_->setObjectName("statusLabel");

    layout->addWidget(commandBar_);
    layout->addWidget(viewportHost_, 1);
    layout->addWidget(statusLabel_);
    setCentralWidget(root);
    refreshWorkspace();
}

QWidget* StandaloneMainWindow::viewportHost() const
{
    return viewportHost_;
}

void StandaloneMainWindow::requestImport(const QStringList& paths)
{
    if (paths.isEmpty())
        return;
    if (!state_.meshes().isEmpty() && !confirmWorkspaceReplacement())
        return;
    emit importRequested(paths);
}

void StandaloneMainWindow::presentImportOutcome(const WorkspaceImportOutcome& outcome)
{
    if (!outcome.result.ok) {
        showStatusMessage(outcome.result.error);
        if (!outcome.fileErrors.isEmpty()) {
            QStringList details;
            details.reserve(outcome.fileErrors.size());
            for (const auto& fileError : outcome.fileErrors) {
                details.append(
                    QStringLiteral("%1\n%2").arg(fileError.first, fileError.second));
            }
            QMessageBox box(
                QMessageBox::Warning,
                tr("Import Failed"),
                tr("One or more mesh files could not be imported."),
                QMessageBox::Ok,
                this);
            box.setDetailedText(details.join(QStringLiteral("\n\n")));
            box.exec();
        }
        return;
    }

    refreshWorkspace();
    if (!outcome.notice.isEmpty())
        showStatusMessage(outcome.notice);
    else
        showStatusMessage(tr("Imported %1 meshes.").arg(state_.meshes().size()));
}

void StandaloneMainWindow::refreshWorkspace()
{
    const bool actionsEnabled = state_.phase() == WorkspacePhase::Ready ||
                                state_.phase() == WorkspacePhase::Analyzing;
    coloringButton_->setEnabled(actionsEnabled);
    cameraButton_->setEnabled(actionsEnabled);
    meshCountLabel_->setText(tr("%1 meshes").arg(state_.meshes().size()));
    if (coloringPanel_ != nullptr)
        coloringPanel_->refreshFromState();
    if (cameraPanel_ != nullptr && !cameraPanel_->isHidden())
        cameraPanel_->refreshFromState();
}

void StandaloneMainWindow::bindColoringCommands(IColoringCommands& commands)
{
    unbindColoringCommands();
    coloringPanel_ = new ColoringPanel(state_, commands, centralWidget());
    coloringPanel_->hide();
    connect(
        coloringPanel_,
        &ColoringPanel::statusMessage,
        this,
        &StandaloneMainWindow::presentPanelFailure);
    refreshWorkspace();
}

void StandaloneMainWindow::unbindColoringCommands()
{
    delete coloringPanel_;
    coloringPanel_ = nullptr;
}

void StandaloneMainWindow::bindCameraCommands(ICameraCommands& commands)
{
    unbindCameraCommands();
    cameraPanel_ = new CameraPanel(state_, commands, centralWidget());
    cameraPanel_->hide();
    connect(
        cameraPanel_,
        &CameraPanel::statusMessage,
        this,
        &StandaloneMainWindow::presentPanelFailure);
    refreshWorkspace();
}

void StandaloneMainWindow::unbindCameraCommands()
{
    delete cameraPanel_;
    cameraPanel_ = nullptr;
}

void StandaloneMainWindow::presentAnalysisProgress(
    quint64 generation,
    quint64 batchSerial,
    MeshId meshId,
    int percent,
    const QString& message)
{
    if (coloringPanel_ != nullptr) {
        coloringPanel_->presentAnalysisProgress(
            generation,
            batchSerial,
            meshId,
            percent,
            message);
    }
}

void StandaloneMainWindow::setDiagnosticsMenu(QMenu* menu)
{
    diagnosticsButton_->setMenu(menu);
    diagnosticsButton_->setPopupMode(
        menu == nullptr ? QToolButton::DelayedPopup
                        : QToolButton::InstantPopup);
}

void StandaloneMainWindow::presentAnalysisFinished(
    const AnalysisBatchResult& result)
{
    if (coloringPanel_ != nullptr)
        coloringPanel_->presentAnalysisFinished(result);
    refreshWorkspace();
}

void StandaloneMainWindow::showStatusMessage(const QString& message)
{
    statusLabel_->setText(message);
}

void StandaloneMainWindow::presentPanelFailure(const QString& message)
{
    showStatusMessage(message);
    emit operationFailed(message);
}

void StandaloneMainWindow::dragEnterEvent(QDragEnterEvent* event)
{
    if (localFilePaths(*event->mimeData(), nullptr))
        event->acceptProposedAction();
    else
        event->ignore();
}

void StandaloneMainWindow::dropEvent(QDropEvent* event)
{
    QStringList paths;
    if (!localFilePaths(*event->mimeData(), &paths)) {
        event->ignore();
        return;
    }

    event->acceptProposedAction();
    requestImport(paths);
}

void StandaloneMainWindow::resizeEvent(QResizeEvent* event)
{
    QMainWindow::resizeEvent(event);
    if (coloringPanel_ != nullptr && !coloringPanel_->isHidden())
        positionColoringPanel();
    if (cameraPanel_ != nullptr && !cameraPanel_->isHidden())
        positionCameraPanel();
}

QWidget* StandaloneMainWindow::buildCommandBar(QWidget* parent)
{
    auto* commandBar = new QWidget(parent);
    auto* layout = new QHBoxLayout(commandBar);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(6);

    importMeshesButton_ = new QPushButton(tr("Open"), commandBar);
    importMeshesButton_->setObjectName("importMeshesButton");
    coloringButton_ = new QPushButton(tr("Coloring"), commandBar);
    coloringButton_->setObjectName("coloringButton");
    cameraButton_ = new QPushButton(tr("Camera"), commandBar);
    cameraButton_->setObjectName("cameraButton");
    coloringButton_->setEnabled(false);
    cameraButton_->setEnabled(false);

    auto* linkedCameraLabel = new QLabel(tr("Linked cameras: On"), commandBar);
    meshCountLabel_ = new QLabel(tr("%1 meshes").arg(state_.meshes().size()), commandBar);
    meshCountLabel_->setObjectName("meshCountLabel");
    diagnosticsButton_ = new QToolButton(commandBar);
    diagnosticsButton_->setObjectName(QStringLiteral("diagnosticsButton"));
    diagnosticsButton_->setText(QStringLiteral("…"));

    layout->addWidget(importMeshesButton_);
    layout->addWidget(coloringButton_);
    layout->addWidget(cameraButton_);
    layout->addStretch(1);
    layout->addWidget(linkedCameraLabel);
    layout->addWidget(meshCountLabel_);
    layout->addWidget(diagnosticsButton_);

    connect(importMeshesButton_, &QPushButton::clicked, this, [this] {
        const QStringList paths = QFileDialog::getOpenFileNames(
            this,
            tr("Open Meshes or MeshLab Project"),
            {},
            tr("MeshLab Project (*.mlp);;Mesh Files (*.obj *.ply *.stl *.off);;All Files (*)"));
        requestImport(paths);
    });
    connect(
        coloringButton_,
        &QPushButton::clicked,
        this,
        &StandaloneMainWindow::toggleColoringPanel);
    connect(
        cameraButton_,
        &QPushButton::clicked,
        this,
        &StandaloneMainWindow::toggleCameraPanel);
    return commandBar;
}

void StandaloneMainWindow::toggleColoringPanel()
{
    if (coloringPanel_ == nullptr)
        return;
    if (!coloringPanel_->isHidden()) {
        coloringPanel_->hide();
        return;
    }

    if (cameraPanel_ != nullptr)
        cameraPanel_->hide();
    coloringPanel_->refreshFromState();
    positionColoringPanel();
    coloringPanel_->show();
    coloringPanel_->raise();
}

void StandaloneMainWindow::positionColoringPanel()
{
    if (coloringPanel_ == nullptr || centralWidget() == nullptr)
        return;
    coloringPanel_->adjustSize();
    const QPoint anchor = coloringButton_->mapTo(
        centralWidget(), QPoint(0, coloringButton_->height() + 4));
    const int maximumX = qMax(0, centralWidget()->width() - coloringPanel_->width());
    coloringPanel_->move(qBound(0, anchor.x(), maximumX), anchor.y());
}

void StandaloneMainWindow::toggleCameraPanel()
{
    if (cameraPanel_ == nullptr)
        return;
    if (!cameraPanel_->isHidden()) {
        cameraPanel_->hide();
        return;
    }

    if (coloringPanel_ != nullptr)
        coloringPanel_->hide();
    cameraPanel_->refreshFromState();
    positionCameraPanel();
    cameraPanel_->show();
    cameraPanel_->raise();
}

void StandaloneMainWindow::positionCameraPanel()
{
    if (cameraPanel_ == nullptr || centralWidget() == nullptr)
        return;
    cameraPanel_->adjustSize();
    const QPoint anchor = cameraButton_->mapTo(
        centralWidget(), QPoint(0, cameraButton_->height() + 4));
    const int maximumX = qMax(0, centralWidget()->width() - cameraPanel_->width());
    const int maximumY = qMax(0, centralWidget()->height() - cameraPanel_->height());
    cameraPanel_->move(
        qBound(0, anchor.x(), maximumX),
        qBound(0, anchor.y(), maximumY));
}

bool StandaloneMainWindow::confirmWorkspaceReplacement()
{
    QMessageBox box(
        QMessageBox::Question,
        tr("Replace Current Workspace?"),
        tr("This will replace the current meshes and temporary coloring results."),
        QMessageBox::Cancel,
        this);
    QAbstractButton* replaceButton =
        box.addButton(tr("Replace"), QMessageBox::AcceptRole);
    box.exec();
    return box.clickedButton() == replaceButton;
}
