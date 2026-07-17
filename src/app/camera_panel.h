#pragma once

#include <QFrame>
#include <QString>

class ICameraCommands;
class QLabel;
class QListWidget;
class QPushButton;
class QWidget;
class WorkspaceState;
struct OperationResult;

class CameraPanel final : public QFrame
{
    Q_OBJECT

public:
    explicit CameraPanel(
        WorkspaceState& state,
        ICameraCommands& commands,
        QWidget* parent = nullptr);

    void refreshFromState();

signals:
    void statusMessage(const QString& message);

private:
    QString selectedViewId() const;
    void applySelectedPose();
    void deleteSelectedPose();
    void updateActionState();
    void reportFailure(const OperationResult& result);

    WorkspaceState& state_;
    ICameraCommands& commands_;
    QLabel* uuidLabel_ = nullptr;
    QListWidget* poseList_ = nullptr;
    QPushButton* saveButton_ = nullptr;
    QPushButton* applyButton_ = nullptr;
    QPushButton* deleteButton_ = nullptr;
    QString workspaceUuid_;
    bool snapshotAvailable_ = false;
};
