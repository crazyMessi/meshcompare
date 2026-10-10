#pragma once

#include "camera_pose_sync.h"
#include "core/meshcompare_types.h"

#include <QString>
#include <QtGlobal>

namespace meshcompare {

struct CameraSyncPersistedState
{
    QString boardKey;
    qint64 boardVersion = 0;
    QString syncedAtUtc;
    CameraPoseSyncDocument base;
};

class CameraSyncStateStore
{
public:
    explicit CameraSyncStateStore(QString path);

    QString path() const;
    OperationResult load(
        const QString& boardKey,
        CameraSyncPersistedState* state,
        bool* found = nullptr) const;
    OperationResult save(const CameraSyncPersistedState& state) const;

private:
    QString path_;
};

} // namespace meshcompare
