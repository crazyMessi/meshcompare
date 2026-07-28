#pragma once

#include <functional>
#include <utility>

#include <QDateTime>
#include <QImage>
#include <QStringList>
#include <QVector>

#include "services/camera_pose_store.h"

class FakeCameraPoseStore final : public ICameraPoseStore
{
public:
    QString libraryPath() const override { return libraryPath_; }
    void setLibraryPath(const QString& path) { libraryPath_ = path; }

    void add(
        const QString& uuid,
        const QString& viewId,
        const CameraPose& pose,
        const QString& savedAtUtc,
        const QStringList& tags = {})
    {
        records_.append(
            {CameraPoseStore::normalizeUuid(uuid),
             viewId,
             savedAtUtc,
             tags,
             {},
             pose});
    }

    OperationResult migrateLegacyIfNeeded() override
    {
        return OperationResult::success();
    }

    OperationResult saveWithScreenshot(
        const QString& uuid,
        const CameraPose& pose,
        const QImage& screenshot,
        QString* viewId = nullptr,
        const QStringList& tags = {},
        QString* screenshotPath = nullptr) override
    {
        ++saveWithScreenshotCount_;
        lastSaveUuid_ = uuid;
        lastSavedPose_ = pose;
        lastSavedScreenshot_ = screenshot;
        appendTrace(QStringLiteral("store-save-with-screenshot"));
        if (saveObserver_)
            saveObserver_();
        if (!nextSaveError_.isEmpty())
            return OperationResult::failure(takeError(nextSaveError_));
        if (screenshot.isNull())
            return OperationResult::failure(QStringLiteral("Screenshot is empty."));

        const QString generated = QStringLiteral("view_%1").arg(
            ++nextViewNumber_, 3, 10, QLatin1Char('0'));
        const QString generatedPath = QStringLiteral("/screenshots/%1/%2.png")
            .arg(CameraPoseStore::normalizeUuid(uuid), generated);
        records_.append(
            {CameraPoseStore::normalizeUuid(uuid),
             generated,
             QStringLiteral("2026-07-15T00:00:00Z"),
             tags,
             generatedPath,
             pose});
        if (viewId != nullptr)
            *viewId = generated;
        if (screenshotPath != nullptr)
            *screenshotPath = generatedPath;
        return OperationResult::success();
    }

    QVector<SavedCameraPose> list(
        const QString& uuid,
        OperationResult* result = nullptr) const override
    {
        ++listCount_;
        lastListUuid_ = uuid;
        appendTrace(QStringLiteral("store-list"));
        if (listObserver_)
            listObserver_();
        if (!nextListError_.isEmpty()) {
            const QString error = takeError(nextListError_);
            if (result != nullptr)
                *result = OperationResult::failure(error);
            return {};
        }

        QVector<SavedCameraPose> matching;
        const QString normalized = CameraPoseStore::normalizeUuid(uuid);
        for (const SavedCameraPose& record : records_) {
            if (record.uuid == normalized)
                matching.append(record);
        }
        if (result != nullptr)
            *result = OperationResult::success();
        return matching;
    }

    QVector<SavedCameraPose> listAll(
        OperationResult* result = nullptr) const override
    {
        ++listCount_;
        lastListUuid_.clear();
        appendTrace(QStringLiteral("store-list-all"));
        if (listObserver_)
            listObserver_();
        if (!nextListError_.isEmpty()) {
            const QString error = takeError(nextListError_);
            if (result != nullptr)
                *result = OperationResult::failure(error);
            return {};
        }
        if (result != nullptr)
            *result = OperationResult::success();
        return records_;
    }

    OperationResult load(
        const QString& uuid,
        const QString& viewId,
        CameraPose* pose) const override
    {
        ++loadCount_;
        lastLoadUuid_ = uuid;
        lastLoadViewId_ = viewId;
        appendTrace(QStringLiteral("store-load:%1").arg(viewId));
        if (loadObserver_)
            loadObserver_();
        if (!nextLoadError_.isEmpty())
            return OperationResult::failure(takeError(nextLoadError_));
        if (pose == nullptr)
            return OperationResult::failure(QStringLiteral("Missing pose destination."));

        const QString normalized = CameraPoseStore::normalizeUuid(uuid);
        for (const SavedCameraPose& record : records_) {
            if (record.uuid == normalized && record.viewId == viewId) {
                *pose = record.pose;
                return OperationResult::success();
            }
        }
        return OperationResult::failure(QStringLiteral("Pose not found."));
    }

    OperationResult remove(
        const QString& uuid,
        const QString& viewId) override
    {
        ++removeCount_;
        lastRemoveUuid_ = uuid;
        lastRemoveViewId_ = viewId;
        appendTrace(QStringLiteral("store-remove:%1").arg(viewId));
        if (removeObserver_)
            removeObserver_();
        if (!nextRemoveError_.isEmpty())
            return OperationResult::failure(takeError(nextRemoveError_));

        const QString normalized = CameraPoseStore::normalizeUuid(uuid);
        for (int index = 0; index < records_.size(); ++index) {
            if (records_.at(index).uuid == normalized &&
                records_.at(index).viewId == viewId) {
                records_.removeAt(index);
                return OperationResult::success();
            }
        }
        return OperationResult::failure(QStringLiteral("Pose not found."));
    }

