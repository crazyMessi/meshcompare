#include "standalone_main_window.h"

#include "camera_commands.h"
#include "camera_panel.h"
#include "coloring_commands.h"
#include "coloring_panel.h"
#include "app_theme.h"
#include "workspace_controller.h"
#include "../core/workspace_state.h"

#include <algorithm>

#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QButtonGroup>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QEvent>
#include <QFileDialog>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QMenu>
#include <QMimeData>
#include <QMouseEvent>
#include <QPushButton>
#include <QResizeEvent>
#include <QSizePolicy>
#include <QStyle>
#include <QTimer>
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

int visibleMeshCount(const WorkspaceState& state)
{
    int count = 0;
    for (const MeshEntry& mesh : state.meshes())
        count += mesh.visible ? 1 : 0;
    return count;
}

bool canChangeLayerVisibility(
    const WorkspaceState& state,
    const MeshEntry& mesh,
    int visibleCount)
{
    const bool actionsEnabled =
        state.phase() == WorkspacePhase::Ready ||
        state.phase() == WorkspacePhase::Analyzing;
    const bool gridVisibilityChangeEnabled =
        state.layoutMode() != SceneLayoutMode::ComparisonGrid ||
        state.phase() == WorkspacePhase::Ready;
    return actionsEnabled &&
           gridVisibilityChangeEnabled &&
           (!mesh.visible || visibleCount > 1);
}

bool belongsTo(const QObject* object, const QObject* ancestor)
{
    for (const QObject* current = object;
         current != nullptr;
         current = current->parent()) {
        if (current == ancestor)
            return true;
    }
    return false;
}

bool containsGlobalPosition(
    const QWidget* widget,
    const QPoint& globalPosition)
{
    if (widget == nullptr || !widget->isVisible())
        return false;
    return QRect(widget->mapToGlobal(QPoint(0, 0)), widget->size())
        .contains(globalPosition);
}

bool belongsToPanelInteraction(
    const QPoint& globalPosition,
    const QWidget* panel)
{
    if (containsGlobalPosition(panel, globalPosition))
        return true;

    const QWidget* popup = QApplication::activePopupWidget();
    if (popup != nullptr &&
        belongsTo(popup, panel) &&
        containsGlobalPosition(popup, globalPosition)) {
        return true;
    }
    const QWidget* modal = QApplication::activeModalWidget();
    return modal != nullptr &&
           belongsTo(modal, panel) &&
           containsGlobalPosition(modal, globalPosition);
}

QFrame* toolbarDivider(QWidget* parent)
{
    auto* divider = new QFrame(parent);
    divider->setObjectName(QStringLiteral("toolbarDivider"));
    divider->setFixedSize(1, 24);
    return divider;
}
} // namespace

StandaloneMainWindow::StandaloneMainWindow(WorkspaceState& state, QWidget* parent)
    : QMainWindow(parent), state_(state)
{
    setWindowTitle(QStringLiteral("Mesh Compare"));
    setAcceptDrops(true);
    setStyleSheet(meshCompareApplicationStyleSheet());
    qApp->installEventFilter(this);

    auto* root = new QWidget(this);
    root->setObjectName(QStringLiteral("workspaceRoot"));
    auto* layout = new QVBoxLayout(root);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    commandBar_ = buildCommandBar(root);
    commandBar_->setObjectName("commandBar");

    viewportFrame_ = new QFrame(root);
    viewportFrame_->setObjectName(QStringLiteral("viewportFrame"));
    auto* viewportLayout = new QVBoxLayout(viewportFrame_);
    viewportLayout->setContentsMargins(0, 0, 0, 0);
    viewportLayout->setSpacing(0);

    noticeArea_ = new QWidget(viewportFrame_);
    noticeArea_->setObjectName(QStringLiteral("noticeArea"));
    auto* noticeAreaLayout = new QHBoxLayout(noticeArea_);
    noticeAreaLayout->setContentsMargins(20, 12, 20, 8);
    noticeAreaLayout->setSpacing(0);
    noticeBanner_ = buildNoticeBanner(noticeArea_);
    noticeAreaLayout->addWidget(noticeBanner_);
    noticeAreaLayout->addStretch(1);
    noticeArea_->hide();

    viewportHost_ = new QWidget(viewportFrame_);
    viewportHost_->setObjectName("viewportHost");
    viewportHost_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    viewportLayout->addWidget(noticeArea_);
    viewportLayout->addWidget(viewportHost_, 1);

    statusBar_ = buildStatusBar(root);

    layout->addWidget(commandBar_);
    layout->addWidget(viewportFrame_, 1);
    layout->addWidget(statusBar_);
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
        showPassiveStatusMessage(
            tr("Imported %1 meshes.").arg(state_.meshes().size()));
}

