#include "camera_panel.h"

#include "camera_commands.h"
#include "tag_editor.h"
#include "../core/workspace_state.h"

#include <utility>

#include <QClipboard>
#include <QDateTime>
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

QDateTime parseSavedAtUtc(const QString& value)
{
    QDateTime parsed = QDateTime::fromString(value, Qt::ISODateWithMs);
    if (!parsed.isValid())
        parsed = QDateTime::fromString(value, Qt::ISODate);
    return parsed;
}

const CameraPoseSummary* mostRecentPose(
    const QVector<CameraPoseSummary>& poses)
{
    if (poses.isEmpty())
        return nullptr;

    const CameraPoseSummary* mostRecent = &poses.front();
    QDateTime mostRecentAt = parseSavedAtUtc(mostRecent->savedAtUtc);
    for (int index = 1; index < poses.size(); ++index) {
        const CameraPoseSummary* candidate = &poses.at(index);
        const QDateTime candidateAt = parseSavedAtUtc(candidate->savedAtUtc);
        const bool candidateIsMoreRecent =
            (!mostRecentAt.isValid() && candidateAt.isValid()) ||
            (candidateAt.isValid() == mostRecentAt.isValid() &&
             (!candidateAt.isValid() || candidateAt >= mostRecentAt));
        if (candidateIsMoreRecent) {
            mostRecent = candidate;
            mostRecentAt = candidateAt;
        }
    }
    return mostRecent;
}
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

    auto* newPoseTagRow = new QHBoxLayout;
    auto* newPoseTagLabel = new QLabel(tr("New pose tags"), this);
    newPoseTagRow->addWidget(newPoseTagLabel);
    newPoseTagEditor_ = new TagEditor(this);
    newPoseTagEditor_->setObjectName(QStringLiteral("newPoseTagEditor"));
    newPoseTagEditor_->setAccessibleName(tr("Tags for new pose"));
    newPoseTagLabel->setBuddy(newPoseTagEditor_);
    newPoseTagRow->addWidget(newPoseTagEditor_, 1);
    root->addLayout(newPoseTagRow);

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

    auto* selectedPoseTagRow = new QHBoxLayout;
    selectedPoseTagLabel_ = new QLabel(tr("Selected pose tags"), this);
    selectedPoseTagLabel_->setObjectName(QStringLiteral("selectedPoseTagLabel"));
    selectedPoseTagRow->addWidget(selectedPoseTagLabel_);
    selectedPoseTagEditor_ = new TagEditor(this);
    selectedPoseTagEditor_->setObjectName(QStringLiteral("selectedPoseTagEditor"));
    selectedPoseTagEditor_->setAccessibleName(tr("Tags for selected pose"));
    selectedPoseTagLabel_->setBuddy(selectedPoseTagEditor_);
    selectedPoseTagRow->addWidget(selectedPoseTagEditor_, 1);
    root->addLayout(selectedPoseTagRow);

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
            refreshSelectedTagEditor();
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
        QString savedViewId;
        const QStringList tags = newPoseTagEditor_->tags();
        const OperationResult result = commands_.saveCurrentCameraPose(
            &savedViewId, tags);
        if (!result.ok) {
            reportFailure(result);
            return;
        }
        refreshFromState();
        rememberTags(tags);
        rememberTagsForView(savedViewId);
        newPoseTagEditor_->load(lastUsedTags_);
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
            QString savedViewId;
            const QStringList tags = newPoseTagEditor_->tags();
            const OperationResult result =
                commands_.saveCurrentCameraPoseWithScreenshot(
                    screenshot, &savedViewId, tags);
            if (!result.ok) {
                reportFailure(result);
                return;
            }

            refreshFromState();
            rememberTags(tags);
            rememberTagsForView(savedViewId);
            newPoseTagEditor_->load(lastUsedTags_);
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
    const bool workspaceChanged = tagHistoryUuid_ != workspaceUuid_;
    if (workspaceChanged) {
        tagHistoryUuid_ = workspaceUuid_;
        knownTags_.clear();
        lastUsedTags_.clear();
        pendingTagUpdates_.clear();
        hasLastUsedTags_ = false;
        newPoseTagEditor_->setSuggestions({});
        selectedPoseTagEditor_->setSuggestions({});
    }
    if (snapshotAvailable_ && !hasLastUsedTags_) {
        const CameraPoseSummary* latest = mostRecentPose(snapshot.poses);
        if (latest != nullptr) {
            lastUsedTags_ = latest->tags;
            hasLastUsedTags_ = true;
        }
    }
    if (workspaceChanged)
        newPoseTagEditor_->load(lastUsedTags_);
    poseList_->clear();
    selectedPoseTagEditor_->load({});
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
        auto pending = pendingTagUpdates_.find(pose.viewId);
        if (pending != pendingTagUpdates_.end() && pending.value() == pose.tags)
            pendingTagUpdates_.erase(pending);
    }
    updateTagSuggestions(snapshot);
    for (const CameraPoseSummary& pose : snapshot.poses) {
        const auto pending = pendingTagUpdates_.constFind(pose.viewId);
        const QStringList poseTags = pending == pendingTagUpdates_.constEnd()
            ? pose.tags
            : pending.value();
        QString label = pose.savedAtUtc.isEmpty()
            ? pose.viewId
            : tr("%1 — %2").arg(pose.viewId, pose.savedAtUtc);
        if (!poseTags.isEmpty()) {
            QStringList renderedTags;
            renderedTags.reserve(poseTags.size());
            for (const QString& tag : poseTags)
                renderedTags.append(QStringLiteral("#%1").arg(tag));
            label.append(tr(" · %1").arg(renderedTags.join(QLatin1Char(' '))));
        }
        auto* item = new QListWidgetItem(label, poseList_);
        item->setData(PoseViewIdRole, pose.viewId);
        item->setData(PoseTagsRole, poseTags);
    }
    refreshSelectedTagEditor();
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

