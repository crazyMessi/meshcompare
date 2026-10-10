#include "services/cloud_board_client.h"

#include <QFile>
#include <QHostAddress>
#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QNetworkAccessManager>
#include <QSharedPointer>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTemporaryFile>
#include <QTest>

#include <utility>

namespace {

struct HttpResponse {
	int status = 200;
	QByteArray reason = QByteArrayLiteral("OK");
	QByteArray contentType = QByteArrayLiteral("application/json");
	QByteArray body;
	QByteArray extraHeaders;
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

	void start()
	{
		QVERIFY(listen(QHostAddress::LocalHost, 0));
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

			const QList<QByteArray> headerLines = bytes->left(headerEnd).split('\n');
			if (headerLines.isEmpty())
				return;
			const QList<QByteArray> requestLine = headerLines.first().trimmed().split(' ');
			if (requestLine.size() < 2)
				return;

			HttpRequest request;
			request.method = requestLine.at(0);
			request.target = requestLine.at(1);
			for (int index = 1; index < headerLines.size(); ++index) {
				const QByteArray line = headerLines.at(index).trimmed();
				const int colon = line.indexOf(':');
				if (colon > 0) {
					request.headers.insert(
						line.left(colon).trimmed().toLower(),
						line.mid(colon + 1).trimmed());
				}
			}
			const int bodyLength = request.headers.value(
				QByteArrayLiteral("content-length")).toInt();
			const int totalLength = headerEnd + 4 + bodyLength;
			if (bytes->size() < totalLength)
				return;
			request.body = bytes->mid(headerEnd + 4, bodyLength);
			requests_.append(request);
			emit requestCaptured();

			QVERIFY(!responses_.isEmpty());
			const HttpResponse response = responses_.takeFirst();
			QByteArray output = QByteArrayLiteral("HTTP/1.1 ") +
				QByteArray::number(response.status) + ' ' + response.reason +
				QByteArrayLiteral("\r\nContent-Type: ") + response.contentType +
				QByteArrayLiteral("\r\nContent-Length: ") +
				QByteArray::number(response.body.size()) +
				QByteArrayLiteral("\r\nConnection: close\r\n") +
				response.extraHeaders + QByteArrayLiteral("\r\n") + response.body;
			socket->write(output);
			socket->disconnectFromHost();
		});
		connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
	}

	QVector<HttpResponse> responses_;
	QVector<HttpRequest> requests_;
};

CloudBoardResult boardResult(const QSignalSpy& spy)
{
	return qvariant_cast<CloudBoardResult>(spy.constFirst().at(1));
}

} // namespace

class CloudBoardClientTest final : public QObject
{
	Q_OBJECT

private slots:
	void readsBoardWithBearerAuthentication()
	{
		TestHttpServer server;
		server.start();
		server.enqueue({
			200,
			QByteArrayLiteral("OK"),
			QByteArrayLiteral("application/json"),
			QByteArrayLiteral(
				"{\"key\":\"meshcompare-camera-views\",\"content\":{\"views\":3},"
				"\"version\":7,\"updatedAt\":\"2026-10-10T08:00:00.000Z\"}")});

		QNetworkAccessManager manager;
		CloudBoardClient client(&manager);
		client.setBaseUrl(server.baseUrl());
		client.setApiKey(QStringLiteral("test-key"));
		QSignalSpy finished(&client, &CloudBoardClient::boardReceived);

		const quint64 requestId = client.getBoard(
			QStringLiteral("meshcompare-camera-views"));
		QTRY_COMPARE(finished.size(), 1);
		const CloudBoardResult result = boardResult(finished);
		QVERIFY(result.ok);
		QVERIFY(!result.missing);
		QCOMPARE(result.board.version, qint64(7));
		QCOMPARE(result.board.content.toObject().value(QStringLiteral("views")).toInt(), 3);
		QCOMPARE(finished.first().first().toULongLong(), requestId);
		QCOMPARE(server.requests().size(), 1);
		QCOMPARE(server.requests().first().method, QByteArrayLiteral("GET"));
		QCOMPARE(
			server.requests().first().target,
			QByteArrayLiteral("/cloud/api/boards/meshcompare-camera-views"));
		QCOMPARE(
			server.requests().first().headers.value(QByteArrayLiteral("authorization")),
			QByteArrayLiteral("Bearer test-key"));
	}

