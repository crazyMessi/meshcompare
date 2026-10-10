#include "camera_cloud_sync_service.h"

#include "camera_pose_store.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QMimeDatabase>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSettings>
#include <QSet>
#include <QTemporaryDir>
#include <QUuid>

#include <algorithm>
#include <utility>

namespace {
const QString DefaultBoardKey = QStringLiteral("meshcompare-camera-views");
const QRegularExpression BoardKeyPattern(
    QStringLiteral("^[A-Za-z0-9][A-Za-z0-9._-]{0,127}$"));

bool isValidApiKey(const QString& key)
{
    return key.startsWith(QStringLiteral("cb_")) && key.size() >= 8;
}

QString cloudErrorMessage(const CloudBoardError& error)
{
    if (!error.message.trimmed().isEmpty())
        return error.message.trimmed();
    if (error.httpStatus == 401)
        return QStringLiteral("The Cloud Board API key was rejected.");
    return QStringLiteral("Cloud Board could not complete the request.");
}

QByteArray documentBytes(const meshcompare::CameraPoseSyncDocument& document)
{
    QByteArray bytes;
    if (!meshcompare::serializeCameraPoseSyncDocument(document, &bytes).ok)
        return {};
    return bytes;
}

QString fileSha256(const QString& path)
{
    QFile input(path);
    if (!input.open(QIODevice::ReadOnly))
        return {};
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(&input))
        return {};
    return QString::fromLatin1(hash.result().toHex());
}

bool documentsHaveSameLocalContent(
    const meshcompare::CameraPoseSyncDocument& left,
    const meshcompare::CameraPoseSyncDocument& right)
{
    if (left.views.size() != right.views.size())
        return false;
    QHash<QString, meshcompare::CameraPoseSyncView> leftByIdentity;
    leftByIdentity.reserve(left.views.size());
    for (const auto& view : left.views)
        leftByIdentity.insert(meshcompare::cameraPoseSyncIdentity(view), view);
    for (const auto& view : right.views) {
        const auto found = leftByIdentity.constFind(
            meshcompare::cameraPoseSyncIdentity(view));
        if (found == leftByIdentity.constEnd() ||
            !meshcompare::cameraPoseSyncContentEqual(found.value(), view)) {
            return false;
        }
    }
    return true;
}
}

CameraCloudSyncService::CameraCloudSyncService(
    ICameraPoseStore& cameraStore,
    CloudBoardClient& client,
    ICloudApiKeyStore& apiKeyStore,
    QString statePath,
    QObject* parent)
    : ICameraSyncCommands(parent),
      cameraStore_(cameraStore),
      client_(client),
      apiKeyStore_(apiKeyStore),
      stateStore_(std::move(statePath))
{
    QSettings settings;
    snapshot_.boardKey = settings.value(
        QStringLiteral("camera/cloudBoardKey"), DefaultBoardKey).toString();
    if (snapshot_.boardKey.isEmpty())
        snapshot_.boardKey = DefaultBoardKey;
    const CloudApiKeyLoadResult key = apiKeyStore_.load();
    if (!key.ok) {
        snapshot_.result = OperationResult::failure(key.error);
        snapshot_.statusMessage = key.error;
    } else if (key.found) {
        snapshot_.credentialStored = true;
        const QString savedKey = key.apiKey.trimmed();
        if (isValidApiKey(savedKey)) {
            client_.setApiKey(savedKey);
            snapshot_.configured = true;
        } else {
            snapshot_.result = OperationResult::failure(
                QStringLiteral("The saved Cloud Board API key is invalid."));
            snapshot_.statusMessage = snapshot_.result.error;
        }
    }
    meshcompare::CameraSyncPersistedState state;
    bool found = false;
    if (stateStore_.load(snapshot_.boardKey, &state, &found).ok && found &&
        !state.syncedAtUtc.isEmpty()) {
        snapshot_.lastSyncAtUtc = state.syncedAtUtc;
        snapshot_.lastSyncSucceeded = true;
    }

    connect(&client_, &CloudBoardClient::boardReceived,
            this, &CameraCloudSyncService::handleBoardReceived);
    connect(&client_, &CloudBoardClient::boardWritten,
            this, &CameraCloudSyncService::handleBoardWritten);
    connect(&client_, &CloudBoardClient::fileUploaded,
            this, &CameraCloudSyncService::handleFileUploaded);
    connect(&client_, &CloudBoardClient::fileDownloaded,
            this, &CameraCloudSyncService::handleFileDownloaded);
}

