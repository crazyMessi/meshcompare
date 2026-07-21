#pragma once

#include <QStringList>
#include <QWidget>

class QLineEdit;
class QHBoxLayout;
class QStringListModel;

class TagEditor final : public QWidget
{
    Q_OBJECT

public:
    explicit TagEditor(QWidget* parent = nullptr);

    void load(const QStringList& tags);
    void setSuggestions(const QStringList& tags);
    QStringList tags() const;
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void acceptPendingText();
    void appendTags(const QStringList& tags);
    void rebuildChips();
    void rebuildSuggestionModel();

    QHBoxLayout* layout_ = nullptr;
    QLineEdit* input_ = nullptr;
    QStringListModel* suggestionModel_ = nullptr;
    QStringList acceptedTags_;
    QStringList suggestions_;
};
