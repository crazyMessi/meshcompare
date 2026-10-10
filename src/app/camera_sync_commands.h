#pragma once

#include <QObject>
#include <QString>

#include "../core/meshcompare_types.h"

struct CameraSyncSnapshot
{
    OperationResult result = OperationResult::success();
    bool configured = false;
    // A malformed stored key is not usable, but the user must still be able
    // to remove it from Keychain.
    bool credentialStored = false;
    bool syncing = false;
    QString boardKey;
    QString lastSyncAtUtc;
    QString statusMessage;
    bool lastSyncSucceeded = false;
};

class ICameraSyncCommands : public QObject
{
    Q_OBJECT

public:
    using QObject::QObject;
    ~ICameraSyncCommands() override = default;

    virtual CameraSyncSnapshot cameraSyncSnapshot() const = 0;
    virtual OperationResult configureApiKey(const QString& apiKey) = 0;
    virtual OperationResult clearApiKey() = 0;
    virtual OperationResult startCameraSync(const QString& boardKey) = 0;

signals:
    void cameraSyncStateChanged();
    void cameraSyncCompleted();
};
