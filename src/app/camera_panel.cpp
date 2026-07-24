#include "camera_panel.h"

#include "camera_commands.h"
#include "screenshot_browser.h"
#include "tag_editor.h"
#include "../core/camera_pose_tags.h"
#include "../core/workspace_state.h"

#include <utility>

#include <QClipboard>
#include <QDateTime>
#include <QDesktopServices>
#include <QFileInfo>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QLayout>
#include <QLineEdit>
#include <QListWidget>
#include <QPalette>
#include <QPushButton>
#include <QSettings>
#include <QSignalBlocker>
#include <QVBoxLayout>
#include <QVariant>
#include <QUrl>

namespace
{
constexpr int PoseViewIdRole = Qt::UserRole;
constexpr int PoseTagsRole = Qt::UserRole + 1;
constexpr int PoseScreenshotPathRole = Qt::UserRole + 2;

QDateTime parseSavedAtUtc(const QString& value)
{
    QDateTime parsed = QDateTime::fromString(value, Qt::ISODateWithMs);
    if (!parsed.isValid())
        parsed = QDateTime::fromString(value, Qt::ISODate);
    return parsed;
}

const CameraPoseSummary* mostRecentTaggedPose(
    const QVector<CameraPoseSummary>& poses)
{
    const CameraPoseSummary* mostRecent = nullptr;
    QDateTime mostRecentAt;
    for (int index = 0; index < poses.size(); ++index) {
        const CameraPoseSummary* candidate = &poses.at(index);
        if (candidate->tags.isEmpty())
            continue;
        const QDateTime candidateAt = parseSavedAtUtc(candidate->savedAtUtc);
        const bool candidateIsMoreRecent =
            mostRecent == nullptr ||
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

QString poseDisplayTime(const QString& savedAtUtc)
{
    const QDateTime savedAt = parseSavedAtUtc(savedAtUtc);
    return savedAt.isValid()
        ? savedAt.toLocalTime().toString(QStringLiteral("MM-dd HH:mm"))
        : savedAtUtc;
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
    if (!services_.openLocalFolder) {
        services_.openLocalFolder = [](const QString& path) {
            return QDesktopServices::openUrl(QUrl::fromLocalFile(path));
        };
    }
    if (!services_.loadLastCaptureTags) {
        services_.loadLastCaptureTags = [](QStringList& tags) {
            QSettings settings;
            const QString key = QStringLiteral("camera/lastCaptureTags");
            if (!settings.contains(key))
                return false;
            tags = settings.value(key).toStringList();
            return true;
        };
    }
    if (!services_.saveLastCaptureTags) {
        services_.saveLastCaptureTags = [](const QStringList& tags) {
            QSettings settings;
            settings.setValue(
                QStringLiteral("camera/lastCaptureTags"),
                QVariant::fromValue(tags));
        };
    }

    setObjectName(QStringLiteral("cameraPanel"));
    setWindowModality(Qt::NonModal);
    setFrameShape(QFrame::StyledPanel);
    setFrameShadow(QFrame::Raised);
    setBackgroundRole(QPalette::Window);
    setAutoFillBackground(true);
    setMinimumWidth(400);

    auto* root = new QVBoxLayout(this);
    root->setSizeConstraint(QLayout::SetFixedSize);
    root->setContentsMargins(14, 14, 14, 14);
    root->setSpacing(10);

    auto* title = new QLabel(tr("Camera Views"), this);
    title->setObjectName(QStringLiteral("cameraPanelTitle"));
    root->addWidget(title);

    auto* subtitle = new QLabel(
        tr("Capture and revisit matched viewpoints"), this);
    subtitle->setObjectName(QStringLiteral("cameraPanelSubtitle"));
    root->addWidget(subtitle);

    auto* workspaceRow = new QHBoxLayout;
    workspaceRow->setSpacing(8);
    uuidLabel_ = new QLabel(tr("Workspace"), this);
    uuidLabel_->setObjectName(QStringLiteral("cameraUuidLabel"));
    uuidLabel_->setWordWrap(true);
    workspaceRow->addWidget(uuidLabel_);

    uidInput_ = new QLineEdit(this);
    uidInput_->setObjectName(QStringLiteral("cameraUidInput"));
    uidInput_->setPlaceholderText(tr("Enter workspace UID"));
    workspaceRow->addWidget(uidInput_, 1);
    root->addLayout(workspaceRow);

    auto* captureSection = new QFrame(this);
    captureSection->setObjectName(QStringLiteral("cameraCaptureSection"));
    auto* captureLayout = new QVBoxLayout(captureSection);
    captureLayout->setContentsMargins(10, 10, 10, 10);
    captureLayout->setSpacing(7);

    auto* captureTitle = new QLabel(tr("CAPTURE CURRENT VIEW"), captureSection);
    captureTitle->setObjectName(QStringLiteral("cameraSectionTitle"));
    captureLayout->addWidget(captureTitle);

    auto* newPoseTagLabel = new QLabel(tr("Tags"), captureSection);
    newPoseTagLabel->setObjectName(QStringLiteral("cameraFieldLabel"));
    captureLayout->addWidget(newPoseTagLabel);
    newPoseTagEditor_ = new TagEditor(captureSection);
    newPoseTagEditor_->setObjectName(QStringLiteral("newPoseTagEditor"));
    newPoseTagEditor_->setAccessibleName(tr("Tags for new pose"));
    newPoseTagLabel->setBuddy(newPoseTagEditor_);
    captureLayout->addWidget(newPoseTagEditor_);

    captureHintLabel_ = new QLabel(
        tr("Tags carry over after a successful save"), captureSection);
    captureHintLabel_->setObjectName(QStringLiteral("cameraHintLabel"));
    captureLayout->addWidget(captureHintLabel_);

    auto* saveActions = new QHBoxLayout;
    saveActions->setSpacing(7);
    saveButton_ = new QPushButton(tr("Save Screenshot"), captureSection);
    saveButton_->setObjectName(QStringLiteral("saveCameraPoseButton"));
    saveButton_->setToolTip(
        tr("Save the current pose with exactly one local screenshot"));
    saveActions->addWidget(saveButton_, 1);

    saveAndCopyScreenshotButton_ =
        new QPushButton(tr("Save & Copy"), captureSection);
    saveAndCopyScreenshotButton_->setObjectName(
        QStringLiteral("saveCameraPoseAndCopyScreenshotButton"));
    saveAndCopyScreenshotButton_->setToolTip(
        tr("Save the pose with one local screenshot and also copy it"));
    saveActions->addWidget(saveAndCopyScreenshotButton_);
    captureLayout->addLayout(saveActions);
    root->addWidget(captureSection);

    auto* librarySection = new QFrame(this);
    librarySection->setObjectName(QStringLiteral("cameraLibrarySection"));
    auto* libraryLayout = new QVBoxLayout(librarySection);
    libraryLayout->setContentsMargins(10, 10, 10, 10);
    libraryLayout->setSpacing(7);

    auto* libraryHeader = new QHBoxLayout;
    poseCountLabel_ = new QLabel(tr("Saved views · 0"), librarySection);
    poseCountLabel_->setObjectName(QStringLiteral("cameraSectionTitle"));
    libraryHeader->addWidget(poseCountLabel_);
    libraryHeader->addStretch(1);
    browseScreenshotsButton_ = new QPushButton(tr("Browse"), librarySection);
    browseScreenshotsButton_->setObjectName(
        QStringLiteral("browseCameraScreenshotsButton"));
    browseScreenshotsButton_->setToolTip(
        tr("Browse this workspace's saved screenshots by tag"));
    libraryHeader->addWidget(browseScreenshotsButton_);
    libraryLayout->addLayout(libraryHeader);

    poseList_ = new QListWidget(librarySection);
    poseList_->setObjectName(QStringLiteral("cameraPoseList"));
    poseList_->setSelectionMode(QAbstractItemView::SingleSelection);
    poseList_->setUniformItemSizes(true);
    poseList_->setSpacing(2);
    poseList_->setMinimumHeight(118);
    poseList_->setMaximumHeight(160);
    libraryLayout->addWidget(poseList_);

    emptyPoseLabel_ = new QLabel(tr("No saved views yet"), librarySection);
    emptyPoseLabel_->setObjectName(QStringLiteral("cameraEmptyStateLabel"));
    emptyPoseLabel_->setAlignment(Qt::AlignCenter);
    emptyPoseLabel_->setMinimumHeight(54);
    libraryLayout->addWidget(emptyPoseLabel_);
    root->addWidget(librarySection);

    selectionSection_ = new QFrame(this);
    selectionSection_->setObjectName(QStringLiteral("cameraSelectionSection"));
    auto* selectionLayout = new QVBoxLayout(selectionSection_);
    selectionLayout->setContentsMargins(10, 10, 10, 10);
    selectionLayout->setSpacing(7);

    selectedPoseTagLabel_ = new QLabel(tr("Edit saved view"), selectionSection_);
    selectedPoseTagLabel_->setObjectName(QStringLiteral("selectedPoseTagLabel"));
    selectionLayout->addWidget(selectedPoseTagLabel_);
    selectedPoseTagEditor_ = new TagEditor(selectionSection_);
    selectedPoseTagEditor_->setObjectName(QStringLiteral("selectedPoseTagEditor"));
    selectedPoseTagEditor_->setAccessibleName(tr("Tags for selected pose"));
    selectedPoseTagLabel_->setBuddy(selectedPoseTagEditor_);
    selectionLayout->addWidget(selectedPoseTagEditor_);

    openScreenshotFolderButton_ =
        new QPushButton(tr("Folder"), selectionSection_);
    openScreenshotFolderButton_->setObjectName(
        QStringLiteral("openCameraScreenshotFolderButton"));
    openScreenshotFolderButton_->setToolTip(
        tr("Open the folder containing the selected pose's screenshot"));

    auto* actions = new QHBoxLayout;
    actions->setSpacing(7);
    deleteButton_ = new QPushButton(tr("Delete"), selectionSection_);
    deleteButton_->setObjectName(QStringLiteral("deleteCameraPoseButton"));
    actions->addWidget(deleteButton_);
    actions->addWidget(openScreenshotFolderButton_);
    actions->addStretch(1);
    updateTagsButton_ = new QPushButton(tr("Update"), selectionSection_);
    updateTagsButton_->setObjectName(QStringLiteral("updateCameraPoseTagsButton"));
    actions->addWidget(updateTagsButton_);
    applyButton_ = new QPushButton(tr("Apply"), selectionSection_);
    applyButton_->setObjectName(QStringLiteral("applyCameraPoseButton"));
    actions->addWidget(applyButton_);
    selectionLayout->addLayout(actions);
    root->addWidget(selectionSection_);

    QStringList storedTags;
    if (services_.loadLastCaptureTags(storedTags)) {
        lastUsedTags_ =
            meshcompare::normalizeCameraPoseTags(storedTags);
        hasLastUsedTags_ = true;
        newPoseTagEditor_->load(lastUsedTags_);
        updateLastTagHint();
    }

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
        const QStringList tags = newPoseTagEditor_->tags();
        const OperationResult result = commands_.saveCurrentCameraPose(
            nullptr, tags);
        if (!result.ok) {
            reportFailure(result);
            return;
        }
        rememberTags(tags);
        refreshFromState();
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
            const QStringList tags = newPoseTagEditor_->tags();
            const OperationResult result =
                commands_.saveCurrentCameraPoseWithScreenshot(
                    screenshot, nullptr, tags);
            if (!result.ok) {
                reportFailure(result);
                return;
            }

            rememberTags(tags);
            refreshFromState();
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
        browseScreenshotsButton_,
        &QPushButton::clicked,
        this,
        &CameraPanel::openScreenshotBrowser);
    connect(
        openScreenshotFolderButton_,
        &QPushButton::clicked,
        this,
        &CameraPanel::openSelectedScreenshotFolder);
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
        pendingTagUpdates_.clear();
        newPoseTagEditor_->setSuggestions({});
        selectedPoseTagEditor_->setSuggestions({});
    }
    bool restoredFallbackTags = false;
    if (snapshotAvailable_ && !hasLastUsedTags_) {
        const CameraPoseSummary* latest =
            mostRecentTaggedPose(snapshot.poses);
        if (latest != nullptr) {
            lastUsedTags_ =
                meshcompare::normalizeCameraPoseTags(latest->tags);
            hasLastUsedTags_ = true;
            restoredFallbackTags = true;
            updateLastTagHint();
        }
    }
    if (restoredFallbackTags)
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
        clearBrowserPoses();
        uuidLabel_->setText(tr("Camera poses are unavailable."));
        uidInput_->setEnabled(false);
        poseList_->setEnabled(false);
        updatePoseListPresentation(tr("Saved views are unavailable"));
        updateActionState();
        reportFailure(snapshot.result);
        return;
    }

