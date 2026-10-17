#include "camera_panel.h"

#include "camera_commands.h"
#include "camera_sync_commands.h"
#include "screenshot_browser.h"
#include "tag_editor.h"
#include "../core/camera_pose_tags.h"
#include "../core/workspace_state.h"

#include <utility>

#include <QClipboard>
#include <QDateTime>
#include <QDesktopServices>
#include <QDialog>
#include <QFormLayout>
#include <QFileInfo>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QLayout>
#include <QLineEdit>
#include <QListWidget>
#include <QPalette>
#include <QPlainTextEdit>
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
constexpr int PoseNoteRole = Qt::UserRole + 3;
const QString DefaultCloudBoardKey =
    QStringLiteral("meshcompare-camera-views");

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
    CameraPanelServices services,
    ICameraSyncCommands* syncCommands)
    : QFrame(parent),
      state_(state),
      commands_(commands),
      services_(std::move(services)),
      syncCommands_(syncCommands)
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
    cameraPoseLibraryPath_ = commands_.cameraPoseLibraryPath();

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
    root->setSpacing(8);

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
    captureLayout->setSpacing(6);

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

    auto* newPoseNoteLabel = new QLabel(tr("Note"), captureSection);
    newPoseNoteLabel->setObjectName(QStringLiteral("cameraFieldLabel"));
    captureLayout->addWidget(newPoseNoteLabel);
    newPoseNoteEditor_ = new QPlainTextEdit(captureSection);
    newPoseNoteEditor_->setObjectName(QStringLiteral("newPoseNoteEditor"));
    newPoseNoteEditor_->setAccessibleName(tr("Note for new view"));
    newPoseNoteEditor_->setPlaceholderText(tr("Add a note for this view (optional)"));
    newPoseNoteEditor_->setTabChangesFocus(true);
    newPoseNoteEditor_->setFixedHeight(newPoseNoteEditor_->fontMetrics().lineSpacing() * 2 + 12);
    newPoseNoteLabel->setBuddy(newPoseNoteEditor_);
    captureLayout->addWidget(newPoseNoteEditor_);

    captureHintLabel_ = new QLabel(
        tr("Tags carry over after a successful save"), captureSection);
    captureHintLabel_->setObjectName(QStringLiteral("cameraHintLabel"));
    captureLayout->addWidget(captureHintLabel_);

    auto* saveActions = new QHBoxLayout;
    saveActions->setSpacing(7);
    saveButton_ = new QPushButton(tr("Save View"), captureSection);
    saveButton_->setObjectName(QStringLiteral("saveCameraPoseButton"));
    saveButton_->setToolTip(
        tr("Save the current view, tags, note, and one local screenshot"));
    saveActions->addWidget(saveButton_, 1);

    saveAndCopyScreenshotButton_ =
        new QPushButton(tr("Save View && Copy"), captureSection);
    saveAndCopyScreenshotButton_->setObjectName(
        QStringLiteral("saveCameraPoseAndCopyScreenshotButton"));
    saveAndCopyScreenshotButton_->setToolTip(
        tr("Save the view, tags, and note with one screenshot, and also copy it"));
    saveActions->addWidget(saveAndCopyScreenshotButton_, 1);
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
        tr("Browse all locally saved screenshots by tag"));
    libraryHeader->addWidget(browseScreenshotsButton_);
    openJsonFolderButton_ =
        new QPushButton(tr("Open JSON Folder"), librarySection);
    openJsonFolderButton_->setObjectName(
        QStringLiteral("openCameraJsonFolderButton"));
    openJsonFolderButton_->setToolTip(
        tr("Open the folder containing the camera pose JSON library"));
    libraryHeader->addWidget(openJsonFolderButton_);
    libraryLayout->addLayout(libraryHeader);

    auto* cloudSyncRow = new QHBoxLayout;
    cloudSyncRow->setSpacing(7);
    cloudSyncSummaryLabel_ = new QLabel(tr("Cloud · Not configured"), librarySection);
    cloudSyncSummaryLabel_->setObjectName(QStringLiteral("cameraCloudSyncSummary"));
    cloudSyncSummaryLabel_->setTextFormat(Qt::PlainText);
    cloudSyncRow->addWidget(cloudSyncSummaryLabel_, 1);
    cloudSyncButton_ = new QPushButton(tr("Sync to Cloud…"), librarySection);
    cloudSyncButton_->setObjectName(QStringLiteral("cameraCloudSyncButton"));
    cloudSyncButton_->setToolTip(tr("Configure and sync saved camera views"));
    cloudSyncRow->addWidget(cloudSyncButton_);
    libraryLayout->addLayout(cloudSyncRow);

    poseList_ = new QListWidget(librarySection);
    poseList_->setObjectName(QStringLiteral("cameraPoseList"));
    poseList_->setSelectionMode(QAbstractItemView::SingleSelection);
    poseList_->setUniformItemSizes(true);
    poseList_->setSpacing(2);
    poseList_->setMinimumHeight(90);
    poseList_->setMaximumHeight(120);
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
    selectionLayout->setSpacing(6);

    selectedViewLabel_ = new QLabel(tr("Selected view"), selectionSection_);
    selectedViewLabel_->setObjectName(QStringLiteral("selectedCameraViewLabel"));
    selectionLayout->addWidget(selectedViewLabel_);
    selectedPoseTagLabel_ = new QLabel(tr("Tags"), selectionSection_);
    selectedPoseTagLabel_->setObjectName(QStringLiteral("selectedPoseTagLabel"));
    selectionLayout->addWidget(selectedPoseTagLabel_);
    selectedPoseTagEditor_ = new TagEditor(selectionSection_);
    selectedPoseTagEditor_->setObjectName(QStringLiteral("selectedPoseTagEditor"));
    selectedPoseTagEditor_->setAccessibleName(tr("Tags for selected pose"));
    selectedPoseTagLabel_->setBuddy(selectedPoseTagEditor_);
    selectionLayout->addWidget(selectedPoseTagEditor_);

    auto* noteLabel = new QLabel(tr("Note"), selectionSection_);
    noteLabel->setObjectName(QStringLiteral("cameraFieldLabel"));
    selectionLayout->addWidget(noteLabel);

    selectedPoseNoteEditor_ = new QPlainTextEdit(selectionSection_);
    selectedPoseNoteEditor_->setObjectName(QStringLiteral("selectedPoseNoteEditor"));
    selectedPoseNoteEditor_->setAccessibleName(tr("Notes for selected view"));
    selectedPoseNoteEditor_->setPlaceholderText(tr("Add a note for this view (optional)"));
    selectedPoseNoteEditor_->setTabChangesFocus(true);
    selectedPoseNoteEditor_->setFixedHeight(
        selectedPoseNoteEditor_->fontMetrics().lineSpacing() * 3 + 12);
    noteLabel->setBuddy(selectedPoseNoteEditor_);
    selectionLayout->addWidget(selectedPoseNoteEditor_);
    selectedViewStatus_ = new QLabel(selectionSection_);
    selectedViewStatus_->setObjectName(QStringLiteral("cameraViewDetailsStatus"));
    selectedViewStatus_->setTextFormat(Qt::PlainText);
    selectedViewStatus_->setWordWrap(true);
    selectionLayout->addWidget(selectedViewStatus_);

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
    saveChangesButton_ = new QPushButton(tr("Save Changes"), selectionSection_);
    saveChangesButton_->setObjectName(QStringLiteral("saveCameraPoseChangesButton"));
    saveChangesButton_->setToolTip(tr("Save tags and note together for the selected view"));
    actions->addWidget(saveChangesButton_);
    applyButton_ = new QPushButton(tr("Apply View"), selectionSection_);
    applyButton_->setObjectName(QStringLiteral("applyCameraPoseButton"));
    applyButton_->setToolTip(tr("Move the camera to the selected saved view"));
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
            refreshSelectedPoseForm();
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
        QString savedViewId;
        const OperationResult result = commands_.saveCurrentCameraPoseWithNote(
            &savedViewId, tags, newPoseNoteEditor_->toPlainText());
        if (!result.ok) {
            reportFailure(result);
            return;
        }
        rememberTags(tags);
        refreshFromState();
        newPoseTagEditor_->load(lastUsedTags_);
        newPoseNoteEditor_->clear();
        selectSavedPose(savedViewId);
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
            QString savedViewId;
            const OperationResult result =
                commands_.saveCurrentCameraPoseWithScreenshotAndNote(
                    screenshot, &savedViewId, tags, newPoseNoteEditor_->toPlainText());
            if (!result.ok) {
                reportFailure(result);
                return;
            }

            rememberTags(tags);
            refreshFromState();
            newPoseTagEditor_->load(lastUsedTags_);
            newPoseNoteEditor_->clear();
            selectSavedPose(savedViewId);
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
        saveChangesButton_,
        &QPushButton::clicked,
        this,
        &CameraPanel::updateSelectedPoseDetails);
    connect(selectedPoseTagEditor_, &TagEditor::tagsChanged,
            this, &CameraPanel::rememberSelectedPoseDraft);
    connect(selectedPoseNoteEditor_, &QPlainTextEdit::textChanged,
            this, &CameraPanel::rememberSelectedPoseDraft);
    connect(
        browseScreenshotsButton_,
        &QPushButton::clicked,
        this,
        &CameraPanel::openScreenshotBrowser);
    connect(
        openJsonFolderButton_,
        &QPushButton::clicked,
        this,
        &CameraPanel::openCameraPoseLibraryFolder);
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
    connect(
        cloudSyncButton_,
        &QPushButton::clicked,
        this,
        &CameraPanel::openCloudSyncDialog);
    if (syncCommands_ != nullptr) {
        connect(
            syncCommands_,
            &ICameraSyncCommands::cameraSyncStateChanged,
            this,
            &CameraPanel::refreshCloudSyncUi);
        connect(
            syncCommands_,
            &ICameraSyncCommands::cameraSyncCompleted,
            this,
            [this] {
                refreshFromState();
                refreshCloudSyncUi();
                const CameraSyncSnapshot snapshot =
                    syncCommands_->cameraSyncSnapshot();
                if (!snapshot.result.ok) {
                    reportFailure(snapshot.result);
                } else if (snapshot.lastSyncSucceeded) {
                    emit confirmationMessage(tr("Cloud sync completed."));
                } else if (!snapshot.statusMessage.isEmpty()) {
                    reportFailure(OperationResult::failure(
                        snapshot.statusMessage));
                }
            });
    }

    refreshCloudSyncUi();
    refreshFromState();
}

