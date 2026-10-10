#include <QtTest>

#include <cmath>

#include <QAbstractButton>
#include <QAbstractItemView>
#include <QApplication>
#include <QComboBox>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileOpenEvent>
#include <QFrame>
#include <QLabel>
#include <QListWidget>
#include <QMdiArea>
#include <QMenuBar>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QMouseEvent>
#include <QPushButton>
#include <QPlainTextEdit>
#include <QSharedPointer>
#include <QSignalSpy>
#include <QTabBar>
#include <QTimer>
#include <QTemporaryDir>
#include <QToolButton>
#include <QUrl>

#include "app/camera_commands.h"
#include "app/application_startup.h"
#include "app/camera_panel.h"
#include "app/coloring_commands.h"
#include "app/coloring_panel.h"
#include "app/standalone_main_window.h"
#include "app/workspace_controller.h"
#include "core/workspace_state.h"

namespace
{
MeshEntry entry(MeshId id)
{
    return {id,
            id + 100,
            QStringLiteral("/tmp/mesh-%1.obj").arg(id),
            QStringLiteral("mesh-%1.obj").arg(id),
            {},
            false};
}

OperationResult makeReady(WorkspaceState& state)
{
    state.beginLoading();
    return state.commitWorkspace({entry(1), entry(2)}, 1);
}

double linearColorComponent(qreal component)
{
    return component <= 0.03928
        ? component / 12.92
        : std::pow((component + 0.055) / 1.055, 2.4);
}

double relativeLuminance(const QColor& color)
{
    return
        (0.2126 * linearColorComponent(color.redF())) +
        (0.7152 * linearColorComponent(color.greenF())) +
        (0.0722 * linearColorComponent(color.blueF()));
}

double contrastRatio(const QColor& first, const QColor& second)
{
    const double firstLuminance = relativeLuminance(first);
    const double secondLuminance = relativeLuminance(second);
    const double lighter = qMax(firstLuminance, secondLuminance);
    const double darker = qMin(firstLuminance, secondLuminance);
    return (lighter + 0.05) / (darker + 0.05);
}

class WindowColoringCommands final : public IColoringCommands
{
public:
    explicit WindowColoringCommands(WorkspaceState& state)
        : state_(state)
    {
    }

    OperationResult setReference(MeshId meshId) override
    {
        return state_.setReference(meshId);
    }
    OperationResult selectMesh(MeshId meshId) override
    {
        return state_.setSelectedMesh(meshId);
    }
    OperationResult setUniformColor(MeshId, const QColor&) override
    {
        return OperationResult::success();
    }
    OperationResult startAnalysis(
        SurfaceComparisonMetric,
        const SurfaceComparisonOptions&) override
    {
        return state_.beginAnalysis();
    }
    void cancelAnalysis() override { state_.finishAnalysis(); }
    OperationResult clearColoring() override
    {
        return OperationResult::success();
    }

private:
    WorkspaceState& state_;
};

class WindowCameraCommands final : public ICameraCommands
{
public:
    QString screenshotPath;
    int applyCount = 0;
    CameraPanelSnapshot cameraPanelSnapshot() const override
    {
        CameraPanelSnapshot snapshot;
        snapshot.result = OperationResult::success();
        snapshot.workspaceUuid =
            QStringLiteral("00112233445566778899aabbccddeeff");
        snapshot.poses = {
            {QStringLiteral("view_001"),
             QStringLiteral("2026-07-15T01:00:00.000Z")}};
        snapshot.poses[0].screenshotPath = screenshotPath;
        snapshot.poses[0].workspaceUuid = snapshot.workspaceUuid;
        return snapshot;
    }

    OperationResult setCameraPoseUid(const QString&) override
    {
        return OperationResult::success();
    }

    OperationResult saveCurrentCameraPose(
        QString*,
        const QStringList&) override
    {
        return OperationResult::success();
    }
    OperationResult saveCurrentCameraPoseWithScreenshot(
        QImage& screenshot,
        QString*,
        const QStringList&) override
    {
        screenshot = QImage(4, 3, QImage::Format_RGB32);
        screenshot.fill(QColor(42, 96, 164));
        return OperationResult::success();
    }
    OperationResult applyCameraPose(const QString&) override
    {
        ++applyCount;
        return OperationResult::success();
    }
    OperationResult saveCurrentCameraPoseWithNote(
        QString* id, const QStringList& tags, const QString&) override
    {
        return saveCurrentCameraPose(id, tags);
    }
    OperationResult saveCurrentCameraPoseWithScreenshotAndNote(
        QImage& screenshot, QString* id, const QStringList& tags,
        const QString&) override
    {
        return saveCurrentCameraPoseWithScreenshot(screenshot, id, tags);
    }
    OperationResult setCameraPoseDetailsForWorkspace(
        const QString&, const QString&, const QStringList&, const QString&) override
    {
        return OperationResult::success();
    }
    OperationResult setCameraPoseTags(
        const QString&,
        const QStringList&) override
    {
        return OperationResult::success();
    }
    OperationResult setCameraPoseNote(const QString&, const QString&) override
    {
        return OperationResult::success();
    }
    OperationResult setCameraPoseNoteForWorkspace(
        const QString&, const QString&, const QString&) override
    {
        return OperationResult::success();
    }
    OperationResult deleteCameraPose(const QString&) override
    {
        return OperationResult::success();
    }
};

struct MessageBoxProbe {
    bool found = false;
    bool requestedRoleFound = false;
    int dialogCount = 0;
    QString capturedText;
};

QSharedPointer<MessageBoxProbe> clickNextMessageBoxRole(
    QMessageBox::ButtonRole role,
    QObject* context)
{
    auto probe = QSharedPointer<MessageBoxProbe>::create();
    QTimer::singleShot(0, context, [role, probe] {
        auto* messageBox =
            qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
        for (QWidget* widget : QApplication::topLevelWidgets()) {
            auto* candidate = qobject_cast<QMessageBox*>(widget);
            if (candidate == nullptr || !candidate->isVisible())
                continue;
            ++probe->dialogCount;
            if (messageBox == nullptr)
                messageBox = candidate;
        }

        probe->found = messageBox != nullptr;
        if (messageBox == nullptr)
            return;
        probe->capturedText = messageBox->text() + QLatin1Char('\n') +
                              messageBox->informativeText() + QLatin1Char('\n') +
                              messageBox->detailedText();
        for (QAbstractButton* button : messageBox->buttons()) {
            if (messageBox->buttonRole(button) == role) {
                probe->requestedRoleFound = true;
                button->click();
                return;
            }
        }

        messageBox->reject();
    });
    return probe;
}
} // namespace