    OperationResult setTags(
        const QString& uuid,
        const QString& viewId,
        const QStringList& tags) override
    {
        ++setTagsCount_;
        lastSetTagsUuid_ = uuid;
        lastSetTagsViewId_ = viewId;
        lastSetTags_ = tags;
        if (!nextSetTagsError_.isEmpty())
            return OperationResult::failure(takeError(nextSetTagsError_));

        const QString normalized = CameraPoseStore::normalizeUuid(uuid);
        for (SavedCameraPose& record : records_) {
            if (record.uuid == normalized && record.viewId == viewId) {
                record.tags = tags;
                return OperationResult::success();
            }
        }
        return OperationResult::failure(QStringLiteral("Pose not found."));
    }

    void failNextSave(const QString& error) { nextSaveError_ = error; }
    void failNextList(const QString& error) const { nextListError_ = error; }
    void failNextLoad(const QString& error) const { nextLoadError_ = error; }
    void failNextRemove(const QString& error) { nextRemoveError_ = error; }
    void failNextSetTags(const QString& error) { nextSetTagsError_ = error; }

    void setTrace(QStringList* trace) const { trace_ = trace; }
    void setSaveObserver(std::function<void()> observer)
    {
        saveObserver_ = std::move(observer);
    }
    void setListObserver(std::function<void()> observer) const
    {
        listObserver_ = std::move(observer);
    }
    void setLoadObserver(std::function<void()> observer) const
    {
        loadObserver_ = std::move(observer);
    }
    void setRemoveObserver(std::function<void()> observer)
    {
        removeObserver_ = std::move(observer);
    }

    int saveWithScreenshotCount() const { return saveWithScreenshotCount_; }
    int listCount() const { return listCount_; }
    int loadCount() const { return loadCount_; }
    int removeCount() const { return removeCount_; }
    int setTagsCount() const { return setTagsCount_; }
    QString lastSaveUuid() const { return lastSaveUuid_; }
    QString lastListUuid() const { return lastListUuid_; }
    QString lastLoadUuid() const { return lastLoadUuid_; }
    QString lastLoadViewId() const { return lastLoadViewId_; }
    QString lastRemoveUuid() const { return lastRemoveUuid_; }
    QString lastRemoveViewId() const { return lastRemoveViewId_; }
    QString lastSetTagsUuid() const { return lastSetTagsUuid_; }
    QString lastSetTagsViewId() const { return lastSetTagsViewId_; }
    QStringList lastSetTags() const { return lastSetTags_; }
    CameraPose lastSavedPose() const { return lastSavedPose_; }
    QImage lastSavedScreenshot() const { return lastSavedScreenshot_; }

    void resetObservations()
    {
        saveWithScreenshotCount_ = 0;
        listCount_ = 0;
        loadCount_ = 0;
        removeCount_ = 0;
        setTagsCount_ = 0;
        lastSaveUuid_.clear();
        lastListUuid_.clear();
        lastLoadUuid_.clear();
        lastLoadViewId_.clear();
        lastRemoveUuid_.clear();
        lastRemoveViewId_.clear();
        lastSetTagsUuid_.clear();
        lastSetTagsViewId_.clear();
        lastSetTags_.clear();
        lastSavedPose_ = {};
        lastSavedScreenshot_ = {};
    }

private:
    static QString takeError(QString& source)
    {
        const QString error = source;
        source.clear();
        return error;
    }

    void appendTrace(const QString& event) const
    {
        if (trace_ != nullptr)
            trace_->append(event);
    }

    QVector<SavedCameraPose> records_;
    QString libraryPath_;
    mutable QString nextListError_;
    mutable QString nextLoadError_;
    QString nextSaveError_;
    QString nextRemoveError_;
    QString nextSetTagsError_;
    mutable QStringList* trace_ = nullptr;
    std::function<void()> saveObserver_;
    mutable std::function<void()> listObserver_;
    mutable std::function<void()> loadObserver_;
    std::function<void()> removeObserver_;
    QString lastSaveUuid_;
    mutable QString lastListUuid_;
    mutable QString lastLoadUuid_;
    mutable QString lastLoadViewId_;
    QString lastRemoveUuid_;
    QString lastRemoveViewId_;
    QString lastSetTagsUuid_;
    QString lastSetTagsViewId_;
    QStringList lastSetTags_;
    CameraPose lastSavedPose_;
    QImage lastSavedScreenshot_;
    int nextViewNumber_ = 0;
    int saveWithScreenshotCount_ = 0;
    mutable int listCount_ = 0;
    mutable int loadCount_ = 0;
    int removeCount_ = 0;
    int setTagsCount_ = 0;
};
