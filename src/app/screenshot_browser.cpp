#include "screenshot_browser.h"
#include "tag_editor.h"

#include <algorithm>
#include <utility>

#include <QComboBox>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QImageReader>
#include <QLabel>
#include <QListView>
#include <QListWidget>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSet>
#include <QSignalBlocker>
#include <QVBoxLayout>

namespace
{
constexpr int ViewIdRole = Qt::UserRole;
constexpr int ScreenshotPathRole = Qt::UserRole + 1;
constexpr int TagsRole = Qt::UserRole + 2;
constexpr int WorkspaceUuidRole = Qt::UserRole + 3;
constexpr int NoteRole = Qt::UserRole + 4;
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

QString poseCaption(const CameraPoseSummary& pose)
{
    const QString identity = pose.workspaceUuid.isEmpty()
        ? pose.viewId
        : QStringLiteral("%1 · %2")
              .arg(pose.workspaceUuid.left(8), pose.viewId);
    return pose.tags.isEmpty()
        ? identity
        : QStringLiteral("%1\n%2").arg(identity, renderTags(pose.tags));
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
    setWindowModality(Qt::NonModal);
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
    previewColumn->addWidget(preview_, 1);

    previewDetails_ = new QLabel(this);
    previewDetails_->setObjectName(QStringLiteral("screenshotPreviewDetails"));
    previewDetails_->setTextFormat(Qt::PlainText);
    previewDetails_->setWordWrap(true);
    previewColumn->addWidget(previewDetails_);

    previewTagsLabel_ = new QLabel(tr("Tags"), this);
    previewTags_ = new TagEditor(this);
    previewTags_->setObjectName(QStringLiteral("screenshotPreviewTags"));
    previewTags_->setAccessibleName(tr("Tags for selected view"));
    previewTagsLabel_->setBuddy(previewTags_);
    previewColumn->addWidget(previewTagsLabel_);
    previewColumn->addWidget(previewTags_);
    previewNoteLabel_ = new QLabel(tr("Note"), this);
    previewNoteLabel_->setObjectName(QStringLiteral("screenshotPreviewNoteLabel"));
    previewColumn->addWidget(previewNoteLabel_);
    previewNote_ = new QPlainTextEdit(this);
    previewNote_->setObjectName(QStringLiteral("screenshotPreviewNote"));
    previewNote_->setReadOnly(true);
    previewNote_->setPlaceholderText(tr("Add a note for this view…"));
    previewNote_->setMinimumHeight(72);
    previewNote_->setMaximumHeight(140);
    previewNoteLabel_->setBuddy(previewNote_);
    previewColumn->addWidget(previewNote_);
    auto* detailsFooter = new QHBoxLayout;
    detailsStatus_ = new QLabel(this);
    detailsStatus_->setObjectName(QStringLiteral("screenshotDetailsStatus"));
    detailsStatus_->setTextFormat(Qt::PlainText);
    detailsStatus_->setWordWrap(true);
    detailsFooter->addWidget(detailsStatus_, 1);
    saveChanges_ = new QPushButton(tr("Save Changes"), this);
    saveChanges_->setObjectName(QStringLiteral("screenshotSaveChanges"));
    saveChanges_->setAutoDefault(false);
    detailsFooter->addWidget(saveChanges_);
    previewColumn->addLayout(detailsFooter);
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
    connect(previewTags_, &TagEditor::tagsChanged, this, &ScreenshotBrowser::rememberDetailsDraft);
    connect(previewNote_, &QPlainTextEdit::textChanged, this, &ScreenshotBrowser::rememberDetailsDraft);
    connect(saveChanges_, &QPushButton::clicked, this, &ScreenshotBrowser::saveDetails);

    rebuildTagFilter();
}

void ScreenshotBrowser::setDetailsSaveHandler(std::function<OperationResult(
    const QString&, const QString&, const QStringList&, const QString&)> handler)
{
    detailsSaveHandler_ = std::move(handler);
    updateDetailsEditorState();
}

void ScreenshotBrowser::setPoses(const QVector<CameraPoseSummary>& poses)
{
    QSet<QPair<QString, QString>> incomingViews;
    incomingViews.reserve(poses.size());
    for (const CameraPoseSummary& pose : poses)
        incomingViews.insert(qMakePair(pose.workspaceUuid, pose.viewId));
    for (auto draft = detailsDrafts_.begin(); draft != detailsDrafts_.end();) {
        if (!incomingViews.contains(draft.key()))
            draft = detailsDrafts_.erase(draft);
        else
            ++draft;
    }

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
        const auto identity = qMakePair(pose.workspaceUuid, pose.viewId);
        const auto draft = detailsDrafts_.constFind(identity);
        if (draft != detailsDrafts_.cend() &&
            draft->tags == pose.tags && draft->note == pose.note)
            detailsDrafts_.remove(identity);
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
    previewTags_->setSuggestions(tags);
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
    const QListWidgetItem* selectedItem = gallery_->currentItem();
    const QString selectedWorkspace = selectedItem == nullptr
        ? QString() : selectedItem->data(WorkspaceUuidRole).toString();
    const QString selectedView = selectedItem == nullptr
        ? QString() : selectedItem->data(ViewIdRole).toString();
    const QSignalBlocker blocker(gallery_);
    gallery_->clear();
    int selectedRow = 0;
    for (const CameraPoseSummary& pose : screenshotPoses_) {
        const bool matches = mode == TagFilterMode::All ||
            (mode == TagFilterMode::Untagged && pose.tags.isEmpty()) ||
            (mode == TagFilterMode::Tag && hasTag(pose, selectedTag));
        if (!matches)
            continue;

        QImageReader reader(pose.screenshotPath);
        const QSize sourceSize = reader.size();
        if (sourceSize.isValid()) {
            QSize thumbnailSize = sourceSize;
            thumbnailSize.scale(
                gallery_->iconSize(), Qt::KeepAspectRatio);
            reader.setScaledSize(thumbnailSize);
        }
        const QImage image = reader.read();
        if (image.isNull())
            continue;

        auto* item = new QListWidgetItem(gallery_);
        item->setData(ViewIdRole, pose.viewId);
        item->setData(ScreenshotPathRole, pose.screenshotPath);
        item->setData(TagsRole, pose.tags);
        item->setData(WorkspaceUuidRole, pose.workspaceUuid);
        item->setData(NoteRole, pose.note);
        item->setIcon(QPixmap::fromImage(image.scaled(
            gallery_->iconSize(), Qt::KeepAspectRatio, Qt::SmoothTransformation)));
        item->setText(poseCaption(pose));
        item->setToolTip(tr("%1\n%2").arg(pose.viewId, pose.screenshotPath));
        if (pose.workspaceUuid == selectedWorkspace && pose.viewId == selectedView)
            selectedRow = gallery_->row(item);
    }

    emptyState_->setVisible(gallery_->count() == 0);
    if (gallery_->count() > 0)
        gallery_->setCurrentRow(selectedRow);
    updatePreview();
}

void ScreenshotBrowser::updatePreview()
{
    const QListWidgetItem* item = gallery_->currentItem();
    DetailsDraft form;
    if (item != nullptr) {
        const auto identity = qMakePair(
            item->data(WorkspaceUuidRole).toString(),
            item->data(ViewIdRole).toString());
        form = detailsDrafts_.value(identity, {
            item->data(TagsRole).toStringList(), item->data(NoteRole).toString()});
    }
    {
        const QSignalBlocker noteBlocker(previewNote_);
        const QSignalBlocker tagsBlocker(previewTags_);
        if (previewTags_->tags() != form.tags)
            previewTags_->load(form.tags);
        if (previewNote_->toPlainText() != form.note)
            previewNote_->setPlainText(form.note);
    }
    updateDetailsEditorState();
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
    QStringList details;
    const QString workspaceUuid =
        item->data(WorkspaceUuidRole).toString();
    if (!workspaceUuid.isEmpty())
        details.append(tr("Workspace %1").arg(workspaceUuid));
    details.append(item->data(ViewIdRole).toString());
    if (!tags.isEmpty())
        details.append(renderTags(tags));
    previewDetails_->setText(details.join(QStringLiteral(" · ")));
}

void ScreenshotBrowser::rememberDetailsDraft()
{
    const QListWidgetItem* item = gallery_->currentItem();
    if (item == nullptr || !detailsSaveHandler_)
        return;
    const auto identity = qMakePair(
        item->data(WorkspaceUuidRole).toString(), item->data(ViewIdRole).toString());
    const DetailsDraft details = {previewTags_->tags(), previewNote_->toPlainText()};
    if (details.tags == item->data(TagsRole).toStringList() &&
        details.note == item->data(NoteRole).toString())
        detailsDrafts_.remove(identity);
    else
        detailsDrafts_.insert(identity, details);
    updateDetailsEditorState();
}

void ScreenshotBrowser::updateDetailsEditorState()
{
    const QListWidgetItem* item = gallery_->currentItem();
    const bool hasSelection = item != nullptr;
    previewTagsLabel_->setVisible(hasSelection);
    previewTags_->setVisible(hasSelection);
    previewTags_->setEnabled(bool(detailsSaveHandler_));
    previewNoteLabel_->setVisible(hasSelection);
    previewNote_->setVisible(hasSelection);
    saveChanges_->setVisible(hasSelection);
    detailsStatus_->setVisible(hasSelection);
    previewNote_->setReadOnly(!detailsSaveHandler_);
    const bool changed = hasSelection &&
        (previewTags_->tags() != item->data(TagsRole).toStringList() ||
         previewNote_->toPlainText() != item->data(NoteRole).toString());
    saveChanges_->setEnabled(detailsSaveHandler_ && changed);
    detailsStatus_->setText(changed ? tr("Unsaved changes") : QString());
}

void ScreenshotBrowser::saveDetails()
{
    const QListWidgetItem* item = gallery_->currentItem();
    if (item == nullptr || !detailsSaveHandler_ || !saveChanges_->isEnabled())
        return;
    const QString workspaceUuid = item->data(WorkspaceUuidRole).toString();
    const QString viewId = item->data(ViewIdRole).toString();
    const QStringList tags = previewTags_->tags();
    const QString note = previewNote_->toPlainText();
    const OperationResult result = detailsSaveHandler_(workspaceUuid, viewId, tags, note);
    if (!result.ok) {
        detailsStatus_->setText(tr("Could not save changes: %1").arg(result.error));
        return;
    }

    for (CameraPoseSummary& pose : screenshotPoses_) {
        if (pose.workspaceUuid == workspaceUuid && pose.viewId == viewId) {
            pose.tags = tags;
            pose.note = note;
        }
    }
    for (int index = 0; index < gallery_->count(); ++index) {
        QListWidgetItem* galleryItem = gallery_->item(index);
        if (galleryItem->data(WorkspaceUuidRole).toString() == workspaceUuid &&
            galleryItem->data(ViewIdRole).toString() == viewId) {
            galleryItem->setData(TagsRole, tags);
            galleryItem->setData(NoteRole, note);
        }
    }
    detailsDrafts_.remove(qMakePair(workspaceUuid, viewId));
    emit detailsSaved(workspaceUuid, viewId, tags, note);
    rebuildTagFilter();
    const QListWidgetItem* selected = gallery_->currentItem();
    if (selected != nullptr &&
        selected->data(WorkspaceUuidRole).toString() == workspaceUuid &&
        selected->data(ViewIdRole).toString() == viewId)
        detailsStatus_->setText(tr("Saved"));
}
