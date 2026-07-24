#include "screenshot_browser.h"

#include <algorithm>

#include <QComboBox>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QImageReader>
#include <QLabel>
#include <QListView>
#include <QListWidget>
#include <QPixmap>
#include <QSignalBlocker>
#include <QVBoxLayout>

namespace
{
constexpr int ViewIdRole = Qt::UserRole;
constexpr int ScreenshotPathRole = Qt::UserRole + 1;
constexpr int TagsRole = Qt::UserRole + 2;
constexpr int FilterModeRole = Qt::UserRole;
constexpr int FilterTagRole = Qt::UserRole + 1;

enum class TagFilterMode {
    All,
    Untagged,
    Tag
};

QString renderTags(const QStringList& tags)
{
    QStringList rendered;
    rendered.reserve(tags.size());
    for (const QString& tag : tags)
        rendered.append(QStringLiteral("#%1").arg(tag));
    return rendered.join(QLatin1Char(' '));
}

bool hasTag(const CameraPoseSummary& pose, const QString& tag)
{
    return std::any_of(
        pose.tags.cbegin(),
        pose.tags.cend(),
        [&tag](const QString& poseTag) {
            return poseTag.compare(tag, Qt::CaseInsensitive) == 0;
        });
}

TagFilterMode selectedFilterMode(const QComboBox& filter)
{
    return static_cast<TagFilterMode>(
        filter.currentData(FilterModeRole).toInt());
}

void addFilterItem(
    QComboBox& filter,
    const QString& text,
    TagFilterMode mode,
    const QString& tag = {})
{
    filter.addItem(text);
    const int index = filter.count() - 1;
    filter.setItemData(index, static_cast<int>(mode), FilterModeRole);
    filter.setItemData(index, tag, FilterTagRole);
}
}

ScreenshotBrowser::ScreenshotBrowser(QWidget* parent)
    : QDialog(parent)
{
    setObjectName(QStringLiteral("screenshotBrowser"));
    setWindowTitle(tr("Screenshot Browser"));
    setWindowModality(Qt::ApplicationModal);
    resize(920, 600);

    auto* root = new QHBoxLayout(this);
    root->setContentsMargins(12, 12, 12, 12);
    root->setSpacing(12);

    auto* galleryColumn = new QVBoxLayout;
    auto* filterLabel = new QLabel(tr("Tag"), this);
    tagFilter_ = new QComboBox(this);
    tagFilter_->setObjectName(QStringLiteral("screenshotTagFilter"));
    filterLabel->setBuddy(tagFilter_);
    galleryColumn->addWidget(filterLabel);
    galleryColumn->addWidget(tagFilter_);

    gallery_ = new QListWidget(this);
    gallery_->setObjectName(QStringLiteral("screenshotGallery"));
    gallery_->setViewMode(QListView::IconMode);
    gallery_->setResizeMode(QListView::Adjust);
    gallery_->setMovement(QListView::Static);
    gallery_->setSelectionMode(QAbstractItemView::SingleSelection);
    gallery_->setIconSize(QSize(176, 112));
    gallery_->setGridSize(QSize(204, 158));
    gallery_->setWordWrap(true);
    gallery_->setMinimumWidth(440);
    galleryColumn->addWidget(gallery_, 1);

    emptyState_ = new QLabel(tr("No screenshots match this tag."), this);
    emptyState_->setObjectName(QStringLiteral("screenshotBrowserEmptyState"));
    emptyState_->setWordWrap(true);
    galleryColumn->addWidget(emptyState_);
    root->addLayout(galleryColumn, 3);

    auto* previewColumn = new QVBoxLayout;
    auto* previewLabel = new QLabel(tr("Preview"), this);
    previewColumn->addWidget(previewLabel);

    preview_ = new QLabel(tr("Select a screenshot to preview it."), this);
    preview_->setObjectName(QStringLiteral("screenshotPreview"));
    preview_->setAlignment(Qt::AlignCenter);
    preview_->setWordWrap(true);
    preview_->setMinimumSize(360, 270);
    preview_->setStyleSheet(QStringLiteral("QLabel { background: palette(base); }"));
    previewColumn->addWidget(preview_, 1);

    previewDetails_ = new QLabel(this);
    previewDetails_->setObjectName(QStringLiteral("screenshotPreviewDetails"));
    previewDetails_->setWordWrap(true);
    previewColumn->addWidget(previewDetails_);
    root->addLayout(previewColumn, 2);

    connect(
        tagFilter_,
        &QComboBox::currentTextChanged,
        this,
        [this](const QString&) { rebuildGallery(); });
    connect(
        gallery_,
        &QListWidget::itemSelectionChanged,
        this,
        &ScreenshotBrowser::updatePreview);

    rebuildTagFilter();
}

