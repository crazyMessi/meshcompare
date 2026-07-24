#pragma once

#include <QDialog>
#include <QVector>

#include "camera_commands.h"

class QComboBox;
class QLabel;
class QListWidget;

class ScreenshotBrowser final : public QDialog
{
    Q_OBJECT

public:
    explicit ScreenshotBrowser(QWidget* parent = nullptr);

    void setPoses(const QVector<CameraPoseSummary>& poses);

private:
    void rebuildTagFilter();
    void rebuildGallery();
    void updatePreview();

    QVector<CameraPoseSummary> screenshotPoses_;
    QComboBox* tagFilter_ = nullptr;
    QListWidget* gallery_ = nullptr;
    QLabel* emptyState_ = nullptr;
    QLabel* preview_ = nullptr;
    QLabel* previewDetails_ = nullptr;
};