void StandaloneMainWindow::refreshWorkspace()
{
    const bool actionsEnabled = state_.phase() == WorkspacePhase::Ready ||
                                state_.phase() == WorkspacePhase::Analyzing;
    const bool viewSwitchingEnabled =
        state_.phase() == WorkspacePhase::Ready;
    coloringButton_->setEnabled(actionsEnabled);
    cameraButton_->setEnabled(actionsEnabled);
    overlayViewButton_->setEnabled(viewSwitchingEnabled);
    gridViewButton_->setEnabled(viewSwitchingEnabled);
    overlayViewButton_->setChecked(
        state_.layoutMode() == SceneLayoutMode::Overlay);
    gridViewButton_->setChecked(
        state_.layoutMode() == SceneLayoutMode::ComparisonGrid);
    const int visibleCount = visibleMeshCount(state_);
    const bool grid = state_.layoutMode() == SceneLayoutMode::ComparisonGrid;
    const bool layersEnabled =
        actionsEnabled && (!grid || viewSwitchingEnabled);
    layersButton_->setEnabled(layersEnabled);
    layersButton_->setText(
        !state_.meshes().isEmpty()
            ? tr("Layers %1/%2")
                  .arg(visibleCount)
                  .arg(state_.meshes().size())
            : tr("Layers"));
    layersButton_->setToolTip(
        grid
            ? tr("Choose which mesh layers appear in Grid")
            : tr("Choose which mesh layers are visible in Overlay"));
    meshCountLabel_->setText(tr("%1 meshes").arg(state_.meshes().size()));
    updateWorkspaceStatus();
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
    noticeLabel_->setText(message);
    noticeBanner_->setVisible(!message.isEmpty());
    noticeArea_->setVisible(!message.isEmpty());
}

void StandaloneMainWindow::showPassiveStatusMessage(const QString& message)
{
    statusLabel_->setText(message);
    noticeArea_->hide();
}

void StandaloneMainWindow::presentPanelFailure(const QString& message)
{
    showStatusMessage(message);
    emit operationFailed(message);
}

bool StandaloneMainWindow::eventFilter(QObject* watched, QEvent* event)
{
    if (event->type() == QEvent::MouseButtonPress) {
        const auto* mouseEvent = static_cast<QMouseEvent*>(event);
        const QPoint globalPosition = mouseEvent->globalPos();
        if (coloringPanel_ != nullptr &&
            coloringPanel_->isVisible() &&
            !belongsToPanelInteraction(globalPosition, coloringPanel_) &&
            !containsGlobalPosition(coloringButton_, globalPosition)) {
            coloringPanel_->hide();
        }
        if (cameraPanel_ != nullptr &&
            cameraPanel_->isVisible() &&
            !belongsToPanelInteraction(globalPosition, cameraPanel_) &&
            !containsGlobalPosition(cameraButton_, globalPosition)) {
            cameraPanel_->hide();
        }
    }
    else if (event->type() == QEvent::ApplicationDeactivate) {
        if (coloringPanel_ != nullptr)
            coloringPanel_->hide();
        if (cameraPanel_ != nullptr)
            cameraPanel_->hide();
    }
    return QMainWindow::eventFilter(watched, event);
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

QFrame* StandaloneMainWindow::buildNoticeBanner(QWidget* parent)
{
    auto* banner = new QFrame(parent);
    banner->setObjectName(QStringLiteral("noticeBanner"));
    auto* layout = new QHBoxLayout(banner);
    layout->setContentsMargins(12, 8, 8, 8);
    layout->setSpacing(8);

    auto* icon = new QLabel(QStringLiteral("△"), banner);
    icon->setObjectName(QStringLiteral("noticeIcon"));
    icon->setAlignment(Qt::AlignCenter);
    noticeLabel_ = new QLabel(banner);
    noticeLabel_->setObjectName(QStringLiteral("noticeLabel"));
    noticeLabel_->setWordWrap(true);
    auto* dismissButton = new QToolButton(banner);
    dismissButton->setObjectName(QStringLiteral("dismissNoticeButton"));
    dismissButton->setText(QStringLiteral("×"));
    dismissButton->setToolTip(tr("Dismiss"));

    layout->addWidget(icon);
    layout->addWidget(noticeLabel_, 1);
    layout->addWidget(dismissButton);
    connect(
        dismissButton,
        &QToolButton::clicked,
        parent,
        &QWidget::hide);
    banner->hide();
    return banner;
}

QFrame* StandaloneMainWindow::buildStatusBar(QWidget* parent)
{
    auto* bar = new QFrame(parent);
    bar->setObjectName(QStringLiteral("statusBar"));
    bar->setFixedHeight(31);
    auto* layout = new QHBoxLayout(bar);
    layout->setContentsMargins(14, 0, 14, 0);
    layout->setSpacing(12);

    statusLabel_ = new QLabel(
        tr("Open a 2–8 layer MeshLab project or import 2–8 meshes to begin."),
        bar);
    statusLabel_->setObjectName(QStringLiteral("statusLabel"));
    statusLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    workspaceStatusLabel_ = new QLabel(bar);
    workspaceStatusLabel_->setObjectName(
        QStringLiteral("workspaceStatusLabel"));
    workspaceStatusLabel_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);

    layout->addWidget(statusLabel_, 1);
    layout->addWidget(workspaceStatusLabel_);
    return bar;
}