void ScreenshotBrowser::setPoses(const QVector<CameraPoseSummary>& poses)
{
    screenshotPoses_.clear();
    screenshotPoses_.reserve(poses.size());
    for (const CameraPoseSummary& pose : poses) {
        const QFileInfo screenshotFile(pose.screenshotPath);
        if (pose.screenshotPath.isEmpty() || !screenshotFile.isFile())
            continue;

        QImageReader reader(pose.screenshotPath);
        if (!reader.canRead())
            continue;
        screenshotPoses_.append(pose);
    }

    rebuildTagFilter();
}

void ScreenshotBrowser::rebuildTagFilter()
{
    const TagFilterMode previousMode = selectedFilterMode(*tagFilter_);
    const QString previousTag = tagFilter_->currentData(FilterTagRole).toString();
    QStringList tags;
    bool hasUntaggedScreenshots = false;
    for (const CameraPoseSummary& pose : screenshotPoses_) {
        if (pose.tags.isEmpty())
            hasUntaggedScreenshots = true;
        for (const QString& tag : pose.tags) {
            if (!tag.trimmed().isEmpty())
                tags.append(tag);
        }
    }
    tags.removeDuplicates();
    std::sort(
        tags.begin(),
        tags.end(),
        [](const QString& left, const QString& right) {
            return left.compare(right, Qt::CaseInsensitive) < 0;
        });

    const QSignalBlocker blocker(tagFilter_);
    tagFilter_->clear();
    addFilterItem(*tagFilter_, tr("All tags"), TagFilterMode::All);
    if (hasUntaggedScreenshots)
        addFilterItem(*tagFilter_, tr("Untagged"), TagFilterMode::Untagged);
    for (const QString& tag : tags)
        addFilterItem(*tagFilter_, tag, TagFilterMode::Tag, tag);

    int selectedIndex = 0;
    for (int index = 0; index < tagFilter_->count(); ++index) {
        const TagFilterMode mode = static_cast<TagFilterMode>(
            tagFilter_->itemData(index, FilterModeRole).toInt());
        if (mode == previousMode &&
            (mode != TagFilterMode::Tag ||
             tagFilter_->itemData(index, FilterTagRole).toString() == previousTag)) {
            selectedIndex = index;
            break;
        }
    }
    tagFilter_->setCurrentIndex(selectedIndex);
    rebuildGallery();
}

void ScreenshotBrowser::rebuildGallery()
{
    const TagFilterMode mode = selectedFilterMode(*tagFilter_);
    const QString selectedTag = tagFilter_->currentData(FilterTagRole).toString();
    gallery_->clear();
    for (const CameraPoseSummary& pose : screenshotPoses_) {
        const bool matches = mode == TagFilterMode::All ||
            (mode == TagFilterMode::Untagged && pose.tags.isEmpty()) ||
            (mode == TagFilterMode::Tag && hasTag(pose, selectedTag));
        if (!matches)
            continue;

        QImageReader reader(pose.screenshotPath);
        const QImage image = reader.read();
        if (image.isNull())
            continue;

        auto* item = new QListWidgetItem(gallery_);
        item->setData(ViewIdRole, pose.viewId);
        item->setData(ScreenshotPathRole, pose.screenshotPath);
        item->setData(TagsRole, pose.tags);
        item->setIcon(QPixmap::fromImage(image.scaled(
            gallery_->iconSize(), Qt::KeepAspectRatio, Qt::SmoothTransformation)));
        item->setText(pose.tags.isEmpty()
                ? pose.viewId
                : tr("%1\n%2").arg(pose.viewId, renderTags(pose.tags)));
        item->setToolTip(tr("%1\n%2").arg(pose.viewId, pose.screenshotPath));
    }

    emptyState_->setVisible(gallery_->count() == 0);
    if (gallery_->count() > 0)
        gallery_->setCurrentRow(0);
    else
        updatePreview();
}

void ScreenshotBrowser::updatePreview()
{
    const QListWidgetItem* item = gallery_->currentItem();
    if (item == nullptr) {
        preview_->setPixmap(QPixmap());
        preview_->setText(tr("Select a screenshot to preview it."));
        previewDetails_->clear();
        return;
    }

    QImageReader reader(item->data(ScreenshotPathRole).toString());
    const QImage image = reader.read();
    if (image.isNull()) {
        preview_->setPixmap(QPixmap());
        preview_->setText(tr("The selected screenshot can no longer be read."));
        previewDetails_->setText(item->data(ScreenshotPathRole).toString());
        return;
    }

    preview_->setText({});
    preview_->setPixmap(QPixmap::fromImage(image.scaled(
        preview_->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation)));
    const QStringList tags = item->data(TagsRole).toStringList();
    previewDetails_->setText(
        tags.isEmpty()
            ? item->data(ViewIdRole).toString()
            : tr("%1 · %2").arg(
                  item->data(ViewIdRole).toString(), renderTags(tags)));
}
