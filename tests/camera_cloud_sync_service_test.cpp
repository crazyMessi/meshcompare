#include <QtTest>

#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QImage>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QNetworkAccessManager>
#include <QSettings>
#include <QSharedPointer>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>

#include "services/camera_cloud_sync_service.h"
#include "services/camera_pose_store.h"

#include <functional>
#include <utility>

namespace {

const QString BoardKey = QStringLiteral("meshcompare-camera-views");

struct HttpResponse {
    int status = 200;
    QByteArray reason = QByteArrayLiteral("OK");
    QByteArray contentType = QByteArrayLiteral("application/json");
    QByteArray body;
};

struct HttpRequest {
    QByteArray method;
    QByteArray target;
    QMap<QByteArray, QByteArray> headers;
    QByteArray body;
};

class TestHttpServer final : public QTcpServer
{
    Q_OBJECT

public:
    explicit TestHttpServer(QObject* parent = nullptr) : QTcpServer(parent)
    {
        connect(this, &QTcpServer::newConnection, this, [this]() {
            while (hasPendingConnections())
                watch(nextPendingConnection());
        });
    }

    bool start()
    {
        return listen(QHostAddress::LocalHost, 0);
    }

    QUrl baseUrl() const
    {
        return QUrl(QStringLiteral("http://127.0.0.1:%1/cloud/api")
                        .arg(serverPort()));
    }

    void enqueue(HttpResponse response)
    {
        responses_.append(std::move(response));
    }

    const QVector<HttpRequest>& requests() const { return requests_; }

signals:
    void requestCaptured();

private:
    void watch(QTcpSocket* socket)
    {
        QSharedPointer<QByteArray> bytes(new QByteArray);
        connect(socket, &QTcpSocket::readyRead, this, [this, socket, bytes]() {
            bytes->append(socket->readAll());
            const int headerEnd = bytes->indexOf("\r\n\r\n");
            if (headerEnd < 0)
                return;
            const QList<QByteArray> lines = bytes->left(headerEnd).split('\n');
            if (lines.isEmpty())
                return;
            const QList<QByteArray> requestLine = lines.first().trimmed().split(' ');
            if (requestLine.size() < 2)
                return;

            HttpRequest request;
            request.method = requestLine.at(0);
            request.target = requestLine.at(1);
            for (int index = 1; index < lines.size(); ++index) {
                const QByteArray line = lines.at(index).trimmed();
                const int colon = line.indexOf(':');
                if (colon > 0) {
                    request.headers.insert(
                        line.left(colon).trimmed().toLower(),
                        line.mid(colon + 1).trimmed());
                }
            }
            const int bodyLength = request.headers.value(
                QByteArrayLiteral("content-length")).toInt();
            const int requestLength = headerEnd + 4 + bodyLength;
            if (bytes->size() < requestLength)
                return;
            request.body = bytes->mid(headerEnd + 4, bodyLength);
            requests_.append(request);
            emit requestCaptured();

            if (responses_.isEmpty()) {
                socket->disconnectFromHost();
                return;
            }
            const HttpResponse response = responses_.takeFirst();
            const QByteArray output = QByteArrayLiteral("HTTP/1.1 ") +
                QByteArray::number(response.status) + ' ' + response.reason +
                QByteArrayLiteral("\r\nContent-Type: ") + response.contentType +
                QByteArrayLiteral("\r\nContent-Length: ") +
                QByteArray::number(response.body.size()) +
                QByteArrayLiteral("\r\nConnection: close\r\n\r\n") + response.body;
            socket->write(output);
            socket->disconnectFromHost();
        });
        connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
    }

    QVector<HttpResponse> responses_;
    QVector<HttpRequest> requests_;
};

class FakeApiKeyStore final : public ICloudApiKeyStore
{
public:
    CloudApiKeyLoadResult load() const override
    {
        CloudApiKeyLoadResult result;
        result.found = true;
        result.apiKey = QStringLiteral("cb_test_key");
        return result;
    }

    OperationResult save(const QString& apiKey) override
    {
        savedKey = apiKey;
        return OperationResult::success();
    }

    OperationResult clear() override
    {
        savedKey.clear();
        return OperationResult::success();
    }

    QString savedKey;
};

class SyncCameraPoseStore final : public ICameraPoseStore
{
public:
    OperationResult migrateLegacyIfNeeded() override
    {
        return OperationResult::success();
    }

    OperationResult saveWithScreenshot(
        const QString&,
        const CameraPose&,
        const QImage&,
        QString*,
        const QStringList&,
        QString*,
        const QString&) override
    {
        return OperationResult::failure(QStringLiteral("Not implemented in this test."));
    }

    QVector<SavedCameraPose> list(
        const QString& uuid,
        OperationResult* result) const override
    {
        QVector<SavedCameraPose> matching;
        for (const SavedCameraPose& pose : records) {
            if (pose.uuid == uuid)
                matching.append(pose);
        }
        if (result != nullptr)
            *result = OperationResult::success();
        return matching;
    }

    QVector<SavedCameraPose> listAll(OperationResult* result) const override
    {
        if (result != nullptr)
            *result = OperationResult::success();
        return records;
    }

    OperationResult replaceAllForSync(
        const QVector<SavedCameraPose>& poses) override
    {
        ++replaceCount;
        if (beforeReplace)
            beforeReplace();
        appliedScreenshotBytes.clear();
        for (const SavedCameraPose& pose : poses) {
            if (pose.screenshotPath.isEmpty())
                continue;
            QFile input(pose.screenshotPath);
            if (!input.open(QIODevice::ReadOnly)) {
                return OperationResult::failure(
                    QStringLiteral("A synchronized screenshot is unreadable."));
            }
            appliedScreenshotBytes.insert(pose.viewId, input.readAll());
        }
        records = poses;
        return OperationResult::success();
    }

