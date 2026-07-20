#include "camera_panel.h"

#include "camera_commands.h"
#include "../core/workspace_state.h"

#include <utility>

#include <QClipboard>
#include <QFont>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QLayout>
#include <QLineEdit>
#include <QListWidget>
#include <QPalette>
#include <QPushButton>
#include <QSignalBlocker>
#include <QVBoxLayout>
#include <QVariant>

namespace
{
constexpr int PoseViewIdRole = Qt::UserRole;
constexpr int PoseTagsRole = Qt::UserRole + 1;
}

CameraPanel::CameraPanel(
    WorkspaceState& state,
    ICameraCommands& commands,
    QWidget* parent,
    CameraPanelServices services)
    : QFrame(parent),
      state_(state),
      commands_(commands),
      services_(std::move(services))
{
    if (!services_.copyImageToClipboard) {
        services_.copyImageToClipboard = [](const QImage& image) {
            QClipboard* clipboard = QGuiApplication::clipboard();
            if (clipboard == nullptr)
                return false;
            clipboard->setImage(image, QClipboard::Clipboard);
            return true;
        };
    }

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

    uidInput_ = new QLineEdit(this);
    uidInput_->setObjectName(QStringLiteral("cameraUidInput"));
    uidInput_->setPlaceholderText(tr("Enter workspace UID"));
    root->addWidget(uidInput_);

    saveButton_ = new QPushButton(tr("Save Current Pose"), this);
    saveButton_->setObjectName(QStringLiteral("saveCameraPoseButton"));
    root->addWidget(saveButton_);

    saveAndCopyScreenshotButton_ =
        new QPushButton(tr("Save Pose & Copy Screenshot"), this);
    saveAndCopyScreenshotButton_->setObjectName(
        QStringLiteral("saveCameraPoseAndCopyScreenshotButton"));
    saveAndCopyScreenshotButton_->setToolTip(
        tr("Save the current pose and copy the complete 3D viewport area"));
    root->addWidget(saveAndCopyScreenshotButton_);

    poseList_ = new QListWidget(this);
    poseList_->setObjectName(QStringLiteral("cameraPoseList"));
    poseList_->setSelectionMode(QAbstractItemView::SingleSelection);
    poseList_->setMinimumHeight(144);
    root->addWidget(poseList_);

    tagInput_ = new QLineEdit(this);
    tagInput_->setObjectName(QStringLiteral("cameraPoseTagInput"));
    tagInput_->setPlaceholderText(tr("Tags (comma separated)"));
    root->addWidget(tagInput_);

    updateTagsButton_ = new QPushButton(tr("Update Tags"), this);
    updateTagsButton_->setObjectName(QStringLiteral("updateCameraPoseTagsButton"));
    root->addWidget(updateTagsButton_);

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
        [this] {
            refreshTagInput();
            updateActionState();
        });
    connect(
        uidInput_,
        &QLineEdit::textChanged,
        this,
        &CameraPanel::updateActionState);
    connect(uidInput_, &QLineEdit::editingFinished, this, [this] {
        const OperationResult result = commitUidInput(true);
        reportFailure(result);
    });
    connect(saveButton_, &QPushButton::clicked, this, [this] {
        const OperationResult selectedUid = commitUidInput(false);
        if (!selectedUid.ok) {
            reportFailure(selectedUid);
            return;
        }
        const OperationResult result = commands_.saveCurrentCameraPose();
        if (!result.ok) {
            reportFailure(result);
            return;
        }
        refreshFromState();
    });
    connect(
        saveAndCopyScreenshotButton_,
        &QPushButton::clicked,
        this,
        [this] {
            const OperationResult selectedUid = commitUidInput(false);
            if (!selectedUid.ok) {
                reportFailure(selectedUid);
                return;
            }

            QImage screenshot;
            const OperationResult result =
                commands_.saveCurrentCameraPoseWithScreenshot(screenshot);
            if (!result.ok) {
                reportFailure(result);
                return;
            }

            refreshFromState();
            if (screenshot.isNull() ||
                !services_.copyImageToClipboard(screenshot)) {
                reportFailure(OperationResult::failure(tr(
                    "Camera pose was saved, but the screenshot could not be copied.")));
            }
        });
    connect(
        applyButton_,
        &QPushButton::clicked,
        this,
        &CameraPanel::applySelectedPose);
    connect(
        updateTagsButton_,
        &QPushButton::clicked,
        this,
        &CameraPanel::updateSelectedPoseTags);
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
    {
        const QSignalBlocker tagBlocker(tagInput_);
        tagInput_->clear();
    }
    {
        const QSignalBlocker inputBlocker(uidInput_);
        uidInput_->setText(
            workspaceUuid_.isEmpty()
                ? snapshot.suggestedWorkspaceUid
                : workspaceUuid_);
    }

    if (!snapshotAvailable_) {
        uuidLabel_->setText(tr("Camera poses are unavailable."));
        uidInput_->setEnabled(false);
        poseList_->setEnabled(false);
        updateActionState();
        reportFailure(snapshot.result);
        return;
    }

    uidInput_->setEnabled(state_.phase() == WorkspacePhase::Ready);
    if (workspaceUuid_.isEmpty()) {
        uuidLabel_->setText(
            snapshot.suggestedWorkspaceUid.isEmpty()
                ? tr("Enter a workspace UID.")
                : tr("No UID was detected. Confirm or edit it."));
        poseList_->setEnabled(false);
        updateActionState();
        return;
    }

    uuidLabel_->setText(tr("Workspace UID"));
    poseList_->setEnabled(true);
    for (const CameraPoseSummary& pose : snapshot.poses) {
        QString label = pose.savedAtUtc.isEmpty()
            ? pose.viewId
            : tr("%1 — %2").arg(pose.viewId, pose.savedAtUtc);
        if (!pose.tags.isEmpty()) {
            QStringList renderedTags;
            renderedTags.reserve(pose.tags.size());
            for (const QString& tag : pose.tags)
                renderedTags.append(QStringLiteral("#%1").arg(tag));
            label.append(tr(" · %1").arg(renderedTags.join(QLatin1Char(' '))));
        }
        auto* item = new QListWidgetItem(label, poseList_);
        item->setData(PoseViewIdRole, pose.viewId);
        item->setData(PoseTagsRole, pose.tags);
    }
    updateActionState();
}

