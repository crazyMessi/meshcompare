#include <QtTest>

#include <QComboBox>
#include <QCoreApplication>
#include <QImage>
#include <QImageReader>
#include <QLibraryInfo>
#include <QListWidget>
#include <QLabel>
#include <QTemporaryDir>
#include <QPixmap>
#include <QWidget>

#include "app/app_theme.h"
#include "app/camera_commands.h"
#include "app/screenshot_browser.h"

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
