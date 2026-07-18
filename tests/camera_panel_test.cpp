#include <QtTest>

#include <functional>

#include <QApplication>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPalette>
#include <QPushButton>
#include <QSignalSpy>
#include <QWidget>

#include "app/camera_commands.h"
#include "app/camera_panel.h"
#include "core/workspace_state.h"

namespace
{
MeshEntry entry(MeshId id)
{
    MeshEntry result;
    result.id = id;
    result.resourceId = id + 100;
    result.sourcePath = QStringLiteral("/tmp/mesh-%1.obj").arg(id);
    result.displayName = QStringLiteral("mesh-%1.obj").arg(id);
    return result;
}

void makeReady(WorkspaceState& state)
{
    const OperationResult result =
        state.commitWorkspace({entry(1), entry(2)}, 1);
    Q_ASSERT(result.ok);
}

CameraPoseSummary summary(const QString& viewId, const QString& savedAtUtc)
{
    CameraPoseSummary result;
    result.viewId = viewId;
    result.savedAtUtc = savedAtUtc;
    return result;
}

class RecordingCameraCommands final : public ICameraCommands
{
public:
    CameraPanelSnapshot cameraPanelSnapshot() const override
    {
        ++snapshotCalls;
        return snapshot;
    }

    OperationResult setCameraPoseUid(const QString& uid) override
    {
        ++setUidCalls;
        setUids.append(uid);
        if (!setUidResult.ok)
            return setUidResult;
        snapshot.workspaceUuid = uid.trimmed().toCaseFolded();
        return setUidResult;
    }

    OperationResult saveCurrentCameraPose(QString* savedViewId) override
    {
        ++saveCalls;
        if (!saveResult.ok)
            return saveResult;
        if (saveAction)
            saveAction();
        if (savedViewId != nullptr)
            *savedViewId = QStringLiteral("view_saved");
        return saveResult;
    }

    OperationResult saveCurrentCameraPoseWithScreenshot(
        QImage& screenshot,
        QString* savedViewId) override
    {
        ++saveWithScreenshotCalls;
        if (!saveWithScreenshotResult.ok)
            return saveWithScreenshotResult;
        if (saveWithScreenshotAction)
            saveWithScreenshotAction();
        screenshot = screenshotToCopy;
        if (savedViewId != nullptr)
            *savedViewId = QStringLiteral("view_screenshot");
        return saveWithScreenshotResult;
    }

    OperationResult applyCameraPose(const QString& viewId) override
    {
        if (applyAction)
            applyAction();
        applyViewIds.append(viewId);
        return applyResult;
    }

    OperationResult deleteCameraPose(const QString& viewId) override
    {
        if (deleteAction)
            deleteAction();
        deleteViewIds.append(viewId);
        return deleteResult;
    }

    CameraPanelSnapshot snapshot;
    OperationResult saveResult = OperationResult::success();
    OperationResult saveWithScreenshotResult = OperationResult::success();
    OperationResult setUidResult = OperationResult::success();
    OperationResult applyResult = OperationResult::success();
    OperationResult deleteResult = OperationResult::success();
    std::function<void()> saveAction;
    std::function<void()> saveWithScreenshotAction;
    std::function<void()> applyAction;
    std::function<void()> deleteAction;
    mutable int snapshotCalls = 0;
    int saveCalls = 0;
    int saveWithScreenshotCalls = 0;
    int setUidCalls = 0;
    QStringList setUids;
    QStringList applyViewIds;
    QStringList deleteViewIds;
    QImage screenshotToCopy;
};

struct Controls {
    QLabel* uuid = nullptr;
    QLineEdit* uidInput = nullptr;
    QListWidget* poses = nullptr;
    QPushButton* save = nullptr;
    QPushButton* saveAndCopyScreenshot = nullptr;
    QPushButton* apply = nullptr;
    QPushButton* remove = nullptr;
};

Controls controls(CameraPanel& panel)
{
    Controls result;
    result.uuid =
        panel.findChild<QLabel*>(QStringLiteral("cameraUuidLabel"));
    result.uidInput =
        panel.findChild<QLineEdit*>(QStringLiteral("cameraUidInput"));
    result.poses =
        panel.findChild<QListWidget*>(QStringLiteral("cameraPoseList"));
    result.save = panel.findChild<QPushButton*>(
        QStringLiteral("saveCameraPoseButton"));
    result.saveAndCopyScreenshot = panel.findChild<QPushButton*>(
        QStringLiteral("saveCameraPoseAndCopyScreenshotButton"));
    result.apply = panel.findChild<QPushButton*>(
        QStringLiteral("applyCameraPoseButton"));
    result.remove = panel.findChild<QPushButton*>(
        QStringLiteral("deleteCameraPoseButton"));
    return result;
}
} // namespace