    OperationResult load(
        const QString& uuid,
        const QString& viewId,
        CameraPose* pose) const override
    {
        for (const SavedCameraPose& record : records) {
            if (record.uuid == uuid && record.viewId == viewId) {
                if (pose != nullptr)
                    *pose = record.pose;
                return OperationResult::success();
            }
        }
        return OperationResult::failure(QStringLiteral("Pose not found."));
    }

    OperationResult setTags(
        const QString&,
        const QString&,
        const QStringList&) override
    {
        return OperationResult::failure(QStringLiteral("Not implemented in this test."));
    }

    OperationResult setNote(
        const QString&,
        const QString&,
        const QString&) override
    {
        return OperationResult::failure(QStringLiteral("Not implemented in this test."));
    }

    OperationResult setDetails(
        const QString&,
        const QString&,
        const QStringList&,
        const QString&) override
    {
        return OperationResult::failure(QStringLiteral("Not implemented in this test."));
    }

    OperationResult remove(const QString&, const QString&) override
    {
        return OperationResult::failure(QStringLiteral("Not implemented in this test."));
    }

    QVector<SavedCameraPose> records;
    QHash<QString, QByteArray> appliedScreenshotBytes;
    std::function<void()> beforeReplace;
    int replaceCount = 0;
};

SavedCameraPose savedPose(
    const QString& workspace,
    const QString& viewId,
    const QString& poseXml,
    const QString& screenshotPath = {})
{
    return {workspace,
            viewId,
            QStringLiteral("2026-10-10T08:00:00.000Z"),
            {QStringLiteral("inspection")},
            screenshotPath,
            CameraPose{poseXml},
            QStringLiteral("review note")};
}

QJsonObject syncContent(const meshcompare::CameraPoseSyncDocument& document)
{
    QByteArray bytes;
    const OperationResult result =
        meshcompare::serializeCameraPoseSyncDocument(document, &bytes);
    if (!result.ok)
        return {};
    return QJsonDocument::fromJson(bytes).object();
}

QByteArray boardResponse(
    const QString& key,
    const QJsonObject& content,
    qint64 version)
{
    QJsonObject object;
    object.insert(QStringLiteral("key"), key);
    object.insert(QStringLiteral("content"), content);
    object.insert(QStringLiteral("version"), static_cast<double>(version));
    object.insert(
        QStringLiteral("updatedAt"),
        QStringLiteral("2026-10-10T08:00:00.000Z"));
    return QJsonDocument(object).toJson(QJsonDocument::Compact);
}

QByteArray conflictResponse(
    const QString& key,
    const QJsonObject& content,
    qint64 version)
{
    QJsonObject current;
    current.insert(QStringLiteral("key"), key);
    current.insert(QStringLiteral("content"), content);
    current.insert(QStringLiteral("version"), static_cast<double>(version));
    current.insert(
        QStringLiteral("updatedAt"),
        QStringLiteral("2026-10-10T08:00:00.000Z"));
    QJsonObject error;
    error.insert(QStringLiteral("code"), QStringLiteral("version_conflict"));
    error.insert(QStringLiteral("message"), QStringLiteral("Board changed"));
    QJsonObject response;
    response.insert(QStringLiteral("error"), error);
    response.insert(QStringLiteral("current"), current);
    return QJsonDocument(response).toJson(QJsonDocument::Compact);
}

QByteArray fileUploadResponse(const QString& fileId, qint64 size)
{
    QJsonObject file;
    file.insert(QStringLiteral("fileId"), fileId);
    file.insert(
        QStringLiteral("url"),
        QStringLiteral("https://memo.gfyh.fun/cloud/api/files/") + fileId);
    file.insert(QStringLiteral("mimeType"), QStringLiteral("image/png"));
    file.insert(QStringLiteral("size"), static_cast<double>(size));
    file.insert(
        QStringLiteral("createdAt"),
        QStringLiteral("2026-10-10T08:00:00.000Z"));
    return QJsonDocument(file).toJson(QJsonDocument::Compact);
}

QByteArray readAll(const QString& path)
{
    QFile input(path);
    if (!input.open(QIODevice::ReadOnly))
        return {};
    return input.readAll();
}

} // namespace