void StandaloneMainWindow::requestMeshVisibilityChange(
    MeshId meshId,
    bool visible)
{
    QTimer::singleShot(
        0,
        this,
        [this, meshId, visible] {
            emit meshVisibilityRequested(meshId, visible);
        });
}

void StandaloneMainWindow::updateWorkspaceStatus()
{
    if (workspaceStatusLabel_ == nullptr)
        return;

    QString text;
    QString phase;
    switch (state_.phase()) {
    case WorkspacePhase::Empty:
        text = tr("●  EMPTY");
        phase = QStringLiteral("inactive");
        break;
    case WorkspacePhase::Loading:
        text = tr("●  LOADING");
        phase = QStringLiteral("busy");
        break;
    case WorkspacePhase::Ready:
        text = tr("●  READY");
        phase = QStringLiteral("ready");
        break;
    case WorkspacePhase::Analyzing:
        text = tr("●  ANALYZING");
        phase = QStringLiteral("busy");
        break;
    case WorkspacePhase::FatalError:
        text = tr("●  RENDERER ERROR");
        phase = QStringLiteral("inactive");
        break;
    }
    workspaceStatusLabel_->setText(text);
    workspaceStatusLabel_->setProperty("phase", phase);
    workspaceStatusLabel_->style()->unpolish(workspaceStatusLabel_);
    workspaceStatusLabel_->style()->polish(workspaceStatusLabel_);
}