	void treatsMissingBoardAsFirstSync()
	{
		TestHttpServer server;
		server.start();
		server.enqueue({
			404,
			QByteArrayLiteral("Not Found"),
			QByteArrayLiteral("application/json"),
			QByteArrayLiteral("{\"error\":{\"code\":\"not_found\",\"message\":\"Missing\"}}")});
		QNetworkAccessManager manager;
		CloudBoardClient client(&manager);
		client.setBaseUrl(server.baseUrl());
		client.setApiKey(QStringLiteral("test-key"));
		QSignalSpy finished(&client, &CloudBoardClient::boardReceived);
		client.getBoard(QStringLiteral("new-board"));
		QTRY_COMPARE(finished.size(), 1);
		const CloudBoardResult result = boardResult(finished);
		QVERIFY(!result.ok);
		QVERIFY(result.missing);
		QCOMPARE(result.error.httpStatus, 404);
	}

	void sendsVersionAndReturnsCurrentBoardOnConflict()
	{
		TestHttpServer server;
		server.start();
		server.enqueue({
			409,
			QByteArrayLiteral("Conflict"),
			QByteArrayLiteral("application/json"),
			QByteArrayLiteral(
				"{\"error\":{\"code\":\"version_conflict\",\"message\":\"Changed\"},"
				"\"current\":{\"key\":\"views\",\"content\":{\"remote\":true},"
				"\"version\":9,\"updatedAt\":\"2026-10-10T08:00:00.000Z\"}}")});
		QNetworkAccessManager manager;
		CloudBoardClient client(&manager);
		client.setBaseUrl(server.baseUrl());
		client.setApiKey(QStringLiteral("test-key"));
		QSignalSpy finished(&client, &CloudBoardClient::boardWritten);
		client.putBoard(
			QStringLiteral("views"),
			QJsonObject{{QStringLiteral("local"), true}},
			8);
		QTRY_COMPARE(finished.size(), 1);
		const CloudBoardResult result = boardResult(finished);
		QVERIFY(!result.ok);
		QVERIFY(result.conflict);
		QVERIFY(result.hasCurrentBoard);
		QCOMPARE(result.currentBoard.version, qint64(9));
		QCOMPARE(result.error.code, QStringLiteral("version_conflict"));
		const QJsonObject sent = QJsonDocument::fromJson(
			server.requests().first().body).object();
		QCOMPARE(sent.value(QStringLiteral("version")).toInt(), 8);
		QVERIFY(sent.value(QStringLiteral("content")).toObject()
			.value(QStringLiteral("local")).toBool());
	}

	void rejectsInvalidAndOversizedBoardsBeforeNetwork()
	{
		TestHttpServer server;
		server.start();
		QNetworkAccessManager manager;
		CloudBoardClient client(&manager);
		client.setBaseUrl(server.baseUrl());
		client.setApiKey(QStringLiteral("test-key"));
		QSignalSpy finished(&client, &CloudBoardClient::boardWritten);

		client.putBoard(
			QStringLiteral("invalid board"), QJsonObject{}, 0);
		QTRY_COMPARE(finished.size(), 1);
		QCOMPARE(boardResult(finished).error.code, QStringLiteral("invalid_board_key"));
		finished.clear();

		const QString oversized(
			CloudBoardClient::MaximumBoardContentBytes + 1, QLatin1Char('x'));
		client.putBoard(QStringLiteral("views"), oversized, 0);
		QTRY_COMPARE(finished.size(), 1);
		QCOMPARE(boardResult(finished).error.code, QStringLiteral("board_too_large"));
		QCOMPARE(server.requests().size(), 0);
	}

