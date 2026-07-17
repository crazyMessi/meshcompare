#pragma once

#include <functional>
#include <utility>

#include <QDateTime>
#include <QStringList>
#include <QVector>

#include "services/camera_pose_store.h"

class FakeCameraPoseStore final : public ICameraPoseStore
{
public:
    void add(
        const QString& uuid,
        const QString& viewId,
        const CameraPose& pose,
        const QString& savedAtUtc)
    {
        records_.append(
            {CameraPoseStore::normalizeUuid(uuid), viewId, savedAtUtc, pose});
    }

    OperationResult migrateLegacyIfNeeded() override
    {
        return OperationResult::success();
    }

    OperationResult save(
        const QString& uuid,
        const CameraPose& pose,
        QString* viewId = nullptr) override
    {
        ++saveCount_;
        lastSaveUuid_ = uuid;
        lastSavedPose_ = pose;
        appendTrace(QStringLiteral("store-save"));
        if (saveObserver_)
            saveObserver_();
        if (!nextSaveError_.isEmpty()) {
            const QString error = takeError(nextSaveError_);
            return OperationResult::failure(error);
        }

        const QString generated = QStringLiteral("view_%1").arg(
            ++nextViewNumber_, 3, 10, QLatin1Char('0'));
        records_.append(
            {CameraPoseStore::normalizeUuid(uuid),
             generated,
             QStringLiteral("2026-07-15T00:00:00Z"),
             pose});
        if (viewId != nullptr)
            *viewId = generated;
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

    void failNextSave(const QString& error) { nextSaveError_ = error; }
    void failNextList(const QString& error) const { nextListError_ = error; }
    void failNextLoad(const QString& error) const { nextLoadError_ = error; }
    void failNextRemove(const QString& error) { nextRemoveError_ = error; }

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

    int saveCount() const { return saveCount_; }
    int listCount() const { return listCount_; }
    int loadCount() const { return loadCount_; }
    int removeCount() const { return removeCount_; }
    QString lastSaveUuid() const { return lastSaveUuid_; }
    QString lastListUuid() const { return lastListUuid_; }
    QString lastLoadUuid() const { return lastLoadUuid_; }
    QString lastLoadViewId() const { return lastLoadViewId_; }
    QString lastRemoveUuid() const { return lastRemoveUuid_; }
    QString lastRemoveViewId() const { return lastRemoveViewId_; }
    CameraPose lastSavedPose() const { return lastSavedPose_; }

    void resetObservations()
    {
        saveCount_ = 0;
        listCount_ = 0;
        loadCount_ = 0;
        removeCount_ = 0;
        lastSaveUuid_.clear();
        lastListUuid_.clear();
        lastLoadUuid_.clear();
        lastLoadViewId_.clear();
        lastRemoveUuid_.clear();
        lastRemoveViewId_.clear();
        lastSavedPose_ = {};
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
    mutable QString nextListError_;
    mutable QString nextLoadError_;
    QString nextSaveError_;
    QString nextRemoveError_;
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
    CameraPose lastSavedPose_;
    int nextViewNumber_ = 0;
    int saveCount_ = 0;
    mutable int listCount_ = 0;
    mutable int loadCount_ = 0;
    int removeCount_ = 0;
};