class StandaloneMainWindowTest : public QObject
{
    Q_OBJECT

private slots:
    void fileOpenBridgeBatchesQueuedFinderEventsInOrder()
    {
        FileOpenEventBridge bridge(*qApp);
        QFileOpenEvent first(QStringLiteral("/tmp/first.obj"));
        QFileOpenEvent second(QStringLiteral("/tmp/second.obj"));
        QVERIFY(QApplication::sendEvent(qApp, &first));
        QVERIFY(QApplication::sendEvent(qApp, &second));

        QStringList openedPaths;
        int openCount = 0;
        bridge.setOpenHandler(
            [&](const QStringList& paths) {
                ++openCount;
                openedPaths = paths;
            });

        QTRY_COMPARE(openCount, 1);
        QCOMPARE(
            openedPaths,
            QStringList({QStringLiteral("/tmp/first.obj"),
                         QStringLiteral("/tmp/second.obj")}));
    }

    void finderOpenAddsToExistingWindowWithoutConfirmation()
    {
        WorkspaceState state;
        QVERIFY(makeReady(state).ok);
        StandaloneMainWindow window(state);
        QSignalSpy importSpy(&window, &StandaloneMainWindow::importRequested);
        FileOpenEventBridge bridge(*qApp);
        bridge.setOpenHandler([&window](const QStringList& paths) {
            window.requestImport(paths);
        });
        const auto probe =
            clickNextMessageBoxRole(QMessageBox::RejectRole, &window);
        QFileOpenEvent event(QStringLiteral("/tmp/new.obj"));

        QVERIFY(QApplication::sendEvent(qApp, &event));

        QTRY_COMPARE(importSpy.size(), 1);
        QVERIFY(!probe->found);
        QCOMPARE(importSpy.takeFirst().front().toStringList(),
                 QStringList({QStringLiteral("/tmp/new.obj")}));
    }

    void hasOnlyTheMinimalShell()
    {
        WorkspaceState state;
        StandaloneMainWindow window(state);

        QVERIFY(window.findChild<QWidget*>("commandBar"));
        QVERIFY(window.findChild<QPushButton*>("importMeshesButton"));
        QVERIFY(window.findChild<QWidget*>("viewportHost"));
        QVERIFY(window.findChild<QLabel*>("statusLabel"));
        QVERIFY(window.findChild<QMdiArea*>() == nullptr);
        QCOMPARE(window.menuBar()->actions().size(), 0);
    }

    void disablesEmptyWorkspaceActions()
    {
        WorkspaceState state;
        StandaloneMainWindow window(state);

        auto* coloringButton = window.findChild<QPushButton*>("coloringButton");
        auto* cameraButton = window.findChild<QPushButton*>("cameraButton");
        auto* overlayButton =
            window.findChild<QPushButton*>("overlayViewButton");
        auto* gridButton =
            window.findChild<QPushButton*>("gridViewButton");
        auto* normalizeButton =
            window.findChild<QPushButton*>("normalizeGridButton");
        auto* layersButton =
            window.findChild<QToolButton*>("layersButton");

        QVERIFY(coloringButton);
        QVERIFY(cameraButton);
        QVERIFY(overlayButton);
        QVERIFY(gridButton);
        QVERIFY(normalizeButton);
        QVERIFY(layersButton);
        QVERIFY(!coloringButton->isEnabled());
        QVERIFY(!cameraButton->isEnabled());
        QVERIFY(!overlayButton->isEnabled());
        QVERIFY(!gridButton->isEnabled());
        QVERIFY(!normalizeButton->isEnabled());
        QVERIFY(!layersButton->isEnabled());
    }

    void exposesVisibleOverlayAndGridViewControls()
    {
        WorkspaceState state;
        state.beginLoading();
        QVERIFY(state
                    .commitWorkspace(
                        {entry(1), entry(2)},
                        1,
                        SceneLayoutMode::Overlay)
                    .ok);
        StandaloneMainWindow window(state);
        auto* overlayButton =
            window.findChild<QPushButton*>("overlayViewButton");
        auto* gridButton =
            window.findChild<QPushButton*>("gridViewButton");
        auto* normalizeButton =
            window.findChild<QPushButton*>("normalizeGridButton");
        int requestCount = 0;
        SceneLayoutMode requestedMode = SceneLayoutMode::Overlay;
        connect(
            &window,
            &StandaloneMainWindow::layoutModeRequested,
            [&](SceneLayoutMode mode) {
                ++requestCount;
                requestedMode = mode;
                QVERIFY(state.setLayoutMode(mode).ok);
            });
        connect(
            &window,
            &StandaloneMainWindow::gridNormalizationRequested,
            [&](bool enabled) {
                QVERIFY(state.setGridNormalizationEnabled(enabled).ok);
            });

        QVERIFY(overlayButton);
        QVERIFY(gridButton);
        QVERIFY(normalizeButton);
        QVERIFY(overlayButton->isEnabled());
        QVERIFY(gridButton->isEnabled());
        QVERIFY(overlayButton->isChecked());
        QVERIFY(!gridButton->isChecked());
        QVERIFY(!normalizeButton->isEnabled());
        QVERIFY(!normalizeButton->isChecked());

        gridButton->click();

        QCOMPARE(requestCount, 1);
        QCOMPARE(requestedMode, SceneLayoutMode::ComparisonGrid);
        QVERIFY(!overlayButton->isChecked());
        QVERIFY(gridButton->isChecked());
        QVERIFY(normalizeButton->isEnabled());

        normalizeButton->click();

        QVERIFY(state.gridNormalizationEnabled());
        QVERIFY(normalizeButton->isChecked());

        QVERIFY(state.beginAnalysis().ok);
        window.refreshWorkspace();
        QVERIFY(!overlayButton->isEnabled());
        QVERIFY(!gridButton->isEnabled());
        QVERIFY(!normalizeButton->isEnabled());
    }