	void rejectsMissingCredentialsWithoutSendingRequest()
	{
		TestHttpServer server;
		server.start();
		QNetworkAccessManager manager;
		CloudBoardClient client(&manager);
		client.setBaseUrl(server.baseUrl());
		QSignalSpy finished(&client, &CloudBoardClient::boardReceived);
		client.getBoard(QStringLiteral("views"));
		QTRY_COMPARE(finished.size(), 1);
		QCOMPARE(
			boardResult(finished).error.code,
			QStringLiteral("authentication_required"));
		QCOMPARE(server.requests().size(), 0);
	}

	void rejectsOutOfRangeResponseNumbers()
	{
		TestHttpServer server;
		server.start();
		server.enqueue({
			200,
			QByteArrayLiteral("OK"),
			QByteArrayLiteral("application/json"),
			QByteArrayLiteral(
				"{\"key\":\"views\",\"content\":{},\"version\":9007199254740992,"
				"\"updatedAt\":\"2026-10-10T08:00:00.000Z\"}")});

		QNetworkAccessManager manager;
		CloudBoardClient client(&manager);
		client.setBaseUrl(server.baseUrl());
		client.setApiKey(QStringLiteral("test-key"));
		QSignalSpy boardFinished(&client, &CloudBoardClient::boardReceived);
		client.getBoard(QStringLiteral("views"));
		QTRY_COMPARE(boardFinished.size(), 1);
		const CloudBoardResult board = boardResult(boardFinished);
		QVERIFY(!board.ok);
		QCOMPARE(board.error.code, QStringLiteral("invalid_response"));

		QTemporaryFile image(
			QDir::tempPath() + QStringLiteral("/meshcompare-XXXXXX.png"));
		QVERIFY(image.open());
		const QByteArray png = QByteArray::fromBase64(
			"iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR4nGNgYAAAAAMA"
			"ASsJTYQAAAAASUVORK5CYII=");
		QCOMPARE(image.write(png), png.size());
		image.flush();
		server.enqueue({
			201,
			QByteArrayLiteral("Created"),
			QByteArrayLiteral("application/json"),
			QByteArrayLiteral(
				"{\"fileId\":\"file-1\",\"url\":\"https://memo.gfyh.fun/cloud/api/files/file-1\","
				"\"mimeType\":\"image/png\",\"size\":1e300,"
				"\"createdAt\":\"2026-10-10T08:00:00.000Z\"}")});
		QSignalSpy uploadFinished(&client, &CloudBoardClient::fileUploaded);
		client.uploadFile(image.fileName());
		QTRY_COMPARE(uploadFinished.size(), 1);
		const CloudFileUploadResult upload =
			qvariant_cast<CloudFileUploadResult>(uploadFinished.first().at(1));
		QVERIFY(!upload.ok);
		QCOMPARE(upload.error.code, QStringLiteral("invalid_response"));
	}

	void capsUploadMetadataResponses()
	{
		QTemporaryFile image(
			QDir::tempPath() + QStringLiteral("/meshcompare-XXXXXX.png"));
		QVERIFY(image.open());
		const QByteArray png = QByteArray::fromBase64(
			"iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR4nGNgYAAAAAMA"
			"ASsJTYQAAAAASUVORK5CYII=");
		QCOMPARE(image.write(png), png.size());
		image.flush();

		TestHttpServer server;
		server.start();
		server.enqueue({
			201,
			QByteArrayLiteral("Created"),
			QByteArrayLiteral("application/json"),
			QByteArray(256 * 1024 + 1, 'x')});
		QNetworkAccessManager manager;
		CloudBoardClient client(&manager);
		client.setBaseUrl(server.baseUrl());
		client.setApiKey(QStringLiteral("test-key"));
		QSignalSpy finished(&client, &CloudBoardClient::fileUploaded);
		client.uploadFile(image.fileName());
		QTRY_COMPARE(finished.size(), 1);
		const CloudFileUploadResult result =
			qvariant_cast<CloudFileUploadResult>(finished.first().at(1));
		QVERIFY(!result.ok);
		QCOMPARE(result.error.code, QStringLiteral("invalid_response"));
	}

