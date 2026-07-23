#pragma once

#include <functional>

#include <QFrame>
#include <QHash>
#include <QImage>
#include <QString>
#include <QStringList>

class ICameraCommands;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class TagEditor;
class QWidget;
class WorkspaceState;
struct CameraPanelSnapshot;
struct OperationResult;

struct CameraPanelServices
{
    std::function<bool(const QImage&)> copyImageToClipboard;
    std::function<bool(const QString&)> openLocalFolder;
};

class CameraPanel final : public QFrame
{
    Q_OBJECT

public:
    explicit CameraPanel(
        WorkspaceState& state,
        ICameraCommands& commands,
        QWidget* parent = nullptr,
        CameraPanelServices services = {});

    void refreshFromState();

signals:
    void statusMessage(const QString& message);

private:
    QString selectedViewId() const;
    void refreshSelectedTagEditor();
    void updateTagSuggestions(const CameraPanelSnapshot& snapshot);
    void appendKnownTags(const QStringList& tags);
    void publishKnownTags();
    void rememberKnownTags(const QStringList& tags);
    void rememberTags(const QStringList& tags);
    void rememberTagsForView(const QString& viewId);
    OperationResult commitUidInput(bool refreshAfterCommit);
    void applySelectedPose();
    void updateSelectedPoseTags();
    void openSelectedScreenshotFolder();
    void deleteSelectedPose();
    void updateActionState();
    void reportFailure(const OperationResult& result);

    WorkspaceState& state_;
    ICameraCommands& commands_;
    CameraPanelServices services_;
    QLabel* uuidLabel_ = nullptr;
    QLineEdit* uidInput_ = nullptr;
    QListWidget* poseList_ = nullptr;
    TagEditor* newPoseTagEditor_ = nullptr;
    QLabel* selectedPoseTagLabel_ = nullptr;
    TagEditor* selectedPoseTagEditor_ = nullptr;
    QPushButton* saveButton_ = nullptr;
    QPushButton* saveAndCopyScreenshotButton_ = nullptr;
    QPushButton* applyButton_ = nullptr;
    QPushButton* updateTagsButton_ = nullptr;
    QPushButton* openScreenshotFolderButton_ = nullptr;
    QPushButton* deleteButton_ = nullptr;
    QString workspaceUuid_;
    QString tagHistoryUuid_;
    QStringList knownTags_;
    QStringList lastUsedTags_;
    QHash<QString, QStringList> pendingTagUpdates_;
    bool hasLastUsedTags_ = false;
    bool snapshotAvailable_ = false;
};