class CameraPanelTest : public QObject
{
    Q_OBJECT

private slots:
    void exposesOpaqueEnglishControlsAndScopedPoseSummaries()
    {
        WorkspaceState state;
        makeReady(state);
        RecordingCameraCommands commands;
        commands.snapshot.workspaceUuid =
            QStringLiteral("123e4567-e89b-12d3-a456-426614174000");
        commands.snapshot.poses = {
            summary(
                QStringLiteral("view_001"),
                QStringLiteral("2026-07-14T01:00:00.000Z")),
            summary(
                QStringLiteral("view_002"),
                QStringLiteral("2026-07-14T02:00:00.000Z"))};
        QWidget parent;
        parent.resize(900, 640);
        const QSize parentSize = parent.size();

        CameraPanel panel(state, commands, &parent);
        const Controls ui = controls(panel);

        QCOMPARE(panel.objectName(), QStringLiteral("cameraPanel"));
        QVERIFY(!panel.isWindow());
        QCOMPARE(panel.parentWidget(), &parent);
        QCOMPARE(parent.size(), parentSize);
        QVERIFY(panel.autoFillBackground());
        QCOMPARE(panel.backgroundRole(), QPalette::Window);
        QVERIFY(panel.palette().brush(QPalette::Window).isOpaque());
        QVERIFY(ui.uuid != nullptr);
        QCOMPARE(
            ui.uuid->text(),
            QStringLiteral("Workspace UID"));
        QVERIFY(ui.uidInput != nullptr);
        QCOMPARE(
            ui.uidInput->text(),
            QStringLiteral("123e4567-e89b-12d3-a456-426614174000"));
        QVERIFY(ui.poses != nullptr);
        QCOMPARE(ui.poses->count(), 2);
        QVERIFY(ui.poses->item(0)->text().contains(QStringLiteral("view_001")));
        QVERIFY(ui.poses->item(0)->text().contains(
            QStringLiteral("2026-07-14T01:00:00.000Z")));
        QCOMPARE(
            ui.poses->item(0)->data(Qt::UserRole).toString(),
            QStringLiteral("view_001"));
        QVERIFY(ui.save != nullptr);
        QCOMPARE(ui.save->text(), QStringLiteral("Save Current Pose"));
        QVERIFY(ui.saveAndCopyScreenshot != nullptr);
        QCOMPARE(
            ui.saveAndCopyScreenshot->text(),
            QStringLiteral("Save Pose & Copy Screenshot"));
        QVERIFY(ui.apply != nullptr);
        QCOMPARE(ui.apply->text(), QStringLiteral("Apply"));
        QVERIFY(ui.remove != nullptr);
        QCOMPARE(ui.remove->text(), QStringLiteral("Delete"));
        QVERIFY(ui.save->isEnabled());
        QVERIFY(ui.saveAndCopyScreenshot->isEnabled());
        QVERIFY(!ui.apply->isEnabled());
        QVERIFY(!ui.remove->isEnabled());
        QCOMPARE(commands.snapshotCalls, 1);
    }

