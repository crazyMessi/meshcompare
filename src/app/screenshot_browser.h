#pragma once

#include <functional>

#include <QDialog>
#include <QMap>
#include <QVector>

#include "camera_commands.h"

class QComboBox;
class QLabel;
class QListWidget;
class QPlainTextEdit;
class QPushButton;
class TagEditor;

class ScreenshotBrowser final : public QDialog
{
    Q_OBJECT

public:
    explicit ScreenshotBrowser(QWidget* parent = nullptr);

    void setPoses(const QVector<CameraPoseSummary>& poses);
    void setDetailsSaveHandler(std::function<OperationResult(
        const QString&, const QString&, const QStringList&, const QString&)> handler);

signals:
    void detailsSaved(
        const QString& workspaceUuid,
        const QString& viewId,
        const QStringList& tags,
        const QString& note);

private:
    void rebuildTagFilter();
    void rebuildGallery();
    void updatePreview();
    void rememberDetailsDraft();
    void updateDetailsEditorState();
    void saveDetails();

    struct DetailsDraft {
        QStringList tags;
        QString note;
    };

    QVector<CameraPoseSummary> screenshotPoses_;
    QMap<QPair<QString, QString>, DetailsDraft> detailsDrafts_;
    std::function<OperationResult(
        const QString&, const QString&, const QStringList&, const QString&)> detailsSaveHandler_;
    QComboBox* tagFilter_ = nullptr;
    QListWidget* gallery_ = nullptr;
    QLabel* emptyState_ = nullptr;
    QLabel* preview_ = nullptr;
    QLabel* previewDetails_ = nullptr;
    QLabel* previewTagsLabel_ = nullptr;
    TagEditor* previewTags_ = nullptr;
    QLabel* previewNoteLabel_ = nullptr;
    QPlainTextEdit* previewNote_ = nullptr;
    QPushButton* saveChanges_ = nullptr;
    QLabel* detailsStatus_ = nullptr;
};