CameraSyncSnapshot CameraCloudSyncService::cameraSyncSnapshot() const
{
    return snapshot_;
}

OperationResult CameraCloudSyncService::configureApiKey(const QString& apiKey)
{
    if (snapshot_.syncing)
        return OperationResult::failure(QStringLiteral("Wait for cloud sync to finish."));
    const QString key = apiKey.trimmed();
    if (!isValidApiKey(key))
        return OperationResult::failure(QStringLiteral("Enter a valid Cloud Board API key."));
    const OperationResult saved = apiKeyStore_.save(key);
    if (!saved.ok)
        return saved;
    client_.setApiKey(key);
    snapshot_.configured = true;
    snapshot_.credentialStored = true;
    snapshot_.result = OperationResult::success();
    snapshot_.statusMessage = QStringLiteral("API key saved securely.");
    emit cameraSyncStateChanged();
    return OperationResult::success();
}

OperationResult CameraCloudSyncService::clearApiKey()
{
    if (snapshot_.syncing)
        return OperationResult::failure(QStringLiteral("Wait for cloud sync to finish."));
    const OperationResult cleared = apiKeyStore_.clear();
    if (!cleared.ok)
        return cleared;
    client_.clearApiKey();
    snapshot_.configured = false;
    snapshot_.credentialStored = false;
    snapshot_.result = OperationResult::success();
    snapshot_.statusMessage = QStringLiteral("API key removed from Keychain.");
    emit cameraSyncStateChanged();
    return OperationResult::success();
}

OperationResult CameraCloudSyncService::startCameraSync(const QString& boardKey)
{
    if (snapshot_.syncing)
        return OperationResult::failure(QStringLiteral("Cloud sync is already running."));
    if (!snapshot_.configured || !client_.hasApiKey())
        return OperationResult::failure(QStringLiteral("Save a Cloud Board API key first."));
    const QString normalizedKey = boardKey.trimmed();
    if (!BoardKeyPattern.match(normalizedKey).hasMatch())
        return OperationResult::failure(QStringLiteral("Enter a valid Cloud Board key."));

    OperationResult listed;
    const QVector<SavedCameraPose> localPoses = cameraStore_.listAll(&listed);
    if (!listed.ok)
        return listed;
    meshcompare::CameraPoseSyncDocument local;
    const OperationResult built =
        meshcompare::buildCameraPoseSyncDocument(localPoses, &local);
    if (!built.ok)
        return built;
    const QByteArray localBytes = documentBytes(local);
    if (localBytes.isEmpty())
        return OperationResult::failure(QStringLiteral("Local saved views could not be prepared for sync."));

    resetOperation();
    activeBoardKey_ = normalizedKey;
    localAtStart_ = std::move(local);
    localDigestAtStart_ = QCryptographicHash::hash(localBytes, QCryptographicHash::Sha256);
    const OperationResult loaded = stateStore_.load(
        activeBoardKey_, &persistedState_, &baseFound_);
    if (!loaded.ok) {
        // The sidecar is only merge history; the camera library remains the
        // source of truth. Discard corrupt history and continue additively.
        const QString statePath = stateStore_.path();
        if (!statePath.isEmpty() && QFileInfo::exists(statePath) &&
            !QFile::remove(statePath)) {
            resetOperation();
            return OperationResult::failure(QStringLiteral(
                "Cloud sync history is unreadable and could not be reset."));
        }
        persistedState_ = {};
        baseFound_ = false;
    }
    QSettings().setValue(QStringLiteral("camera/cloudBoardKey"), activeBoardKey_);
    snapshot_.boardKey = activeBoardKey_;
    snapshot_.lastSyncAtUtc =
        baseFound_ ? persistedState_.syncedAtUtc : QString();
    snapshot_.lastSyncSucceeded = !snapshot_.lastSyncAtUtc.isEmpty();
    snapshot_.syncing = true;
    snapshot_.result = OperationResult::success();
    updateStatus(QStringLiteral("Reading Cloud Board…"));
    boardRequestId_ = client_.getBoard(activeBoardKey_);
    return OperationResult::success();
}