    void selectionAppliesTheExactViewAndEnablesSelectionActions()
    {
        WorkspaceState state;
        makeReady(state);
        RecordingCameraCommands commands;
        commands.snapshot.workspaceUuid = QStringLiteral(
            "123e4567-e89b-12d3-a456-426614174000");
        commands.snapshot.poses = {
            summary(QStringLiteral("view_001"), QStringLiteral("first")),
            summary(QStringLiteral("view_002"), QStringLiteral("second"))};
        CameraPanel panel(state, commands);
        Controls ui = controls(panel);
        QVERIFY(ui.poses != nullptr);
        QVERIFY(ui.apply != nullptr);
        QVERIFY(ui.remove != nullptr);

        ui.poses->setCurrentRow(1);

        QVERIFY(ui.apply->isEnabled());
        QVERIFY(ui.remove->isEnabled());
        QTest::mouseClick(ui.apply, Qt::LeftButton);
        QCOMPARE(commands.applyViewIds, QStringList({QStringLiteral("view_002")}));
    }

    void saveAndCopyScreenshotUsesOneCombinedCommandAndRefreshes()
    {
        WorkspaceState state;
        makeReady(state);
        RecordingCameraCommands commands;
        commands.snapshot.workspaceUuid = QStringLiteral(
            "123e4567-e89b-12d3-a456-426614174000");
        commands.screenshotToCopy =
            QImage(3, 2, QImage::Format_ARGB32_Premultiplied);
        commands.screenshotToCopy.fill(QColor(24, 96, 180));
        commands.saveWithScreenshotAction = [&commands] {
            commands.snapshot.poses.append(summary(
                QStringLiteral("view_screenshot"),
                QStringLiteral("latest")));
        };
        QImage copiedImage;
        CameraPanelServices services;
        services.copyImageToClipboard = [&copiedImage](const QImage& image) {
            copiedImage = image;
            return true;
        };
        CameraPanel panel(state, commands, nullptr, std::move(services));
        const Controls ui = controls(panel);

        QTest::mouseClick(ui.saveAndCopyScreenshot, Qt::LeftButton);

        QCOMPARE(commands.saveWithScreenshotCalls, 1);
        QCOMPARE(commands.saveCalls, 0);
        QCOMPARE(commands.snapshotCalls, 2);
        QCOMPARE(ui.poses->count(), 1);
        QCOMPARE(copiedImage, commands.screenshotToCopy);
    }

    void missingUidPreloadsProjectNameAndCanSaveImmediately()
    {
        WorkspaceState state;
        makeReady(state);
        RecordingCameraCommands commands;
        commands.snapshot.suggestedWorkspaceUid =
            QStringLiteral("comparison_run-42");
        commands.snapshot.poses = {
            summary(QStringLiteral("wrong-scope"), QStringLiteral("ignored"))};

        CameraPanel panel(state, commands);
        const Controls ui = controls(panel);

        QCOMPARE(
            ui.uuid->text(),
            QStringLiteral("No UID was detected. Confirm or edit it."));
        QCOMPARE(ui.uidInput->text(), QStringLiteral("comparison_run-42"));
        QCOMPARE(ui.poses->count(), 0);
        QVERIFY(ui.save->isEnabled());
        QVERIFY(ui.saveAndCopyScreenshot->isEnabled());
        QVERIFY(!ui.apply->isEnabled());
        QVERIFY(!ui.remove->isEnabled());

        QTest::mouseClick(ui.save, Qt::LeftButton);

        QCOMPARE(commands.setUids, QStringList({QStringLiteral("comparison_run-42")}));
        QCOMPARE(commands.saveCalls, 1);
        QCOMPARE(
            commands.snapshot.workspaceUuid,
            QStringLiteral("comparison_run-42"));
    }

    void emptyUidInputKeepsCameraActionsUnavailable()
    {
        WorkspaceState state;
        makeReady(state);
        RecordingCameraCommands commands;

        CameraPanel panel(state, commands);
        const Controls ui = controls(panel);

        QCOMPARE(
            ui.uuid->text(),
            QStringLiteral("Enter a workspace UID."));
        QVERIFY(ui.uidInput->text().isEmpty());
        QVERIFY(!ui.save->isEnabled());
        QVERIFY(!ui.saveAndCopyScreenshot->isEnabled());
        QVERIFY(!ui.apply->isEnabled());
        QVERIFY(!ui.remove->isEnabled());
    }