	void uploadsSupportedImageAndParsesMetadata()
	{
		QTemporaryFile image(QDir::tempPath() + QStringLiteral("/meshcompare-XXXXXX.png"));
		QVERIFY(image.open());
		const QByteArray png = QByteArray::fromHex(
			"89504e470d0a1a0a0000000d49484452");
		QCOMPARE(image.write(png), png.size());
		image.flush();

		TestHttpServer server;
		server.start();
		server.enqueue({
			201,
			QByteArrayLiteral("Created"),
			QByteArrayLiteral("application/json"),
			QByteArrayLiteral(
				"{\"fileId\":\"file-1\",\"url\":\"https://memo.gfyh.fun/cloud/api/files/file-1\","
				"\"mimeType\":\"image/png\",\"size\":16,"
				"\"createdAt\":\"2026-10-10T08:00:00.000Z\"}")});
		QNetworkAccessManager manager;
		CloudBoardClient client(&manager);
		client.setBaseUrl(server.baseUrl());
		client.setApiKey(QStringLiteral("test-key"));
		QSignalSpy finished(&client, &CloudBoardClient::fileUploaded);
		client.uploadFile(image.fileName());
		QTRY_COMPARE(finished.size(), 1);
		const CloudFileUploadResult result =
			qvariant_cast<CloudFileUploadResult>(finished.first().at(1));
		QVERIFY(result.ok);
		QCOMPARE(result.file.fileId, QStringLiteral("file-1"));
		QVERIFY(server.requests().first().body.contains(png));
		QVERIFY(server.requests().first().body.contains("name=\"file\""));
	}

	void rejectsOversizedUploadBeforeNetwork()
	{
		QTemporaryFile image(QDir::tempPath() + QStringLiteral("/meshcompare-XXXXXX.png"));
		QVERIFY(image.open());
		QVERIFY(image.resize(CloudBoardClient::MaximumFileBytes + 1));
		image.flush();
		TestHttpServer server;
		server.start();
		QNetworkAccessManager manager;
		CloudBoardClient client(&manager);
		client.setBaseUrl(server.baseUrl());
		client.setApiKey(QStringLiteral("test-key"));
		QSignalSpy finished(&client, &CloudBoardClient::fileUploaded);
		client.uploadFile(image.fileName());
		QTRY_COMPARE(finished.size(), 1);
		const CloudFileUploadResult result =
			qvariant_cast<CloudFileUploadResult>(finished.first().at(1));
		QCOMPARE(result.error.code, QStringLiteral("file_too_large"));
		QCOMPARE(server.requests().size(), 0);
	}

	void downloadsFileAtomically()
	{
		const QByteArray png = QByteArray::fromHex("89504e470d0a1a0a01020304");
		TestHttpServer server;
		server.start();
		server.enqueue({
			200,
			QByteArrayLiteral("OK"),
			QByteArrayLiteral("image/png"),
			png});
		QTemporaryDir destination;
		QVERIFY(destination.isValid());
		const QString path = destination.filePath(QStringLiteral("download.png"));
		QNetworkAccessManager manager;
		CloudBoardClient client(&manager);
		client.setBaseUrl(server.baseUrl());
		client.setApiKey(QStringLiteral("test-key"));
		QSignalSpy finished(&client, &CloudBoardClient::fileDownloaded);
		client.downloadFile(QStringLiteral("file-1"), path);
		QTRY_COMPARE(finished.size(), 1);
		const CloudFileDownloadResult result =
			qvariant_cast<CloudFileDownloadResult>(finished.first().at(1));
		QVERIFY(result.ok);
		QCOMPARE(result.size, qint64(png.size()));
		QFile output(path);
		QVERIFY(output.open(QIODevice::ReadOnly));
		QCOMPARE(output.readAll(), png);
		QCOMPARE(
			server.requests().first().target,
			QByteArrayLiteral("/cloud/api/files/file-1"));
	}
};

QTEST_GUILESS_MAIN(CloudBoardClientTest)

#include "cloud_board_client_test.moc"