void CameraCloudSyncService::updateStatus(const QString& message)
{
    snapshot_.statusMessage = message;
    emit cameraSyncStateChanged();
}

OperationResult CameraCloudSyncService::loadRemoteDocument(
    const CloudBoard& board,
    meshcompare::CameraPoseSyncDocument* document) const
{
    if (board.key != activeBoardKey_) {
        return OperationResult::failure(
            QStringLiteral("Cloud Board returned data for a different board."));
    }
    if (!board.content.isObject())
        return OperationResult::failure(QStringLiteral("The Cloud Board contains data from an unsupported app."));
    return meshcompare::parseCameraPoseSyncDocument(
        QJsonDocument(board.content.toObject()).toJson(QJsonDocument::Compact),
        document);
}

void CameraCloudSyncService::adoptRemoteGeneration(
    meshcompare::CameraPoseSyncDocument* remote,
    bool boardIsMissing)
{
    const QString remoteGeneration = remote->generationId;
    if (baseFound_ &&
        (boardIsMissing || remoteGeneration.isEmpty() ||
         persistedState_.base.generationId.isEmpty() ||
         persistedState_.base.generationId != remoteGeneration)) {
        baseFound_ = false;
        persistedState_ = {};
    }
    remote->generationId = remoteGeneration.isEmpty()
        ? QUuid::createUuid().toString(QUuid::WithoutBraces).toLower()
        : remoteGeneration;
}

void CameraCloudSyncService::handleBoardReceived(
    quint64 requestId,
    const CloudBoardResult& result)
{
    if (!snapshot_.syncing || requestId != boardRequestId_)
        return;
    meshcompare::CameraPoseSyncDocument remote;
    qint64 version = 0;
    if (result.missing) {
        adoptRemoteGeneration(&remote, true);
    } else if (!result.ok) {
        finishFailure(cloudErrorMessage(result.error));
        return;
    } else {
        const OperationResult parsed = loadRemoteDocument(result.board, &remote);
        if (!parsed.ok) {
            finishFailure(parsed.error);
            return;
        }
        version = result.board.version;
        adoptRemoteGeneration(&remote, false);
        if (baseFound_ && version < persistedState_.boardVersion) {
            // The key now refers to an older board generation, so the prior
            // three-way base is no longer authoritative.
            baseFound_ = false;
            persistedState_ = {};
        }
    }
    mergeAndTransfer(remote, version);
}

void CameraCloudSyncService::mergeAndTransfer(
    const meshcompare::CameraPoseSyncDocument& remote,
    qint64 remoteVersion)
{
    meshcompare::CameraPoseSyncMergeResult merge;
    const OperationResult merged = meshcompare::mergeCameraPoseSyncDocuments(
        baseFound_ ? &persistedState_.base : nullptr,
        localAtStart_, remote, &merge);
    if (!merged.ok) {
        finishFailure(merged.error);
        return;
    }
    merged_ = std::move(merge.document);
    conflicts_ = std::move(merge.conflicts);
    remoteVersion_ = remoteVersion;
    const auto remember = [this](const meshcompare::CameraPoseSyncDocument& document) {
        for (const auto& view : document.views) {
            const auto& screenshot = view.screenshot;
            if (!screenshot.sha256.isEmpty() && !screenshot.sourcePath.isEmpty())
                sourceBySha_.insert(screenshot.sha256, screenshot.sourcePath);
            if (!screenshot.sha256.isEmpty() && !screenshot.fileId.isEmpty() &&
                !fileBySha_.contains(screenshot.sha256)) {
                fileBySha_.insert(screenshot.sha256, screenshot);
            }
        }
    };
    remember(localAtStart_);
    remember(remote);
    if (baseFound_)
        remember(persistedState_.base);
    for (auto& view : merged_.views) {
        auto& screenshot = view.screenshot;
        if (screenshot.sha256.isEmpty())
            continue;
        if (screenshot.sourcePath.isEmpty())
            screenshot.sourcePath = sourceBySha_.value(screenshot.sha256);
        if (screenshot.fileId.isEmpty() && fileBySha_.contains(screenshot.sha256)) {
            const auto transport = fileBySha_.value(screenshot.sha256);
            if (transport.mimeType != screenshot.mimeType ||
                transport.size != screenshot.size) {
                finishFailure(QStringLiteral(
                    "Cloud camera data contains inconsistent screenshot metadata."));
                return;
            }
            screenshot.fileId = transport.fileId;
        }
    }
    prepareTransfers();
}