    void nonReadyWorkspaceDisablesCameraMutationsButKeepsTheListVisible()
    {
        WorkspaceState state;
        makeReady(state);
        QVERIFY(state.beginAnalysis().ok);
        RecordingCameraCommands commands;
        commands.snapshot.workspaceUuid = QStringLiteral(
            "123e4567-e89b-12d3-a456-426614174000");
        commands.snapshot.poses = {
            summary(QStringLiteral("view_001"), QStringLiteral("saved"))};

        CameraPanel panel(state, commands);
        Controls ui = controls(panel);
        ui.poses->setCurrentRow(0);

        QCOMPARE(ui.poses->count(), 1);
        QVERIFY(!ui.save->isEnabled());
        QVERIFY(!ui.saveAndCopyScreenshot->isEnabled());
        QVERIFY(!ui.apply->isEnabled());
        QVERIFY(!ui.remove->isEnabled());
    }

    void successfulSaveAndDeleteRefreshTheCommittedSnapshot()
    {
        WorkspaceState state;
        makeReady(state);
        RecordingCameraCommands commands;
        commands.snapshot.workspaceUuid = QStringLiteral(
            "123e4567-e89b-12d3-a456-426614174000");
        commands.snapshot.poses = {
            summary(QStringLiteral("view_001"), QStringLiteral("first"))};
        CameraPanel panel(state, commands);
        Controls ui = controls(panel);
        commands.saveAction = [&commands] {
            commands.snapshot.poses.append(
                summary(QStringLiteral("view_saved"), QStringLiteral("latest")));
        };

        QTest::mouseClick(ui.save, Qt::LeftButton);

        QCOMPARE(commands.saveCalls, 1);
        QCOMPARE(commands.snapshotCalls, 2);
        QCOMPARE(ui.poses->count(), 2);
        QCOMPARE(
            ui.poses->item(1)->data(Qt::UserRole).toString(),
            QStringLiteral("view_saved"));

        ui.poses->setCurrentRow(0);
        commands.deleteAction = [&commands] {
            commands.snapshot.poses.removeFirst();
        };

        QTest::mouseClick(ui.remove, Qt::LeftButton);

        QCOMPARE(commands.deleteViewIds, QStringList({QStringLiteral("view_001")}));
        QCOMPARE(commands.snapshotCalls, 3);
        QCOMPARE(ui.poses->count(), 1);
        QCOMPARE(
            ui.poses->item(0)->data(Qt::UserRole).toString(),
            QStringLiteral("view_saved"));
    }

    void commandFailuresEmitOneStatusAndDoNotClaimARefresh()
    {
        WorkspaceState state;
        makeReady(state);
        RecordingCameraCommands commands;
        commands.snapshot.workspaceUuid = QStringLiteral(
            "123e4567-e89b-12d3-a456-426614174000");
        commands.snapshot.poses = {
            summary(QStringLiteral("view_001"), QStringLiteral("saved"))};
        CameraPanel panel(state, commands);
        Controls ui = controls(panel);
        QSignalSpy status(&panel, &CameraPanel::statusMessage);

        commands.saveResult =
            OperationResult::failure(QStringLiteral("Save failed."));
        QTest::mouseClick(ui.save, Qt::LeftButton);
        QCOMPARE(status.count(), 1);
        QCOMPARE(status.takeFirst().at(0).toString(), QStringLiteral("Save failed."));
        QCOMPARE(commands.snapshotCalls, 1);

        commands.saveWithScreenshotResult =
            OperationResult::failure(QStringLiteral("Capture failed."));
        QTest::mouseClick(ui.saveAndCopyScreenshot, Qt::LeftButton);
        QCOMPARE(status.count(), 1);
        QCOMPARE(
            status.takeFirst().at(0).toString(),
            QStringLiteral("Capture failed."));
        QCOMPARE(commands.snapshotCalls, 1);

        ui.poses->setCurrentRow(0);
        commands.applyResult =
            OperationResult::failure(QStringLiteral("Apply failed."));
        QTest::mouseClick(ui.apply, Qt::LeftButton);
        QCOMPARE(status.count(), 1);
        QCOMPARE(status.takeFirst().at(0).toString(), QStringLiteral("Apply failed."));
        QCOMPARE(commands.snapshotCalls, 1);

        commands.deleteResult =
            OperationResult::failure(QStringLiteral("Delete failed."));
        QTest::mouseClick(ui.remove, Qt::LeftButton);
        QCOMPARE(status.count(), 1);
        QCOMPARE(status.takeFirst().at(0).toString(), QStringLiteral("Delete failed."));
        QCOMPARE(commands.snapshotCalls, 1);
        QCOMPARE(ui.poses->count(), 1);
    }

