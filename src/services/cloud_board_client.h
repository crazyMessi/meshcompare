#pragma once

#include <QJsonValue>
#include <QNetworkReply>
#include <QObject>
#include <QUrl>

class QNetworkAccessManager;
class QNetworkRequest;

struct CloudBoard {
	QString key;
	QJsonValue content;
	qint64 version = 0;
	QString updatedAt;
};

struct CloudBoardError {
	QString code;
	QString message;
	int httpStatus = 0;
	QNetworkReply::NetworkError networkError = QNetworkReply::NoError;
};

struct CloudBoardResult {
	bool ok = false;
	bool missing = false;
	bool conflict = false;
	CloudBoard board;
	bool hasCurrentBoard = false;
	CloudBoard currentBoard;
	CloudBoardError error;
};

struct CloudFile {
	QString fileId;
	QUrl url;
	QString mimeType;
	qint64 size = 0;
	QString createdAt;
};

struct CloudFileUploadResult {
	bool ok = false;
	CloudFile file;
	CloudBoardError error;
};

struct CloudFileDownloadResult {
	bool ok = false;
	QString fileId;
	QString destination;
	qint64 size = 0;
	CloudBoardError error;
};

Q_DECLARE_METATYPE(CloudBoard)
Q_DECLARE_METATYPE(CloudBoardError)
Q_DECLARE_METATYPE(CloudBoardResult)
Q_DECLARE_METATYPE(CloudFile)
Q_DECLARE_METATYPE(CloudFileUploadResult)
Q_DECLARE_METATYPE(CloudFileDownloadResult)

class CloudBoardClient final : public QObject
{
	Q_OBJECT

public:
	static constexpr qint64 MaximumBoardContentBytes = 2 * 1024 * 1024;
	static constexpr qint64 MaximumFileBytes = 10 * 1024 * 1024;

	explicit CloudBoardClient(QObject* parent = nullptr);
	explicit CloudBoardClient(
		QNetworkAccessManager* networkAccessManager,
		QObject* parent = nullptr);

	QUrl baseUrl() const;
	void setBaseUrl(const QUrl& baseUrl);
	void setApiKey(const QString& apiKey);
	void clearApiKey();
	bool hasApiKey() const;

	quint64 getBoard(const QString& key);
	quint64 putBoard(
		const QString& key,
		const QJsonValue& content,
		qint64 version);
	quint64 uploadFile(const QString& path);
	quint64 downloadFile(
		const QString& fileId,
		const QString& destination);

signals:
	void boardReceived(quint64 requestId, const CloudBoardResult& result);
	void boardWritten(quint64 requestId, const CloudBoardResult& result);
	void fileUploaded(quint64 requestId, const CloudFileUploadResult& result);
	void fileDownloaded(
		quint64 requestId,
		const CloudFileDownloadResult& result);

private:
	quint64 nextRequestId();
	QUrl endpoint(const QString& path) const;
	QNetworkRequest requestFor(const QUrl& url) const;
	QNetworkReply* sendGet(const QUrl& url);
	bool validateCredentials(CloudBoardError* error) const;

	QNetworkAccessManager* networkAccessManager_ = nullptr;
	QUrl baseUrl_;
	QString apiKey_;
	quint64 nextRequestId_ = 1;
};
