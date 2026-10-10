#include <QtTest>

#include <functional>

#include <QApplication>
#include <QCompleter>
#include <QFrame>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPalette>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QWidget>

#include "app/app_theme.h"
#include "app/camera_commands.h"
#include "app/camera_panel.h"
#include "app/tag_editor.h"
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

CameraPoseSummary summary(
    const QString& viewId,
    const QString& savedAtUtc,
    const QStringList& tags = {},
    const QString& screenshotPath = {},
    const QString& note = {})
{
    CameraPoseSummary result;
    result.viewId = viewId;
    result.savedAtUtc = savedAtUtc;
    result.tags = tags;
    result.screenshotPath = screenshotPath;
    result.note = note;
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

    QString cameraPoseLibraryPath() const override
    {
        return libraryPath;
    }

    CameraScreenshotLibrarySnapshot cameraScreenshotLibrarySnapshot() const override
    {
        ++screenshotLibraryCalls;
        return screenshotLibrary;
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

    OperationResult saveCurrentCameraPose(
        QString* savedViewId,
        const QStringList& tags) override
    {
        ++saveCalls;
        lastSaveTags = tags;
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
        QString* savedViewId,
        const QStringList& tags) override
    {
        ++saveWithScreenshotCalls;
        lastSaveTags = tags;
        if (!saveWithScreenshotResult.ok)
            return saveWithScreenshotResult;
        if (saveWithScreenshotAction)
            saveWithScreenshotAction();
        screenshot = screenshotToCopy;
        if (savedViewId != nullptr)
            *savedViewId = QStringLiteral("view_screenshot");
        return saveWithScreenshotResult;
    }

    OperationResult saveCurrentCameraPoseWithNote(
        QString* savedViewId, const QStringList& tags, const QString& note) override
    {
        ++combinedCaptureCalls;
        lastSaveNote = note;
        return saveCurrentCameraPose(savedViewId, tags);
    }

    OperationResult saveCurrentCameraPoseWithScreenshotAndNote(
        QImage& screenshot, QString* savedViewId,
        const QStringList& tags, const QString& note) override
    {
        ++combinedCaptureCalls;
        lastSaveNote = note;
        return saveCurrentCameraPoseWithScreenshot(screenshot, savedViewId, tags);
    }

    OperationResult applyCameraPose(const QString& viewId) override
    {
        if (applyAction)
            applyAction();
        applyViewIds.append(viewId);
        return applyResult;
    }

    OperationResult setCameraPoseTags(const QString&, const QStringList&) override
    {
        ++legacyDetailsCalls;
        return setDetailsResult;
    }

    OperationResult setCameraPoseNote(const QString&, const QString&) override
    {
        ++legacyDetailsCalls;
        return setDetailsResult;
    }

    OperationResult setCameraPoseNoteForWorkspace(
        const QString&, const QString&, const QString&) override
    {
        ++legacyDetailsCalls;
        return setDetailsResult;
    }

    OperationResult setCameraPoseDetailsForWorkspace(
        const QString& workspaceUuid,
        const QString& viewId,
        const QStringList& tags,
        const QString& note) override
    {
        ++detailsCalls;
        taggedViewIds.append(viewId);
        assignedTags.append(tags);
        notedViewIds.append(viewId);
        assignedNotes.append(note);
        notedWorkspaceUids.append(workspaceUuid);
        workspaceNotedViewIds.append(viewId);
        workspaceAssignedNotes.append(note);
        if (!setDetailsResult.ok)
            return setDetailsResult;
        if (setDetailsAction)
            setDetailsAction();
        return setDetailsResult;
    }

    OperationResult deleteCameraPose(const QString& viewId) override
    {
        if (deleteAction)
            deleteAction();
        deleteViewIds.append(viewId);
        return deleteResult;
    }

    CameraPanelSnapshot snapshot;
    CameraScreenshotLibrarySnapshot screenshotLibrary;
    QString libraryPath;
    OperationResult saveResult = OperationResult::success();
    OperationResult saveWithScreenshotResult = OperationResult::success();
    OperationResult setUidResult = OperationResult::success();
    OperationResult applyResult = OperationResult::success();
    OperationResult setDetailsResult = OperationResult::success();
    OperationResult deleteResult = OperationResult::success();
    std::function<void()> saveAction;
    std::function<void()> saveWithScreenshotAction;
    std::function<void()> applyAction;
    std::function<void()> setDetailsAction;
    std::function<void()> deleteAction;
    mutable int snapshotCalls = 0;
    mutable int screenshotLibraryCalls = 0;
    int saveCalls = 0;
    int saveWithScreenshotCalls = 0;
    int setUidCalls = 0;
    int detailsCalls = 0;
    int legacyDetailsCalls = 0;
    int combinedCaptureCalls = 0;
    QStringList setUids;
    QStringList applyViewIds;
    QStringList taggedViewIds;
    QVector<QStringList> assignedTags;
    QStringList notedViewIds;
    QStringList assignedNotes;
    QStringList notedWorkspaceUids;
    QStringList workspaceNotedViewIds;
    QStringList workspaceAssignedNotes;
    QStringList lastSaveTags;
    QString lastSaveNote;
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
    TagEditor* newPoseTags = nullptr;
    TagEditor* selectedPoseTags = nullptr;
    QPushButton* saveChanges = nullptr;
    QPlainTextEdit* newNote = nullptr;
    QPlainTextEdit* note = nullptr;
    QPushButton* browseScreenshots = nullptr;
    QPushButton* openJsonFolder = nullptr;
    QPushButton* openScreenshotFolder = nullptr;
    QPushButton* remove = nullptr;
    QFrame* selectionSection = nullptr;
    QLabel* emptyState = nullptr;
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
    result.newPoseTags = panel.findChild<TagEditor*>(
        QStringLiteral("newPoseTagEditor"));
    result.selectedPoseTags = panel.findChild<TagEditor*>(
        QStringLiteral("selectedPoseTagEditor"));
    result.saveChanges = panel.findChild<QPushButton*>(
        QStringLiteral("saveCameraPoseChangesButton"));
    result.newNote = panel.findChild<QPlainTextEdit*>(
        QStringLiteral("newPoseNoteEditor"));
    result.note = panel.findChild<QPlainTextEdit*>(
        QStringLiteral("selectedPoseNoteEditor"));
    result.browseScreenshots = panel.findChild<QPushButton*>(
        QStringLiteral("browseCameraScreenshotsButton"));
    result.openJsonFolder = panel.findChild<QPushButton*>(
        QStringLiteral("openCameraJsonFolderButton"));
    result.openScreenshotFolder = panel.findChild<QPushButton*>(
        QStringLiteral("openCameraScreenshotFolderButton"));
    result.remove = panel.findChild<QPushButton*>(
        QStringLiteral("deleteCameraPoseButton"));
    result.selectionSection = panel.findChild<QFrame*>(
        QStringLiteral("cameraSelectionSection"));
    result.emptyState = panel.findChild<QLabel*>(
        QStringLiteral("cameraEmptyStateLabel"));
    return result;
}
} // namespace