void CameraCloudSyncService::prepareTransfers()
{
    uploads_.clear();
    downloads_.clear();
    QSet<QString> uploadHashes;
    QSet<QString> downloadHashes;
    for (const auto& view : merged_.views) {
        const auto& screenshot = view.screenshot;
        if (screenshot.sha256.isEmpty())
            continue;
        if (screenshot.fileId.isEmpty()) {
            if (screenshot.sourcePath.isEmpty()) {
                finishFailure(QStringLiteral("A merged screenshot has no local or cloud copy."));
                return;
            }
            if (!uploadHashes.contains(screenshot.sha256)) {
                uploadHashes.insert(screenshot.sha256);
                uploads_.enqueue({screenshot.sha256, screenshot.sourcePath, {}});
            }
        } else if (screenshot.sourcePath.isEmpty() &&
                   !downloadHashes.contains(screenshot.sha256)) {
            downloadHashes.insert(screenshot.sha256);
            downloads_.enqueue({screenshot.sha256, {}, screenshot.fileId});
        }
    }
    if (!downloads_.isEmpty() && !downloadDirectory_)
        downloadDirectory_.reset(new QTemporaryDir);
    if (downloadDirectory_ && !downloadDirectory_->isValid()) {
        finishFailure(QStringLiteral("A temporary folder for cloud screenshots could not be created."));
        return;
    }
    startNextUpload();
}

void CameraCloudSyncService::startNextUpload()
{
    if (uploads_.isEmpty()) {
        startNextDownload();
        return;
    }
    activeTransfer_ = uploads_.dequeue();
    updateStatus(QStringLiteral("Uploading screenshots…"));
    fileRequestId_ = client_.uploadFile(activeTransfer_.sourcePath);
}

void CameraCloudSyncService::handleFileUploaded(
    quint64 requestId,
    const CloudFileUploadResult& result)
{
    if (!snapshot_.syncing || requestId != fileRequestId_)
        return;
    if (!result.ok) {
        finishFailure(cloudErrorMessage(result.error));
        return;
    }
    bool matchedScreenshot = false;
    for (const auto& view : merged_.views) {
        if (view.screenshot.sha256 != activeTransfer_.sha256)
            continue;
        matchedScreenshot = true;
        if (view.screenshot.size != result.file.size ||
            view.screenshot.mimeType != result.file.mimeType) {
            finishFailure(QStringLiteral(
                "Cloud Board returned inconsistent screenshot metadata."));
            return;
        }
    }
    if (!matchedScreenshot) {
        finishFailure(QStringLiteral(
            "Cloud Board returned an unexpected screenshot upload."));
        return;
    }
    meshcompare::CameraPoseSyncScreenshot transport;
    transport.sha256 = activeTransfer_.sha256;
    transport.fileId = result.file.fileId;
    transport.mimeType = result.file.mimeType;
    transport.size = result.file.size;
    fileBySha_.insert(activeTransfer_.sha256, transport);
    for (auto& view : merged_.views) {
        if (view.screenshot.sha256 == activeTransfer_.sha256) {
            view.screenshot.fileId = result.file.fileId;
            view.screenshot.mimeType = result.file.mimeType;
            view.screenshot.size = result.file.size;
        }
    }
    startNextUpload();
}