    void layersMenuControlsIndividualVisibilityInBothViews()
    {
        WorkspaceState state;
        state.beginLoading();
        QVERIFY(state
                    .commitWorkspace(
                        {entry(1), entry(2)},
                        1,
                        SceneLayoutMode::Overlay)
                    .ok);
        StandaloneMainWindow window(state);
        auto* layersButton =
            window.findChild<QToolButton*>("layersButton");
        auto* layersMenu =
            window.findChild<QMenu*>("layersMenu");
        QVERIFY(layersButton != nullptr);
        QVERIFY(layersMenu != nullptr);
        QCOMPARE(layersButton->text(), QStringLiteral("Layers 2/2"));
        QVERIFY(layersButton->isEnabled());
        layersMenu->popup(QPoint(20, 20));
        QApplication::processEvents();
        QCOMPARE(layersMenu->actions().size(), 2);
        layersMenu->hide();
        connect(
            &window,
            &StandaloneMainWindow::meshVisibilityRequested,
            [&](MeshId meshId, bool visible) {
                QVERIFY(state.setMeshVisible(meshId, visible).ok);
                window.refreshWorkspace();
            });
        QAction* second = window.findChild<QAction*>(
            QStringLiteral("meshVisibilityAction_2"));
        QVERIFY(second != nullptr);

        second->setChecked(false);
        QApplication::processEvents();

        QVERIFY(!state.mesh(2)->visible);
        QCOMPARE(layersButton->text(), QStringLiteral("Layers 1/2"));
        layersMenu->popup(QPoint(20, 20));
        QApplication::processEvents();
        layersMenu->hide();
        QAction* first = window.findChild<QAction*>(
            QStringLiteral("meshVisibilityAction_1"));
        QVERIFY(first != nullptr);
        QVERIFY(!first->isEnabled());

        QVERIFY(state.setLayoutMode(SceneLayoutMode::ComparisonGrid).ok);
        window.refreshWorkspace();
        QVERIFY(layersButton->isEnabled());
        QCOMPARE(layersButton->text(), QStringLiteral("Layers 1/2"));
        layersMenu->popup(QPoint(20, 20));
        QApplication::processEvents();
        layersMenu->hide();
        first = window.findChild<QAction*>(
            QStringLiteral("meshVisibilityAction_1"));
        QVERIFY(first != nullptr);
        QVERIFY(!first->isEnabled());
    }

    void layersMenuSwitchesToLaterImportedLayers_data()
    {
        QTest::addColumn<int>("layoutMode");
        QTest::newRow("grid")
            << static_cast<int>(SceneLayoutMode::ComparisonGrid);
        QTest::newRow("overlay")
            << static_cast<int>(SceneLayoutMode::Overlay);
    }

    void layersMenuSwitchesToLaterImportedLayers()
    {
        QFETCH(int, layoutMode);
        WorkspaceState state;
        QVector<MeshEntry> meshes;
        for (MeshId id = 1; id <= 12; ++id) {
            MeshEntry mesh = entry(id);
            mesh.visible = id <= meshcompare::MaxVisibleMeshLayers;
            meshes.append(mesh);
        }
        state.beginLoading();
        QVERIFY(state.commitWorkspace(
            meshes, 1, static_cast<SceneLayoutMode>(layoutMode)).ok);
        StandaloneMainWindow window(state);
        auto* layersButton = window.findChild<QToolButton*>("layersButton");
        auto* layersMenu = window.findChild<QMenu*>("layersMenu");
        QVERIFY(layersButton != nullptr);
        QVERIFY(layersMenu != nullptr);
        QCOMPARE(layersButton->text(), QStringLiteral("Layers 8/12"));
        connect(
            &window,
            &StandaloneMainWindow::meshVisibilityRequested,
            [&](MeshId meshId, bool visible) {
                QVERIFY(state.setMeshVisible(meshId, visible).ok);
                window.refreshWorkspace();
            });
        const auto rebuildMenu = [&] {
            layersMenu->popup(QPoint(20, 20));
            QApplication::processEvents();
            layersMenu->hide();
        };

        rebuildMenu();
        int layerActionCount = 0;
        for (QAction* action : layersMenu->actions()) {
            if (action->isCheckable())
                ++layerActionCount;
        }
        QCOMPARE(layerActionCount, 12);
        auto* hint = window.findChild<QAction*>("meshVisibilityLimitHint");
        QVERIFY(hint != nullptr);
        QVERIFY(!hint->isEnabled());
        QVERIFY(hint->text().contains(QStringLiteral("Uncheck one")));
        auto* first = window.findChild<QAction*>("meshVisibilityAction_1");
        auto* later = window.findChild<QAction*>("meshVisibilityAction_12");
        QVERIFY(first != nullptr);
        QVERIFY(later != nullptr);
        QVERIFY(first->isChecked());
        QVERIFY(first->isEnabled());
        QVERIFY(!later->isChecked());
        QVERIFY(!later->isEnabled());

        first->trigger();
        QApplication::processEvents();
        QVERIFY(!state.mesh(1)->visible);
        QCOMPARE(layersButton->text(), QStringLiteral("Layers 7/12"));

        rebuildMenu();
        later = window.findChild<QAction*>("meshVisibilityAction_12");
        QVERIFY(later != nullptr);
        QVERIFY(later->isEnabled());
        later->trigger();
        QApplication::processEvents();
        QVERIFY(state.mesh(12)->visible);
        QVERIFY(!state.mesh(1)->visible);
        QCOMPARE(layersButton->text(), QStringLiteral("Layers 8/12"));

        rebuildMenu();
        first = window.findChild<QAction*>("meshVisibilityAction_1");
        later = window.findChild<QAction*>("meshVisibilityAction_12");
        QVERIFY(first != nullptr);
        QVERIFY(later != nullptr);
        QVERIFY(!first->isEnabled());
        QVERIFY(!first->isChecked());
        QVERIFY(later->isEnabled());
        QVERIFY(later->isChecked());
    }