void CameraPanel::refreshFromState()
{
    rememberSelectedPoseDraft();
    const QString previousViewId = selectedViewId();
    const QSignalBlocker listBlocker(poseList_);
    const CameraPanelSnapshot snapshot = commands_.cameraPanelSnapshot();
    workspaceUuid_ = snapshot.workspaceUuid;
    snapshotAvailable_ = snapshot.result.ok;
    const bool workspaceChanged = tagHistoryUuid_ != workspaceUuid_;
    if (workspaceChanged) {
        tagHistoryUuid_ = workspaceUuid_;
        knownTags_.clear();
        pendingDetailUpdates_.clear();
        detailDrafts_.clear();
        editingViewId_.clear();
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
    {
        const QSignalBlocker inputBlocker(uidInput_);
        uidInput_->setText(
            workspaceUuid_.isEmpty()
                ? snapshot.suggestedWorkspaceUid
                : workspaceUuid_);
    }

    if (!snapshotAvailable_) {
        refreshBrowserPoses();
        uuidLabel_->setText(tr("Camera poses are unavailable."));
        uidInput_->setEnabled(false);
        poseList_->setEnabled(false);
        updatePoseListPresentation(tr("Saved views are unavailable"));
        refreshSelectedPoseForm();
        updateActionState();
        reportFailure(snapshot.result);
        return;
    }

    uidInput_->setEnabled(state_.phase() == WorkspacePhase::Ready);
    if (workspaceUuid_.isEmpty()) {
        refreshBrowserPoses();
        uuidLabel_->setText(
            snapshot.suggestedWorkspaceUid.isEmpty()
                ? tr("Enter a workspace UID.")
                : tr("No UID was detected. Confirm or edit it."));
        poseList_->setEnabled(false);
        updatePoseListPresentation(
            tr("Set a workspace UID to start saving views"));
        refreshSelectedPoseForm();
        updateActionState();
        return;
    }

    uuidLabel_->setText(tr("Workspace"));
    uuidLabel_->setToolTip(workspaceUuid_);
    poseList_->setEnabled(true);
    for (const CameraPoseSummary& pose : snapshot.poses) {
        const auto pending = pendingDetailUpdates_.find(pose.viewId);
        if (pending != pendingDetailUpdates_.end() &&
            pending.value() == ViewDetails{pose.tags, pose.note})
            pendingDetailUpdates_.erase(pending);
    }
    updateTagSuggestions(snapshot);
    refreshBrowserPoses();
    for (const CameraPoseSummary& pose : snapshot.poses) {
        const ViewDetails details = pendingDetailUpdates_.value(
            pose.viewId, ViewDetails{pose.tags, pose.note});
        const QStringList& poseTags = details.tags;
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
        item->setData(PoseNoteRole, details.note);
        if (!workspaceChanged && pose.viewId == previousViewId)
            poseList_->setCurrentItem(item);
    }
    updatePoseListPresentation(tr("No saved views yet"));
    refreshSelectedPoseForm();
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

void CameraPanel::rememberSelectedPoseDraft()
{
    if (editingViewId_.isEmpty())
        return;
    const ViewDetails draft{selectedPoseTagEditor_->tags(), selectedPoseNoteEditor_->toPlainText()};
    if (draft == displayedDetails_)
        detailDrafts_.remove(editingViewId_);
    else
        detailDrafts_.insert(editingViewId_, draft);
    selectedViewStatus_->setText(draft == displayedDetails_ ? tr("Saved") : tr("Unsaved changes"));
}

void CameraPanel::refreshSelectedPoseForm()
{
    rememberSelectedPoseDraft();
    const QListWidgetItem* item = poseList_->currentItem();
    editingViewId_ = item == nullptr ? QString() : item->data(PoseViewIdRole).toString();
    displayedDetails_ = item == nullptr
        ? ViewDetails{} : ViewDetails{item->data(PoseTagsRole).toStringList(), item->data(PoseNoteRole).toString()};
    const ViewDetails details = detailDrafts_.value(editingViewId_, displayedDetails_);
    selectedViewLabel_->setText(editingViewId_.isEmpty()
        ? tr("Selected view") : tr("View %1").arg(editingViewId_));
    const QSignalBlocker tagsBlocker(selectedPoseTagEditor_);
    const QSignalBlocker noteBlocker(selectedPoseNoteEditor_);
    if (selectedPoseTagEditor_->tags() != details.tags)
        selectedPoseTagEditor_->load(details.tags);
    if (selectedPoseNoteEditor_->toPlainText() != details.note)
        selectedPoseNoteEditor_->setPlainText(details.note);
    selectedViewStatus_->setText(details == displayedDetails_ ? tr("Saved") : tr("Unsaved changes"));
}

void CameraPanel::selectSavedPose(const QString& viewId)
{
    for (int index = 0; index < poseList_->count(); ++index) {
        QListWidgetItem* item = poseList_->item(index);
        if (item->data(PoseViewIdRole).toString() == viewId) {
            poseList_->setCurrentItem(item);
            poseList_->scrollToItem(item);
            selectedPoseNoteEditor_->setFocus(Qt::OtherFocusReason);
            return;
        }
    }
}

void CameraPanel::updateTagSuggestions(const CameraPanelSnapshot& snapshot)
{
    knownTags_.clear();
    appendKnownTags(lastUsedTags_);
    for (const CameraPoseSummary& pose : snapshot.poses)
        appendKnownTags(pose.tags);
    for (const ViewDetails& details : pendingDetailUpdates_)
        appendKnownTags(details.tags);
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

void CameraPanel::refreshBrowserPoses()
{
    if (screenshotBrowser_ == nullptr)
        return;
    const CameraScreenshotLibrarySnapshot library =
        commands_.cameraScreenshotLibrarySnapshot();
    if (!library.result.ok) {
        reportFailure(library.result);
        return;
    }
    browserPoses_ = library.poses;
    publishBrowserPoses();
}

void CameraPanel::publishBrowserPoses()
{
    for (CameraPoseSummary& pose : browserPoses_) {
        if (pose.workspaceUuid != workspaceUuid_)
            continue;
        const auto pending = pendingDetailUpdates_.constFind(pose.viewId);
        if (pending != pendingDetailUpdates_.constEnd()) {
            pose.tags = pending->tags;
            pose.note = pending->note;
        }
    }
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
    const OperationResult result = commands_.applyCameraPose(viewId);
    if (!result.ok) {
        reportFailure(result);
        return;
    }
    emit confirmationMessage(tr("View %1 applied.").arg(viewId));
}

void CameraPanel::updateSelectedPoseDetails()
{
    const QString viewId = selectedViewId();
    const QString uid = workspaceUuid_;
    if (viewId.isEmpty() || !saveChangesButton_->isEnabled())
        return;
    const ViewDetails details{selectedPoseTagEditor_->tags(), selectedPoseNoteEditor_->toPlainText()};
    const OperationResult result = commands_.setCameraPoseDetailsForWorkspace(
        uid, viewId, details.tags, details.note);
    if (!result.ok) {
        selectedViewStatus_->setText(tr("Could not save changes: %1").arg(result.error));
        reportFailure(result);
        return;
    }
    if (workspaceUuid_ != uid)
        return;
    pendingDetailUpdates_.insert(viewId, details);
    detailDrafts_.remove(viewId);
    if (editingViewId_ == viewId)
        displayedDetails_ = details;
    refreshFromState();
    rememberKnownTags(details.tags);
    emit confirmationMessage(tr("Changes saved for %1.").arg(viewId));
}

void CameraPanel::openScreenshotBrowser()
{
    const CameraScreenshotLibrarySnapshot library =
        commands_.cameraScreenshotLibrarySnapshot();
    if (!library.result.ok) {
        reportFailure(library.result);
        return;
    }
    browserPoses_ = library.poses;
    if (screenshotBrowser_ == nullptr) {
        screenshotBrowser_ = new ScreenshotBrowser(this);
        screenshotBrowser_->setDetailsSaveHandler(
            [this](const QString& uid, const QString& viewId, const QStringList& tags, const QString& note) {
                return commands_.setCameraPoseDetailsForWorkspace(uid, viewId, tags, note);
            });
        connect(screenshotBrowser_, &ScreenshotBrowser::detailsSaved, this,
            [this](const QString& uid, const QString& viewId, const QStringList& tags, const QString& note) {
                rememberSelectedPoseDraft();
                for (CameraPoseSummary& pose : browserPoses_) {
                    if (pose.workspaceUuid == uid && pose.viewId == viewId) {
                        pose.tags = tags;
                        pose.note = note;
                    }
                }
                if (uid == workspaceUuid_) {
                    pendingDetailUpdates_.insert(viewId, ViewDetails{tags, note});
                    refreshFromState();
                } else {
                    refreshBrowserPoses();
                }
            });
    }
    publishBrowserPoses();
    screenshotBrowser_->show();
}

void CameraPanel::openCameraPoseLibraryFolder()
{
    const QString folderPath =
        QFileInfo(cameraPoseLibraryPath_).absolutePath();
    if (cameraPoseLibraryPath_.isEmpty() ||
        folderPath.isEmpty() ||
        !services_.openLocalFolder(folderPath)) {
        reportFailure(OperationResult::failure(
            tr("The camera pose JSON folder could not be opened.")));
    }
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

void CameraPanel::openCloudSyncDialog()
{
    if (syncCommands_ == nullptr)
        return;

    if (cloudSyncDialog_ == nullptr) {
        cloudSyncDialog_ = new QDialog(this);
        cloudSyncDialog_->setObjectName(QStringLiteral("cameraCloudSyncDialog"));
        cloudSyncDialog_->setWindowTitle(tr("Cloud Sync"));
        cloudSyncDialog_->setWindowModality(Qt::NonModal);
        cloudSyncDialog_->setMinimumWidth(430);

        auto* root = new QVBoxLayout(cloudSyncDialog_);
        root->setContentsMargins(16, 16, 16, 16);
        root->setSpacing(10);

        auto* title = new QLabel(tr("Cloud Sync"), cloudSyncDialog_);
        title->setObjectName(QStringLiteral("cameraCloudSyncTitle"));
        root->addWidget(title);

        auto* explanation = new QLabel(
            tr("Sync saved views, screenshots, tags, and notes to Cloud Board."),
            cloudSyncDialog_);
        explanation->setObjectName(QStringLiteral("cameraCloudSyncExplanation"));
        explanation->setWordWrap(true);
        root->addWidget(explanation);

        cloudKeyStatusLabel_ = new QLabel(cloudSyncDialog_);
        cloudKeyStatusLabel_->setObjectName(
            QStringLiteral("cameraCloudKeyStatus"));
        cloudKeyStatusLabel_->setTextFormat(Qt::PlainText);
        root->addWidget(cloudKeyStatusLabel_);

        auto* form = new QFormLayout;
        form->setHorizontalSpacing(10);
        form->setVerticalSpacing(8);
        cloudApiKeyInput_ = new QLineEdit(cloudSyncDialog_);
        cloudApiKeyInput_->setObjectName(
            QStringLiteral("cameraCloudApiKeyInput"));
        cloudApiKeyInput_->setAccessibleName(tr("Cloud Board API key"));
        cloudApiKeyInput_->setEchoMode(QLineEdit::Password);
        cloudApiKeyInput_->setClearButtonEnabled(true);
        cloudApiKeyInput_->setPlaceholderText(tr("Paste API key"));
        form->addRow(tr("API key"), cloudApiKeyInput_);

        cloudBoardKeyInput_ = new QLineEdit(cloudSyncDialog_);
        cloudBoardKeyInput_->setObjectName(
            QStringLiteral("cameraCloudBoardKeyInput"));
        cloudBoardKeyInput_->setAccessibleName(tr("Cloud Board key"));
        cloudBoardKeyInput_->setText(DefaultCloudBoardKey);
        form->addRow(tr("Board key"), cloudBoardKeyInput_);
        root->addLayout(form);

        auto* keyActions = new QHBoxLayout;
        keyActions->setSpacing(7);
        saveCloudKeyButton_ = new QPushButton(tr("Save Key"), cloudSyncDialog_);
        saveCloudKeyButton_->setObjectName(
            QStringLiteral("cameraCloudSaveKeyButton"));
        keyActions->addWidget(saveCloudKeyButton_);
        clearCloudKeyButton_ = new QPushButton(tr("Clear Key"), cloudSyncDialog_);
        clearCloudKeyButton_->setObjectName(
            QStringLiteral("cameraCloudClearKeyButton"));
        keyActions->addWidget(clearCloudKeyButton_);
        keyActions->addStretch(1);
        root->addLayout(keyActions);

        cloudLastSyncLabel_ = new QLabel(cloudSyncDialog_);
        cloudLastSyncLabel_->setObjectName(
            QStringLiteral("cameraCloudLastSync"));
        cloudLastSyncLabel_->setTextFormat(Qt::PlainText);
        root->addWidget(cloudLastSyncLabel_);

        cloudSyncStatusLabel_ = new QLabel(cloudSyncDialog_);
        cloudSyncStatusLabel_->setObjectName(
            QStringLiteral("cameraCloudSyncStatus"));
        cloudSyncStatusLabel_->setTextFormat(Qt::PlainText);
        cloudSyncStatusLabel_->setWordWrap(true);
        root->addWidget(cloudSyncStatusLabel_);

        auto* syncActions = new QHBoxLayout;
        syncActions->setSpacing(7);
        syncNowButton_ = new QPushButton(tr("Sync Now"), cloudSyncDialog_);
        syncNowButton_->setObjectName(
            QStringLiteral("cameraCloudSyncNowButton"));
        syncActions->addWidget(syncNowButton_);
        syncActions->addStretch(1);
        auto* closeButton = new QPushButton(tr("Close"), cloudSyncDialog_);
        closeButton->setObjectName(
            QStringLiteral("cameraCloudSyncCloseButton"));
        syncActions->addWidget(closeButton);
        root->addLayout(syncActions);

        connect(
            cloudApiKeyInput_,
            &QLineEdit::textChanged,
            this,
            [this] {
                if (saveCloudKeyButton_ != nullptr &&
                    syncCommands_ != nullptr) {
                    const CameraSyncSnapshot snapshot =
                        syncCommands_->cameraSyncSnapshot();
                    saveCloudKeyButton_->setEnabled(
                        !snapshot.syncing &&
                        !cloudApiKeyInput_->text().trimmed().isEmpty());
                }
            });
        connect(
            cloudBoardKeyInput_,
            &QLineEdit::textChanged,
            this,
            [this] {
                if (syncNowButton_ != nullptr && syncCommands_ != nullptr) {
                    const CameraSyncSnapshot snapshot =
                        syncCommands_->cameraSyncSnapshot();
                    syncNowButton_->setEnabled(
                        snapshot.configured &&
                        !snapshot.syncing &&
                        !cloudBoardKeyInput_->text().trimmed().isEmpty());
                }
            });
        connect(
            saveCloudKeyButton_,
            &QPushButton::clicked,
            this,
            &CameraPanel::saveCloudApiKey);
        connect(
            clearCloudKeyButton_,
            &QPushButton::clicked,
            this,
            &CameraPanel::clearCloudApiKey);
        connect(
            syncNowButton_,
            &QPushButton::clicked,
            this,
            &CameraPanel::startCloudSync);
        connect(
            closeButton,
            &QPushButton::clicked,
            cloudSyncDialog_,
            &QDialog::close);
    }

    refreshCloudSyncUi();
    cloudSyncDialog_->show();
    cloudSyncDialog_->raise();
    cloudSyncDialog_->activateWindow();
}

void CameraPanel::refreshCloudSyncUi()
{
    if (syncCommands_ == nullptr) {
        cloudSyncSummaryLabel_->setText(tr("Cloud · Unavailable"));
        cloudSyncButton_->setEnabled(false);
        return;
    }

    const CameraSyncSnapshot snapshot = syncCommands_->cameraSyncSnapshot();
    cloudSyncButton_->setEnabled(true);
    if (!snapshot.result.ok) {
        cloudSyncSummaryLabel_->setText(tr("Cloud · Needs attention"));
    } else if (snapshot.syncing) {
        cloudSyncSummaryLabel_->setText(tr("Cloud · Syncing…"));
    } else if (!snapshot.configured) {
        cloudSyncSummaryLabel_->setText(tr("Cloud · Not configured"));
    } else if (!snapshot.lastSyncAtUtc.isEmpty() &&
               snapshot.lastSyncSucceeded) {
        cloudSyncSummaryLabel_->setText(
            tr("Cloud · Synced %1").arg(
                poseDisplayTime(snapshot.lastSyncAtUtc)));
    } else if (!snapshot.lastSyncAtUtc.isEmpty()) {
        cloudSyncSummaryLabel_->setText(tr("Cloud · Last sync failed"));
    } else {
        cloudSyncSummaryLabel_->setText(tr("Cloud · Ready"));
    }

    if (cloudSyncDialog_ == nullptr)
        return;

    cloudKeyStatusLabel_->setText(
        snapshot.configured
            ? tr("API key: Saved")
            : tr("API key: Not configured"));
    if (!cloudBoardKeyInput_->isModified()) {
        cloudBoardKeyInput_->setText(
            snapshot.boardKey.trimmed().isEmpty()
                ? DefaultCloudBoardKey
                : snapshot.boardKey.trimmed());
        cloudBoardKeyInput_->setModified(false);
    }
    cloudLastSyncLabel_->setText(
        snapshot.lastSyncAtUtc.isEmpty()
            ? tr("Last sync: Never")
            : tr("Last sync: %1 · %2")
                  .arg(
                      poseDisplayTime(snapshot.lastSyncAtUtc),
                      snapshot.lastSyncSucceeded
                          ? tr("Succeeded")
                          : tr("Failed")));

    if (!snapshot.result.ok) {
        cloudSyncStatusLabel_->setText(snapshot.result.error);
    } else if (snapshot.syncing) {
        cloudSyncStatusLabel_->setText(
            snapshot.statusMessage.isEmpty()
                ? tr("Syncing camera views…")
                : snapshot.statusMessage);
    } else if (!snapshot.statusMessage.isEmpty()) {
        cloudSyncStatusLabel_->setText(snapshot.statusMessage);
    } else if (!snapshot.configured) {
        cloudSyncStatusLabel_->setText(
            tr("Save an API key before syncing."));
    } else {
        cloudSyncStatusLabel_->clear();
    }

    const bool controlsEnabled = !snapshot.syncing;
    cloudApiKeyInput_->setEnabled(controlsEnabled);
    cloudBoardKeyInput_->setEnabled(controlsEnabled);
    saveCloudKeyButton_->setEnabled(
        controlsEnabled &&
        !cloudApiKeyInput_->text().trimmed().isEmpty());
    clearCloudKeyButton_->setEnabled(
        controlsEnabled &&
        (snapshot.configured || snapshot.credentialStored));
    syncNowButton_->setEnabled(
        controlsEnabled && snapshot.configured &&
        !cloudBoardKeyInput_->text().trimmed().isEmpty());
}

void CameraPanel::saveCloudApiKey()
{
    if (syncCommands_ == nullptr || cloudApiKeyInput_ == nullptr)
        return;
    const QString apiKey = cloudApiKeyInput_->text();
    if (apiKey.trimmed().isEmpty()) {
        cloudSyncStatusLabel_->setText(tr("Enter an API key to save."));
        return;
    }
    const OperationResult result = syncCommands_->configureApiKey(apiKey);
    if (!result.ok) {
        cloudSyncStatusLabel_->setText(
            tr("Could not save the API key: %1").arg(result.error));
        reportFailure(result);
        return;
    }
    cloudApiKeyInput_->clear();
    refreshCloudSyncUi();
    emit confirmationMessage(tr("Cloud API key saved."));
}

void CameraPanel::clearCloudApiKey()
{
    if (syncCommands_ == nullptr)
        return;
    const OperationResult result = syncCommands_->clearApiKey();
    if (!result.ok) {
        cloudSyncStatusLabel_->setText(
            tr("Could not clear the API key: %1").arg(result.error));
        reportFailure(result);
        return;
    }
    if (cloudApiKeyInput_ != nullptr)
        cloudApiKeyInput_->clear();
    refreshCloudSyncUi();
    emit confirmationMessage(tr("Cloud API key cleared."));
}

void CameraPanel::startCloudSync()
{
    if (syncCommands_ == nullptr || cloudBoardKeyInput_ == nullptr)
        return;
    const QString boardKey = cloudBoardKeyInput_->text().trimmed();
    if (boardKey.isEmpty()) {
        cloudSyncStatusLabel_->setText(tr("Enter a board key before syncing."));
        return;
    }
    const OperationResult result = syncCommands_->startCameraSync(boardKey);
    if (!result.ok) {
        cloudSyncStatusLabel_->setText(
            tr("Could not start cloud sync: %1").arg(result.error));
        reportFailure(result);
        return;
    }
    cloudBoardKeyInput_->setModified(false);
    refreshCloudSyncUi();
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
    pendingDetailUpdates_.remove(viewId);
    detailDrafts_.remove(viewId);
    if (editingViewId_ == viewId)
        editingViewId_.clear();
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
    newPoseNoteEditor_->setEnabled(ready && hasUidInput);
    selectedPoseTagLabel_->setEnabled(
        ready && inputMatchesSnapshot && hasSelection);
    selectedPoseTagEditor_->setEnabled(
        ready && inputMatchesSnapshot && hasSelection);
    saveChangesButton_->setEnabled(ready && inputMatchesSnapshot && hasSelection);
    selectedPoseNoteEditor_->setEnabled(
        ready && snapshotAvailable_ && inputMatchesSnapshot && hasSelection);
    browseScreenshotsButton_->setEnabled(snapshotAvailable_);
    openJsonFolderButton_->setEnabled(!cameraPoseLibraryPath_.isEmpty());
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