void CameraCloudSyncService::startNextDownload()
{
    if (downloads_.isEmpty()) {
        putMergedBoard();
        return;
    }
    activeTransfer_ = downloads_.dequeue();
    activeTransfer_.sourcePath = downloadDirectory_->filePath(
        activeTransfer_.sha256 + QStringLiteral(".image"));
    updateStatus(QStringLiteral("Downloading screenshots…"));
    fileRequestId_ = client_.downloadFile(
        activeTransfer_.fileId, activeTransfer_.sourcePath);
}

void CameraCloudSyncService::handleFileDownloaded(
    quint64 requestId,
    const CloudFileDownloadResult& result)
{
    if (!snapshot_.syncing || requestId != fileRequestId_)
        return;
    if (!result.ok) {
        finishFailure(cloudErrorMessage(result.error));
        return;
    }
    if (fileSha256(activeTransfer_.sourcePath) != activeTransfer_.sha256) {
        finishFailure(QStringLiteral("A downloaded screenshot failed its integrity check."));
        return;
    }
    bool matchedScreenshot = false;
    const QString downloadedMime = QMimeDatabase().mimeTypeForFile(
        activeTransfer_.sourcePath, QMimeDatabase::MatchContent).name();
    for (const auto& view : merged_.views) {
        if (view.screenshot.sha256 != activeTransfer_.sha256)
            continue;
        matchedScreenshot = true;
        if (view.screenshot.size != result.size ||
            view.screenshot.mimeType != downloadedMime) {
            finishFailure(QStringLiteral(
                "A downloaded screenshot has inconsistent metadata."));
            return;
        }
    }
    if (!matchedScreenshot) {
        finishFailure(QStringLiteral(
            "Cloud Board returned an unexpected screenshot download."));
        return;
    }
    sourceBySha_.insert(activeTransfer_.sha256, activeTransfer_.sourcePath);
    for (auto& view : merged_.views) {
        if (view.screenshot.sha256 == activeTransfer_.sha256)
            view.screenshot.sourcePath = activeTransfer_.sourcePath;
    }
    startNextDownload();
}

OperationResult CameraCloudSyncService::verifyLocalUnchanged() const
{
    OperationResult listed;
    const QVector<SavedCameraPose> current = cameraStore_.listAll(&listed);
    if (!listed.ok)
        return listed;
    meshcompare::CameraPoseSyncDocument document;
    const OperationResult built = meshcompare::buildCameraPoseSyncDocument(current, &document);
    if (!built.ok)
        return built;
    const QByteArray bytes = documentBytes(document);
    if (bytes.isEmpty() ||
        QCryptographicHash::hash(bytes, QCryptographicHash::Sha256) != localDigestAtStart_) {
        return OperationResult::failure(QStringLiteral(
            "Saved views changed during cloud sync. Run Sync Now again."));
    }
    return OperationResult::success();
}