    void clipboardFailureReportsAfterRefreshingTheSavedPose()
    {
        WorkspaceState state;
        makeReady(state);
        RecordingCameraCommands commands;
        commands.snapshot.workspaceUuid = QStringLiteral(
            "123e4567-e89b-12d3-a456-426614174000");
        commands.screenshotToCopy = QImage(2, 2, QImage::Format_RGB32);
        commands.screenshotToCopy.fill(Qt::green);
        commands.saveWithScreenshotAction = [&commands] {
            commands.snapshot.poses.append(summary(
                QStringLiteral("view_screenshot"),
                QStringLiteral("latest")));
        };
        CameraPanelServices services;
        services.copyImageToClipboard = [](const QImage&) { return false; };
        CameraPanel panel(state, commands, nullptr, std::move(services));
        const Controls ui = controls(panel);
        QSignalSpy status(&panel, &CameraPanel::statusMessage);

        QTest::mouseClick(ui.saveAndCopyScreenshot, Qt::LeftButton);

        QCOMPARE(commands.saveWithScreenshotCalls, 1);
        QCOMPARE(commands.snapshotCalls, 2);
        QCOMPARE(ui.poses->count(), 1);
        QCOMPARE(status.count(), 1);
        QCOMPARE(
            status.takeFirst().at(0).toString(),
            QStringLiteral(
                "Camera pose was saved, but the screenshot could not be copied."));
    }

    void snapshotFailureEmitsOneStatusAndLeavesActionsDisabled()
    {
        WorkspaceState state;
        makeReady(state);
        RecordingCameraCommands commands;
        commands.snapshot.result =
            OperationResult::failure(QStringLiteral("Camera list failed."));
        CameraPanel panel(state, commands);
        QSignalSpy status(&panel, &CameraPanel::statusMessage);

        panel.refreshFromState();

        const Controls ui = controls(panel);
        QCOMPARE(status.count(), 1);
        QCOMPARE(
            status.takeFirst().at(0).toString(),
            QStringLiteral("Camera list failed."));
        QCOMPARE(ui.poses->count(), 0);
        QVERIFY(!ui.save->isEnabled());
        QVERIFY(!ui.saveAndCopyScreenshot->isEnabled());
        QVERIFY(!ui.apply->isEnabled());
        QVERIFY(!ui.remove->isEnabled());
    }

    void synchronousRefreshDuringApplyDoesNotInvalidateTheSelectedViewId()
    {
        WorkspaceState state;
        makeReady(state);
        RecordingCameraCommands commands;
        commands.snapshot.workspaceUuid = QStringLiteral(
            "123e4567-e89b-12d3-a456-426614174000");
        commands.snapshot.poses = {
            summary(QStringLiteral("view_001"), QStringLiteral("first")),
            summary(QStringLiteral("view_002"), QStringLiteral("second"))};
        CameraPanel panel(state, commands);
        Controls ui = controls(panel);
        ui.poses->setCurrentRow(1);
        commands.applyAction = [&panel, &commands] {
            commands.snapshot.poses.clear();
            panel.refreshFromState();
        };

        QTest::mouseClick(ui.apply, Qt::LeftButton);

        QCOMPARE(commands.applyViewIds, QStringList({QStringLiteral("view_002")}));
        QCOMPARE(ui.poses->count(), 0);
    }
};

QTEST_MAIN(CameraPanelTest)
#include "camera_panel_test.moc"