void CameraPanel::refreshSelectedTagEditor()
{
    const QListWidgetItem* item = poseList_->currentItem();
    selectedPoseTagLabel_->setText(
        item == nullptr
            ? tr("Selected pose tags")
            : tr("Selected pose tags — %1")
                  .arg(item->data(PoseViewIdRole).toString()));
    selectedPoseTagEditor_->load(
        item == nullptr
            ? QStringList()
            : item->data(PoseTagsRole).toStringList());
}

void CameraPanel::updateTagSuggestions(const CameraPanelSnapshot& snapshot)
{
    knownTags_.clear();
    appendKnownTags(lastUsedTags_);
    for (const CameraPoseSummary& pose : snapshot.poses)
        appendKnownTags(pose.tags);
    for (auto pending = pendingTagUpdates_.cbegin();
         pending != pendingTagUpdates_.cend();
         ++pending) {
        appendKnownTags(pending.value());
    }
    publishKnownTags();
}

void CameraPanel::appendKnownTags(const QStringList& tags)
{
    for (const QString& tag : tags) {
        if (!tag.isEmpty() && !knownTags_.contains(tag, Qt::CaseInsensitive))
            knownTags_.append(tag);
    }
}

void CameraPanel::publishKnownTags()
{
    newPoseTagEditor_->setSuggestions(knownTags_);
    selectedPoseTagEditor_->setSuggestions(knownTags_);
}

void CameraPanel::rememberKnownTags(const QStringList& tags)
{
    appendKnownTags(tags);
    publishKnownTags();
}

void CameraPanel::rememberTags(const QStringList& tags)
{
    lastUsedTags_ = tags;
    hasLastUsedTags_ = true;
    rememberKnownTags(tags);
}

void CameraPanel::rememberTagsForView(const QString& viewId)
{
    for (int index = 0; index < poseList_->count(); ++index) {
        const QListWidgetItem* item = poseList_->item(index);
        if (item == nullptr || item->data(PoseViewIdRole).toString() != viewId)
            continue;
        rememberTags(item->data(PoseTagsRole).toStringList());
        return;
    }
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
    const QStringList tags = selectedPoseTagEditor_->tags();
    const OperationResult result = commands_.setCameraPoseTags(viewId, tags);
    if (!result.ok) {
        reportFailure(result);
        return;
    }
    pendingTagUpdates_.insert(viewId, tags);
    refreshFromState();
    rememberKnownTags(tags);
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
    pendingTagUpdates_.remove(viewId);
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
    newPoseTagEditor_->setEnabled(ready && hasUidInput);
    selectedPoseTagLabel_->setEnabled(
        ready && inputMatchesSnapshot && hasSelection);
    selectedPoseTagEditor_->setEnabled(
        ready && inputMatchesSnapshot && hasSelection);
    updateTagsButton_->setEnabled(ready && inputMatchesSnapshot && hasSelection);
    deleteButton_->setEnabled(ready && inputMatchesSnapshot && hasSelection);
}

void CameraPanel::reportFailure(const OperationResult& result)
{
    if (!result.ok && !result.error.isEmpty())
        emit statusMessage(result.error);
}
