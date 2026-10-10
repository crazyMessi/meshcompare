#pragma once

#include "app/camera_sync_commands.h"
#include "camera_pose_sync.h"
#include "camera_sync_state.h"
#include "cloud_board_client.h"
#include "cloud_api_key_store.h"

#include <QHash>
#include <QQueue>
#include <QTemporaryDir>

#include <memory>

class ICameraPoseStore;

class CameraCloudSyncService final : public ICameraSyncCommands
{
    Q_OBJECT

public:
    CameraCloudSyncService(
        ICameraPoseStore& cameraStore,
        CloudBoardClient& client,
        ICloudApiKeyStore& apiKeyStore,
        QString statePath,
        QObject* parent = nullptr);

    CameraSyncSnapshot cameraSyncSnapshot() const override;
    OperationResult configureApiKey(const QString& apiKey) override;
    OperationResult clearApiKey() override;
    OperationResult startCameraSync(const QString& boardKey) override;

private:
    struct ScreenshotTransport {
        QString sha256;
        QString sourcePath;
        QString fileId;
    };

    void updateStatus(const QString& message);
    OperationResult loadRemoteDocument(
        const CloudBoard& board,
        meshcompare::CameraPoseSyncDocument* document) const;
    void adoptRemoteGeneration(
        meshcompare::CameraPoseSyncDocument* remote,
        bool boardIsMissing);
    void mergeAndTransfer(
        const meshcompare::CameraPoseSyncDocument& remote,
        qint64 remoteVersion);
    void prepareTransfers();
    void startNextUpload();
    void startNextDownload();
    void putMergedBoard();
    void handleBoardReceived(quint64 requestId, const CloudBoardResult& result);
    void handleBoardWritten(quint64 requestId, const CloudBoardResult& result);
    void handleFileUploaded(quint64 requestId, const CloudFileUploadResult& result);
    void handleFileDownloaded(quint64 requestId, const CloudFileDownloadResult& result);
    OperationResult verifyLocalUnchanged() const;
    OperationResult stageMergedScreenshots();
    OperationResult saveSyncBase(
        qint64 boardVersion,
        const QString& syncedAtUtc,
        const meshcompare::CameraPoseSyncDocument& base);
    OperationResult applyMergedLocally(qint64 boardVersion);
    void finishSuccess(qint64 boardVersion);
    void finishFailure(const QString& message);
    void resetOperation();

    ICameraPoseStore& cameraStore_;
    CloudBoardClient& client_;
    ICloudApiKeyStore& apiKeyStore_;
    meshcompare::CameraSyncStateStore stateStore_;
    CameraSyncSnapshot snapshot_;
    QString activeBoardKey_;
    qint64 remoteVersion_ = 0;
    quint64 boardRequestId_ = 0;
    quint64 fileRequestId_ = 0;
    int conflictRetries_ = 0;
    bool baseFound_ = false;
    meshcompare::CameraSyncPersistedState persistedState_;
    meshcompare::CameraPoseSyncDocument localAtStart_;
    QByteArray localDigestAtStart_;
    meshcompare::CameraPoseSyncDocument merged_;
    QVector<meshcompare::CameraPoseSyncConflict> conflicts_;
    QHash<QString, QString> sourceBySha_;
    // Kept for the duration of an operation so an optimistic-lock retry can
    // reuse immutable files that were already uploaded.
    QHash<QString, meshcompare::CameraPoseSyncScreenshot> fileBySha_;
    QQueue<ScreenshotTransport> uploads_;
    QQueue<ScreenshotTransport> downloads_;
    ScreenshotTransport activeTransfer_;
    std::unique_ptr<QTemporaryDir> downloadDirectory_;
};