void CameraCloudSyncService::putMergedBoard()
{
    const OperationResult unchanged = verifyLocalUnchanged();
    if (!unchanged.ok) {
        finishFailure(unchanged.error);
        return;
    }
    QByteArray bytes;
    const OperationResult serialized =
        meshcompare::serializeCameraPoseSyncDocument(merged_, &bytes);
    if (!serialized.ok) {
        finishFailure(serialized.error);
        return;
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(bytes, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        finishFailure(QStringLiteral("Cloud sync data could not be encoded."));
        return;
    }
    updateStatus(QStringLiteral("Saving Cloud Board…"));
    boardRequestId_ = client_.putBoard(
        activeBoardKey_, document.object(), remoteVersion_);
}

void CameraCloudSyncService::handleBoardWritten(
    quint64 requestId,
    const CloudBoardResult& result)
{
    if (!snapshot_.syncing || requestId != boardRequestId_)
        return;
    if (!result.ok) {
        if (result.conflict && result.hasCurrentBoard && conflictRetries_ < 2) {
            ++conflictRetries_;
            meshcompare::CameraPoseSyncDocument current;
            const OperationResult parsed = loadRemoteDocument(result.currentBoard, &current);
            if (!parsed.ok) {
                finishFailure(parsed.error);
                return;
            }
            adoptRemoteGeneration(&current, false);
            if (baseFound_ &&
                result.currentBoard.version < persistedState_.boardVersion) {
                // The board was replaced between GET and PUT. Its contents
                // cannot be compared against the prior generation's base.
                baseFound_ = false;
                persistedState_ = {};
            }
            updateStatus(QStringLiteral("Cloud changed; merging again…"));
            mergeAndTransfer(current, result.currentBoard.version);
            return;
        }
        finishFailure(cloudErrorMessage(result.error));
        return;
    }
    if (result.board.key != activeBoardKey_ || result.board.version < 1) {
        finishFailure(QStringLiteral(
            "Cloud Board returned an invalid save confirmation."));
        return;
    }
    const OperationResult unchanged = verifyLocalUnchanged();
    if (!unchanged.ok) {
        meshcompare::CameraPoseSyncDocument pendingBase = localAtStart_;
        pendingBase.generationId = merged_.generationId;
        const OperationResult pending = saveSyncBase(
            result.board.version, snapshot_.lastSyncAtUtc, pendingBase);
        if (!pending.ok) {
            finishFailure(QStringLiteral(
                "Cloud was saved and local views changed, but recovery state could not be saved: %1. Run Sync Now again.")
                              .arg(pending.error));
        } else {
            finishFailure(QStringLiteral("Cloud was saved, but %1")
                              .arg(unchanged.error));
        }
        return;
    }
    const OperationResult applied = applyMergedLocally(result.board.version);
    if (!applied.ok) {
        finishFailure(QStringLiteral(
            "Cloud was saved, but local sync could not be completed: %1. Run Sync Now again.")
            .arg(applied.error));
        return;
    }
    finishSuccess(result.board.version);
}

OperationResult CameraCloudSyncService::stageMergedScreenshots()
{
    if (!downloadDirectory_)
        downloadDirectory_.reset(new QTemporaryDir);
    if (!downloadDirectory_->isValid()) {
        return OperationResult::failure(QStringLiteral(
            "A temporary folder for synchronized screenshots could not be created."));
    }

    QHash<QString, QString> stagedBySha;
    QHash<QString, QString> stagedMimeBySha;
    QMimeDatabase mimeDatabase;
    for (auto& view : merged_.views) {
        auto& screenshot = view.screenshot;
        if (screenshot.sha256.isEmpty())
            continue;
        if (screenshot.sourcePath.isEmpty()) {
            return OperationResult::failure(QStringLiteral(
                "A synchronized screenshot is unavailable locally."));
        }
        const auto existing = stagedBySha.constFind(screenshot.sha256);
        if (existing != stagedBySha.constEnd()) {
            if (QFileInfo(existing.value()).size() != screenshot.size ||
                stagedMimeBySha.value(screenshot.sha256) != screenshot.mimeType) {
                return OperationResult::failure(QStringLiteral(
                    "Synchronized screenshot metadata is inconsistent."));
            }
            screenshot.sourcePath = existing.value();
            continue;
        }

        QFile input(screenshot.sourcePath);
        if (!input.open(QIODevice::ReadOnly)) {
            return OperationResult::failure(QStringLiteral(
                "A synchronized screenshot could not be staged."));
        }
        const QString detectedMime = mimeDatabase.mimeTypeForFile(
            screenshot.sourcePath, QMimeDatabase::MatchContent).name();
        if (detectedMime != screenshot.mimeType) {
            return OperationResult::failure(QStringLiteral(
                "Synchronized screenshot metadata is inconsistent."));
        }
        const QString stagedPath = downloadDirectory_->filePath(
            QStringLiteral("staged-") + screenshot.sha256 + QStringLiteral(".image"));
        QSaveFile output(stagedPath);
        if (!output.open(QIODevice::WriteOnly)) {
            return OperationResult::failure(QStringLiteral(
                "A synchronized screenshot could not be staged."));
        }
        QCryptographicHash hash(QCryptographicHash::Sha256);
        qint64 copied = 0;
        while (!input.atEnd()) {
            const QByteArray chunk = input.read(64 * 1024);
            if (chunk.isEmpty() && input.error() != QFileDevice::NoError) {
                output.cancelWriting();
                return OperationResult::failure(QStringLiteral(
                    "A synchronized screenshot could not be staged."));
            }
            if (chunk.isEmpty())
                break;
            hash.addData(chunk);
            copied += chunk.size();
            if (output.write(chunk) != chunk.size()) {
                output.cancelWriting();
                return OperationResult::failure(QStringLiteral(
                    "A synchronized screenshot could not be staged."));
            }
        }
        if (input.error() != QFileDevice::NoError ||
            copied != screenshot.size ||
            QString::fromLatin1(hash.result().toHex()) != screenshot.sha256) {
            output.cancelWriting();
            return OperationResult::failure(QStringLiteral(
                "A synchronized screenshot changed before it could be imported."));
        }
        if (!output.commit()) {
            return OperationResult::failure(QStringLiteral(
                "A synchronized screenshot could not be staged."));
        }
        stagedBySha.insert(screenshot.sha256, stagedPath);
        stagedMimeBySha.insert(screenshot.sha256, detectedMime);
        screenshot.sourcePath = stagedPath;
    }
    return OperationResult::success();
}

OperationResult CameraCloudSyncService::saveSyncBase(
    qint64 boardVersion,
    const QString& syncedAtUtc,
    const meshcompare::CameraPoseSyncDocument& base)
{
    meshcompare::CameraSyncPersistedState state;
    state.boardKey = activeBoardKey_;
    state.boardVersion = boardVersion;
    state.syncedAtUtc = syncedAtUtc;
    state.base = base;
    for (auto& view : state.base.views)
        view.screenshot.sourcePath.clear();
    const OperationResult saved = stateStore_.save(state);
    if (saved.ok && !syncedAtUtc.isEmpty())
        snapshot_.lastSyncAtUtc = syncedAtUtc;
    return saved;
}

OperationResult CameraCloudSyncService::applyMergedLocally(qint64 boardVersion)
{
    if (!documentsHaveSameLocalContent(localAtStart_, merged_)) {
        const OperationResult staged = stageMergedScreenshots();
        if (!staged.ok)
            return staged;
        QVector<SavedCameraPose> poses;
        poses.reserve(merged_.views.size());
        for (const auto& view : merged_.views) {
            if (!view.screenshot.sha256.isEmpty() && view.screenshot.sourcePath.isEmpty()) {
                return OperationResult::failure(QStringLiteral(
                    "A synchronized screenshot is unavailable locally."));
            }
            poses.append({view.workspaceUuid, view.viewId, view.savedAtUtc,
                          view.tags, view.screenshot.sourcePath,
                          CameraPose{view.poseXml}, view.note});
        }
        const OperationResult replaced = cameraStore_.replaceAllForSync(poses);
        if (!replaced.ok)
            return replaced;
    }
    return saveSyncBase(
        boardVersion,
        QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs),
        merged_);
}