    void layerSubsetNoticeTracksCurrentVisibility()
    {
        WorkspaceState state;
        StandaloneMainWindow window(state);
        auto* notice = window.findChild<QLabel*>("layerVisibilityNotice");
        QVERIFY(notice != nullptr);
        QVERIFY(notice->isHidden());

        QVector<MeshEntry> meshes;
        for (MeshId id = 1; id <= 7; ++id)
            meshes.append(entry(id));
        state.beginLoading();
        QVERIFY(state.commitWorkspace(meshes, 1).ok);
        window.presentImportOutcome({OperationResult::success(), {}, {}});

        QVERIFY(notice->isHidden());
        QVERIFY(notice->text().isEmpty());
        QVERIFY(state.setMeshVisible(7, false).ok);
        window.refreshWorkspace();
        QVERIFY(!notice->isHidden());
        QCOMPARE(notice->text(), QStringLiteral(
            "Showing 6 of 7 layers (1 hidden). Use Layers to change what is shown."));
        QVERIFY(state.setMeshVisible(1, false).ok);
        window.refreshWorkspace();
        QCOMPARE(notice->text(), QStringLiteral(
            "Showing 5 of 7 layers (2 hidden). Use Layers to change what is shown."));

        QVERIFY(state.setLayoutMode(SceneLayoutMode::Overlay).ok);
        window.refreshWorkspace();
        QVERIFY(!notice->isHidden());
        QVERIFY(state.setMeshVisible(1, true).ok);
        QVERIFY(state.setMeshVisible(7, true).ok);
        window.refreshWorkspace();
        QVERIFY(notice->isHidden());
        QVERIFY(notice->text().isEmpty());

        QVERIFY(state.setMeshVisible(7, false).ok);
        window.refreshWorkspace();
        QVERIFY(!notice->isHidden());
        state = WorkspaceState{};
        window.refreshWorkspace();
        QVERIFY(notice->isHidden());
        QVERIFY(notice->text().isEmpty());
    }

    void layerSubsetNoticeCoexistsWithDismissibleStatusNotices()
    {
        WorkspaceState state;
        QVERIFY(makeReady(state).ok);
        QVERIFY(state.setMeshVisible(2, false).ok);
        StandaloneMainWindow window(state);
        auto* layerNotice = window.findChild<QLabel*>("layerVisibilityNotice");
        auto* noticeArea = window.findChild<QWidget*>("noticeArea");
        auto* noticeLabel = window.findChild<QLabel*>("noticeLabel");
        auto* dismiss = window.findChild<QToolButton*>("dismissNoticeButton");
        QVERIFY(layerNotice != nullptr);
        QVERIFY(noticeArea != nullptr);
        QVERIFY(noticeLabel != nullptr);
        QVERIFY(dismiss != nullptr);
        const QString expected = QStringLiteral(
            "Showing 1 of 2 layers (1 hidden). Use Layers to change what is shown.");

        window.presentImportOutcome(
            {OperationResult::success(), {}, QStringLiteral("Camera pose warning")});
        QVERIFY(!layerNotice->isHidden());
        QCOMPARE(layerNotice->text(), expected);
        QVERIFY(!noticeArea->isHidden());
        QCOMPARE(noticeLabel->text(), QStringLiteral("Camera pose warning"));
        dismiss->click();
        QVERIFY(noticeArea->isHidden());
        QVERIFY(!layerNotice->isHidden());

        window.showStatusMessage(QStringLiteral("Analysis failed"));
        window.refreshWorkspace();
        QVERIFY(!noticeArea->isHidden());
        QCOMPARE(noticeLabel->text(), QStringLiteral("Analysis failed"));
        QCOMPARE(layerNotice->text(), expected);
        QVERIFY(!layerNotice->isHidden());

        window.presentImportOutcome({OperationResult::success(), {}, {}});
        QVERIFY(noticeArea->isHidden());
        QVERIFY(!layerNotice->isHidden());
        QCOMPARE(layerNotice->text(), expected);
    }

    void layersControlDoesNotReserveViewportWidth()
    {
        WorkspaceState state;
        state.beginLoading();
        QVERIFY(state
                    .commitWorkspace(
                        {entry(1), entry(2)},
                        1,
                        SceneLayoutMode::Overlay)
                    .ok);
        StandaloneMainWindow window(state);
        window.resize(980, 620);
        window.show();
        QApplication::processEvents();

        auto* viewportFrame =
            window.findChild<QFrame*>(QStringLiteral("viewportFrame"));
        QVERIFY(viewportFrame != nullptr);
        QVERIFY(window.findChild<QFrame*>(QStringLiteral("layersPanel")) == nullptr);
        QCOMPARE(viewportFrame->width(), window.centralWidget()->width());
    }

    void diagnosticsMenuIsAttachedOnlyToTheOverflowButton()
    {
        WorkspaceState state;
        StandaloneMainWindow window(state);
        QMenu diagnosticsMenu;

        window.setDiagnosticsMenu(&diagnosticsMenu);

        auto* overflow =
            window.findChild<QToolButton*>(QStringLiteral("diagnosticsButton"));
        QVERIFY(overflow != nullptr);
        QCOMPARE(overflow->menu(), &diagnosticsMenu);
        QCOMPARE(overflow->popupMode(), QToolButton::InstantPopup);
        QCOMPARE(window.menuBar()->actions().size(), 0);
    }

    void importIntoExistingWorkspaceEmitsOneOrderedBatchWithoutConfirmation()
    {
        WorkspaceState state;
        const OperationResult ready = makeReady(state);
        QVERIFY2(ready.ok, qPrintable(ready.error));
        StandaloneMainWindow window(state);
        QSignalSpy importSpy(&window, &StandaloneMainWindow::importRequested);
        const auto probe =
            clickNextMessageBoxRole(QMessageBox::RejectRole, &window);
        const QStringList paths = {
            QStringLiteral("/tmp/first.obj"), QStringLiteral("/tmp/second.obj")};

        window.requestImport(paths);
        QApplication::processEvents();

        QVERIFY(!probe->found);
        QCOMPARE(probe->dialogCount, 0);
        QCOMPARE(importSpy.size(), 1);
        QCOMPARE(importSpy.takeFirst().front().toStringList(), paths);
    }