QWidget* StandaloneMainWindow::buildCommandBar(QWidget* parent)
{
    auto* commandBar = new QWidget(parent);
    commandBar->setFixedHeight(56);
    auto* layout = new QHBoxLayout(commandBar);
    layout->setContentsMargins(14, 8, 14, 8);
    layout->setSpacing(8);

    auto* brandLabel = new QLabel(tr("Mesh Compare"), commandBar);
    brandLabel->setObjectName(QStringLiteral("brandLabel"));
    importMeshesButton_ = new QPushButton(tr("Open"), commandBar);
    importMeshesButton_->setObjectName("importMeshesButton");
    coloringButton_ = new QPushButton(
        tr("Coloring") + QStringLiteral("  ▾"),
        commandBar);
    coloringButton_->setObjectName("coloringButton");
    cameraButton_ = new QPushButton(
        tr("Camera") + QStringLiteral("  ▾"),
        commandBar);
    cameraButton_->setObjectName("cameraButton");
    auto* viewLabel = new QLabel(tr("View:"), commandBar);
    viewLabel->setObjectName("viewModeLabel");
    overlayViewButton_ = new QPushButton(tr("Overlay"), commandBar);
    overlayViewButton_->setObjectName("overlayViewButton");
    overlayViewButton_->setCheckable(true);
    overlayViewButton_->setToolTip(
        tr("Show all mesh layers in one viewport"));
    gridViewButton_ = new QPushButton(tr("Grid"), commandBar);
    gridViewButton_->setObjectName("gridViewButton");
    gridViewButton_->setCheckable(true);
    gridViewButton_->setToolTip(
        tr("Show each mesh layer in its own viewport"));
    auto* viewModeGroup = new QButtonGroup(commandBar);
    viewModeGroup->setExclusive(true);
    viewModeGroup->addButton(overlayViewButton_);
    viewModeGroup->addButton(gridViewButton_);
    layersButton_ = new QToolButton(commandBar);
    layersButton_->setObjectName(QStringLiteral("layersButton"));
    layersButton_->setText(tr("Layers"));
    layersButton_->setToolButtonStyle(Qt::ToolButtonTextOnly);
    layersMenu_ = new QMenu(layersButton_);
    layersMenu_->setObjectName(QStringLiteral("layersMenu"));
    layersButton_->setMenu(layersMenu_);
    layersButton_->setPopupMode(QToolButton::InstantPopup);
    coloringButton_->setEnabled(false);
    cameraButton_->setEnabled(false);
    overlayViewButton_->setEnabled(false);
    gridViewButton_->setEnabled(false);
    layersButton_->setEnabled(false);

    auto* linkedCameraLabel = new QLabel(
        QStringLiteral(
            "<span style=\"color:#54C891\">●</span>&nbsp;&nbsp;%1")
            .arg(tr("Linked cameras")),
        commandBar);
    linkedCameraLabel->setObjectName(QStringLiteral("linkedCameraLabel"));
    linkedCameraLabel->setTextFormat(Qt::RichText);
    meshCountLabel_ = new QLabel(tr("%1 meshes").arg(state_.meshes().size()), commandBar);
    meshCountLabel_->setObjectName("meshCountLabel");
    diagnosticsButton_ = new QToolButton(commandBar);
    diagnosticsButton_->setObjectName(QStringLiteral("diagnosticsButton"));
    diagnosticsButton_->setText(QStringLiteral("…"));

    layout->addWidget(brandLabel);
    layout->addWidget(toolbarDivider(commandBar));
    layout->addWidget(importMeshesButton_);
    layout->addWidget(coloringButton_);
    layout->addWidget(cameraButton_);
    layout->addWidget(toolbarDivider(commandBar));
    layout->addWidget(viewLabel);
    layout->addWidget(overlayViewButton_);
    layout->addWidget(gridViewButton_);
    layout->addWidget(toolbarDivider(commandBar));
    layout->addWidget(layersButton_);
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
    connect(overlayViewButton_, &QPushButton::clicked, this, [this] {
        if (state_.layoutMode() != SceneLayoutMode::Overlay)
            emit layoutModeRequested(SceneLayoutMode::Overlay);
        refreshWorkspace();
    });
    connect(gridViewButton_, &QPushButton::clicked, this, [this] {
        if (state_.layoutMode() != SceneLayoutMode::ComparisonGrid)
            emit layoutModeRequested(SceneLayoutMode::ComparisonGrid);
        refreshWorkspace();
    });
    connect(
        layersMenu_,
        &QMenu::aboutToShow,
        this,
        &StandaloneMainWindow::rebuildLayersMenu);
    return commandBar;
}

void StandaloneMainWindow::rebuildLayersMenu()
{
    layersMenu_->clear();
    const int visibleCount = visibleMeshCount(state_);

    if (state_.meshes().isEmpty()) {
        QAction* empty = layersMenu_->addAction(tr("No mesh layers"));
        empty->setEnabled(false);
        return;
    }

    for (const MeshEntry& mesh : state_.meshes()) {
        QAction* action = layersMenu_->addAction(mesh.displayName);
        action->setObjectName(
            QStringLiteral("meshVisibilityAction_%1").arg(mesh.id));
        action->setCheckable(true);
        action->setChecked(mesh.visible);
        action->setEnabled(
            canChangeLayerVisibility(state_, mesh, visibleCount));
        connect(
            action,
            &QAction::toggled,
            this,
            [this, meshId = mesh.id](bool visible) {
                requestMeshVisibilityChange(meshId, visible);
            });
    }
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