    uidInput_->setEnabled(state_.phase() == WorkspacePhase::Ready);
    if (workspaceUuid_.isEmpty()) {
        clearBrowserPoses();
        uuidLabel_->setText(
            snapshot.suggestedWorkspaceUid.isEmpty()
                ? tr("Enter a workspace UID.")
                : tr("No UID was detected. Confirm or edit it."));
        poseList_->setEnabled(false);
        updatePoseListPresentation(
            tr("Set a workspace UID to start saving views"));
        updateActionState();
        return;
    }

    uuidLabel_->setText(tr("Workspace"));
    uuidLabel_->setToolTip(workspaceUuid_);
    poseList_->setEnabled(true);
    for (const CameraPoseSummary& pose : snapshot.poses) {
        auto pending = pendingTagUpdates_.find(pose.viewId);
        if (pending != pendingTagUpdates_.end() && pending.value() == pose.tags)
            pendingTagUpdates_.erase(pending);
    }
    updateTagSuggestions(snapshot);
    refreshBrowserPoses(snapshot);
    for (const CameraPoseSummary& pose : snapshot.poses) {
        const auto pending = pendingTagUpdates_.constFind(pose.viewId);
        const QStringList poseTags = pending == pendingTagUpdates_.constEnd()
            ? pose.tags
            : pending.value();
        QString label = pose.viewId;
        if (!poseTags.isEmpty()) {
            QStringList renderedTags;
            renderedTags.reserve(poseTags.size());
            for (const QString& tag : poseTags)
                renderedTags.append(QStringLiteral("#%1").arg(tag));
            label.append(tr("  %1").arg(renderedTags.join(QLatin1Char(' '))));
        }
        const QString displayTime = poseDisplayTime(pose.savedAtUtc);
        if (!displayTime.isEmpty())
            label.append(tr("  ·  %1").arg(displayTime));
        auto* item = new QListWidgetItem(label, poseList_);
        item->setToolTip(
            pose.savedAtUtc.isEmpty()
                ? pose.viewId
                : tr("%1\nSaved %2").arg(pose.viewId, pose.savedAtUtc));
        item->setData(PoseViewIdRole, pose.viewId);
        item->setData(PoseTagsRole, poseTags);
        item->setData(PoseScreenshotPathRole, pose.screenshotPath);
    }
    updatePoseListPresentation(tr("No saved views yet"));
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
            ? tr("Edit saved view")
            : tr("Tags for %1")
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
    lastUsedTags_ = meshcompare::normalizeCameraPoseTags(tags);
    hasLastUsedTags_ = true;
    rememberKnownTags(lastUsedTags_);
    services_.saveLastCaptureTags(lastUsedTags_);
    updateLastTagHint();
}