class CameraPanelTest : public QObject
{
    Q_OBJECT

private slots:
    void init()
    {
        QSettings settings;
        settings.remove(QStringLiteral("camera/lastCaptureTags"));
    }

    void opensScreenshotBrowserForAllLocalWorkspaces()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString currentScreenshotPath =
            directory.filePath(QStringLiteral("view_current.png"));
        const QString otherScreenshotPath =
            directory.filePath(QStringLiteral("view_other.png"));
        QImage screenshot(20, 12, QImage::Format_ARGB32_Premultiplied);
        screenshot.fill(Qt::green);
        QVERIFY(screenshot.save(currentScreenshotPath, "PNG"));
        QVERIFY(screenshot.save(otherScreenshotPath, "PNG"));

        WorkspaceState state;
        makeReady(state);
        RecordingCameraCommands commands;
        commands.snapshot.workspaceUuid = QStringLiteral(
            "123e4567-e89b-12d3-a456-426614174000");
        commands.snapshot.poses = {summary(
            QStringLiteral("view_current"),
            QStringLiteral("2026-07-21T12:00:00Z"),
            {QStringLiteral("hole")},
            currentScreenshotPath)};
        commands.screenshotLibrary.poses = {
            commands.snapshot.poses.front(),
            summary(
                QStringLiteral("view_other"),
                QStringLiteral("2026-07-21T13:00:00Z"),
                {QStringLiteral("hole")},
                otherScreenshotPath)};
        CameraPanel panel(state, commands);
        const Controls ui = controls(panel);

        QVERIFY(ui.browseScreenshots != nullptr);
        QTest::mouseClick(ui.browseScreenshots, Qt::LeftButton);