OperationResult CameraPanel::commitUidInput(bool refreshAfterCommit)
{
    const QString enteredUid = uidInput_->text().trimmed();
    if (enteredUid.isEmpty()) {
        return OperationResult::failure(
            tr("Enter a workspace UID before using camera poses."));
    }
    if (!workspaceUuid_.isEmpty() &&
        enteredUid.compare(workspaceUuid_, Qt::CaseInsensitive) == 0) {
        return OperationResult::success();
    }

    const OperationResult result = commands_.setCameraPoseUid(enteredUid);
    if (result.ok && refreshAfterCommit)
        refreshFromState();
    return result;
}

QString CameraPanel::selectedViewId() const
{
    const QListWidgetItem* item = poseList_->currentItem();
    return item == nullptr
        ? QString()
        : item->data(PoseViewIdRole).toString();
}

void CameraPanel::refreshTagInput()
{
    const QListWidgetItem* item = poseList_->currentItem();
    const QStringList tags = item == nullptr
        ? QStringList()
        : item->data(PoseTagsRole).toStringList();
    const QSignalBlocker blocker(tagInput_);
    tagInput_->setText(tags.join(QStringLiteral(", ")));
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

void CameraPanel::updateSelectedPoseTags()
{
    const QString viewId = selectedViewId();
    if (viewId.isEmpty()) {
        updateActionState();
        return;
    }
    const OperationResult result = commands_.setCameraPoseTags(
        viewId,
        tagInput_->text().split(QLatin1Char(','), Qt::SkipEmptyParts));
    if (!result.ok) {
        reportFailure(result);
        return;
    }
    refreshFromState();
    for (int index = 0; index < poseList_->count(); ++index) {
        QListWidgetItem* item = poseList_->item(index);
        if (item != nullptr && item->data(PoseViewIdRole).toString() == viewId) {
            poseList_->setCurrentItem(item);
            break;
        }
    }
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
    const QString enteredUid = uidInput_->text().trimmed();
    const bool hasUidInput = snapshotAvailable_ && !enteredUid.isEmpty();
    const bool inputMatchesSnapshot =
        !workspaceUuid_.isEmpty() &&
        enteredUid.compare(workspaceUuid_, Qt::CaseInsensitive) == 0;
    const bool hasSelection = !selectedViewId().isEmpty();
    saveButton_->setEnabled(ready && hasUidInput);
    saveAndCopyScreenshotButton_->setEnabled(ready && hasUidInput);
    applyButton_->setEnabled(ready && inputMatchesSnapshot && hasSelection);
    tagInput_->setEnabled(ready && inputMatchesSnapshot && hasSelection);
    updateTagsButton_->setEnabled(ready && inputMatchesSnapshot && hasSelection);
    deleteButton_->setEnabled(ready && inputMatchesSnapshot && hasSelection);
}

void CameraPanel::reportFailure(const OperationResult& result)
{
    if (!result.ok && !result.error.isEmpty())
        emit statusMessage(result.error);
}
