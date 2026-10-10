#pragma once

#include <functional>

#include <QFrame>
#include <QHash>
#include <QImage>
#include <QString>
#include <QStringList>
#include <QVector>

#include "camera_commands.h"

class ICameraCommands;
class ICameraSyncCommands;
class QDialog;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QPlainTextEdit;
class ScreenshotBrowser;
class TagEditor;
class QWidget;
class WorkspaceState;
struct OperationResult;

struct CameraPanelServices
{
    std::function<bool(const QImage&)> copyImageToClipboard;
    std::function<bool(const QString&)> openLocalFolder;
    std::function<bool(QStringList&)> loadLastCaptureTags;
    std::function<void(const QStringList&)> saveLastCaptureTags;
};

class CameraPanel final : public QFrame
{
    Q_OBJECT

public:
    explicit CameraPanel(
        WorkspaceState& state,
        ICameraCommands& commands,
        QWidget* parent = nullptr,
        CameraPanelServices services = {},
        ICameraSyncCommands* syncCommands = nullptr);

    void refreshFromState();

signals:
    void statusMessage(const QString& message);
    void confirmationMessage(const QString& message);

private:
    QString selectedViewId() const;
    void refreshSelectedPoseForm();
    void rememberSelectedPoseDraft();
    void selectSavedPose(const QString& viewId);
    void updateTagSuggestions(const CameraPanelSnapshot& snapshot);
    void appendKnownTags(const QStringList& tags);
    void publishKnownTags();
    void rememberKnownTags(const QStringList& tags);
    void rememberTags(const QStringList& tags);
    void updateLastTagHint();
    void refreshBrowserPoses();
    void publishBrowserPoses();
    void updatePoseListPresentation(const QString& emptyMessage);
    OperationResult commitUidInput(bool refreshAfterCommit);
    void applySelectedPose();
    void updateSelectedPoseDetails();
    void openScreenshotBrowser();
    void openCameraPoseLibraryFolder();
    void openSelectedScreenshotFolder();
    void openCloudSyncDialog();
    void refreshCloudSyncUi();
    void saveCloudApiKey();
    void clearCloudApiKey();
    void startCloudSync();
    void deleteSelectedPose();
    void updateActionState();
    void reportFailure(const OperationResult& result);

    WorkspaceState& state_;
    ICameraCommands& commands_;
    CameraPanelServices services_;
    ICameraSyncCommands* syncCommands_ = nullptr;
    QLabel* uuidLabel_ = nullptr;
    QLineEdit* uidInput_ = nullptr;
    QListWidget* poseList_ = nullptr;
    QLabel* poseCountLabel_ = nullptr;
    QLabel* emptyPoseLabel_ = nullptr;
    QLabel* captureHintLabel_ = nullptr;
    TagEditor* newPoseTagEditor_ = nullptr;
    QPlainTextEdit* newPoseNoteEditor_ = nullptr;
    QLabel* selectedViewLabel_ = nullptr;
    QLabel* selectedViewStatus_ = nullptr;
    QLabel* selectedPoseTagLabel_ = nullptr;
    TagEditor* selectedPoseTagEditor_ = nullptr;
    QPlainTextEdit* selectedPoseNoteEditor_ = nullptr;
    QFrame* selectionSection_ = nullptr;
    QPushButton* saveButton_ = nullptr;
    QPushButton* saveAndCopyScreenshotButton_ = nullptr;
    QPushButton* applyButton_ = nullptr;
    QPushButton* saveChangesButton_ = nullptr;
    QPushButton* browseScreenshotsButton_ = nullptr;
    QPushButton* openJsonFolderButton_ = nullptr;
    QPushButton* openScreenshotFolderButton_ = nullptr;
    QPushButton* deleteButton_ = nullptr;
    QLabel* cloudSyncSummaryLabel_ = nullptr;
    QPushButton* cloudSyncButton_ = nullptr;
    QDialog* cloudSyncDialog_ = nullptr;
    QLabel* cloudKeyStatusLabel_ = nullptr;
    QLineEdit* cloudApiKeyInput_ = nullptr;
    QLineEdit* cloudBoardKeyInput_ = nullptr;
    QLabel* cloudLastSyncLabel_ = nullptr;
    QLabel* cloudSyncStatusLabel_ = nullptr;
    QPushButton* saveCloudKeyButton_ = nullptr;
    QPushButton* clearCloudKeyButton_ = nullptr;
    QPushButton* syncNowButton_ = nullptr;
    ScreenshotBrowser* screenshotBrowser_ = nullptr;
    QString workspaceUuid_;
    QString cameraPoseLibraryPath_;
    QString tagHistoryUuid_;
    QStringList knownTags_;
    QStringList lastUsedTags_;
    struct ViewDetails {
        QStringList tags;
        QString note;

        bool operator==(const ViewDetails& other) const
        {
            return tags == other.tags && note == other.note;
        }
    };
    QHash<QString, ViewDetails> detailDrafts_;
    QHash<QString, ViewDetails> pendingDetailUpdates_;
    QString editingViewId_;
    ViewDetails displayedDetails_;
    QVector<CameraPoseSummary> browserPoses_;
    bool hasLastUsedTags_ = false;
    bool snapshotAvailable_ = false;
};
