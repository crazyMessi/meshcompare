#pragma once

#include <functional>

#include <QFrame>
#include <QImage>
#include <QString>

class ICameraCommands;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QWidget;
class WorkspaceState;
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
    void refreshTagInput();
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
    QPushButton* saveButton_ = nullptr;
    QPushButton* saveAndCopyScreenshotButton_ = nullptr;
    QPushButton* applyButton_ = nullptr;
    QPushButton* updateTagsButton_ = nullptr;
    QPushButton* deleteButton_ = nullptr;
    QString workspaceUuid_;
    bool snapshotAvailable_ = false;
};
