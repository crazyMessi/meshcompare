#include "tag_editor.h"

#include "../core/camera_pose_tags.h"

#include <QCompleter>
#include <QEvent>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLineEdit>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QStringListModel>
#include <QToolButton>

namespace
{
QStringList normalizedEditorTags(const QStringList& tags)
{
    QStringList normalized;
    for (const QString& tag : meshcompare::normalizeCameraPoseTags(tags)) {
        if (!normalized.contains(tag, Qt::CaseInsensitive))
            normalized.append(tag);
    }
    return normalized;
}
}

TagEditor::TagEditor(QWidget* parent)
    : QWidget(parent)
{
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    auto* outerLayout = new QHBoxLayout(this);
    outerLayout->setContentsMargins(0, 0, 0, 0);

    auto* scroller = new QScrollArea(this);
    scroller->setObjectName(QStringLiteral("tagEditorScroller"));
    scroller->setFrameShape(QFrame::NoFrame);
    scroller->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroller->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroller->setSizeAdjustPolicy(QAbstractScrollArea::AdjustIgnored);
    scroller->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    scroller->setFixedHeight(sizeHint().height());
    scroller->setAutoFillBackground(false);
    scroller->viewport()->setObjectName(QStringLiteral("tagEditorViewport"));
    scroller->viewport()->setAutoFillBackground(false);
    outerLayout->addWidget(scroller);

    auto* contents = new QWidget(scroller);
    contents->setObjectName(QStringLiteral("tagEditorContents"));
    contents->setAutoFillBackground(false);
    layout_ = new QHBoxLayout(contents);
    layout_->setContentsMargins(0, 0, 0, 0);
    layout_->setSpacing(4);
    layout_->setSizeConstraint(QLayout::SetMinimumSize);
    input_ = new QLineEdit(contents);
    input_->setObjectName(QStringLiteral("tagEditorInput"));
    input_->setPlaceholderText(tr("Type tags"));
    input_->installEventFilter(this);
    layout_->addWidget(input_, 1);
    scroller->setWidget(contents);
    scroller->setWidgetResizable(true);
    setFocusProxy(input_);

    suggestionModel_ = new QStringListModel(this);
    auto* completer = new QCompleter(suggestionModel_, this);
    completer->setCaseSensitivity(Qt::CaseInsensitive);
    completer->setFilterMode(Qt::MatchStartsWith);
    completer->setCompletionMode(QCompleter::PopupCompletion);
    input_->setCompleter(completer);

    connect(input_, &QLineEdit::textEdited, this, [this, completer](const QString& text) {
        if (text.contains(QLatin1Char(','))) {
            const bool endsWithSeparator = text.endsWith(QLatin1Char(','));
            QStringList parts = text.split(QLatin1Char(','), Qt::KeepEmptyParts);
            const QString remainder = endsWithSeparator ? QString() : parts.takeLast();
            appendTags(parts);
            const QSignalBlocker blocker(input_);
            input_->setText(remainder.trimmed());
        }
        const QString prefix = input_->text().trimmed();
        completer->setCompletionPrefix(prefix);
        if (!prefix.isEmpty())
            completer->complete();
    });
    connect(
        completer,
        static_cast<void (QCompleter::*)(const QString&)>(&QCompleter::activated),
        this,
        [this](const QString& tag) {
            appendTags({tag});
            const QSignalBlocker blocker(input_);
            input_->clear();
    });
}

QSize TagEditor::sizeHint() const
{
    const int inputHeight = input_ == nullptr
        ? fontMetrics().height() + 12
        : input_->sizeHint().height();
    return QSize(220, inputHeight + 2);
}

QSize TagEditor::minimumSizeHint() const
{
    QSize minimum = sizeHint();
    minimum.setWidth(120);
    return minimum;
}

void TagEditor::load(const QStringList& tags)
{
    acceptedTags_ = normalizedEditorTags(tags);
    {
        const QSignalBlocker blocker(input_);
        input_->clear();
    }
    rebuildChips();
    rebuildSuggestionModel();
}

void TagEditor::setSuggestions(const QStringList& tags)
{
    suggestions_ = normalizedEditorTags(tags);
    rebuildSuggestionModel();
}

QStringList TagEditor::tags() const
{
    QStringList result = acceptedTags_;
    result.append(input_->text().split(QLatin1Char(','), Qt::SkipEmptyParts));
    return normalizedEditorTags(result);
}

bool TagEditor::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == input_ && event->type() == QEvent::KeyPress) {
        const auto* keyEvent = static_cast<QKeyEvent*>(event);
        if (keyEvent->key() == Qt::Key_Comma ||
            keyEvent->key() == Qt::Key_Return ||
            keyEvent->key() == Qt::Key_Enter) {
            acceptPendingText();
            return true;
        }
        if (keyEvent->key() == Qt::Key_Backspace && input_->text().isEmpty() &&
            !acceptedTags_.isEmpty()) {
            acceptedTags_.removeLast();
            rebuildChips();
            rebuildSuggestionModel();
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

void TagEditor::acceptPendingText()
{
    appendTags(input_->text().split(QLatin1Char(','), Qt::SkipEmptyParts));
    const QSignalBlocker blocker(input_);
    input_->clear();
}

void TagEditor::appendTags(const QStringList& tags)
{
    QStringList combined = acceptedTags_;
    combined.append(tags);
    const QStringList normalized = normalizedEditorTags(combined);
    if (normalized == acceptedTags_)
        return;
    acceptedTags_ = normalized;
    rebuildChips();
    rebuildSuggestionModel();
}

void TagEditor::rebuildChips()
{
    const QList<QToolButton*> oldChips =
        findChildren<QToolButton*>(QStringLiteral("tagChip"));
    for (QToolButton* chip : oldChips) {
        layout_->removeWidget(chip);
        chip->setParent(nullptr);
        chip->deleteLater();
    }

    for (const QString& tag : acceptedTags_) {
        auto* chip = new QToolButton(this);
        chip->setObjectName(QStringLiteral("tagChip"));
        chip->setText(QStringLiteral("#%1  ×").arg(tag));
        chip->setToolTip(tr("Remove tag %1").arg(tag));
        chip->setAccessibleName(tr("Remove tag %1").arg(tag));
        chip->setAutoRaise(false);
        connect(chip, &QToolButton::clicked, this, [this, tag] {
            for (int index = 0; index < acceptedTags_.size(); ++index) {
                if (acceptedTags_.at(index).compare(tag, Qt::CaseInsensitive) == 0) {
                    acceptedTags_.removeAt(index);
                    break;
                }
            }
            rebuildChips();
            rebuildSuggestionModel();
        });
        layout_->insertWidget(layout_->count() - 1, chip);
    }
}

void TagEditor::rebuildSuggestionModel()
{
    QStringList available;
    for (const QString& suggestion : suggestions_) {
        if (!acceptedTags_.contains(suggestion, Qt::CaseInsensitive))
            available.append(suggestion);
    }
    suggestionModel_->setStringList(available);
}