        QCOMPARE(commands.screenshotLibraryCalls, 1);
        auto* browser = panel.findChild<QWidget*>(
            QStringLiteral("screenshotBrowser"));
        QVERIFY(browser != nullptr);
        QVERIFY(browser->isVisible());
        QCOMPARE(browser->windowModality(), Qt::NonModal);
        auto* gallery = browser->findChild<QListWidget*>(
            QStringLiteral("screenshotGallery"));
        QVERIFY(gallery != nullptr);
        QCOMPARE(gallery->count(), 2);
    }

    void applyViewWorksWhileTheScreenshotBrowserIsOpen()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString screenshotPath = directory.filePath(QStringLiteral("view.png"));
        QImage screenshot(20, 12, QImage::Format_RGB32);
        screenshot.fill(Qt::green);
        QVERIFY(screenshot.save(screenshotPath, "PNG"));
        WorkspaceState state;
        makeReady(state);
        RecordingCameraCommands commands;
        commands.snapshot.workspaceUuid = QStringLiteral("workspace-a");
        commands.snapshot.poses = {summary(QStringLiteral("view_001"), {}, {}, screenshotPath)};
        commands.snapshot.poses[0].workspaceUuid = commands.snapshot.workspaceUuid;
        CameraPoseSummary other = commands.snapshot.poses[0];
        other.workspaceUuid = QStringLiteral("workspace-b");
        commands.screenshotLibrary.poses = {commands.snapshot.poses[0], other};
        CameraPanel panel(state, commands);
        const Controls ui = controls(panel);
        panel.show();
        ui.poses->setCurrentRow(0);
        ui.note->setPlainText(QStringLiteral("Unsaved note stays a draft"));
        QTest::mouseClick(ui.browseScreenshots, Qt::LeftButton);
        QApplication::processEvents();
        auto* browser = panel.findChild<QWidget*>(QStringLiteral("screenshotBrowser"));
        auto* gallery = panel.findChild<QListWidget*>(QStringLiteral("screenshotGallery"));
        QVERIFY(browser != nullptr);
        QVERIFY(gallery != nullptr);
        QVERIFY(browser->isVisible());
        QVERIFY(QApplication::activeModalWidget() == nullptr);
        gallery->setCurrentRow(1);
        QSignalSpy status(&panel, &CameraPanel::statusMessage);
        QSignalSpy confirmation(&panel, &CameraPanel::confirmationMessage);
        commands.applyAction = [&] { panel.refreshFromState(); };

        QTest::mouseClick(ui.apply, Qt::LeftButton);

        QCOMPARE(commands.applyViewIds, QStringList({QStringLiteral("view_001")}));
        QCOMPARE(status.count(), 0);
        QCOMPARE(confirmation.count(), 1);
        QCOMPARE(confirmation.takeFirst().at(0).toString(), QStringLiteral("View view_001 applied."));
        QVERIFY(commands.notedViewIds.isEmpty());
        QCOMPARE(ui.note->toPlainText(), QStringLiteral("Unsaved note stays a draft"));
        QCOMPARE(gallery->count(), 2);
        QCOMPARE(gallery->currentRow(), 1);
        QVERIFY(browser->isVisible());
    }

    void browserNotesSaveToTheirWorkspaceAndStayInSyncWithThePanel()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString screenshotPath = directory.filePath(QStringLiteral("view.png"));
        QImage screenshot(20, 12, QImage::Format_RGB32);
        screenshot.fill(Qt::green);
        QVERIFY(screenshot.save(screenshotPath, "PNG"));
        WorkspaceState state;
        makeReady(state);
        RecordingCameraCommands commands;
        commands.snapshot.workspaceUuid = QStringLiteral("workspace-a");
        commands.snapshot.poses = {summary(
            QStringLiteral("view_001"), {}, {}, screenshotPath, QStringLiteral("A saved"))};
        commands.snapshot.poses[0].workspaceUuid = commands.snapshot.workspaceUuid;
        CameraPoseSummary other = commands.snapshot.poses[0];
        other.workspaceUuid = QStringLiteral("workspace-b");
        other.note = QStringLiteral("B saved");
        commands.screenshotLibrary.poses = {commands.snapshot.poses[0], other};
        CameraPanel panel(state, commands);
        const Controls ui = controls(panel);
        ui.poses->setCurrentRow(0);
        ui.note->setPlainText(QStringLiteral("A local draft"));
        ui.browseScreenshots->click();
        auto* gallery = panel.findChild<QListWidget*>(QStringLiteral("screenshotGallery"));
        auto* browserNote = panel.findChild<QPlainTextEdit*>(QStringLiteral("screenshotPreviewNote"));
        auto* browserSave = panel.findChild<QPushButton*>(QStringLiteral("screenshotSaveChanges"));
        QVERIFY(gallery != nullptr);
        QVERIFY(browserNote != nullptr);
        QVERIFY(browserSave != nullptr);
        auto* browserTags = panel.findChild<TagEditor*>(QStringLiteral("screenshotPreviewTags"));
        QVERIFY(browserTags != nullptr);
        gallery->setCurrentRow(1);
        QCOMPARE(browserNote->toPlainText(), QStringLiteral("B saved"));
        browserTags->load({QStringLiteral("from-preview")});
        browserNote->setPlainText(QStringLiteral("B note from preview"));
        commands.setDetailsAction = [&] {
            const QString uid = commands.notedWorkspaceUids.last();
            const QStringList tags = commands.assignedTags.last();
            const QString note = commands.assignedNotes.last();
            for (CameraPoseSummary& pose : commands.screenshotLibrary.poses) {
                if (pose.workspaceUuid == uid) {
                    pose.tags = tags;
                    pose.note = note;
                }
            }
            if (commands.snapshot.workspaceUuid == uid) {
                commands.snapshot.poses[0].tags = tags;
                commands.snapshot.poses[0].note = note;
            }
            panel.refreshFromState();
        };

        browserSave->click();

        QCOMPARE(commands.notedWorkspaceUids, QStringList({QStringLiteral("workspace-b")}));
        QCOMPARE(commands.workspaceNotedViewIds, QStringList({QStringLiteral("view_001")}));
        QCOMPARE(commands.workspaceAssignedNotes, QStringList({QStringLiteral("B note from preview")}));
        QCOMPARE(commands.assignedTags.last(), QStringList({QStringLiteral("from-preview")}));
        QCOMPARE(commands.detailsCalls, 1);
        QCOMPARE(commands.legacyDetailsCalls, 0);
        QCOMPARE(commands.snapshot.workspaceUuid, QStringLiteral("workspace-a"));
        QVERIFY(commands.setUids.isEmpty());
        QCOMPARE(ui.note->toPlainText(), QStringLiteral("A local draft"));
        QCOMPARE(gallery->count(), 2);
        QCOMPARE(gallery->currentRow(), 1);
        QCOMPARE(browserNote->toPlainText(), QStringLiteral("B note from preview"));

        gallery->setCurrentRow(0);
        browserNote->setPlainText(QStringLiteral("A note from preview"));
        browserSave->click();
        QCOMPARE(ui.note->toPlainText(), QStringLiteral("A local draft"));
        QCOMPARE(commands.snapshot.poses[0].note, QStringLiteral("A note from preview"));
        auto* detailsStatus = panel.findChild<QLabel*>(QStringLiteral("cameraViewDetailsStatus"));
        QVERIFY(detailsStatus != nullptr);
        QCOMPARE(detailsStatus->text(), QStringLiteral("Unsaved changes"));

        commands.setDetailsAction = [&] {
            const QString savedNote = commands.assignedNotes.last();
            commands.snapshot.poses[0].note = savedNote;
            commands.screenshotLibrary.poses[0].note = savedNote;
            panel.refreshFromState();
        };
        ui.note->setPlainText(QStringLiteral("A note from panel"));
        ui.saveChanges->click();
        QCOMPARE(browserNote->toPlainText(), QStringLiteral("A note from panel"));
        QCOMPARE(gallery->currentRow(), 0);
        QCOMPARE(gallery->count(), 2);
        QCOMPARE(ui.poses->currentRow(), 0);
    }

    void selectedPoseCanOpenItsScreenshotFolder()
    {
        WorkspaceState state;
        makeReady(state);
        RecordingCameraCommands commands;
        commands.snapshot.workspaceUuid = QStringLiteral(
            "123e4567-e89b-12d3-a456-426614174000");
        commands.snapshot.poses = {summary(
            QStringLiteral("view_001"),
            QStringLiteral("2026-07-21T12:00:00Z"),
            {QStringLiteral("inspection")},
            QStringLiteral("/tmp/camera-shots/inspection/view_001.png"))};
        QString openedFolder;
        CameraPanelServices services;
        services.openLocalFolder = [&openedFolder](const QString& path) {
            openedFolder = path;
            return true;
        };
        CameraPanel panel(state, commands, nullptr, std::move(services));
        const Controls ui = controls(panel);

        QVERIFY(ui.openScreenshotFolder != nullptr);
        QVERIFY(!ui.openScreenshotFolder->isEnabled());
        ui.poses->setCurrentRow(0);
        QVERIFY(ui.openScreenshotFolder->isEnabled());

        ui.openScreenshotFolder->click();

        QCOMPARE(openedFolder, QStringLiteral("/tmp/camera-shots/inspection"));
    }

    void cameraLibraryButtonOpensTheJsonParentFolder()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());

        WorkspaceState state;
        makeReady(state);
        RecordingCameraCommands commands;
        commands.libraryPath =
            directory.filePath(QStringLiteral("camera/poses.json"));
        QString openedFolder;
        CameraPanelServices services;
        services.openLocalFolder = [&openedFolder](const QString& path) {
            openedFolder = path;
            return true;
        };
        CameraPanel panel(state, commands, nullptr, std::move(services));
        const Controls ui = controls(panel);

        QVERIFY(ui.openJsonFolder != nullptr);
        QCOMPARE(ui.openJsonFolder->text(), QStringLiteral("Open JSON Folder"));
        QVERIFY(ui.openJsonFolder->isEnabled());

        ui.openJsonFolder->click();

        QCOMPARE(
            openedFolder,
            directory.filePath(QStringLiteral("camera")));
    }

    void selectingAPoseDoesNotOverwriteTheNewPoseTagDraft()
    {
        WorkspaceState state;
        makeReady(state);
        RecordingCameraCommands commands;
        commands.snapshot.workspaceUuid = QStringLiteral(
            "123e4567-e89b-12d3-a456-426614174000");
        commands.snapshot.poses = {
            summary(QStringLiteral("view_001"), QStringLiteral("2026-07-21T11:00:00Z"),
                    {QStringLiteral("hole")}),
            summary(QStringLiteral("view_002"), QStringLiteral("2026-07-21T12:00:00Z"),
                    {QStringLiteral("inspection"), QStringLiteral("underside")})};
        CameraPanel panel(state, commands);
        const Controls ui = controls(panel);

        QVERIFY(ui.newPoseTags != nullptr);
        QVERIFY(ui.selectedPoseTags != nullptr);
        QCOMPARE(
            ui.newPoseTags->tags(),
            QStringList({QStringLiteral("inspection"), QStringLiteral("underside")}));
        QVERIFY(!ui.selectedPoseTags->isEnabled());

        ui.newPoseTags->load({QStringLiteral("draft")});
        ui.poses->setCurrentRow(0);

        QCOMPARE(ui.newPoseTags->tags(), QStringList({QStringLiteral("draft")}));
        QCOMPARE(ui.selectedPoseTags->tags(), QStringList({QStringLiteral("hole")}));
        QVERIFY(ui.selectedPoseTags->isEnabled());
    }

    void updatingSelectedPoseTagsDoesNotChangeTheNewPoseDraft()
    {
        WorkspaceState state;
        makeReady(state);
        RecordingCameraCommands commands;
        commands.snapshot.workspaceUuid = QStringLiteral(
            "123e4567-e89b-12d3-a456-426614174000");
        commands.snapshot.poses = {summary(
            QStringLiteral("view_001"),
            QStringLiteral("2026-07-21T12:00:00Z"),
            {QStringLiteral("hole")})};
        CameraPanel panel(state, commands);
        const Controls ui = controls(panel);
        ui.newPoseTags->load({QStringLiteral("next-shot")});
        ui.poses->setCurrentRow(0);
        ui.selectedPoseTags->load({QStringLiteral("reviewed")});
        commands.setDetailsAction = [&commands] {
            commands.snapshot.poses[0].tags =
                QStringList({QStringLiteral("reviewed")});
        };

        ui.saveChanges->click();

        QCOMPARE(
            ui.newPoseTags->tags(),
            QStringList({QStringLiteral("next-shot")}));
        QCOMPARE(
            ui.selectedPoseTags->tags(),
            QStringList({QStringLiteral("reviewed")}));
    }

    void successfulSelectedTagUpdateSurvivesALaggingSnapshot()
    {
        WorkspaceState state;
        makeReady(state);
        RecordingCameraCommands commands;
        commands.snapshot.workspaceUuid = QStringLiteral(
            "123e4567-e89b-12d3-a456-426614174000");
        commands.snapshot.poses = {summary(
            QStringLiteral("view_001"),
            QStringLiteral("2026-07-21T12:00:00Z"),
            {QStringLiteral("hole")})};
        CameraPanel panel(state, commands);
        const Controls ui = controls(panel);
        ui.poses->setCurrentRow(0);
        ui.selectedPoseTags->load({QStringLiteral("reviewed")});

        ui.saveChanges->click();

        QCOMPARE(
            commands.assignedTags,
            QVector<QStringList>({{QStringLiteral("reviewed")}}));
        QCOMPARE(
            ui.selectedPoseTags->tags(),
            QStringList({QStringLiteral("reviewed")}));
        QVERIFY(ui.poses->currentItem()->text().contains(
            QStringLiteral("#reviewed")));
        QVERIFY(!ui.poses->currentItem()->text().contains(
            QStringLiteral("#hole")));
    }

    void exposesOpaqueEnglishControlsAndScopedPoseSummaries()
    {
        WorkspaceState state;
        makeReady(state);
        RecordingCameraCommands commands;
        commands.libraryPath =
            QStringLiteral("/tmp/camera-poses/poses.json");
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
            QStringLiteral("Workspace"));
        QVERIFY(ui.uidInput != nullptr);
        QCOMPARE(
            ui.uidInput->text(),
            QStringLiteral("123e4567-e89b-12d3-a456-426614174000"));
        QVERIFY(ui.poses != nullptr);
        QCOMPARE(ui.poses->count(), 2);
        QVERIFY(ui.poses->item(0)->text().contains(QStringLiteral("view_001")));
        QVERIFY(ui.poses->item(0)->toolTip().contains(
            QStringLiteral("2026-07-14T01:00:00.000Z")));
        QCOMPARE(
            ui.poses->item(0)->data(Qt::UserRole).toString(),
            QStringLiteral("view_001"));
        QVERIFY(ui.save != nullptr);
        QCOMPARE(ui.save->text(), QStringLiteral("Save View"));
        QVERIFY(ui.saveAndCopyScreenshot != nullptr);
        QCOMPARE(
            ui.saveAndCopyScreenshot->text(),
            QStringLiteral("Save View && Copy"));
        QCOMPARE(ui.save->width(), ui.saveAndCopyScreenshot->width());
        QVERIFY(ui.openJsonFolder != nullptr);
        QCOMPARE(
            ui.openJsonFolder->text(),
            QStringLiteral("Open JSON Folder"));
        QVERIFY(ui.openJsonFolder->isEnabled());
        QVERIFY(ui.openScreenshotFolder != nullptr);
        QCOMPARE(
            ui.openScreenshotFolder->text(),
            QStringLiteral("Folder"));
        QVERIFY(ui.apply != nullptr);
        QCOMPARE(ui.apply->text(), QStringLiteral("Apply View"));
        QVERIFY(ui.remove != nullptr);
        QCOMPARE(ui.remove->text(), QStringLiteral("Delete"));
        QVERIFY(ui.save->isEnabled());
        QVERIFY(ui.saveAndCopyScreenshot->isEnabled());
        QVERIFY(!ui.apply->isEnabled());
        QVERIFY(!ui.openScreenshotFolder->isEnabled());
        QVERIFY(!ui.remove->isEnabled());
        QVERIFY(ui.selectionSection != nullptr);
        QVERIFY(ui.selectionSection->isHidden());
        QVERIFY(ui.emptyState != nullptr);
        QVERIFY(ui.emptyState->isHidden());
        QVERIFY(panel.sizeHint().width() <= 440);
        QCOMPARE(commands.snapshotCalls, 1);
    }

    void selectionAppliesTheExactViewAndEnablesSelectionActions()
    {
        WorkspaceState state;
        makeReady(state);
        RecordingCameraCommands commands;
        commands.snapshot.workspaceUuid = QStringLiteral(
            "123e4567-e89b-12d3-a456-426614174000");
        // Legacy pose libraries may omit milliseconds and do not promise list order.
        commands.snapshot.poses = {
            summary(QStringLiteral("view_001"), QStringLiteral("first")),
            summary(QStringLiteral("view_002"), QStringLiteral("second"))};
        CameraPanel panel(state, commands);
        panel.show();
        QApplication::processEvents();
        Controls ui = controls(panel);
        QVERIFY(ui.poses != nullptr);
        QVERIFY(ui.apply != nullptr);
        QVERIFY(ui.remove != nullptr);
        QVERIFY(ui.openScreenshotFolder != nullptr);

        ui.poses->setCurrentRow(1);

        QVERIFY(!ui.selectionSection->isHidden());
        QVERIFY(ui.apply->isEnabled());
        QVERIFY(ui.remove->isEnabled());
        QVERIFY(!ui.openScreenshotFolder->isEnabled());
        QTest::mouseClick(ui.apply, Qt::LeftButton);
        QCOMPARE(commands.applyViewIds, QStringList({QStringLiteral("view_002")}));
    }

    void selectedPoseDisplaysAndUpdatesItsTags()
    {
        WorkspaceState state;
        makeReady(state);
        RecordingCameraCommands commands;
        commands.snapshot.workspaceUuid = QStringLiteral(
            "123e4567-e89b-12d3-a456-426614174000");
        commands.snapshot.poses = {summary(
            QStringLiteral("view_001"),
            QStringLiteral("saved"),
            {QStringLiteral("hole"), QStringLiteral("edge")})};
        CameraPanel panel(state, commands);
        const Controls ui = controls(panel);
        QVERIFY(ui.selectedPoseTags != nullptr);
        QVERIFY(ui.saveChanges != nullptr);

        ui.poses->setCurrentRow(0);
        QCOMPARE(
            ui.selectedPoseTags->tags(),
            QStringList({QStringLiteral("hole"), QStringLiteral("edge")}));
        ui.selectedPoseTags->load(
            {QStringLiteral("inspection"), QStringLiteral("underside")});
        commands.setDetailsAction = [&commands] {
            commands.snapshot.poses[0].tags = QStringList{
                QStringLiteral("inspection"), QStringLiteral("underside")};
        };

        ui.saveChanges->click();

        QCOMPARE(commands.taggedViewIds, QStringList({QStringLiteral("view_001")}));
        QCOMPARE(
            commands.assignedTags,
            QVector<QStringList>({
                {QStringLiteral("inspection"), QStringLiteral("underside")}}));
        QCOMPARE(
            ui.selectedPoseTags->tags(),
            QStringList({QStringLiteral("inspection"), QStringLiteral("underside")}));
        QVERIFY(ui.poses->item(0)->text().contains(QStringLiteral("#inspection")));
        QVERIFY(ui.poses->item(0)->text().contains(QStringLiteral("#underside")));
    }

    void selectedViewShowsItsNoteAndKeepsDraftsAcrossRefreshes()
    {
        WorkspaceState state;
        makeReady(state);
        RecordingCameraCommands commands;
        commands.snapshot.workspaceUuid = QStringLiteral("workspace-a");
        commands.snapshot.poses = {
            summary(QStringLiteral("view_001"), {}, {}, {}, QStringLiteral("First note")),
            summary(QStringLiteral("view_002"), {}, {}, {}, QStringLiteral("Second note"))};
        CameraPanel panel(state, commands);
        const Controls ui = controls(panel);
        QVERIFY(ui.note != nullptr);
        QVERIFY(ui.saveChanges != nullptr);
        QVERIFY(!ui.note->isEnabled());
        QVERIFY(!ui.saveChanges->isEnabled());

        ui.poses->setCurrentRow(0);
        QCOMPARE(ui.note->toPlainText(), QStringLiteral("First note"));
        QVERIFY(ui.note->isEnabled());
        QVERIFY(ui.saveChanges->isEnabled());
        ui.note->setPlainText(QStringLiteral("Draft for first view"));
        panel.refreshFromState();
        QCOMPARE(ui.poses->currentRow(), 0);
        QCOMPARE(ui.note->toPlainText(), QStringLiteral("Draft for first view"));

        ui.poses->setCurrentRow(1);
        QCOMPARE(ui.note->toPlainText(), QStringLiteral("Second note"));
        ui.poses->setCurrentRow(0);
        QCOMPARE(ui.note->toPlainText(), QStringLiteral("Draft for first view"));
        QVERIFY(commands.notedViewIds.isEmpty());
    }

    void combinedFormsFitTheCameraPanelWithTheApplicationTheme()
    {
        WorkspaceState state;
        makeReady(state);
        RecordingCameraCommands commands;
        commands.snapshot.workspaceUuid = QStringLiteral("inspection-demo");
        commands.snapshot.poses = {
            summary(QStringLiteral("view_001"), QStringLiteral("2026-10-10T08:00:00Z"),
                    {QStringLiteral("surface"), QStringLiteral("edge")}, {},
                    QStringLiteral("Small gap along the left edge.\nInspect the underside next.")),
            summary(QStringLiteral("view_002"), QStringLiteral("2026-10-10T08:01:00Z"))};
        CameraPanel panel(state, commands);
        panel.setStyleSheet(meshCompareApplicationStyleSheet());
        const Controls ui = controls(panel);
        ui.poses->setCurrentRow(0);
        panel.show();
        QApplication::processEvents();

        QVERIFY(ui.saveChanges->isVisible());
        QVERIFY(ui.newNote->isVisible());
        QVERIFY(ui.note->isVisible());
        QVERIFY2(panel.height() <= 860, qPrintable(QStringLiteral("Panel height is %1").arg(panel.height())));
        const QRect saveBounds(ui.saveChanges->mapTo(&panel, QPoint()), ui.saveChanges->size());
        QVERIFY(panel.rect().contains(saveBounds));
        QVERIFY(ui.newNote->height() < ui.note->height());
        qInfo() << "Combined camera form size:" << panel.size();
        const QString previewPath = qEnvironmentVariable("MESHCOMPARE_CAMERA_FORM_PREVIEW");
        if (!previewPath.isEmpty())
            QVERIFY(panel.grab().save(previewPath, "PNG"));
    }

    void capturesTagsAndNoteTogetherAndSelectsTheNewView_data()
    {
        QTest::addColumn<bool>("copyScreenshot");
        QTest::newRow("save-view") << false;
        QTest::newRow("save-view-and-copy") << true;
    }

    void capturesTagsAndNoteTogetherAndSelectsTheNewView()
    {
        QFETCH(bool, copyScreenshot);
        WorkspaceState state;
        makeReady(state);
        RecordingCameraCommands commands;
        commands.snapshot.workspaceUuid = QStringLiteral("workspace-a");
        commands.snapshot.poses = {summary(QStringLiteral("view_old"), {})};
        commands.screenshotToCopy = QImage(2, 2, QImage::Format_RGB32);
        commands.screenshotToCopy.fill(Qt::green);
        CameraPanelServices services;
        services.copyImageToClipboard = [](const QImage&) { return true; };
        CameraPanel panel(state, commands, nullptr, services);
        panel.show();
        const Controls ui = controls(panel);
        QVERIFY(ui.newNote != nullptr);
        ui.poses->setCurrentRow(0);
        const QStringList tags{QStringLiteral("inspection"), QStringLiteral("edge")};
        const QString note = QString::fromUtf8("检查边缘\nSecond line");
        ui.newPoseTags->load(tags);
        ui.newNote->setPlainText(note);
        const QString savedId = copyScreenshot ? QStringLiteral("view_screenshot") : QStringLiteral("view_saved");
        auto saved = [&] {
            commands.snapshot.poses.append(summary(savedId, {}, commands.lastSaveTags, {}, commands.lastSaveNote));
            panel.refreshFromState();
        };
        commands.saveAction = saved;
        commands.saveWithScreenshotAction = saved;

        (copyScreenshot ? ui.saveAndCopyScreenshot : ui.save)->click();

        QCOMPARE(commands.combinedCaptureCalls, 1);
        QCOMPARE(commands.lastSaveTags, tags);
        QCOMPARE(commands.lastSaveNote, note);
        QCOMPARE(commands.detailsCalls, 0);
        QCOMPARE(ui.poses->currentItem()->data(Qt::UserRole).toString(), savedId);
        QCOMPARE(ui.selectedPoseTags->tags(), tags);
        QCOMPARE(ui.note->toPlainText(), note);
        QCOMPARE(panel.focusWidget(), ui.note);
        QVERIFY(ui.newNote->toPlainText().isEmpty());
        QCOMPARE(ui.newPoseTags->tags(), tags);
        QVERIFY(panel.findChild<QPushButton*>(QStringLiteral("saveCameraPoseNoteButton")) == nullptr);
        QCOMPARE(ui.saveChanges->text(), QStringLiteral("Save Changes"));
    }

    void failedCaptureKeepsTheCompleteFormDraft()
    {
        WorkspaceState state;
        makeReady(state);
        RecordingCameraCommands commands;
        commands.snapshot.workspaceUuid = QStringLiteral("workspace-a");
        commands.saveResult = OperationResult::failure(QStringLiteral("Capture failed."));
        CameraPanel panel(state, commands);
        const Controls ui = controls(panel);
        ui.newPoseTags->load({QStringLiteral("review")});
        ui.newNote->setPlainText(QStringLiteral("Capture note"));
        ui.save->click();
        QCOMPARE(commands.combinedCaptureCalls, 1);
        QCOMPARE(ui.newPoseTags->tags(), QStringList({QStringLiteral("review")}));
        QCOMPARE(ui.newNote->toPlainText(), QStringLiteral("Capture note"));
        QCOMPARE(ui.poses->count(), 0);
    }

    void savesMultilineUnicodeNoteAndCanClearIt()
    {
        WorkspaceState state;
        makeReady(state);
        RecordingCameraCommands commands;
        commands.snapshot.workspaceUuid = QStringLiteral("workspace-a");
        commands.snapshot.poses = {summary(QStringLiteral("view_001"), {})};
        CameraPanel panel(state, commands);
        const Controls ui = controls(panel);
        QSignalSpy status(&panel, &CameraPanel::statusMessage);
        QSignalSpy confirmation(&panel, &CameraPanel::confirmationMessage);
        ui.poses->setCurrentRow(0);
        const QString note = QString::fromUtf8("  检查边缘\nCafé — underside\nSecond line  ");
        ui.selectedPoseTags->load({QStringLiteral("reviewed")});
        ui.note->setPlainText(note);
        commands.setDetailsAction = [&] {
            commands.snapshot.poses[0].note = commands.assignedNotes.last();
            commands.snapshot.poses[0].tags = commands.assignedTags.last();
            panel.refreshFromState();
        };

        ui.saveChanges->click();

        QCOMPARE(status.count(), 0);
        QCOMPARE(confirmation.count(), 1);
        QCOMPARE(confirmation.takeFirst().at(0).toString(), QStringLiteral("Changes saved for view_001."));
        QCOMPARE(commands.notedViewIds, QStringList({QStringLiteral("view_001")}));
        QCOMPARE(commands.assignedNotes, QStringList({note}));
        QCOMPARE(commands.assignedTags, QVector<QStringList>({{QStringLiteral("reviewed")}}));
        QCOMPARE(commands.detailsCalls, 1);
        QCOMPARE(commands.legacyDetailsCalls, 0);
        QCOMPARE(ui.note->toPlainText(), note);
        QCOMPARE(ui.poses->currentRow(), 0);

        ui.selectedPoseTags->load({});
        ui.note->clear();
        ui.saveChanges->click();

        QCOMPARE(status.count(), 0);
        QCOMPARE(confirmation.count(), 1);
        QCOMPARE(commands.notedViewIds.size(), 2);
        QCOMPARE(commands.detailsCalls, 2);
        QVERIFY(commands.assignedTags.last().isEmpty());
        QVERIFY(commands.assignedNotes.last().isEmpty());
        QVERIFY(commands.snapshot.poses[0].note.isEmpty());
        QVERIFY(ui.note->toPlainText().isEmpty());
    }

    void failedNoteSaveKeepsTheDraftAndCanBeRetried()
    {
        WorkspaceState state;
        makeReady(state);
        RecordingCameraCommands commands;
        commands.snapshot.workspaceUuid = QStringLiteral("workspace-a");
        commands.snapshot.poses = {
            summary(QStringLiteral("view_001"), {}, {}, {}, QStringLiteral("Saved note"))};
        CameraPanel panel(state, commands);
        const Controls ui = controls(panel);
        QSignalSpy status(&panel, &CameraPanel::statusMessage);
        ui.poses->setCurrentRow(0);
        ui.selectedPoseTags->load({QStringLiteral("draft-tag")});
        ui.note->setPlainText(QStringLiteral("Unsaved\nDraft"));
        commands.setDetailsResult = OperationResult::failure(QStringLiteral("Could not save note."));
        commands.setDetailsAction = [&] { panel.refreshFromState(); };

        ui.saveChanges->click();

        QCOMPARE(status.count(), 1);
        QCOMPARE(status.takeFirst().at(0).toString(), QStringLiteral("Could not save note."));
        QCOMPARE(ui.note->toPlainText(), QStringLiteral("Unsaved\nDraft"));
        QCOMPARE(commands.snapshot.poses[0].note, QStringLiteral("Saved note"));
        QVERIFY(commands.snapshot.poses[0].tags.isEmpty());
        panel.refreshFromState();
        QCOMPARE(ui.selectedPoseTags->tags(), QStringList({QStringLiteral("draft-tag")}));
        QCOMPARE(ui.note->toPlainText(), QStringLiteral("Unsaved\nDraft"));
        commands.setDetailsResult = OperationResult::success();
        ui.saveChanges->click();
        panel.refreshFromState();
        QCOMPARE(commands.notedViewIds.size(), 2);
        // The UI also retains a successful save until a delayed snapshot catches up.
        QCOMPARE(ui.note->toPlainText(), QStringLiteral("Unsaved\nDraft"));
    }

    void noteActionsRequireTheSelectedReadyWorkspaceAndDoNotTransferDrafts()
    {
        WorkspaceState state;
        makeReady(state);
        RecordingCameraCommands commands;
        commands.snapshot.workspaceUuid = QStringLiteral("workspace-a");
        commands.snapshot.poses = {
            summary(QStringLiteral("view_001"), {}, {}, {}, QStringLiteral("Workspace A note"))};
        CameraPanel panel(state, commands);
        const Controls ui = controls(panel);
        ui.poses->setCurrentRow(0);
        ui.note->setPlainText(QStringLiteral("Workspace A draft"));
        ui.uidInput->setText(QStringLiteral("workspace-b"));
        QVERIFY(!ui.note->isEnabled());
        QVERIFY(!ui.saveChanges->isEnabled());
        ui.saveChanges->click();
        QVERIFY(commands.notedViewIds.isEmpty());

        commands.snapshot.workspaceUuid = QStringLiteral("workspace-b");
        commands.snapshot.poses[0].note = QStringLiteral("Workspace B note");
        panel.refreshFromState();
        QVERIFY(ui.note->toPlainText().isEmpty());
        QVERIFY(!ui.saveChanges->isEnabled());
        ui.poses->setCurrentRow(0);
        QCOMPARE(ui.note->toPlainText(), QStringLiteral("Workspace B note"));
        QVERIFY(state.beginAnalysis().ok);
        panel.refreshFromState();
        QVERIFY(!ui.note->isEnabled());
        QVERIFY(!ui.saveChanges->isEnabled());
    }

    void latestTaggedPosePrefillsEvenWhenTheNewestPoseIsUntagged()
    {
        WorkspaceState state;
        makeReady(state);
        RecordingCameraCommands commands;
        commands.snapshot.workspaceUuid = QStringLiteral(
            "123e4567-e89b-12d3-a456-426614174000");
        commands.snapshot.poses = {
            summary(QStringLiteral("view_003"), QStringLiteral("2026-07-21T13:00:00Z")),
            summary(QStringLiteral("view_002"), QStringLiteral("2026-07-21T12:00:00Z"),
                    {QStringLiteral("inspection"), QStringLiteral("underside")}),
            summary(QStringLiteral("view_001"), QStringLiteral("2026-07-21T11:00:00Z"),
                    {QStringLiteral("hole")})};
        CameraPanel panel(state, commands);
        const Controls ui = controls(panel);

        QCOMPARE(
            ui.newPoseTags->tags(),
            QStringList({QStringLiteral("inspection"), QStringLiteral("underside")}));
        QVERIFY(ui.newPoseTags->isEnabled());
        QTest::mouseClick(ui.save, Qt::LeftButton);

        QCOMPARE(
            commands.lastSaveTags,
            QStringList({QStringLiteral("inspection"), QStringLiteral("underside")}));
    }

    void successfulSavePersistsTagsAcrossPanelsAndWorkspaces()
    {
        bool hasStoredTags = false;
        QStringList storedTags;
        auto services = [&hasStoredTags, &storedTags] {
            CameraPanelServices result;
            result.loadLastCaptureTags =
                [&hasStoredTags, &storedTags](QStringList& tags) {
                    if (!hasStoredTags)
                        return false;
                    tags = storedTags;
                    return true;
                };
            result.saveLastCaptureTags =
                [&hasStoredTags, &storedTags](const QStringList& tags) {
                    hasStoredTags = true;
                    storedTags = tags;
                };
            return result;
        };

        WorkspaceState state;
        makeReady(state);
        RecordingCameraCommands firstCommands;
        firstCommands.snapshot.workspaceUuid = QStringLiteral("workspace-a");
        firstCommands.snapshot.poses = {summary(
            QStringLiteral("view_001"),
            QStringLiteral("2026-07-21T12:00:00Z"),
            {QStringLiteral("hole")})};
        {
            CameraPanel firstPanel(
                state, firstCommands, nullptr, services());
            const Controls firstUi = controls(firstPanel);
            firstUi.newPoseTags->load(
                {QStringLiteral("inspection"), QStringLiteral("underside")});

            QTest::mouseClick(firstUi.save, Qt::LeftButton);

            QVERIFY(hasStoredTags);
            QCOMPARE(
                storedTags,
                QStringList({
                    QStringLiteral("inspection"),
                    QStringLiteral("underside")}));
        }

        RecordingCameraCommands secondCommands;
        secondCommands.snapshot.workspaceUuid = QStringLiteral("workspace-b");
        secondCommands.snapshot.poses = {summary(
            QStringLiteral("view_001"),
            QStringLiteral("2026-07-22T12:00:00Z"),
            {QStringLiteral("edge")})};
        CameraPanel secondPanel(
            state, secondCommands, nullptr, services());

        QCOMPARE(
            controls(secondPanel).newPoseTags->tags(),
            QStringList({
                QStringLiteral("inspection"),
                QStringLiteral("underside")}));
    }

    void persistedEmptyTagsOverrideOlderTaggedPoses()
    {
        QStringList storedTags;
        CameraPanelServices services;
        services.loadLastCaptureTags = [&storedTags](QStringList& tags) {
            tags = storedTags;
            return true;
        };
        services.saveLastCaptureTags = [](const QStringList&) {};

        WorkspaceState state;
        makeReady(state);
        RecordingCameraCommands commands;
        commands.snapshot.workspaceUuid = QStringLiteral("workspace-a");
        commands.snapshot.poses = {summary(
            QStringLiteral("view_001"),
            QStringLiteral("2026-07-21T12:00:00Z"),
            {QStringLiteral("hole")})};

        CameraPanel panel(state, commands, nullptr, std::move(services));

        QVERIFY(controls(panel).newPoseTags->tags().isEmpty());
    }

    void workspaceRefreshDoesNotOverwriteAnEditedCaptureTag()
    {
        WorkspaceState state;
        makeReady(state);
        RecordingCameraCommands commands;
        commands.snapshot.workspaceUuid = QStringLiteral("workspace-a");
        commands.snapshot.poses = {summary(
            QStringLiteral("view_001"),
            QStringLiteral("2026-07-21T12:00:00Z"),
            {QStringLiteral("hole")})};
        CameraPanel panel(state, commands);
        const Controls ui = controls(panel);
        ui.newPoseTags->load({QStringLiteral("custom-review")});

        commands.snapshot.workspaceUuid = QStringLiteral("workspace-b");
        panel.refreshFromState();

        QCOMPARE(
            ui.newPoseTags->tags(),
            QStringList({QStringLiteral("custom-review")}));
    }

    void successfulSaveRemembersTagsWhenTheRefreshDoesNotYetContainThePose()
    {
        WorkspaceState state;
        makeReady(state);
        RecordingCameraCommands commands;
        commands.snapshot.workspaceUuid = QStringLiteral(
            "123e4567-e89b-12d3-a456-426614174000");
        commands.snapshot.poses = {summary(
            QStringLiteral("view_001"),
            QStringLiteral("2026-07-21T12:00:00Z"),
            {QStringLiteral("hole")})};
        CameraPanel panel(state, commands);
        const Controls ui = controls(panel);

        ui.newPoseTags->load(
            {QStringLiteral("inspection"), QStringLiteral("underside")});
        QTest::mouseClick(ui.save, Qt::LeftButton);

        QCOMPARE(
            commands.lastSaveTags,
            QStringList({QStringLiteral("inspection"), QStringLiteral("underside")}));
        QCOMPARE(
            ui.newPoseTags->tags(),
            QStringList({QStringLiteral("inspection"), QStringLiteral("underside")}));

        ui.newPoseTags->load({QStringLiteral("hole")});
        auto* input = qobject_cast<QLineEdit*>(ui.newPoseTags->focusProxy());
        QVERIFY(input != nullptr);
        QTest::keyClicks(input, QStringLiteral("ins"));
        QCOMPARE(input->completer()->completionCount(), 1);
        QCOMPARE(
            input->completer()->currentCompletion(),
            QStringLiteral("inspection"));
    }

    void tagSuggestionsMatchThePendingPrefix()
    {
        WorkspaceState state;
        makeReady(state);
        RecordingCameraCommands commands;
        commands.snapshot.workspaceUuid = QStringLiteral(
            "123e4567-e89b-12d3-a456-426614174000");
        commands.snapshot.poses = {
            summary(QStringLiteral("view_001"), QStringLiteral("first"),
                    {QStringLiteral("hole"), QStringLiteral("inspection")}),
            summary(QStringLiteral("view_002"), QStringLiteral("latest"),
                    {QStringLiteral("underside")})};
        CameraPanel panel(state, commands);
        const Controls ui = controls(panel);
        QVERIFY(ui.newPoseTags != nullptr);
        auto* input = qobject_cast<QLineEdit*>(ui.newPoseTags->focusProxy());
        QVERIFY(input != nullptr);
        QCompleter* completer = input->completer();
        QVERIFY(completer != nullptr);

        ui.newPoseTags->load({QStringLiteral("hole")});
        QTest::keyClicks(input, QStringLiteral("un"));

        QCOMPARE(input->text(), QStringLiteral("un"));
        QCOMPARE(completer->completionCount(), 1);
        QCOMPARE(
            completer->currentCompletion(),
            QStringLiteral("underside"));
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

        ui.remove->click();

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
        ui.apply->click();
        QCOMPARE(status.count(), 1);
        QCOMPARE(status.takeFirst().at(0).toString(), QStringLiteral("Apply failed."));
        QCOMPARE(commands.snapshotCalls, 1);

        commands.deleteResult =
            OperationResult::failure(QStringLiteral("Delete failed."));
        ui.remove->click();
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

        ui.apply->click();

        QCOMPARE(commands.applyViewIds, QStringList({QStringLiteral("view_002")}));
        QCOMPARE(ui.poses->count(), 0);
    }
};

QTEST_MAIN(CameraPanelTest)
#include "camera_panel_test.moc"