    void emptyImportRequestDoesNotEmitAnImportOrShowConfirmation()
    {
        WorkspaceState state;
        const OperationResult ready = makeReady(state);
        QVERIFY2(ready.ok, qPrintable(ready.error));
        StandaloneMainWindow window(state);
        QSignalSpy importSpy(&window, &StandaloneMainWindow::importRequested);
        const auto probe =
            clickNextMessageBoxRole(QMessageBox::RejectRole, &window);

        window.requestImport({});
        QApplication::processEvents();

        QVERIFY(!probe->found);
        QCOMPARE(probe->dialogCount, 0);
        QCOMPARE(importSpy.size(), 0);
    }

    void allLocalDropEmitsOneOrderedBatchAndMixedDropIsRejected_data()
    {
        QTest::addColumn<bool>("populated");
        QTest::newRow("empty-workspace") << false;
        QTest::newRow("existing-workspace") << true;
    }

    void allLocalDropEmitsOneOrderedBatchAndMixedDropIsRejected()
    {
        QFETCH(bool, populated);
        WorkspaceState state;
        if (populated)
            QVERIFY(makeReady(state).ok);
        StandaloneMainWindow window(state);
        QSignalSpy importSpy(&window, &StandaloneMainWindow::importRequested);
        const auto probe =
            clickNextMessageBoxRole(QMessageBox::RejectRole, &window);
        QMimeData localMime;
        const QList<QUrl> localUrls = {
            QUrl::fromLocalFile(QStringLiteral("/tmp/first.obj")),
            QUrl::fromLocalFile(QStringLiteral("/tmp/second.obj"))};
        localMime.setUrls(localUrls);
        QDragEnterEvent localDrag(
            QPoint(5, 5),
            Qt::CopyAction,
            &localMime,
            Qt::LeftButton,
            Qt::NoModifier);

        QApplication::sendEvent(&window, &localDrag);

        QVERIFY(localDrag.isAccepted());
        QDropEvent localDrop(
            QPointF(5, 5),
            Qt::CopyAction,
            &localMime,
            Qt::LeftButton,
            Qt::NoModifier);
        QApplication::sendEvent(&window, &localDrop);
        QApplication::processEvents();
        QVERIFY(localDrop.isAccepted());
        QVERIFY(!probe->found);
        QCOMPARE(importSpy.size(), 1);
        QCOMPARE(importSpy.takeFirst().front().toStringList(),
                 QStringList({QStringLiteral("/tmp/first.obj"),
                              QStringLiteral("/tmp/second.obj")}));

        QMimeData mixedMime;
        mixedMime.setUrls(
            {QUrl::fromLocalFile(QStringLiteral("/tmp/local.obj")),
             QUrl(QStringLiteral("https://example.com/remote.obj"))});
        QDragEnterEvent mixedDrag(
            QPoint(5, 5),
            Qt::CopyAction,
            &mixedMime,
            Qt::LeftButton,
            Qt::NoModifier);
        QApplication::sendEvent(&window, &mixedDrag);
        QVERIFY(!mixedDrag.isAccepted());
        QCOMPARE(importSpy.size(), 0);
    }

    void fileErrorsUseOneGroupedDialog()
    {
        WorkspaceState state;
        StandaloneMainWindow window(state);
        const WorkspaceImportOutcome outcome{
            OperationResult::failure(QStringLiteral("Batch import failed")),
            {{QStringLiteral("/tmp/bad-a.obj"), QStringLiteral("unsupported format")},
             {QStringLiteral("/tmp/bad-b.obj"), QStringLiteral("file not found")}},
            {}};
        const auto probe =
            clickNextMessageBoxRole(QMessageBox::AcceptRole, &window);

        window.presentImportOutcome(outcome);

        QVERIFY(probe->found);
        QVERIFY(probe->requestedRoleFound);
        QCOMPARE(probe->dialogCount, 1);
        QVERIFY(probe->capturedText.contains(QStringLiteral("/tmp/bad-a.obj")));
        QVERIFY(probe->capturedText.contains(QStringLiteral("unsupported format")));
        QVERIFY(probe->capturedText.contains(QStringLiteral("/tmp/bad-b.obj")));
        QVERIFY(probe->capturedText.contains(QStringLiteral("file not found")));
    }

    void ordinaryFailureAndReferenceNoticeUseTheStatusArea()
    {
        WorkspaceState state;
        StandaloneMainWindow window(state);
        auto* status = window.findChild<QLabel*>(QStringLiteral("statusLabel"));
        QVERIFY(status != nullptr);

        window.presentImportOutcome(
            {OperationResult::failure(QStringLiteral("GPU upload failed")), {}, {}});
        QCOMPARE(status->text(), QStringLiteral("GPU upload failed"));

        window.presentImportOutcome(
            {OperationResult::success(),
             {},
             QStringLiteral("No gt mesh found; using the first import.")});
        QCOMPARE(status->text(),
                 QStringLiteral("No gt mesh found; using the first import."));
    }