void CameraCloudSyncService::finishSuccess(qint64)
{
    snapshot_.syncing = false;
    snapshot_.lastSyncSucceeded = true;
    snapshot_.result = OperationResult::success();
    snapshot_.statusMessage = conflicts_.isEmpty()
        ? QStringLiteral("%1 saved views are in sync.").arg(merged_.views.size())
        : QStringLiteral("%1 saved views are in sync; %2 conflict copies were preserved.")
              .arg(merged_.views.size()).arg(conflicts_.size());
    emit cameraSyncStateChanged();
    emit cameraSyncCompleted();
    resetOperation();
}

void CameraCloudSyncService::finishFailure(const QString& message)
{
    snapshot_.syncing = false;
    snapshot_.result = OperationResult::failure(message);
    snapshot_.statusMessage = message;
    emit cameraSyncStateChanged();
    emit cameraSyncCompleted();
    resetOperation();
}

void CameraCloudSyncService::resetOperation()
{
    activeBoardKey_.clear();
    remoteVersion_ = 0;
    boardRequestId_ = 0;
    fileRequestId_ = 0;
    conflictRetries_ = 0;
    baseFound_ = false;
    persistedState_ = {};
    localAtStart_ = {};
    localDigestAtStart_.clear();
    merged_ = {};
    conflicts_.clear();
    sourceBySha_.clear();
    fileBySha_.clear();
    uploads_.clear();
    downloads_.clear();
    activeTransfer_ = {};
    downloadDirectory_.reset();
}
