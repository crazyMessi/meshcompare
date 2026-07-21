#pragma once

#include <functional>

#include <QFrame>
#include <QImage>
#include <QString>

class ICameraCommands;
class QCompleter;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QStringListModel;
class QWidget;
class WorkspaceState;
struct CameraPanelSnapshot;
struct OperationResult;

struct CameraPanelServices
{
    std::function<bool(const QImage&)> copyImageToClipboard;
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
    QStringList enteredTags() const;
    void refreshTagInput();
    void updateTagSuggestions(const CameraPanelSnapshot& snapshot);
    void rememberTags(const QStringList& tags);
    void rememberTagsForView(const QString& viewId);
    OperationResult commitUidInput(bool refreshAfterCommit);
    void applySelectedPose();
    void updateSelectedPoseTags();
    void deleteSelectedPose();
    void updateActionState();
    void reportFailure(const OperationResult& result);

    WorkspaceState& state_;
    ICameraCommands& commands_;
    CameraPanelServices services_;
    QLabel* uuidLabel_ = nullptr;
    QLineEdit* uidInput_ = nullptr;
    QListWidget* poseList_ = nullptr;
    QLineEdit* tagInput_ = nullptr;
    QStringListModel* tagSuggestionModel_ = nullptr;
    QCompleter* tagCompleter_ = nullptr;
    QPushButton* saveButton_ = nullptr;
    QPushButton* saveAndCopyScreenshotButton_ = nullptr;
    QPushButton* applyButton_ = nullptr;
    QPushButton* updateTagsButton_ = nullptr;
    QPushButton* deleteButton_ = nullptr;
    QString workspaceUuid_;
    QString tagHistoryUuid_;
    QStringList lastUsedTags_;
    bool hasLastUsedTags_ = false;
    bool snapshotAvailable_ = false;
};