    void statusNoticeCanBeDismissedAndReturnsForANewMessage()
    {
        WorkspaceState state;
        QVERIFY(makeReady(state).ok);
        StandaloneMainWindow window(state);
        auto* noticeArea =
            window.findChild<QWidget*>(QStringLiteral("noticeArea"));
        auto* noticeLabel =
            window.findChild<QLabel*>(QStringLiteral("noticeLabel"));
        auto* dismiss =
            window.findChild<QToolButton*>(
                QStringLiteral("dismissNoticeButton"));
        auto* status =
            window.findChild<QLabel*>(QStringLiteral("statusLabel"));
        QVERIFY(noticeArea != nullptr);
        QVERIFY(noticeLabel != nullptr);
        QVERIFY(dismiss != nullptr);
        QVERIFY(status != nullptr);
        QVERIFY(noticeArea->isHidden());

        window.presentImportOutcome(
            {OperationResult::success(),
             {},
             QStringLiteral("Import warning")});

        QVERIFY(!noticeArea->isHidden());
        QCOMPARE(noticeLabel->text(), QStringLiteral("Import warning"));
        dismiss->click();
        QVERIFY(noticeArea->isHidden());

        window.presentImportOutcome(
            {OperationResult::failure(QStringLiteral("Analysis failed")),
             {},
             {}});

        QVERIFY(!noticeArea->isHidden());
        QCOMPARE(noticeLabel->text(), QStringLiteral("Analysis failed"));

        window.presentImportOutcome(
            {OperationResult::success(), {}, {}});

        QVERIFY(noticeArea->isHidden());
        QCOMPARE(status->text(),
                 QStringLiteral("Import complete. Workspace contains 2 meshes."));
    }

    void successfulOutcomeRefreshesMeshCountAndWorkspaceActions()
    {
        WorkspaceState state;
        StandaloneMainWindow window(state);
        const OperationResult ready = makeReady(state);
        QVERIFY2(ready.ok, qPrintable(ready.error));

        window.presentImportOutcome({OperationResult::success(), {}, {}});

        auto* meshCount = window.findChild<QLabel*>(QStringLiteral("meshCountLabel"));
        auto* coloringButton = window.findChild<QPushButton*>(QStringLiteral("coloringButton"));
        auto* cameraButton = window.findChild<QPushButton*>(QStringLiteral("cameraButton"));
        QVERIFY(meshCount != nullptr);
        QCOMPARE(meshCount->text(), QStringLiteral("2 meshes"));
        QVERIFY(coloringButton->isEnabled());
        QVERIFY(cameraButton->isEnabled());
    }

    void singleMeshUsesSingularImportAndCountText()
    {
        WorkspaceState state;
        state.beginLoading();
        QVERIFY(state.commitWorkspace({entry(1)}, 1).ok);
        StandaloneMainWindow window(state);

        window.presentImportOutcome({OperationResult::success(), {}, {}});

        auto* meshCount =
            window.findChild<QLabel*>(QStringLiteral("meshCountLabel"));
        auto* status =
            window.findChild<QLabel*>(QStringLiteral("statusLabel"));
        QVERIFY(meshCount != nullptr);
        QVERIFY(status != nullptr);
        QCOMPARE(meshCount->text(), QStringLiteral("1 mesh"));
        QCOMPARE(status->text(),
                 QStringLiteral("Import complete. Workspace contains 1 mesh."));
    }

    void coloringPanelFloatsBelowTheButtonWithoutResizingTheViewportHost()
    {
        WorkspaceState state;
        QVERIFY(makeReady(state).ok);
        WindowColoringCommands commands(state);
        StandaloneMainWindow window(state);
        window.resize(960, 640);
        window.show();
        QApplication::processEvents();
        window.bindColoringCommands(commands);

        auto* button =
            window.findChild<QPushButton*>(QStringLiteral("coloringButton"));
        auto* host = window.findChild<QWidget*>(QStringLiteral("viewportHost"));
        auto* panel =
            window.findChild<ColoringPanel*>(QStringLiteral("coloringPanel"));
        QVERIFY(button != nullptr);
        QVERIFY(host != nullptr);
        QVERIFY(panel != nullptr);
        const QRect viewportGeometry = host->geometry();
        QVERIFY(panel->isHidden());

        QTest::mouseClick(button, Qt::LeftButton);
        QApplication::processEvents();

        QVERIFY(panel->isVisible());
        QCOMPARE(panel->parentWidget(), window.centralWidget());
        const QPoint buttonBottom = button->mapTo(
            window.centralWidget(), QPoint(0, button->height()));
        QCOMPARE(panel->y(), buttonBottom.y() + 4);
        QCOMPARE(host->geometry(), viewportGeometry);

        auto* tabs =
            panel->findChild<QTabBar*>(QStringLiteral("coloringModeTabs"));
        auto* advanced = panel->findChild<QToolButton*>(
            QStringLiteral("advancedParametersToggle"));
        QVERIFY(tabs != nullptr);
        QVERIFY(advanced != nullptr);
        const int collapsedHeight = panel->height();
        tabs->setCurrentIndex(1);
        QTest::mouseClick(advanced, Qt::LeftButton);
        QApplication::processEvents();

        QVERIFY(panel->height() > collapsedHeight);
        QVERIFY(panel->height() >= panel->sizeHint().height());
        QCOMPARE(panel->y(), buttonBottom.y() + 4);
        QCOMPARE(host->geometry(), viewportGeometry);

        QTest::mouseClick(button, Qt::LeftButton);
        QVERIFY(panel->isHidden());
        QCOMPARE(host->geometry(), viewportGeometry);
    }

    void comboPopupUsesReadableDarkTheme()
    {
        WorkspaceState state;
        QVERIFY(makeReady(state).ok);
        WindowColoringCommands commands(state);
        StandaloneMainWindow window(state);
        window.resize(960, 640);
        window.bindColoringCommands(commands);
        window.show();
        QApplication::processEvents();

        auto* coloringButton =
            window.findChild<QPushButton*>(QStringLiteral("coloringButton"));
        auto* referenceCombo =
            window.findChild<QComboBox*>(QStringLiteral("referenceCombo"));
        QVERIFY(coloringButton != nullptr);
        QVERIFY(referenceCombo != nullptr);
        QTest::mouseClick(coloringButton, Qt::LeftButton);
        referenceCombo->showPopup();
        QApplication::processEvents();
        QVERIFY(referenceCombo->view()->isVisible());

        const QPalette palette = referenceCombo->view()->palette();
        const QColor base = palette.color(QPalette::Base);
        const QColor text = palette.color(QPalette::Text);
        const QColor highlight = palette.color(QPalette::Highlight);
        const QColor highlightedText =
            palette.color(QPalette::HighlightedText);
        QVERIFY2(
            base.lightnessF() < 0.25,
            qPrintable(QStringLiteral("Popup base is too light: %1")
                           .arg(base.name(QColor::HexArgb))));
        QVERIFY2(
            contrastRatio(text, base) >= 4.5,
            qPrintable(QStringLiteral(
                "Popup text contrast is only %1:1")
                           .arg(contrastRatio(text, base), 0, 'f', 2)));
        QVERIFY2(
            contrastRatio(highlightedText, highlight) >= 4.5,
            qPrintable(QStringLiteral(
                "Selected popup text contrast is only %1:1")
                           .arg(
                               contrastRatio(highlightedText, highlight),
                               0,
                               'f',
                               2)));
        QVERIFY2(
            referenceCombo->view()->sizeHintForRow(0) >= 30,
            qPrintable(QStringLiteral("Popup row is only %1px high")
                           .arg(referenceCombo->view()->sizeHintForRow(0))));

        referenceCombo->hidePopup();
    }