class CameraCloudSyncServiceTest final : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase()
    {
        QCoreApplication::setOrganizationName(QStringLiteral("MeshCompareTests"));
        QCoreApplication::setApplicationName(
            QStringLiteral("CameraCloudSyncServiceTest"));
    }

    void init()
    {
        QSettings().clear();
    }

    void missingRemoteBoardDoesNotDeleteViewsFromAStaleBase()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString statePath = directory.filePath(QStringLiteral("sync-state.json"));

        SyncCameraPoseStore store;
        store.records.append(savedPose(
            QStringLiteral("workspace-a"),
            QStringLiteral("view_001"),
            QStringLiteral("<camera>local</camera>")));
        meshcompare::CameraPoseSyncDocument base;
        const OperationResult built =
            meshcompare::buildCameraPoseSyncDocument(store.records, &base);
        QVERIFY2(built.ok, qPrintable(built.error));
        meshcompare::CameraSyncPersistedState previous;
        previous.boardKey = BoardKey;
        previous.boardVersion = 7;
        previous.syncedAtUtc = QStringLiteral("2026-10-10T07:00:00.000Z");
        previous.base = base;
        meshcompare::CameraSyncStateStore stateStore(statePath);
        QVERIFY(stateStore.save(previous).ok);

        TestHttpServer server;
        QVERIFY(server.start());
        server.enqueue({
            404,
            QByteArrayLiteral("Not Found"),
            QByteArrayLiteral("application/json"),
            QByteArrayLiteral("{\"error\":{\"code\":\"not_found\"}}")});
        server.enqueue({
            201,
            QByteArrayLiteral("Created"),
            QByteArrayLiteral("application/json"),
            boardResponse(BoardKey, QJsonObject{}, 1)});

        QNetworkAccessManager network;
        CloudBoardClient client(&network);
        client.setBaseUrl(server.baseUrl());
        FakeApiKeyStore keys;
        CameraCloudSyncService service(store, client, keys, statePath);
        QSignalSpy completed(&service, &ICameraSyncCommands::cameraSyncCompleted);

        const OperationResult started = service.startCameraSync(BoardKey);
        QVERIFY2(started.ok, qPrintable(started.error));
        QTRY_COMPARE(completed.size(), 1);
        QVERIFY2(
            service.cameraSyncSnapshot().lastSyncSucceeded,
            qPrintable(service.cameraSyncSnapshot().statusMessage));
        QCOMPARE(store.replaceCount, 0);
        QCOMPARE(store.records.size(), 1);
        QCOMPARE(store.records.front().viewId, QStringLiteral("view_001"));
        QCOMPARE(server.requests().size(), 2);
        QCOMPARE(server.requests().at(0).method, QByteArrayLiteral("GET"));
        QCOMPARE(server.requests().at(1).method, QByteArrayLiteral("PUT"));

        const QJsonObject put = QJsonDocument::fromJson(
            server.requests().at(1).body).object();
        QCOMPARE(put.value(QStringLiteral("version")).toInt(), 0);
        meshcompare::CameraPoseSyncDocument uploaded;
        const OperationResult parsed = meshcompare::parseCameraPoseSyncDocument(
            QJsonDocument(put.value(QStringLiteral("content")).toObject())
                .toJson(QJsonDocument::Compact),
            &uploaded);
        QVERIFY2(parsed.ok, qPrintable(parsed.error));
        QCOMPARE(uploaded.views.size(), 1);

        meshcompare::CameraSyncPersistedState saved;
        bool found = false;
        QVERIFY(stateStore.load(BoardKey, &saved, &found).ok);
        QVERIFY(found);
        QCOMPARE(saved.boardVersion, qint64(1));
        QCOMPARE(saved.base.views.size(), 1);
        QVERIFY(!saved.base.generationId.isEmpty());
    }

    void initialLocalOnlySyncDoesNotRewriteScreenshotLibrary()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString imagePath = directory.filePath(QStringLiteral("local.png"));
        QImage image(2, 2, QImage::Format_ARGB32);
        image.fill(Qt::green);
        QVERIFY(image.save(imagePath));

        SyncCameraPoseStore store;
        store.records.append(savedPose(
            QStringLiteral("workspace-a"),
            QStringLiteral("view_001"),
            QStringLiteral("<camera>local</camera>"),
            imagePath));
        TestHttpServer server;
        QVERIFY(server.start());
        server.enqueue({
            404,
            QByteArrayLiteral("Not Found"),
            QByteArrayLiteral("application/json"),
            QByteArrayLiteral("{\"error\":{\"code\":\"not_found\"}}")});
        server.enqueue({
            201,
            QByteArrayLiteral("Created"),
            QByteArrayLiteral("application/json"),
            fileUploadResponse(
                QStringLiteral("local-file"), QFileInfo(imagePath).size())});
        server.enqueue({
            201,
            QByteArrayLiteral("Created"),
            QByteArrayLiteral("application/json"),
            boardResponse(BoardKey, QJsonObject{}, 1)});

        QNetworkAccessManager network;
        CloudBoardClient client(&network);
        client.setBaseUrl(server.baseUrl());
        FakeApiKeyStore keys;
        const QString statePath = directory.filePath(QStringLiteral("sync-state.json"));
        CameraCloudSyncService service(store, client, keys, statePath);
        QSignalSpy completed(&service, &ICameraSyncCommands::cameraSyncCompleted);

        QVERIFY(service.startCameraSync(BoardKey).ok);
        QTRY_COMPARE(completed.size(), 1);
        QVERIFY2(
            service.cameraSyncSnapshot().lastSyncSucceeded,
            qPrintable(service.cameraSyncSnapshot().statusMessage));
        QCOMPARE(store.replaceCount, 0);
        QCOMPARE(store.records.size(), 1);
        QCOMPARE(store.records.front().screenshotPath, imagePath);

        const QJsonObject put = QJsonDocument::fromJson(
            server.requests().at(2).body).object();
        meshcompare::CameraPoseSyncDocument uploaded;
        const OperationResult parsed = meshcompare::parseCameraPoseSyncDocument(
            QJsonDocument(put.value(QStringLiteral("content")).toObject())
                .toJson(QJsonDocument::Compact),
            &uploaded);
        QVERIFY2(parsed.ok, qPrintable(parsed.error));
        QVERIFY(!uploaded.generationId.isEmpty());
        QCOMPARE(
            uploaded.views.front().screenshot.fileId,
            QStringLiteral("local-file"));

        meshcompare::CameraSyncStateStore stateStore(statePath);
        meshcompare::CameraSyncPersistedState state;
        bool found = false;
        QVERIFY(stateStore.load(BoardKey, &state, &found).ok);
        QVERIFY(found);
        QCOMPARE(state.base.generationId, uploaded.generationId);
    }

    void corruptSyncHistoryIsDiscardedAndRebuilt()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString statePath = directory.filePath(QStringLiteral("sync-state.json"));
        QFile corrupt(statePath);
        QVERIFY(corrupt.open(QIODevice::WriteOnly));
        QCOMPARE(corrupt.write(QByteArrayLiteral("{broken")), qint64(7));
        corrupt.close();

        SyncCameraPoseStore store;
        store.records.append(savedPose(
            QStringLiteral("workspace-a"),
            QStringLiteral("view_001"),
            QStringLiteral("<camera>local</camera>")));
        TestHttpServer server;
        QVERIFY(server.start());
        server.enqueue({
            404,
            QByteArrayLiteral("Not Found"),
            QByteArrayLiteral("application/json"),
            QByteArrayLiteral("{\"error\":{\"code\":\"not_found\"}}")});
        server.enqueue({
            201,
            QByteArrayLiteral("Created"),
            QByteArrayLiteral("application/json"),
            boardResponse(BoardKey, QJsonObject{}, 1)});

        QNetworkAccessManager network;
        CloudBoardClient client(&network);
        client.setBaseUrl(server.baseUrl());
        FakeApiKeyStore keys;
        CameraCloudSyncService service(store, client, keys, statePath);
        QSignalSpy completed(&service, &ICameraSyncCommands::cameraSyncCompleted);

        const OperationResult started = service.startCameraSync(BoardKey);
        QVERIFY2(started.ok, qPrintable(started.error));
        QTRY_COMPARE(completed.size(), 1);
        QVERIFY2(
            service.cameraSyncSnapshot().lastSyncSucceeded,
            qPrintable(service.cameraSyncSnapshot().statusMessage));
        QCOMPARE(store.replaceCount, 0);

        meshcompare::CameraSyncStateStore stateStore(statePath);
        meshcompare::CameraSyncPersistedState state;
        bool found = false;
        const OperationResult loaded = stateStore.load(BoardKey, &state, &found);
        QVERIFY2(loaded.ok, qPrintable(loaded.error));
        QVERIFY(found);
        QCOMPARE(state.boardVersion, qint64(1));
        QCOMPARE(state.base.views.size(), 1);
    }

    void inconsistentUploadMetadataStopsBeforeBoardWrite()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString imagePath = directory.filePath(QStringLiteral("local.png"));
        QImage image(2, 2, QImage::Format_ARGB32);
        image.fill(Qt::yellow);
        QVERIFY(image.save(imagePath));

        SyncCameraPoseStore store;
        store.records.append(savedPose(
            QStringLiteral("workspace-a"),
            QStringLiteral("view_001"),
            QStringLiteral("<camera>local</camera>"),
            imagePath));
        TestHttpServer server;
        QVERIFY(server.start());
        server.enqueue({
            404,
            QByteArrayLiteral("Not Found"),
            QByteArrayLiteral("application/json"),
            QByteArrayLiteral("{\"error\":{\"code\":\"not_found\"}}")});
        server.enqueue({
            201,
            QByteArrayLiteral("Created"),
            QByteArrayLiteral("application/json"),
            fileUploadResponse(
                QStringLiteral("local-file"),
                QFileInfo(imagePath).size() + 1)});

        QNetworkAccessManager network;
        CloudBoardClient client(&network);
        client.setBaseUrl(server.baseUrl());
        FakeApiKeyStore keys;
        CameraCloudSyncService service(
            store,
            client,
            keys,
            directory.filePath(QStringLiteral("sync-state.json")));
        QSignalSpy completed(&service, &ICameraSyncCommands::cameraSyncCompleted);

        QVERIFY(service.startCameraSync(BoardKey).ok);
        QTRY_COMPARE(completed.size(), 1);
        QVERIFY(!service.cameraSyncSnapshot().result.ok);
        QVERIFY(service.cameraSyncSnapshot().statusMessage.contains(
            QStringLiteral("inconsistent screenshot metadata")));
        QCOMPARE(server.requests().size(), 2);
        QCOMPARE(store.replaceCount, 0);
    }

    void recreatedBoardWithHigherVersionDropsTheOldGenerationBase()
    {
        const QString oldGeneration =
            QStringLiteral("11111111-1111-4111-8111-111111111111");
        const QString newGeneration =
            QStringLiteral("22222222-2222-4222-8222-222222222222");
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString statePath = directory.filePath(QStringLiteral("sync-state.json"));
        SyncCameraPoseStore store;
        store.records.append(savedPose(
            QStringLiteral("workspace-a"),
            QStringLiteral("view_001"),
            QStringLiteral("<camera>local</camera>")));
        meshcompare::CameraPoseSyncDocument oldBase;
        const OperationResult built =
            meshcompare::buildCameraPoseSyncDocument(store.records, &oldBase);
        QVERIFY2(built.ok, qPrintable(built.error));
        oldBase.generationId = oldGeneration;
        meshcompare::CameraSyncPersistedState previous;
        previous.boardKey = BoardKey;
        previous.boardVersion = 7;
        previous.syncedAtUtc = QStringLiteral("2026-10-10T07:00:00.000Z");
        previous.base = oldBase;
        meshcompare::CameraSyncStateStore stateStore(statePath);
        QVERIFY(stateStore.save(previous).ok);

        meshcompare::CameraPoseSyncDocument recreated;
        recreated.generationId = newGeneration;
        const QJsonObject oldContent = syncContent(oldBase);
        const QJsonObject recreatedContent = syncContent(recreated);
        QVERIFY(!oldContent.isEmpty());
        QVERIFY(!recreatedContent.isEmpty());
        TestHttpServer server;
        QVERIFY(server.start());
        server.enqueue({
            200,
            QByteArrayLiteral("OK"),
            QByteArrayLiteral("application/json"),
            boardResponse(BoardKey, oldContent, 7)});
        server.enqueue({
            409,
            QByteArrayLiteral("Conflict"),
            QByteArrayLiteral("application/json"),
            conflictResponse(BoardKey, recreatedContent, 8)});
        server.enqueue({
            200,
            QByteArrayLiteral("OK"),
            QByteArrayLiteral("application/json"),
            boardResponse(BoardKey, QJsonObject{}, 9)});

        QNetworkAccessManager network;
        CloudBoardClient client(&network);
        client.setBaseUrl(server.baseUrl());
        FakeApiKeyStore keys;
        CameraCloudSyncService service(store, client, keys, statePath);
        QVERIFY(service.cameraSyncSnapshot().lastSyncSucceeded);
        QSignalSpy completed(&service, &ICameraSyncCommands::cameraSyncCompleted);

        QVERIFY(service.startCameraSync(BoardKey).ok);
        QTRY_COMPARE(completed.size(), 1);
        QVERIFY2(
            service.cameraSyncSnapshot().lastSyncSucceeded,
            qPrintable(service.cameraSyncSnapshot().statusMessage));
        QCOMPARE(store.replaceCount, 0);
        QCOMPARE(store.records.size(), 1);
        QCOMPARE(server.requests().size(), 3);
        const QJsonObject retriedPut = QJsonDocument::fromJson(
            server.requests().at(2).body).object();
        QCOMPARE(retriedPut.value(QStringLiteral("version")).toInt(), 8);
        meshcompare::CameraPoseSyncDocument retried;
        const OperationResult parsed = meshcompare::parseCameraPoseSyncDocument(
            QJsonDocument(retriedPut.value(QStringLiteral("content")).toObject())
                .toJson(QJsonDocument::Compact),
            &retried);
        QVERIFY2(parsed.ok, qPrintable(parsed.error));
        QCOMPARE(retried.generationId, newGeneration);
        QCOMPARE(retried.views.size(), 1);

        meshcompare::CameraSyncPersistedState saved;
        bool found = false;
        QVERIFY(stateStore.load(BoardKey, &saved, &found).ok);
        QVERIFY(found);
        QCOMPARE(saved.boardVersion, qint64(9));
        QCOMPARE(saved.base.generationId, newGeneration);
    }

    void failedAttemptKeepsTheLastSuccessfulSyncStatus()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString statePath = directory.filePath(QStringLiteral("sync-state.json"));
        SyncCameraPoseStore store;
        store.records.append(savedPose(
            QStringLiteral("workspace-a"),
            QStringLiteral("view_001"),
            QStringLiteral("<camera>local</camera>")));
        meshcompare::CameraPoseSyncDocument base;
        QVERIFY(meshcompare::buildCameraPoseSyncDocument(store.records, &base).ok);
        base.generationId =
            QStringLiteral("33333333-3333-4333-8333-333333333333");
        const QString lastSuccess = QStringLiteral("2026-10-10T07:00:00.000Z");
        meshcompare::CameraSyncPersistedState previous;
        previous.boardKey = BoardKey;
        previous.boardVersion = 3;
        previous.syncedAtUtc = lastSuccess;
        previous.base = base;
        meshcompare::CameraSyncStateStore stateStore(statePath);
        QVERIFY(stateStore.save(previous).ok);

        TestHttpServer server;
        QVERIFY(server.start());
        server.enqueue({
            500,
            QByteArrayLiteral("Server Error"),
            QByteArrayLiteral("application/json"),
            QByteArrayLiteral(
                "{\"error\":{\"code\":\"server_error\",\"message\":\"Unavailable\"}}")});
        QNetworkAccessManager network;
        CloudBoardClient client(&network);
        client.setBaseUrl(server.baseUrl());
        FakeApiKeyStore keys;
        CameraCloudSyncService service(store, client, keys, statePath);
        QVERIFY(service.cameraSyncSnapshot().lastSyncSucceeded);
        QCOMPARE(service.cameraSyncSnapshot().lastSyncAtUtc, lastSuccess);
        QSignalSpy completed(&service, &ICameraSyncCommands::cameraSyncCompleted);

        QVERIFY(service.startCameraSync(BoardKey).ok);
        QTRY_COMPARE(completed.size(), 1);
        QVERIFY(!service.cameraSyncSnapshot().result.ok);
        QVERIFY(service.cameraSyncSnapshot().lastSyncSucceeded);
        QCOMPARE(service.cameraSyncSnapshot().lastSyncAtUtc, lastSuccess);
        QCOMPARE(store.replaceCount, 0);
    }

    void conflictRetryReusesUploadAndDownloadsRemoteScreenshot()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString localImagePath =
            directory.filePath(QStringLiteral("local.png"));
        const QString remoteImagePath =
            directory.filePath(QStringLiteral("remote.png"));
        QImage localImage(2, 2, QImage::Format_ARGB32);
        localImage.fill(Qt::red);
        QVERIFY(localImage.save(localImagePath));
        const QByteArray localBytes = readAll(localImagePath);
        QVERIFY(!localBytes.isEmpty());
        QImage remoteImage(2, 2, QImage::Format_ARGB32);
        remoteImage.fill(Qt::blue);
        QVERIFY(remoteImage.save(remoteImagePath));
        const QByteArray remoteBytes = readAll(remoteImagePath);
        QVERIFY(!remoteBytes.isEmpty());

        SyncCameraPoseStore store;
        store.records.append(savedPose(
            QStringLiteral("workspace-a"),
            QStringLiteral("view_001"),
            QStringLiteral("<camera>local</camera>"),
            localImagePath));
        store.beforeReplace = [&]() {
            QFile changed(localImagePath);
            QVERIFY(changed.open(QIODevice::WriteOnly | QIODevice::Truncate));
            QCOMPARE(
                changed.write(QByteArrayLiteral("changed-after-staging")),
                qint64(21));
        };

        QVector<SavedCameraPose> remotePoses;
        remotePoses.append(savedPose(
            QStringLiteral("workspace-b"),
            QStringLiteral("view_002"),
            QStringLiteral("<camera>remote</camera>"),
            remoteImagePath));
        meshcompare::CameraPoseSyncDocument remote;
        const OperationResult built =
            meshcompare::buildCameraPoseSyncDocument(remotePoses, &remote);
        QVERIFY2(built.ok, qPrintable(built.error));
        QCOMPARE(remote.views.size(), 1);
        remote.views.front().screenshot.fileId = QStringLiteral("remote-file");
        remote.views.front().screenshot.sourcePath.clear();
        const QJsonObject remoteContent = syncContent(remote);
        QVERIFY(!remoteContent.isEmpty());

        TestHttpServer server;
        QVERIFY(server.start());
        server.enqueue({
            200,
            QByteArrayLiteral("OK"),
            QByteArrayLiteral("application/json"),
            boardResponse(BoardKey, remoteContent, 8)});
        QJsonObject uploadedFile;
        uploadedFile.insert(QStringLiteral("fileId"), QStringLiteral("local-file"));
        uploadedFile.insert(
            QStringLiteral("url"),
            QStringLiteral("https://memo.gfyh.fun/cloud/api/files/local-file"));
        uploadedFile.insert(QStringLiteral("mimeType"), QStringLiteral("image/png"));
        uploadedFile.insert(
            QStringLiteral("size"),
            static_cast<double>(QFileInfo(localImagePath).size()));
        uploadedFile.insert(
            QStringLiteral("createdAt"),
            QStringLiteral("2026-10-10T08:00:00.000Z"));
        server.enqueue({
            201,
            QByteArrayLiteral("Created"),
            QByteArrayLiteral("application/json"),
            QJsonDocument(uploadedFile).toJson(QJsonDocument::Compact)});
        server.enqueue({
            200,
            QByteArrayLiteral("OK"),
            QByteArrayLiteral("image/png"),
            remoteBytes});
        server.enqueue({
            409,
            QByteArrayLiteral("Conflict"),
            QByteArrayLiteral("application/json"),
            conflictResponse(BoardKey, remoteContent, 9)});
        server.enqueue({
            200,
            QByteArrayLiteral("OK"),
            QByteArrayLiteral("application/json"),
            boardResponse(BoardKey, QJsonObject{}, 10)});

        QNetworkAccessManager network;
        CloudBoardClient client(&network);
        client.setBaseUrl(server.baseUrl());
        FakeApiKeyStore keys;
        const QString statePath = directory.filePath(QStringLiteral("sync-state.json"));
        CameraCloudSyncService service(store, client, keys, statePath);
        QSignalSpy completed(&service, &ICameraSyncCommands::cameraSyncCompleted);

        const OperationResult started = service.startCameraSync(BoardKey);
        QVERIFY2(started.ok, qPrintable(started.error));
        QTRY_COMPARE(completed.size(), 1);
        QVERIFY2(
            service.cameraSyncSnapshot().lastSyncSucceeded,
            qPrintable(service.cameraSyncSnapshot().statusMessage));
        QCOMPARE(store.replaceCount, 1);
        QCOMPARE(store.records.size(), 2);
        QCOMPARE(
            store.appliedScreenshotBytes.value(QStringLiteral("view_001")),
            localBytes);
        QCOMPARE(
            store.appliedScreenshotBytes.value(QStringLiteral("view_002")),
            remoteBytes);

        QCOMPARE(server.requests().size(), 5);
        QCOMPARE(server.requests().at(0).method, QByteArrayLiteral("GET"));
        QCOMPARE(server.requests().at(1).method, QByteArrayLiteral("POST"));
        QCOMPARE(server.requests().at(2).method, QByteArrayLiteral("GET"));
        QCOMPARE(
            server.requests().at(2).target,
            QByteArrayLiteral("/cloud/api/files/remote-file"));
        QCOMPARE(server.requests().at(3).method, QByteArrayLiteral("PUT"));
        QCOMPARE(server.requests().at(4).method, QByteArrayLiteral("PUT"));

        const QJsonObject firstPut = QJsonDocument::fromJson(
            server.requests().at(3).body).object();
        const QJsonObject secondPut = QJsonDocument::fromJson(
            server.requests().at(4).body).object();
        QCOMPARE(firstPut.value(QStringLiteral("version")).toInt(), 8);
        QCOMPARE(secondPut.value(QStringLiteral("version")).toInt(), 9);
        meshcompare::CameraPoseSyncDocument merged;
        const OperationResult parsed = meshcompare::parseCameraPoseSyncDocument(
            QJsonDocument(secondPut.value(QStringLiteral("content")).toObject())
                .toJson(QJsonDocument::Compact),
            &merged);
        QVERIFY2(parsed.ok, qPrintable(parsed.error));
        QCOMPARE(merged.views.size(), 2);
        QHash<QString, QString> fileByView;
        for (const auto& view : merged.views)
            fileByView.insert(view.viewId, view.screenshot.fileId);
        QCOMPARE(fileByView.value(QStringLiteral("view_001")), QStringLiteral("local-file"));
        QCOMPARE(fileByView.value(QStringLiteral("view_002")), QStringLiteral("remote-file"));

        meshcompare::CameraSyncStateStore stateStore(statePath);
        meshcompare::CameraSyncPersistedState saved;
        bool found = false;
        QVERIFY(stateStore.load(BoardKey, &saved, &found).ok);
        QVERIFY(found);
        QCOMPARE(saved.boardVersion, qint64(10));
        QCOMPARE(saved.base.views.size(), 2);
    }

    void localEditAfterPutStartsIsNeverOverwritten()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString statePath = directory.filePath(QStringLiteral("sync-state.json"));
        SyncCameraPoseStore store;
        store.records.append(savedPose(
            QStringLiteral("workspace-a"),
            QStringLiteral("view_001"),
            QStringLiteral("<camera>before</camera>")));

        TestHttpServer server;
        QVERIFY(server.start());
        server.enqueue({
            404,
            QByteArrayLiteral("Not Found"),
            QByteArrayLiteral("application/json"),
            QByteArrayLiteral("{\"error\":{\"code\":\"not_found\"}}")});
        server.enqueue({
            201,
            QByteArrayLiteral("Created"),
            QByteArrayLiteral("application/json"),
            boardResponse(BoardKey, QJsonObject{}, 1)});
        bool injectedEdit = false;
        connect(&server, &TestHttpServer::requestCaptured, &server, [&]() {
            if (!injectedEdit && !server.requests().isEmpty() &&
                server.requests().last().method == QByteArrayLiteral("PUT")) {
                injectedEdit = true;
                store.records.append(savedPose(
                    QStringLiteral("workspace-a"),
                    QStringLiteral("view_002"),
                    QStringLiteral("<camera>new-local-edit</camera>")));
            }
        });

        QNetworkAccessManager network;
        CloudBoardClient client(&network);
        client.setBaseUrl(server.baseUrl());
        FakeApiKeyStore keys;
        CameraCloudSyncService service(store, client, keys, statePath);
        QSignalSpy completed(&service, &ICameraSyncCommands::cameraSyncCompleted);

        const OperationResult started = service.startCameraSync(BoardKey);
        QVERIFY2(started.ok, qPrintable(started.error));
        QTRY_COMPARE(completed.size(), 1);
        QVERIFY(injectedEdit);
        QVERIFY(!service.cameraSyncSnapshot().lastSyncSucceeded);
        QVERIFY(service.cameraSyncSnapshot().statusMessage.startsWith(
            QStringLiteral("Cloud was saved, but Saved views changed")));
        QCOMPARE(store.replaceCount, 0);
        QCOMPARE(store.records.size(), 2);

        meshcompare::CameraSyncStateStore stateStore(statePath);
        meshcompare::CameraSyncPersistedState state;
        bool found = false;
        QVERIFY(stateStore.load(BoardKey, &state, &found).ok);
        QVERIFY(found);
        QVERIFY(state.syncedAtUtc.isEmpty());
        QCOMPARE(state.boardVersion, qint64(1));
        QCOMPARE(state.base.views.size(), 1);
    }

    void firstSyncDeletionDuringPutPropagatesOnRetry()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString statePath = directory.filePath(QStringLiteral("sync-state.json"));
        SyncCameraPoseStore store;
        store.records.append(savedPose(
            QStringLiteral("workspace-a"),
            QStringLiteral("view_001"),
            QStringLiteral("<camera>delete-me</camera>")));

        TestHttpServer server;
        QVERIFY(server.start());
        server.enqueue({
            404,
            QByteArrayLiteral("Not Found"),
            QByteArrayLiteral("application/json"),
            QByteArrayLiteral("{\"error\":{\"code\":\"not_found\"}}")});
        server.enqueue({
            201,
            QByteArrayLiteral("Created"),
            QByteArrayLiteral("application/json"),
            boardResponse(BoardKey, QJsonObject{}, 1)});
        int putCount = 0;
        connect(&server, &TestHttpServer::requestCaptured, &server, [&]() {
            if (!server.requests().isEmpty() &&
                server.requests().last().method == QByteArrayLiteral("PUT") &&
                ++putCount == 1) {
                store.records.clear();
            }
        });

        QNetworkAccessManager network;
        CloudBoardClient client(&network);
        client.setBaseUrl(server.baseUrl());
        FakeApiKeyStore keys;
        CameraCloudSyncService service(store, client, keys, statePath);
        QSignalSpy completed(&service, &ICameraSyncCommands::cameraSyncCompleted);

        QVERIFY(service.startCameraSync(BoardKey).ok);
        QTRY_COMPARE(completed.size(), 1);
        QVERIFY(!service.cameraSyncSnapshot().lastSyncSucceeded);
        QVERIFY(service.cameraSyncSnapshot().lastSyncAtUtc.isEmpty());
        QCOMPARE(store.records.size(), 0);

        const QJsonObject firstPut = QJsonDocument::fromJson(
            server.requests().at(1).body).object();
        const QJsonObject remoteContent =
            firstPut.value(QStringLiteral("content")).toObject();
        QVERIFY(!remoteContent.isEmpty());
        meshcompare::CameraSyncStateStore stateStore(statePath);
        meshcompare::CameraSyncPersistedState pending;
        bool found = false;
        QVERIFY(stateStore.load(BoardKey, &pending, &found).ok);
        QVERIFY(found);
        QVERIFY(pending.syncedAtUtc.isEmpty());
        QCOMPARE(pending.boardVersion, qint64(1));
        QCOMPARE(pending.base.views.size(), 1);

        server.enqueue({
            200,
            QByteArrayLiteral("OK"),
            QByteArrayLiteral("application/json"),
            boardResponse(BoardKey, remoteContent, 1)});
        server.enqueue({
            200,
            QByteArrayLiteral("OK"),
            QByteArrayLiteral("application/json"),
            boardResponse(BoardKey, QJsonObject{}, 2)});
        QVERIFY(service.startCameraSync(BoardKey).ok);
        QTRY_COMPARE(completed.size(), 2);
        QVERIFY2(
            service.cameraSyncSnapshot().lastSyncSucceeded,
            qPrintable(service.cameraSyncSnapshot().statusMessage));
        QVERIFY(!service.cameraSyncSnapshot().lastSyncAtUtc.isEmpty());
        QCOMPARE(store.replaceCount, 0);
        QCOMPARE(server.requests().size(), 4);

        const QJsonObject secondPut = QJsonDocument::fromJson(
            server.requests().at(3).body).object();
        QCOMPARE(secondPut.value(QStringLiteral("version")).toInt(), 1);
        meshcompare::CameraPoseSyncDocument deleted;
        const OperationResult parsed = meshcompare::parseCameraPoseSyncDocument(
            QJsonDocument(secondPut.value(QStringLiteral("content")).toObject())
                .toJson(QJsonDocument::Compact),
            &deleted);
        QVERIFY2(parsed.ok, qPrintable(parsed.error));
        QCOMPARE(deleted.generationId, pending.base.generationId);
        QCOMPARE(deleted.views.size(), 0);

        meshcompare::CameraSyncPersistedState finalState;
        found = false;
        QVERIFY(stateStore.load(BoardKey, &finalState, &found).ok);
        QVERIFY(found);
        QCOMPARE(finalState.boardVersion, qint64(2));
        QVERIFY(!finalState.syncedAtUtc.isEmpty());
        QCOMPARE(finalState.base.views.size(), 0);
    }

    void retryAfterLocalMutationStillImportsRemoteOnlyViews()
    {
        const QString generation =
            QStringLiteral("44444444-4444-4444-8444-444444444444");
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString statePath = directory.filePath(QStringLiteral("sync-state.json"));
        SyncCameraPoseStore store;
        store.records.append(savedPose(
            QStringLiteral("workspace-a"),
            QStringLiteral("view_local"),
            QStringLiteral("<camera>local-before</camera>")));

        QVector<SavedCameraPose> remotePoses;
        remotePoses.append(savedPose(
            QStringLiteral("workspace-b"),
            QStringLiteral("view_remote"),
            QStringLiteral("<camera>remote-only</camera>")));
        meshcompare::CameraPoseSyncDocument initialRemote;
        QVERIFY(meshcompare::buildCameraPoseSyncDocument(
                    remotePoses, &initialRemote).ok);
        initialRemote.generationId = generation;
        const QJsonObject initialRemoteContent = syncContent(initialRemote);
        QVERIFY(!initialRemoteContent.isEmpty());

        TestHttpServer server;
        QVERIFY(server.start());
        server.enqueue({
            200,
            QByteArrayLiteral("OK"),
            QByteArrayLiteral("application/json"),
            boardResponse(BoardKey, initialRemoteContent, 1)});
        server.enqueue({
            200,
            QByteArrayLiteral("OK"),
            QByteArrayLiteral("application/json"),
            boardResponse(BoardKey, QJsonObject{}, 2)});
        int putCount = 0;
        connect(&server, &TestHttpServer::requestCaptured, &server, [&]() {
            if (!server.requests().isEmpty() &&
                server.requests().last().method == QByteArrayLiteral("PUT") &&
                ++putCount == 1) {
                store.records.front().pose =
                    CameraPose{QStringLiteral("<camera>local-after</camera>")};
            }
        });

        QNetworkAccessManager network;
        CloudBoardClient client(&network);
        client.setBaseUrl(server.baseUrl());
        FakeApiKeyStore keys;
        CameraCloudSyncService service(store, client, keys, statePath);
        QSignalSpy completed(&service, &ICameraSyncCommands::cameraSyncCompleted);

        QVERIFY(service.startCameraSync(BoardKey).ok);
        QTRY_COMPARE(completed.size(), 1);
        QVERIFY(!service.cameraSyncSnapshot().result.ok);
        QCOMPARE(store.records.size(), 1);
        QCOMPARE(
            store.records.front().pose.viewStateXml,
            QStringLiteral("<camera>local-after</camera>"));

        const QJsonObject firstPut = QJsonDocument::fromJson(
            server.requests().at(1).body).object();
        const QJsonObject firstMergedContent =
            firstPut.value(QStringLiteral("content")).toObject();
        meshcompare::CameraSyncStateStore stateStore(statePath);
        meshcompare::CameraSyncPersistedState pending;
        bool found = false;
        QVERIFY(stateStore.load(BoardKey, &pending, &found).ok);
        QVERIFY(found);
        QVERIFY(pending.syncedAtUtc.isEmpty());
        QCOMPARE(pending.base.generationId, generation);
        QCOMPARE(pending.base.views.size(), 1);
        QCOMPARE(pending.base.views.front().viewId, QStringLiteral("view_local"));

        server.enqueue({
            200,
            QByteArrayLiteral("OK"),
            QByteArrayLiteral("application/json"),
            boardResponse(BoardKey, firstMergedContent, 2)});
        server.enqueue({
            200,
            QByteArrayLiteral("OK"),
            QByteArrayLiteral("application/json"),
            boardResponse(BoardKey, QJsonObject{}, 3)});
        QVERIFY(service.startCameraSync(BoardKey).ok);
        QTRY_COMPARE(completed.size(), 2);
        QVERIFY2(
            service.cameraSyncSnapshot().lastSyncSucceeded,
            qPrintable(service.cameraSyncSnapshot().statusMessage));
        QCOMPARE(store.replaceCount, 1);
        QCOMPARE(store.records.size(), 2);
        QHash<QString, QString> poseByView;
        for (const SavedCameraPose& pose : store.records)
            poseByView.insert(pose.viewId, pose.pose.viewStateXml);
        QCOMPARE(
            poseByView.value(QStringLiteral("view_local")),
            QStringLiteral("<camera>local-after</camera>"));
        QCOMPARE(
            poseByView.value(QStringLiteral("view_remote")),
            QStringLiteral("<camera>remote-only</camera>"));

        const QJsonObject secondPut = QJsonDocument::fromJson(
            server.requests().at(3).body).object();
        meshcompare::CameraPoseSyncDocument finalRemote;
        const OperationResult parsed = meshcompare::parseCameraPoseSyncDocument(
            QJsonDocument(secondPut.value(QStringLiteral("content")).toObject())
                .toJson(QJsonDocument::Compact),
            &finalRemote);
        QVERIFY2(parsed.ok, qPrintable(parsed.error));
        QCOMPARE(finalRemote.views.size(), 2);
    }
};

QTEST_GUILESS_MAIN(CameraCloudSyncServiceTest)
#include "camera_cloud_sync_service_test.moc"
