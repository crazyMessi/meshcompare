#include <QtTest>

#include <QCompleter>
#include <QLineEdit>
#include <QScrollArea>
#include <QToolButton>

#include "app/tag_editor.h"

class TagEditorTest final : public QObject
{
    Q_OBJECT

private slots:
    void commaAndEnterCreateRemovableTagChips()
    {
        TagEditor editor;
        editor.show();
        QWidget* input = editor.focusProxy();
        QVERIFY(input != nullptr);
        input->setFocus();

        QTest::keyClicks(input, QStringLiteral("hole"));
        QTest::keyClick(input, Qt::Key_Comma);
        QTest::keyClicks(input, QStringLiteral("ext"));
        QTest::keyClick(input, Qt::Key_Return);

        QCOMPARE(
            editor.tags(),
            QStringList({QStringLiteral("hole"), QStringLiteral("ext")}));
        const QList<QToolButton*> chips =
            editor.findChildren<QToolButton*>(QStringLiteral("tagChip"));
        QCOMPARE(chips.size(), 2);

        QTest::mouseClick(chips.first(), Qt::LeftButton);

        QCOMPARE(editor.tags(), QStringList({QStringLiteral("ext")}));
    }

    void suggestionsMatchThePendingPrefixCaseInsensitively()
    {
        TagEditor editor;
        editor.setSuggestions({
            QStringLiteral("hole"),
            QStringLiteral("inspection"),
            QStringLiteral("underside")});
        editor.load({QStringLiteral("hole")});
        editor.show();
        auto* input = qobject_cast<QLineEdit*>(editor.focusProxy());
        QVERIFY(input != nullptr);
        input->setFocus();

        QTest::keyClicks(input, QStringLiteral("UN"));

        QCompleter* completer = input->completer();
        QVERIFY(completer != nullptr);
        QCOMPARE(completer->completionCount(), 1);
        QCOMPARE(completer->currentCompletion(), QStringLiteral("underside"));
    }

    void unconfirmedFreeFormTextIsIncludedWhenTagsAreRead()
    {
        TagEditor editor;
        auto* input = qobject_cast<QLineEdit*>(editor.focusProxy());
        QVERIFY(input != nullptr);

        QTest::keyClicks(input, QStringLiteral("custom"));

        QCOMPARE(editor.tags(), QStringList({QStringLiteral("custom")}));
    }

    void backspaceOnEmptyInputRemovesTheLastChip()
    {
        TagEditor editor;
        editor.load({QStringLiteral("hole"), QStringLiteral("ext")});
        QWidget* input = editor.focusProxy();
        QVERIFY(input != nullptr);

        QTest::keyClick(input, Qt::Key_Backspace);

        QCOMPARE(editor.tags(), QStringList({QStringLiteral("hole")}));
    }

    void manyTagsDoNotIncreaseTheEditorsPreferredWidth()
    {
        TagEditor editor;
        const int initialWidth = editor.sizeHint().width();

        editor.load({
            QStringLiteral("hole"),
            QStringLiteral("inspection"),
            QStringLiteral("underside"),
            QStringLiteral("reviewed"),
            QStringLiteral("long-free-form-label")});

        QCOMPARE(editor.sizeHint().width(), initialWidth);
    }

    void editorDoesNotReserveSpaceForANativeScrollBar()
    {
        TagEditor editor;
        auto* input = qobject_cast<QLineEdit*>(editor.focusProxy());
        auto* scroller = editor.findChild<QScrollArea*>(
            QStringLiteral("tagEditorScroller"));
        QVERIFY(input != nullptr);
        QVERIFY(scroller != nullptr);

        QCOMPARE(
            scroller->horizontalScrollBarPolicy(),
            Qt::ScrollBarAlwaysOff);
        QVERIFY(!scroller->viewport()->autoFillBackground());
        QVERIFY(editor.sizeHint().height() <= input->sizeHint().height() + 2);
    }
};

QTEST_MAIN(TagEditorTest)
#include "tag_editor_test.moc"