    void cameraPanelIsTransientMutuallyExclusiveAndDoesNotResizeTheViewport()
    {
        WorkspaceState state;
        QVERIFY(makeReady(state).ok);
        WindowColoringCommands coloringCommands(state);
        WindowCameraCommands cameraCommands;
        StandaloneMainWindow window(state);
        window.resize(960, 640);
        window.show();
        QApplication::processEvents();
        window.bindColoringCommands(coloringCommands);
        window.bindCameraCommands(cameraCommands);

        auto* cameraButton =
            window.findChild<QPushButton*>(QStringLiteral("cameraButton"));
        auto* coloringButton =
            window.findChild<QPushButton*>(QStringLiteral("coloringButton"));
        auto* host = window.findChild<QWidget*>(QStringLiteral("viewportHost"));
        auto* cameraPanel =
            window.findChild<CameraPanel*>(QStringLiteral("cameraPanel"));
        auto* coloringPanel =
            window.findChild<ColoringPanel*>(QStringLiteral("coloringPanel"));
        QVERIFY(cameraButton != nullptr);
        QVERIFY(coloringButton != nullptr);
        QVERIFY(host != nullptr);
        QVERIFY(cameraPanel != nullptr);
        QVERIFY(coloringPanel != nullptr);
        const QRect viewportGeometry = host->geometry();

        QTest::mouseClick(cameraButton, Qt::LeftButton);
        QApplication::processEvents();

        QVERIFY(cameraPanel->isVisible());
        QVERIFY(coloringPanel->isHidden());
        QCOMPARE(cameraPanel->parentWidget(), window.centralWidget());
        const QPoint buttonBottom = cameraButton->mapTo(
            window.centralWidget(), QPoint(0, cameraButton->height()));
        QCOMPARE(cameraPanel->y(), buttonBottom.y() + 4);
        QVERIFY(cameraPanel->geometry().right() <= window.centralWidget()->width());
        QCOMPARE(host->geometry(), viewportGeometry);

        QTest::mouseClick(coloringButton, Qt::LeftButton);
        QApplication::processEvents();

        QVERIFY(cameraPanel->isHidden());
        QVERIFY(coloringPanel->isVisible());
        QCOMPARE(host->geometry(), viewportGeometry);

        window.unbindCameraCommands();
        QVERIFY(window.findChild<CameraPanel*>(QStringLiteral("cameraPanel")) == nullptr);
        QTest::mouseClick(cameraButton, Qt::LeftButton);
        QCOMPARE(host->geometry(), viewportGeometry);
    }

