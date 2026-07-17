#pragma once

#include <deque>
#include <utility>

#include "services/mesh_import_service.h"

class FakeMeshImportService final : public IMeshImportService
{
public:
    FakeMeshImportService() = default;
    explicit FakeMeshImportService(StagedWorkspace staged)
    {
        enqueue(std::move(staged));
    }

    void enqueue(StagedWorkspace staged)
    {
        staged_.push_back(std::move(staged));
    }

    StagedWorkspace stage(const QStringList& paths) override
    {
        ++stageCount_;
        lastPaths_ = paths;
        if (staged_.empty()) {
            return {OperationResult::failure(
                        QStringLiteral("No staged import result was configured.")),
                    nullptr,
                    {},
                    {}};
        }
        StagedWorkspace result = std::move(staged_.front());
        staged_.pop_front();
        return result;
    }

    int stageCount() const { return stageCount_; }
    const QStringList& lastPaths() const { return lastPaths_; }

private:
    std::deque<StagedWorkspace> staged_;
    int stageCount_ = 0;
    QStringList lastPaths_;
};
