#include <QtTest>

#include <QComboBox>
#include <QCoreApplication>
#include <QFile>
#include <QImage>
#include <QImageReader>
#include <QLibraryInfo>
#include <QListWidget>
#include <QLineEdit>
#include <QLabel>
#include <QTemporaryDir>
#include <QTextCursor>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QWidget>

#include "app/app_theme.h"
#include "app/camera_commands.h"
#include "app/screenshot_browser.h"
#include "app/tag_editor.h"

namespace
{
CameraPoseSummary pose(
    const QString& viewId,
    const QStringList& tags,
    const QString& screenshotPath)
{
    CameraPoseSummary result;
    result.viewId = viewId;
    result.tags = tags;
    result.screenshotPath = screenshotPath;
    return result;
}

QString saveScreenshot(const QTemporaryDir& directory, const QString& name)
{
    const QString path = directory.filePath(name);
    QImage image(24, 16, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::blue);
    return image.save(path, "PNG") ? path : QString();
}

QStringList comboItems(const QComboBox& combo)
{
    QStringList items;
    for (int index = 0; index < combo.count(); ++index)
        items.append(combo.itemText(index));
    return items;
}
}

class ScreenshotBrowserTest : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase()
    {
        QCoreApplication::addLibraryPath(
            QLibraryInfo::location(QLibraryInfo::PluginsPath));
    }

    void filtersReadableScreenshotsByTag()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString holeScreenshot =
            saveScreenshot(directory, QStringLiteral("hole.png"));
        const QString edgeScreenshot =
            saveScreenshot(directory, QStringLiteral("edge.png"));
        QVERIFY(!holeScreenshot.isEmpty());
        QVERIFY(!edgeScreenshot.isEmpty());
        QVERIFY(QImageReader(holeScreenshot).canRead());
        QVERIFY(QImageReader(edgeScreenshot).canRead());

        ScreenshotBrowser browser;
        browser.setPoses({
            pose(QStringLiteral("view_hole"), {QStringLiteral("hole")}, holeScreenshot),
            pose(
                QStringLiteral("view_edge"),
                {QStringLiteral("edge"), QStringLiteral("hole")},
                edgeScreenshot),
            pose(
                QStringLiteral("view_missing"),
                {QStringLiteral("hole")},
                directory.filePath(QStringLiteral("missing.png"))),
            pose(QStringLiteral("view_legacy"), {QStringLiteral("legacy")}, {})});

        auto* filter = browser.findChild<QComboBox*>(
            QStringLiteral("screenshotTagFilter"));
        auto* gallery = browser.findChild<QListWidget*>(
            QStringLiteral("screenshotGallery"));
        QVERIFY(filter != nullptr);
        QVERIFY(gallery != nullptr);
        QCOMPARE(gallery->count(), 2);
        const QStringList expectedTags = {
            QStringLiteral("All tags"),
            QStringLiteral("edge"),
            QStringLiteral("hole")};
        QCOMPARE(comboItems(*filter), expectedTags);

        filter->setCurrentText(QStringLiteral("edge"));

        QCOMPARE(gallery->count(), 1);
        QCOMPARE(
            gallery->item(0)->data(Qt::UserRole).toString(),
            QStringLiteral("view_edge"));

        filter->setCurrentText(QStringLiteral("hole"));

        QCOMPARE(gallery->count(), 2);
    }

    void usesTheApplicationDarkTheme()
    {
        QWidget applicationRoot;
        applicationRoot.setObjectName(QStringLiteral("workspaceRoot"));
        applicationRoot.setStyleSheet(meshCompareApplicationStyleSheet());

        ScreenshotBrowser browser(&applicationRoot);
        browser.ensurePolished();

        const QColor background =
            browser.palette().color(QPalette::Window);
        const QColor text =
            browser.palette().color(QPalette::WindowText);
        QVERIFY2(
            background.lightness() < 80,
            qPrintable(QStringLiteral("Unexpected light background: %1")
                           .arg(background.name())));
        QVERIFY2(
            text.lightness() > 160,
            qPrintable(QStringLiteral("Unexpected dark text: %1")
                           .arg(text.name())));
    }

    void selectsAScreenshotForPreview()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString screenshot =
            saveScreenshot(directory, QStringLiteral("preview.png"));
        QVERIFY(!screenshot.isEmpty());

        ScreenshotBrowser browser;
        browser.setPoses({
            pose(QStringLiteral("view_preview"), {QStringLiteral("hole")}, screenshot)});

        auto* gallery = browser.findChild<QListWidget*>(
            QStringLiteral("screenshotGallery"));
        auto* preview = browser.findChild<QLabel*>(
            QStringLiteral("screenshotPreview"));
        auto* details = browser.findChild<QLabel*>(
            QStringLiteral("screenshotPreviewDetails"));
        QVERIFY(gallery != nullptr);
        QVERIFY(preview != nullptr);
        QVERIFY(details != nullptr);
        QCOMPARE(gallery->currentRow(), 0);
        const QPixmap previewPixmap = preview->pixmap(Qt::ReturnByValue);
        QVERIFY(!previewPixmap.isNull());
        QVERIFY(details->text().contains(QStringLiteral("view_preview")));
    }

    void showsSelectedViewNoteAsPlainText()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString screenshot =
            saveScreenshot(directory, QStringLiteral("note.png"));
        QVERIFY(!screenshot.isEmpty());

        CameraPoseSummary annotated =
            pose(QStringLiteral("view_annotated"), {}, screenshot);
        annotated.note = QStringLiteral(
            "Inspect the upper edge.\n<b>Keep this literal</b> & compare.");
        CameraPoseSummary second =
            pose(QStringLiteral("view_second"), {}, screenshot);
        second.note = QStringLiteral("Another angle");

        ScreenshotBrowser browser;
        browser.setPoses({
            annotated,
            second,
            pose(QStringLiteral("view_without_note"), {}, screenshot)});

        auto* gallery = browser.findChild<QListWidget*>(
            QStringLiteral("screenshotGallery"));
        auto* note = browser.findChild<QPlainTextEdit*>(
            QStringLiteral("screenshotPreviewNote"));
        auto* noteLabel = browser.findChild<QLabel*>(
            QStringLiteral("screenshotPreviewNoteLabel"));
        QVERIFY(gallery != nullptr);
        QVERIFY(note != nullptr);
        QVERIFY(noteLabel != nullptr);
        QVERIFY(note->isReadOnly());
        QCOMPARE(note->toPlainText(), annotated.note);
        QVERIFY(!note->isHidden());
        QVERIFY(!noteLabel->isHidden());

        gallery->setCurrentRow(1);
        QCOMPARE(note->toPlainText(), second.note);

        gallery->setCurrentRow(2);
        QVERIFY(note->toPlainText().isEmpty());
        QVERIFY(!note->isHidden());
        QVERIFY(!noteLabel->isHidden());
        QVERIFY(!note->placeholderText().isEmpty());

        gallery->setCurrentRow(0);
        QCOMPARE(note->toPlainText(), annotated.note);
        browser.setPoses({});
        QVERIFY(note->toPlainText().isEmpty());
        QVERIFY(note->isHidden());
        QVERIFY(noteLabel->isHidden());
    }

    void savesTagsAndUnicodeNotesTogetherFromAnInitiallyEmptyPreview()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString screenshot = saveScreenshot(directory, QStringLiteral("note.png"));
        QVERIFY(!screenshot.isEmpty());
        CameraPoseSummary entry = pose(QStringLiteral("view_001"), {}, screenshot);
        entry.workspaceUuid = QStringLiteral("workspace-1");

        ScreenshotBrowser browser;
        browser.setPoses({entry});
        QString savedWorkspace;
        QString savedView;
        QString savedNote;
        QStringList savedTags;
        browser.setDetailsSaveHandler([&](const QString& workspace, const QString& view,
                                      const QStringList& tags, const QString& note) {
            savedTags = tags;
            savedWorkspace = workspace;
            savedView = view;
            savedNote = note;
            return OperationResult::success();
        });
        QSignalSpy saved(&browser, &ScreenshotBrowser::detailsSaved);
        auto* editor = browser.findChild<QPlainTextEdit*>(QStringLiteral("screenshotPreviewNote"));
        auto* save = browser.findChild<QPushButton*>(QStringLiteral("screenshotSaveChanges"));
        auto* status = browser.findChild<QLabel*>(QStringLiteral("screenshotDetailsStatus"));
        auto* tags = browser.findChild<TagEditor*>(QStringLiteral("screenshotPreviewTags"));
        QVERIFY(editor != nullptr);
        QVERIFY(save != nullptr);
        QVERIFY(status != nullptr);
        QVERIFY(tags != nullptr);
        auto* tagInput = tags->findChild<QLineEdit*>(QStringLiteral("tagEditorInput"));
        QVERIFY(tagInput != nullptr);
        QVERIFY(!editor->isReadOnly());
        QVERIFY(!editor->isHidden());
        QVERIFY(!save->isEnabled());

        const QString note = QStringLiteral("检查交点聚集\n<b>Keep literal</b> & compare.");
        tagInput->setText(QStringLiteral("hole,needs review"));
        QVERIFY(save->isEnabled());
        editor->setPlainText(note);
        QVERIFY(save->isEnabled());
        QCOMPARE(saved.count(), 0);
        QCOMPARE(status->text(), QStringLiteral("Unsaved changes"));
        save->click();
        QCOMPARE(savedWorkspace, entry.workspaceUuid);
        QCOMPARE(savedView, entry.viewId);
        QCOMPARE(savedNote, note);
        const QStringList expectedTags = {QStringLiteral("hole"), QStringLiteral("needs review")};
        QCOMPARE(savedTags, expectedTags);
        QCOMPARE(saved.count(), 1);
        QCOMPARE(saved.at(0).at(2).toStringList(), expectedTags);
        QCOMPARE(saved.at(0).at(3).toString(), note);
        QVERIFY(!save->isEnabled());
        QCOMPARE(status->text(), QStringLiteral("Saved"));

        entry.note = note;
        entry.tags = expectedTags;
        QTextCursor cursor = editor->textCursor();
        cursor.setPosition(2);
        cursor.setPosition(5, QTextCursor::KeepAnchor);
        editor->setTextCursor(cursor);
        tagInput->setCursorPosition(4);
        browser.setPoses({entry});
        QCOMPARE(editor->toPlainText(), note);
        QCOMPARE(tags->tags(), expectedTags);
        QCOMPARE(editor->textCursor().position(), 5);
        QCOMPARE(editor->textCursor().anchor(), 2);
        QCOMPARE(tagInput->cursorPosition(), 4);
        QVERIFY(!save->isEnabled());
    }

    void retainsFailedFormDraftsAndClearsTagsAndNotesTogether()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString screenshot = saveScreenshot(directory, QStringLiteral("note.png"));
        QVERIFY(!screenshot.isEmpty());
        CameraPoseSummary entry = pose(QStringLiteral("view_001"), {}, screenshot);
        entry.workspaceUuid = QStringLiteral("workspace-1");
        entry.note = QStringLiteral("Saved note");

        ScreenshotBrowser browser;
        browser.setPoses({entry});
        bool fail = true;
        QString savedNote;
        QStringList savedTags;
        browser.setDetailsSaveHandler([&](const QString&, const QString&, const QStringList& tags, const QString& note) {
            savedTags = tags;
            savedNote = note;
            return fail ? OperationResult::failure(QStringLiteral("Disk unavailable"))
                        : OperationResult::success();
        });
        QSignalSpy saved(&browser, &ScreenshotBrowser::detailsSaved);
        auto* editor = browser.findChild<QPlainTextEdit*>(QStringLiteral("screenshotPreviewNote"));
        auto* save = browser.findChild<QPushButton*>(QStringLiteral("screenshotSaveChanges"));
        auto* status = browser.findChild<QLabel*>(QStringLiteral("screenshotDetailsStatus"));
        auto* tags = browser.findChild<TagEditor*>(QStringLiteral("screenshotPreviewTags"));
        QVERIFY(editor != nullptr);
        QVERIFY(save != nullptr);
        QVERIFY(status != nullptr);
        QVERIFY(tags != nullptr);
        auto* tagInput = tags->findChild<QLineEdit*>(QStringLiteral("tagEditorInput"));
        QVERIFY(tagInput != nullptr);
        tagInput->setText(QStringLiteral("draft tag"));
        editor->setPlainText(QStringLiteral("Draft note"));
        save->click();
        QCOMPARE(saved.count(), 0);
        QCOMPARE(editor->toPlainText(), QStringLiteral("Draft note"));
        QVERIFY(save->isEnabled());
        QVERIFY(status->text().contains(QStringLiteral("Disk unavailable")));
        browser.setPoses({entry});
        QCOMPARE(editor->toPlainText(), QStringLiteral("Draft note"));
        QCOMPARE(tags->tags(), QStringList({QStringLiteral("draft tag")}));

        fail = false;
        save->click();
        QCOMPARE(saved.count(), 1);
        QCOMPARE(savedNote, QStringLiteral("Draft note"));
        QCOMPARE(savedTags, QStringList({QStringLiteral("draft tag")}));
        tagInput->clear();
        editor->clear();
        QVERIFY(save->isEnabled());
        save->click();
        QCOMPARE(saved.count(), 2);
        QVERIFY(savedNote.isEmpty());
        QVERIFY(savedTags.isEmpty());
        QVERIFY(editor->toPlainText().isEmpty());
        QVERIFY(!editor->isHidden());
        QVERIFY(!save->isEnabled());
    }

    void preservesWorkspaceScopedDraftsAndSelectionAcrossRefreshAndFiltering()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString screenshot = saveScreenshot(directory, QStringLiteral("note.png"));
        QVERIFY(!screenshot.isEmpty());
        CameraPoseSummary first = pose(QStringLiteral("view_001"), {QStringLiteral("first")}, screenshot);
        first.workspaceUuid = QStringLiteral("same-prefix-workspace-1");
        first.note = QStringLiteral("First saved");
        CameraPoseSummary second = pose(QStringLiteral("view_001"), {QStringLiteral("second")}, screenshot);
        second.workspaceUuid = QStringLiteral("same-prefix-workspace-2");
        second.note = QStringLiteral("Second saved");

        ScreenshotBrowser browser;
        QString savedWorkspace;
        browser.setDetailsSaveHandler([&](const QString& workspace, const QString&, const QStringList&, const QString&) {
            savedWorkspace = workspace;
            return OperationResult::success();
        });
        browser.setPoses({first, second});
        auto* editor = browser.findChild<QPlainTextEdit*>(QStringLiteral("screenshotPreviewNote"));
        auto* save = browser.findChild<QPushButton*>(QStringLiteral("screenshotSaveChanges"));
        auto* gallery = browser.findChild<QListWidget*>(QStringLiteral("screenshotGallery"));
        auto* filter = browser.findChild<QComboBox*>(QStringLiteral("screenshotTagFilter"));
        auto* tags = browser.findChild<TagEditor*>(QStringLiteral("screenshotPreviewTags"));
        QVERIFY(editor != nullptr);
        QVERIFY(save != nullptr);
        QVERIFY(gallery != nullptr);
        QVERIFY(filter != nullptr);
        QVERIFY(tags != nullptr);
        auto* tagInput = tags->findChild<QLineEdit*>(QStringLiteral("tagEditorInput"));
        QVERIFY(tagInput != nullptr);
        tagInput->setText(QStringLiteral("first draft tag"));
        editor->setPlainText(QStringLiteral("First draft"));
        gallery->setCurrentRow(1);
        QCOMPARE(editor->toPlainText(), second.note);
        tagInput->setText(QStringLiteral("second draft tag"));
        editor->setPlainText(QStringLiteral("Second draft"));

        first.note = QStringLiteral("First externally updated");
        browser.setPoses({second, first});
        QCOMPARE(gallery->currentRow(), 0);
        QCOMPARE(editor->toPlainText(), QStringLiteral("Second draft"));
        QVERIFY(tags->tags().contains(QStringLiteral("second draft tag")));
        filter->setCurrentText(QStringLiteral("first"));
        QCOMPARE(editor->toPlainText(), QStringLiteral("First draft"));
        QVERIFY(tags->tags().contains(QStringLiteral("first draft tag")));
        filter->setCurrentText(QStringLiteral("All tags"));
        QCOMPARE(gallery->currentRow(), 1);
        QCOMPARE(editor->toPlainText(), QStringLiteral("First draft"));
        save->click();
        QCOMPARE(savedWorkspace, first.workspaceUuid);
        gallery->setCurrentRow(0);
        QCOMPARE(editor->toPlainText(), QStringLiteral("Second draft"));
        QVERIFY(tags->tags().contains(QStringLiteral("second draft tag")));
        QVERIFY(save->isEnabled());

        browser.setPoses({first, second});
        QCOMPARE(gallery->currentRow(), 1);
        QCOMPARE(editor->toPlainText(), QStringLiteral("Second draft"));
    }

    void keepsRefreshedScreenshotPathsWhenSavingDetails()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString original = saveScreenshot(directory, QStringLiteral("original.png"));
        const QString moved = saveScreenshot(directory, QStringLiteral("moved.png"));
        QVERIFY(!original.isEmpty());
        QVERIFY(!moved.isEmpty());
        CameraPoseSummary entry = pose(QStringLiteral("view_001"), {}, original);
        entry.workspaceUuid = QStringLiteral("workspace-1");
        ScreenshotBrowser browser;
        browser.setPoses({entry});
        browser.setDetailsSaveHandler([&](const QString&, const QString&, const QStringList& tags,
                                         const QString& note) {
            entry.tags = tags;
            entry.note = note;
            entry.screenshotPath = moved;
            browser.setPoses({entry});
            return OperationResult::success();
        });
        connect(&browser, &ScreenshotBrowser::detailsSaved, &browser,
                [&] { browser.setPoses({entry}); });
        auto* editor = browser.findChild<QPlainTextEdit*>(QStringLiteral("screenshotPreviewNote"));
        auto* save = browser.findChild<QPushButton*>(QStringLiteral("screenshotSaveChanges"));
        auto* gallery = browser.findChild<QListWidget*>(QStringLiteral("screenshotGallery"));
        QVERIFY(editor != nullptr);
        QVERIFY(save != nullptr);
        QVERIFY(gallery != nullptr);
        editor->setPlainText(QStringLiteral("Saved with moved screenshot"));
        save->click();
        QCOMPARE(gallery->count(), 1);
        QCOMPARE(gallery->currentItem()->data(Qt::UserRole + 1).toString(), moved);
        QCOMPARE(editor->toPlainText(), entry.note);
        QVERIFY(!save->isEnabled());
    }

    void removesDraftsOnlyWhenTheViewIsRemoved()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString screenshot = saveScreenshot(directory, QStringLiteral("view.png"));
        QVERIFY(!screenshot.isEmpty());
        CameraPoseSummary entry = pose(QStringLiteral("view_001"), {QStringLiteral("original")}, screenshot);
        entry.workspaceUuid = QStringLiteral("workspace-1");
        entry.note = QStringLiteral("Original capture");
        ScreenshotBrowser browser;
        browser.setDetailsSaveHandler([](const QString&, const QString&, const QStringList&, const QString&) {
            return OperationResult::success();
        });
        browser.setPoses({entry});
        auto* editor = browser.findChild<QPlainTextEdit*>(QStringLiteral("screenshotPreviewNote"));
        auto* tags = browser.findChild<TagEditor*>(QStringLiteral("screenshotPreviewTags"));
        auto* save = browser.findChild<QPushButton*>(QStringLiteral("screenshotSaveChanges"));
        QVERIFY(editor != nullptr);
        QVERIFY(tags != nullptr);
        QVERIFY(save != nullptr);
        auto* tagInput = tags->findChild<QLineEdit*>(QStringLiteral("tagEditorInput"));
        QVERIFY(tagInput != nullptr);
        tagInput->setText(QStringLiteral("unsaved tag"));
        editor->setPlainText(QStringLiteral("Unsaved note"));

        CameraPoseSummary temporarilyUnreadable = entry;
        temporarilyUnreadable.screenshotPath = directory.filePath(QStringLiteral("missing.png"));
        browser.setPoses({temporarilyUnreadable});
        browser.setPoses({entry});
        QCOMPARE(editor->toPlainText(), QStringLiteral("Unsaved note"));
        QVERIFY(tags->tags().contains(QStringLiteral("unsaved tag")));
        QVERIFY(save->isEnabled());

        browser.setPoses({});
        entry.note = QStringLiteral("New capture with reused ID");
        entry.tags = QStringList{QStringLiteral("new capture")};
        browser.setPoses({entry});
        QCOMPARE(editor->toPlainText(), entry.note);
        QCOMPARE(tags->tags(), entry.tags);
        QVERIFY(!save->isEnabled());
    }

    void retainsSelectedViewWhenSaveMovesScreenshotBeforeTheListenerRefreshes()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString firstScreenshot = saveScreenshot(directory, QStringLiteral("first.png"));
        const QString original = saveScreenshot(directory, QStringLiteral("original.png"));
        const QString moved = directory.filePath(QStringLiteral("retagged.png"));
        QVERIFY(!firstScreenshot.isEmpty());
        QVERIFY(!original.isEmpty());
        CameraPoseSummary first = pose(QStringLiteral("view_001"), {}, firstScreenshot);
        first.workspaceUuid = QStringLiteral("workspace-1");
        CameraPoseSummary selected = pose(QStringLiteral("view_002"), {}, original);
        selected.workspaceUuid = first.workspaceUuid;
        ScreenshotBrowser browser;
        browser.setPoses({first, selected});
        bool movedSuccessfully = false;
        browser.setDetailsSaveHandler([&](const QString&, const QString&, const QStringList& tags,
                                         const QString& note) {
            selected.tags = tags;
            selected.note = note;
            movedSuccessfully = QFile::rename(original, moved);
            selected.screenshotPath = moved;
            return movedSuccessfully ? OperationResult::success()
                                     : OperationResult::failure(QStringLiteral("Move failed"));
        });
        connect(&browser, &ScreenshotBrowser::detailsSaved, &browser,
                [&] { browser.setPoses({first, selected}); });
        auto* editor = browser.findChild<QPlainTextEdit*>(QStringLiteral("screenshotPreviewNote"));
        auto* tags = browser.findChild<TagEditor*>(QStringLiteral("screenshotPreviewTags"));
        auto* save = browser.findChild<QPushButton*>(QStringLiteral("screenshotSaveChanges"));
        auto* gallery = browser.findChild<QListWidget*>(QStringLiteral("screenshotGallery"));
        QVERIFY(editor != nullptr);
        QVERIFY(tags != nullptr);
        QVERIFY(save != nullptr);
        QVERIFY(gallery != nullptr);
        auto* tagInput = tags->findChild<QLineEdit*>(QStringLiteral("tagEditorInput"));
        QVERIFY(tagInput != nullptr);
        gallery->setCurrentRow(1);
        tagInput->setText(QStringLiteral("retagged"));
        editor->setPlainText(QStringLiteral("Keep the selected view"));
        save->click();
        QVERIFY(movedSuccessfully);
        QVERIFY(!QFile::exists(original));
        QCOMPARE(gallery->count(), 2);
        QCOMPARE(gallery->currentRow(), 1);
        QCOMPARE(gallery->currentItem()->data(Qt::UserRole).toString(), selected.viewId);
        QCOMPARE(gallery->currentItem()->data(Qt::UserRole + 1).toString(), moved);
        QCOMPARE(editor->toPlainText(), selected.note);
        QVERIFY(!save->isEnabled());
    }

    void leavesOtherApplicationWindowsInteractive()
    {
        ScreenshotBrowser browser;
        QCOMPARE(browser.windowModality(), Qt::NonModal);
        QVERIFY(!browser.isModal());
    }

    void filtersTagsThatMatchReservedLookingText()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString allLookingScreenshot =
            saveScreenshot(directory, QStringLiteral("all.png"));
        const QString untaggedLookingScreenshot =
            saveScreenshot(directory, QStringLiteral("untagged.png"));
        QVERIFY(!allLookingScreenshot.isEmpty());
        QVERIFY(!untaggedLookingScreenshot.isEmpty());

        ScreenshotBrowser browser;
        browser.setPoses({
            pose(
                QStringLiteral("view_all_looking"),
                {QStringLiteral("__all_tags__")},
                allLookingScreenshot),
            pose(
                QStringLiteral("view_untagged_looking"),
                {QStringLiteral("__untagged__")},
                untaggedLookingScreenshot)});

        auto* filter = browser.findChild<QComboBox*>(
            QStringLiteral("screenshotTagFilter"));
        auto* gallery = browser.findChild<QListWidget*>(
            QStringLiteral("screenshotGallery"));
        QVERIFY(filter != nullptr);
        QVERIFY(gallery != nullptr);

        filter->setCurrentText(QStringLiteral("__all_tags__"));

        QCOMPARE(gallery->count(), 1);
        QCOMPARE(
            gallery->item(0)->data(Qt::UserRole).toString(),
            QStringLiteral("view_all_looking"));

        filter->setCurrentText(QStringLiteral("__untagged__"));

        QCOMPARE(gallery->count(), 1);
        QCOMPARE(
            gallery->item(0)->data(Qt::UserRole).toString(),
            QStringLiteral("view_untagged_looking"));
    }
};

QTEST_MAIN(ScreenshotBrowserTest)

#include "screenshot_browser_test.moc"
