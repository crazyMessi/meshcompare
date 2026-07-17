#include "camera_panel.h"

#include "camera_commands.h"
#include "../core/workspace_state.h"

#include <QFont>
#include <QHBoxLayout>
#include <QLabel>
#include <QLayout>
#include <QListWidget>
#include <QPalette>
#include <QPushButton>
#include <QVBoxLayout>
#include <QVariant>

CameraPanel::CameraPanel(
    WorkspaceState& state,
    ICameraCommands& commands,
    QWidget* parent)
    : QFrame(parent), state_(state), commands_(commands)
{
    setObjectName(QStringLiteral("cameraPanel"));
    setWindowModality(Qt::NonModal);
    setFrameShape(QFrame::StyledPanel);
    setFrameShadow(QFrame::Raised);
    setBackgroundRole(QPalette::Window);
    setAutoFillBackground(true);
    setMinimumWidth(360);

    auto* root = new QVBoxLayout(this);
    root->setSizeConstraint(QLayout::SetFixedSize);
    root->setContentsMargins(12, 12, 12, 12);
    root->setSpacing(8);

    auto* title = new QLabel(tr("Camera"), this);
    QFont titleFont = title->font();
    titleFont.setBold(true);
    title->setFont(titleFont);
    root->addWidget(title);

    uuidLabel_ = new QLabel(this);
    uuidLabel_->setObjectName(QStringLiteral("cameraUuidLabel"));
    uuidLabel_->setWordWrap(true);
    root->addWidget(uuidLabel_);

    saveButton_ = new QPushButton(tr("Save Current Pose"), this);
    saveButton_->setObjectName(QStringLiteral("saveCameraPoseButton"));
    root->addWidget(saveButton_);

    poseList_ = new QListWidget(this);
    poseList_->setObjectName(QStringLiteral("cameraPoseList"));
    poseList_->setSelectionMode(QAbstractItemView::SingleSelection);
    poseList_->setMinimumHeight(144);
    root->addWidget(poseList_);

    auto* actions = new QHBoxLayout;
    deleteButton_ = new QPushButton(tr("Delete"), this);
    deleteButton_->setObjectName(QStringLiteral("deleteCameraPoseButton"));
    applyButton_ = new QPushButton(tr("Apply"), this);
    applyButton_->setObjectName(QStringLiteral("applyCameraPoseButton"));
    actions->addWidget(deleteButton_);
    actions->addStretch(1);
    actions->addWidget(applyButton_);
    root->addLayout(actions);

    connect(
        poseList_,
        &QListWidget::itemSelectionChanged,
        this,
        &CameraPanel::updateActionState);
    connect(saveButton_, &QPushButton::clicked, this, [this] {
        const OperationResult result = commands_.saveCurrentCameraPose();
        if (!result.ok) {
            reportFailure(result);
            return;
        }
        refreshFromState();
    });
    connect(
        applyButton_,
        &QPushButton::clicked,
        this,
        &CameraPanel::applySelectedPose);
    connect(
        deleteButton_,
        &QPushButton::clicked,
        this,
        &CameraPanel::deleteSelectedPose);

    refreshFromState();
}

void CameraPanel::refreshFromState()
{
    const CameraPanelSnapshot snapshot = commands_.cameraPanelSnapshot();
    workspaceUuid_ = snapshot.workspaceUuid;
    snapshotAvailable_ = snapshot.result.ok;
    poseList_->clear();

    if (!snapshotAvailable_) {
        uuidLabel_->setText(tr("Camera poses are unavailable."));
        poseList_->setEnabled(false);
        updateActionState();
        reportFailure(snapshot.result);
        return;
    }

    if (workspaceUuid_.isEmpty()) {
        uuidLabel_->setText(tr("No UUID was found in this workspace."));
        poseList_->setEnabled(false);
        updateActionState();
        return;
    }

    uuidLabel_->setText(tr("Workspace UUID: %1").arg(workspaceUuid_));
    poseList_->setEnabled(true);
    for (const CameraPoseSummary& pose : snapshot.poses) {
        const QString label = pose.savedAtUtc.isEmpty()
            ? pose.viewId
            : tr("%1 — %2").arg(pose.viewId, pose.savedAtUtc);
        auto* item = new QListWidgetItem(label, poseList_);
        item->setData(Qt::UserRole, pose.viewId);
    }
    updateActionState();
}

QString CameraPanel::selectedViewId() const
{
    const QListWidgetItem* item = poseList_->currentItem();
    return item == nullptr
        ? QString()
        : item->data(Qt::UserRole).toString();
}

void CameraPanel::applySelectedPose()
{
    // Copy the semantic ID before dispatch. A command is allowed to publish a
    // synchronous refresh, which can replace every QListWidgetItem.
    const QString viewId = selectedViewId();
    if (viewId.isEmpty()) {
        updateActionState();
        return;
    }
    reportFailure(commands_.applyCameraPose(viewId));
}

void CameraPanel::deleteSelectedPose()
{
    // Never retain a QListWidgetItem pointer across the semantic command.
    const QString viewId = selectedViewId();
    if (viewId.isEmpty()) {
        updateActionState();
        return;
    }
    const OperationResult result = commands_.deleteCameraPose(viewId);
    if (!result.ok) {
        reportFailure(result);
        return;
    }
    refreshFromState();
}

void CameraPanel::updateActionState()
{
    const bool ready = state_.phase() == WorkspacePhase::Ready;
    const bool hasUuid = snapshotAvailable_ && !workspaceUuid_.isEmpty();
    const bool hasSelection = !selectedViewId().isEmpty();
    saveButton_->setEnabled(ready && hasUuid);
    applyButton_->setEnabled(ready && hasUuid && hasSelection);
    deleteButton_->setEnabled(ready && hasUuid && hasSelection);
}

void CameraPanel::reportFailure(const OperationResult& result)
{
    if (!result.ok && !result.error.isEmpty())
        emit statusMessage(result.error);
}