    void modelessPreviewKeepsCameraPanelUsableAndApplyIsNotAnError()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        WindowCameraCommands cameraCommands;
        cameraCommands.screenshotPath = directory.filePath(QStringLiteral("view.png"));
        QImage image(20, 12, QImage::Format_RGB32);
        image.fill(Qt::green);
        QVERIFY(image.save(cameraCommands.screenshotPath));
        WorkspaceState state;
        QVERIFY(makeReady(state).ok);
        StandaloneMainWindow window(state);
        window.resize(1200, 900);
        window.bindCameraCommands(cameraCommands);
        window.show();
        QApplication::processEvents();
        QSignalSpy failures(&window, &StandaloneMainWindow::operationFailed);
        auto* cameraButton = window.findChild<QPushButton*>(QStringLiteral("cameraButton"));
        auto* panel = window.findChild<CameraPanel*>(QStringLiteral("cameraPanel"));
        QVERIFY(cameraButton != nullptr);
        QVERIFY(panel != nullptr);
        QTest::mouseClick(cameraButton, Qt::LeftButton);
        auto* views = panel->findChild<QListWidget*>(QStringLiteral("cameraPoseList"));
        auto* browse = panel->findChild<QPushButton*>(QStringLiteral("browseCameraScreenshotsButton"));
        auto* apply = panel->findChild<QPushButton*>(QStringLiteral("applyCameraPoseButton"));
        QVERIFY(views != nullptr && browse != nullptr && apply != nullptr);
        views->setCurrentRow(0);
        QTest::mouseClick(browse, Qt::LeftButton);
        auto* browser = panel->findChild<QWidget*>(QStringLiteral("screenshotBrowser"));
        auto* note = panel->findChild<QPlainTextEdit*>(QStringLiteral("screenshotPreviewNote"));
        QVERIFY(browser != nullptr && note != nullptr);
        browser->move(panel->mapToGlobal(QPoint(panel->width() + 20, 0)));
        QApplication::processEvents();
        QVERIFY(QApplication::activeModalWidget() == nullptr);
        QTest::mouseClick(note->viewport(), Qt::LeftButton);
        QVERIFY(panel->isVisible());
        QVERIFY(browser->isVisible());
        QTest::mouseClick(apply, Qt::LeftButton);
        QCOMPARE(cameraCommands.applyCount, 1);
        QCOMPARE(failures.count(), 0);
        QVERIFY(panel->isVisible());
    }

    void transientPanelsDismissWhenClickingOutside()
    {
        WorkspaceState state;
        QVERIFY(makeReady(state).ok);
        WindowColoringCommands coloringCommands(state);
        WindowCameraCommands cameraCommands;
        StandaloneMainWindow window(state);
        window.resize(960, 640);
        window.bindColoringCommands(coloringCommands);
        window.bindCameraCommands(cameraCommands);
        window.show();
        QApplication::processEvents();

        auto* coloringButton =
            window.findChild<QPushButton*>(QStringLiteral("coloringButton"));
        auto* cameraButton =
            window.findChild<QPushButton*>(QStringLiteral("cameraButton"));
        auto* host =
            window.findChild<QWidget*>(QStringLiteral("viewportHost"));
        auto* coloringPanel =
            window.findChild<ColoringPanel*>(QStringLiteral("coloringPanel"));
        auto* cameraPanel =
            window.findChild<CameraPanel*>(QStringLiteral("cameraPanel"));
        QVERIFY(coloringButton != nullptr);
        QVERIFY(cameraButton != nullptr);
        QVERIFY(host != nullptr);
        QVERIFY(coloringPanel != nullptr);
        QVERIFY(cameraPanel != nullptr);

        QTest::mouseClick(coloringButton, Qt::LeftButton);
        QVERIFY(coloringPanel->isVisible());
        QTest::mouseClick(host, Qt::LeftButton, Qt::NoModifier, QPoint(20, 20));
        QApplication::processEvents();
        QVERIFY(coloringPanel->isHidden());

        QTest::mouseClick(cameraButton, Qt::LeftButton);
        QVERIFY(cameraPanel->isVisible());
        QEvent applicationDeactivate(QEvent::ApplicationDeactivate);
        QApplication::sendEvent(qApp, &applicationDeactivate);
        QVERIFY(cameraPanel->isHidden());
    }

    void comboPopupOutsideClickDismissesTheContainingPanel()
    {
        WorkspaceState state;
        QVERIFY(makeReady(state).ok);
        WindowColoringCommands coloringCommands(state);
        StandaloneMainWindow window(state);
        window.resize(960, 640);
        window.bindColoringCommands(coloringCommands);
        window.show();
        QApplication::processEvents();

        auto* coloringButton =
            window.findChild<QPushButton*>(QStringLiteral("coloringButton"));
        auto* host =
            window.findChild<QWidget*>(QStringLiteral("viewportHost"));
        auto* coloringPanel =
            window.findChild<ColoringPanel*>(QStringLiteral("coloringPanel"));
        auto* meshCombo =
            window.findChild<QComboBox*>(QStringLiteral("uniformMeshCombo"));
        QVERIFY(coloringButton != nullptr);
        QVERIFY(host != nullptr);
        QVERIFY(coloringPanel != nullptr);
        QVERIFY(meshCombo != nullptr);

        QTest::mouseClick(coloringButton, Qt::LeftButton);
        QVERIFY(coloringPanel->isVisible());
        meshCombo->showPopup();
        QApplication::processEvents();
        QWidget* popup = QApplication::activePopupWidget();
        QVERIFY(popup != nullptr);

        const QPoint globalOutside = host->mapToGlobal(QPoint(20, 20));
        const QPoint popupLocal = popup->mapFromGlobal(globalOutside);
        QMouseEvent outsidePress(
            QEvent::MouseButtonPress,
            popupLocal,
            globalOutside,
            Qt::LeftButton,
            Qt::LeftButton,
            Qt::NoModifier);
        QVERIFY(QApplication::sendEvent(popup, &outsidePress));
        QApplication::processEvents();

        QVERIFY(coloringPanel->isHidden());
    }

    void panelFailuresArePublishedForDiagnostics()
    {
        WorkspaceState state;
        QVERIFY(makeReady(state).ok);
        WindowColoringCommands coloringCommands(state);
        WindowCameraCommands cameraCommands;
        StandaloneMainWindow window(state);
        window.bindColoringCommands(coloringCommands);
        window.bindCameraCommands(cameraCommands);
        QSignalSpy failures(
            &window,
            &StandaloneMainWindow::operationFailed);

        auto* coloringPanel =
            window.findChild<ColoringPanel*>(QStringLiteral("coloringPanel"));
        auto* cameraPanel =
            window.findChild<CameraPanel*>(QStringLiteral("cameraPanel"));
        QVERIFY(coloringPanel != nullptr);
        QVERIFY(cameraPanel != nullptr);
        QVERIFY(QMetaObject::invokeMethod(
            coloringPanel,
            "statusMessage",
            Qt::DirectConnection,
            Q_ARG(QString, QStringLiteral("Coloring failed"))));
        QVERIFY(QMetaObject::invokeMethod(
            cameraPanel,
            "statusMessage",
            Qt::DirectConnection,
            Q_ARG(QString, QStringLiteral("Camera delete failed"))));

        QCOMPARE(failures.size(), 2);
        QCOMPARE(
            failures.at(0).at(0).toString(),
            QStringLiteral("Coloring failed"));
        QCOMPARE(
            failures.at(1).at(0).toString(),
            QStringLiteral("Camera delete failed"));
    }

    void analysisEventsAreForwardedIntoTheBoundColoringPanel()
    {
        WorkspaceState state;
        QVERIFY(makeReady(state).ok);
        WindowColoringCommands commands(state);
        StandaloneMainWindow window(state);
        window.bindColoringCommands(commands);
        auto* progress =
            window.findChild<QLabel*>(QStringLiteral("analysisStatusLabel"));
        QVERIFY(progress != nullptr);
        QVERIFY(state.beginAnalysis().ok);

        window.presentAnalysisProgress(
            state.generation(),
            17,
            2,
            61,
            QStringLiteral("Sampling..."));

        QVERIFY(progress->text().contains(QStringLiteral("mesh-2.obj")));
        QVERIFY(progress->text().contains(QStringLiteral("61%")));
        state.finishAnalysis();
        AnalysisBatchResult result;
        result.result = OperationResult::success();
        result.generation = state.generation();
        result.batchSerial = 17;
        window.presentAnalysisFinished(result);
        QCOMPARE(progress->text(), QStringLiteral("Analysis complete."));
    }
};

QTEST_MAIN(StandaloneMainWindowTest)
#include "standalone_main_window_test.moc"