void CameraPanel::updateLastTagHint()
{
    if (!hasLastUsedTags_) {
        captureHintLabel_->setText(
            tr("Tags carry over after a successful save"));
        return;
    }
    captureHintLabel_->setText(
        lastUsedTags_.isEmpty()
            ? tr("Your last successful save used no tags")
            : tr("Using tags from your last successful save"));
}

void CameraPanel::refreshBrowserPoses(const CameraPanelSnapshot& snapshot)
{
    browserPoses_ = snapshot.poses;
    for (CameraPoseSummary& pose : browserPoses_) {
        const auto pending = pendingTagUpdates_.constFind(pose.viewId);
        if (pending != pendingTagUpdates_.constEnd())
            pose.tags = pending.value();
    }
    if (screenshotBrowser_ != nullptr)
        screenshotBrowser_->setPoses(browserPoses_);
}

void CameraPanel::clearBrowserPoses()
{
    browserPoses_.clear();
    if (screenshotBrowser_ != nullptr)
        screenshotBrowser_->setPoses(browserPoses_);
}

void CameraPanel::updatePoseListPresentation(const QString& emptyMessage)
{
    const int poseCount = poseList_->count();
    poseCountLabel_->setText(tr("Saved views · %1").arg(poseCount));
    emptyPoseLabel_->setText(emptyMessage);
    emptyPoseLabel_->setVisible(poseCount == 0);
    poseList_->setVisible(poseCount > 0);
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

void CameraPanel::openScreenshotBrowser()
{
    if (screenshotBrowser_ == nullptr) {
        screenshotBrowser_ = new ScreenshotBrowser(this);
    }
    screenshotBrowser_->setPoses(browserPoses_);
    screenshotBrowser_->show();
}

void CameraPanel::openSelectedScreenshotFolder()
{
    const QListWidgetItem* item = poseList_->currentItem();
    const QString screenshotPath = item == nullptr
        ? QString()
        : item->data(PoseScreenshotPathRole).toString();
    if (screenshotPath.isEmpty()) {
        reportFailure(OperationResult::failure(
            tr("The selected camera pose has no local screenshot.")));
        return;
    }
    const QString folderPath = QFileInfo(screenshotPath).absolutePath();
    if (folderPath.isEmpty() || !services_.openLocalFolder(folderPath)) {
        reportFailure(OperationResult::failure(
            tr("The screenshot folder could not be opened.")));
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
    const QListWidgetItem* selectedItem = poseList_->currentItem();
    const bool hasScreenshot =
        selectedItem != nullptr
        && !selectedItem->data(PoseScreenshotPathRole).toString().isEmpty();
    const bool selectionVisibilityChanged =
        selectionSection_->isHidden() == hasSelection;
    selectionSection_->setVisible(hasSelection);
    saveButton_->setEnabled(ready && hasUidInput);
    saveAndCopyScreenshotButton_->setEnabled(ready && hasUidInput);
    applyButton_->setEnabled(ready && inputMatchesSnapshot && hasSelection);
    newPoseTagEditor_->setEnabled(ready && hasUidInput);
    selectedPoseTagLabel_->setEnabled(
        ready && inputMatchesSnapshot && hasSelection);
    selectedPoseTagEditor_->setEnabled(
        ready && inputMatchesSnapshot && hasSelection);
    updateTagsButton_->setEnabled(ready && inputMatchesSnapshot && hasSelection);
    browseScreenshotsButton_->setEnabled(
        snapshotAvailable_ && !workspaceUuid_.isEmpty());
    openScreenshotFolderButton_->setEnabled(
        ready && inputMatchesSnapshot && hasSelection && hasScreenshot);
    deleteButton_->setEnabled(ready && inputMatchesSnapshot && hasSelection);
    if (selectionVisibilityChanged) {
        selectionSection_->updateGeometry();
        if (layout() != nullptr)
            layout()->activate();
        adjustSize();
    }
}

void CameraPanel::reportFailure(const OperationResult& result)
{
    if (!result.ok && !result.error.isEmpty())
        emit statusMessage(result.error);
}
