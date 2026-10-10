#include "services/cloud_board_client.h"

#include <QFile>
#include <QFileInfo>
#include <QHttpMultiPart>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QMimeDatabase>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSharedPointer>
#include <QTimer>

#include <cmath>

namespace {

const QUrl DefaultBaseUrl(QStringLiteral("https://memo.gfyh.fun/cloud/api"));
constexpr qint64 MaximumExactJsonInteger = (qint64(1) << 53) - 1;
constexpr qint64 MaximumMetadataResponseBytes = 256 * 1024;

CloudBoardError localError(const QString& code, const QString& message)
{
	CloudBoardError error;
	error.code = code;
	error.message = message;
	return error;
}

CloudBoardError responseError(
	QNetworkReply* reply,
	const QJsonObject& responseObject)
{
	CloudBoardError result;
	result.httpStatus = reply->attribute(
		QNetworkRequest::HttpStatusCodeAttribute).toInt();
	result.networkError = reply->error();

	const QJsonObject errorObject = responseObject.value(
		QStringLiteral("error")).toObject();
	result.code = errorObject.value(QStringLiteral("code")).toString();
	result.message = errorObject.value(QStringLiteral("message")).toString();
	if (result.code.isEmpty()) {
		result.code = result.networkError == QNetworkReply::NoError
			? QStringLiteral("invalid_response")
			: QStringLiteral("network_error");
	}
	if (result.message.isEmpty()) {
		if (result.networkError != QNetworkReply::NoError)
			result.message = reply->errorString();
		else
			result.message = QStringLiteral("Cloud Board returned an invalid response.");
	}
	return result;
}

bool parseObject(
	const QByteArray& bytes,
	QJsonObject* object,
	QString* parseError = nullptr)
{
	QJsonParseError jsonError;
	const QJsonDocument document = QJsonDocument::fromJson(bytes, &jsonError);
	if (jsonError.error != QJsonParseError::NoError || !document.isObject()) {
		if (parseError) {
			*parseError = jsonError.error == QJsonParseError::NoError
				? QStringLiteral("The response is not a JSON object.")
				: jsonError.errorString();
		}
		return false;
	}
	*object = document.object();
	return true;
}

bool parseBoard(const QJsonObject& object, CloudBoard* board)
{
	if (!object.value(QStringLiteral("key")).isString() ||
		!object.contains(QStringLiteral("content")) ||
		!object.value(QStringLiteral("version")).isDouble() ||
		!object.value(QStringLiteral("updatedAt")).isString()) {
		return false;
	}

	const double version = object.value(QStringLiteral("version")).toDouble();
	if (!std::isfinite(version) || version < 1.0 ||
		version > static_cast<double>(MaximumExactJsonInteger) ||
		std::floor(version) != version) {
		return false;
	}

	board->key = object.value(QStringLiteral("key")).toString();
	board->content = object.value(QStringLiteral("content"));
	board->version = static_cast<qint64>(version);
	board->updatedAt = object.value(QStringLiteral("updatedAt")).toString();
	return !board->key.isEmpty();
}

bool parseCurrentBoard(const QJsonObject& root, CloudBoard* board)
{
	const QStringList rootCandidates = {
		QStringLiteral("current"),
		QStringLiteral("currentBoard"),
		QStringLiteral("board")};
	for (const QString& candidate : rootCandidates) {
		const QJsonValue value = root.value(candidate);
		if (value.isObject() && parseBoard(value.toObject(), board))
			return true;
	}

	const QJsonObject error = root.value(QStringLiteral("error")).toObject();
	for (const QString& candidate : rootCandidates) {
		const QJsonValue value = error.value(candidate);
		if (value.isObject() && parseBoard(value.toObject(), board))
			return true;
	}
	const QJsonObject details = error.value(QStringLiteral("details")).toObject();
	for (const QString& candidate : rootCandidates) {
		const QJsonValue value = details.value(candidate);
		if (value.isObject() && parseBoard(value.toObject(), board))
			return true;
	}
	return false;
}

qint64 serializedValueSize(const QJsonValue& value)
{
	QJsonArray wrapper;
	wrapper.append(value);
	const QByteArray encoded = QJsonDocument(wrapper).toJson(QJsonDocument::Compact);
	return qMax<qint64>(0, encoded.size() - 2);
}

QString encodedPathSegment(const QString& value)
{
	return QString::fromLatin1(QUrl::toPercentEncoding(value));
}

bool supportedImageMimeType(const QString& mimeType)
{
	return mimeType == QStringLiteral("image/png") ||
		mimeType == QStringLiteral("image/jpeg") ||
		mimeType == QStringLiteral("image/webp") ||
		mimeType == QStringLiteral("image/gif");
}

CloudBoardError invalidResponseError(
	QNetworkReply* reply,
	const QString& detail)
{
	CloudBoardError error;
	error.code = QStringLiteral("invalid_response");
	error.message = detail.isEmpty()
		? QStringLiteral("Cloud Board returned an invalid response.")
		: QStringLiteral("Cloud Board returned an invalid response: %1").arg(detail);
	error.httpStatus = reply->attribute(
		QNetworkRequest::HttpStatusCodeAttribute).toInt();
	error.networkError = reply->error();
	return error;
}

QSharedPointer<bool> abortWhenReplyExceeds(
	QNetworkReply* reply,
	qint64 maximumBytes)
{
	const QSharedPointer<bool> tooLarge(new bool(false));
	QObject::connect(
		reply,
		&QNetworkReply::metaDataChanged,
		reply,
		[reply, tooLarge, maximumBytes]() {
			const QVariant length = reply->header(
				QNetworkRequest::ContentLengthHeader);
			if (length.isValid() && length.toLongLong() > maximumBytes) {
				*tooLarge = true;
				reply->abort();
			}
		});
	QObject::connect(
		reply,
		&QNetworkReply::downloadProgress,
		reply,
		[reply, tooLarge, maximumBytes](qint64 received, qint64) {
			if (received > maximumBytes) {
				*tooLarge = true;
				reply->abort();
			}
		});
	return tooLarge;
}

} // namespace

constexpr qint64 CloudBoardClient::MaximumBoardContentBytes;
constexpr qint64 CloudBoardClient::MaximumFileBytes;

CloudBoardClient::CloudBoardClient(QObject* parent)
	: QObject(parent),
	  networkAccessManager_(new QNetworkAccessManager(this)),
	  baseUrl_(DefaultBaseUrl)
{
	qRegisterMetaType<CloudBoardResult>();
	qRegisterMetaType<CloudFileUploadResult>();
	qRegisterMetaType<CloudFileDownloadResult>();
}

CloudBoardClient::CloudBoardClient(
	QNetworkAccessManager* networkAccessManager,
	QObject* parent)
	: QObject(parent),
	  networkAccessManager_(networkAccessManager),
	  baseUrl_(DefaultBaseUrl)
{
	Q_ASSERT(networkAccessManager_);
	qRegisterMetaType<CloudBoardResult>();
	qRegisterMetaType<CloudFileUploadResult>();
	qRegisterMetaType<CloudFileDownloadResult>();
}

QUrl CloudBoardClient::baseUrl() const
{
	return baseUrl_;
}

void CloudBoardClient::setBaseUrl(const QUrl& baseUrl)
{
	baseUrl_ = baseUrl;
}

void CloudBoardClient::setApiKey(const QString& apiKey)
{
	apiKey_ = apiKey.trimmed();
}

void CloudBoardClient::clearApiKey()
{
	apiKey_.clear();
}

bool CloudBoardClient::hasApiKey() const
{
	return !apiKey_.isEmpty();
}

quint64 CloudBoardClient::nextRequestId()
{
	const quint64 result = nextRequestId_++;
	if (nextRequestId_ == 0)
		nextRequestId_ = 1;
	return result;
}

QUrl CloudBoardClient::endpoint(const QString& path) const
{
	QString base = baseUrl_.toString(QUrl::RemoveQuery | QUrl::RemoveFragment);
	while (base.endsWith(QLatin1Char('/')))
		base.chop(1);
	return QUrl(base + QLatin1Char('/') + path);
}

QNetworkRequest CloudBoardClient::requestFor(const QUrl& url) const
{
	QNetworkRequest request(url);
	request.setTransferTimeout(120000);
	request.setRawHeader(
		"Authorization",
		QByteArrayLiteral("Bearer ") + apiKey_.toUtf8());
	request.setRawHeader("Accept", "application/json");
	request.setAttribute(
		QNetworkRequest::RedirectPolicyAttribute,
		QNetworkRequest::ManualRedirectPolicy);
	return request;
}

QNetworkReply* CloudBoardClient::sendGet(const QUrl& url)
{
	return networkAccessManager_->get(requestFor(url));
}

bool CloudBoardClient::validateCredentials(CloudBoardError* error) const
{
	if (apiKey_.isEmpty()) {
		*error = localError(
			QStringLiteral("authentication_required"),
			QStringLiteral("Enter a Cloud Board API key before syncing."));
		return false;
	}
	if (!baseUrl_.isValid() ||
		(baseUrl_.scheme() != QStringLiteral("https") &&
		 baseUrl_.scheme() != QStringLiteral("http")) ||
		baseUrl_.host().isEmpty()) {
		*error = localError(
			QStringLiteral("invalid_base_url"),
			QStringLiteral("The Cloud Board service address is invalid."));
		return false;
	}
	return true;
}

quint64 CloudBoardClient::getBoard(const QString& key)
{
	const quint64 requestId = nextRequestId();
	CloudBoardResult immediate;
	if (!validateCredentials(&immediate.error)) {
		QTimer::singleShot(0, this, [this, requestId, immediate]() {
			emit boardReceived(requestId, immediate);
		});
		return requestId;
	}

	static const QRegularExpression keyPattern(
		QStringLiteral("^[A-Za-z0-9][A-Za-z0-9._-]{0,127}$"));
	if (!keyPattern.match(key).hasMatch()) {
		immediate.error = localError(
			QStringLiteral("invalid_board_key"),
			QStringLiteral("The Cloud Board name is invalid."));
		QTimer::singleShot(0, this, [this, requestId, immediate]() {
			emit boardReceived(requestId, immediate);
		});
		return requestId;
	}

	QNetworkReply* reply = sendGet(endpoint(
		QStringLiteral("boards/%1").arg(encodedPathSegment(key))));
	const auto tooLarge = abortWhenReplyExceeds(
		reply, MaximumBoardContentBytes + 64 * 1024);
	connect(reply, &QNetworkReply::finished, this, [this, requestId, reply, tooLarge]() {
		CloudBoardResult result;
		const QByteArray body = reply->readAll();
		const int status = reply->attribute(
			QNetworkRequest::HttpStatusCodeAttribute).toInt();
		QJsonObject object;
		QString parseError;
		const bool validObject = parseObject(body, &object, &parseError);

		if (*tooLarge) {
			result.error = localError(
				QStringLiteral("board_too_large"),
				QStringLiteral("Cloud Board returned more than the 2 MB limit."));
			result.error.httpStatus = status;
			result.error.networkError = reply->error();
		} else if (status == 404) {
			result.missing = true;
			result.error.httpStatus = status;
			result.error.code = QStringLiteral("board_not_found");
		} else if (status == 200 && reply->error() == QNetworkReply::NoError) {
			if (validObject && parseBoard(object, &result.board))
				result.ok = true;
			else
				result.error = invalidResponseError(reply, parseError);
		} else {
			result.error = responseError(reply, object);
		}
		reply->deleteLater();
		emit boardReceived(requestId, result);
	});
	return requestId;
}

quint64 CloudBoardClient::putBoard(
	const QString& key,
	const QJsonValue& content,
	qint64 version)
{
	const quint64 requestId = nextRequestId();
	CloudBoardResult immediate;
	if (!validateCredentials(&immediate.error)) {
		QTimer::singleShot(0, this, [this, requestId, immediate]() {
			emit boardWritten(requestId, immediate);
		});
		return requestId;
	}

	static const QRegularExpression keyPattern(
		QStringLiteral("^[A-Za-z0-9][A-Za-z0-9._-]{0,127}$"));
	if (!keyPattern.match(key).hasMatch()) {
		immediate.error = localError(
			QStringLiteral("invalid_board_key"),
			QStringLiteral("The Cloud Board name is invalid."));
	} else if (version < 0 || version > MaximumExactJsonInteger) {
		immediate.error = localError(
			QStringLiteral("invalid_version"),
			QStringLiteral("The Cloud Board version is invalid."));
	} else if (content.isUndefined()) {
		immediate.error = localError(
			QStringLiteral("invalid_content"),
			QStringLiteral("Cloud Board content cannot be undefined."));
	} else if (serializedValueSize(content) > MaximumBoardContentBytes) {
		immediate.error = localError(
			QStringLiteral("board_too_large"),
			QStringLiteral("Cloud Board data exceeds the 2 MB limit."));
	}
	if (!immediate.error.code.isEmpty()) {
		QTimer::singleShot(0, this, [this, requestId, immediate]() {
			emit boardWritten(requestId, immediate);
		});
		return requestId;
	}

	QJsonObject payload;
	payload.insert(QStringLiteral("content"), content);
	payload.insert(QStringLiteral("version"), static_cast<double>(version));
	const QByteArray body = QJsonDocument(payload).toJson(QJsonDocument::Compact);
	if (body.size() > MaximumBoardContentBytes) {
		immediate.error = localError(
			QStringLiteral("board_too_large"),
			QStringLiteral("Cloud Board data exceeds the 2 MB limit."));
		QTimer::singleShot(0, this, [this, requestId, immediate]() {
			emit boardWritten(requestId, immediate);
		});
		return requestId;
	}
	QNetworkRequest request = requestFor(endpoint(
		QStringLiteral("boards/%1").arg(encodedPathSegment(key))));
	request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
	QNetworkReply* reply = networkAccessManager_->put(request, body);
	const auto tooLarge = abortWhenReplyExceeds(
		reply, MaximumBoardContentBytes + 64 * 1024);
	connect(reply, &QNetworkReply::finished, this, [this, requestId, reply, tooLarge]() {
		CloudBoardResult result;
		const QByteArray responseBody = reply->readAll();
		const int status = reply->attribute(
			QNetworkRequest::HttpStatusCodeAttribute).toInt();
		QJsonObject object;
		QString parseError;
		const bool validObject = parseObject(responseBody, &object, &parseError);

		if (*tooLarge) {
			result.error = localError(
				QStringLiteral("board_too_large"),
				QStringLiteral("Cloud Board returned more than the 2 MB limit."));
			result.error.httpStatus = status;
			result.error.networkError = reply->error();
		} else if ((status == 200 || status == 201) &&
			reply->error() == QNetworkReply::NoError) {
			if (validObject && parseBoard(object, &result.board))
				result.ok = true;
			else
				result.error = invalidResponseError(reply, parseError);
		} else {
			result.conflict = status == 409;
			result.error = responseError(reply, object);
			if (result.conflict && validObject)
				result.hasCurrentBoard = parseCurrentBoard(object, &result.currentBoard);
		}
		reply->deleteLater();
		emit boardWritten(requestId, result);
	});
	return requestId;
}

quint64 CloudBoardClient::uploadFile(const QString& path)
{
	const quint64 requestId = nextRequestId();
	CloudFileUploadResult immediate;
	if (!validateCredentials(&immediate.error)) {
		QTimer::singleShot(0, this, [this, requestId, immediate]() {
			emit fileUploaded(requestId, immediate);
		});
		return requestId;
	}

	const QFileInfo info(path);
	if (!info.exists() || !info.isFile()) {
		immediate.error = localError(
			QStringLiteral("file_not_found"),
			QStringLiteral("The image file does not exist."));
	} else if (info.size() > MaximumFileBytes) {
		immediate.error = localError(
			QStringLiteral("file_too_large"),
			QStringLiteral("The image exceeds the 10 MB limit."));
	}

	QMimeDatabase mimeDatabase;
	QString mimeType;
	if (immediate.error.code.isEmpty()) {
		mimeType = mimeDatabase.mimeTypeForFile(
			info.absoluteFilePath(), QMimeDatabase::MatchContent).name();
		if (!supportedImageMimeType(mimeType)) {
			const QString extensionMime = mimeDatabase.mimeTypeForFile(
				info.absoluteFilePath(), QMimeDatabase::MatchExtension).name();
			if (supportedImageMimeType(extensionMime))
				mimeType = extensionMime;
		}
		if (!supportedImageMimeType(mimeType)) {
			immediate.error = localError(
				QStringLiteral("unsupported_image_type"),
				QStringLiteral("Cloud Board supports PNG, JPEG, WebP, and GIF images."));
		}
	}
	if (!immediate.error.code.isEmpty()) {
		QTimer::singleShot(0, this, [this, requestId, immediate]() {
			emit fileUploaded(requestId, immediate);
		});
		return requestId;
	}

	QHttpMultiPart* multipart = new QHttpMultiPart(QHttpMultiPart::FormDataType);
	QFile* file = new QFile(info.absoluteFilePath(), multipart);
	if (!file->open(QIODevice::ReadOnly)) {
		delete multipart;
		immediate.error = localError(
			QStringLiteral("file_read_failed"),
			QStringLiteral("The image could not be read."));
		QTimer::singleShot(0, this, [this, requestId, immediate]() {
			emit fileUploaded(requestId, immediate);
		});
		return requestId;
	}

	QHttpPart filePart;
	filePart.setHeader(QNetworkRequest::ContentTypeHeader, mimeType);
	filePart.setHeader(
		QNetworkRequest::ContentDispositionHeader,
		QStringLiteral("form-data; name=\"file\"; filename=\"%1\"")
			.arg(info.fileName()));
	filePart.setBodyDevice(file);
	multipart->append(filePart);

	QNetworkRequest request = requestFor(endpoint(QStringLiteral("files")));
	QNetworkReply* reply = networkAccessManager_->post(request, multipart);
	multipart->setParent(reply);
	const auto tooLarge = abortWhenReplyExceeds(
		reply, MaximumMetadataResponseBytes);
	connect(reply, &QNetworkReply::finished, this, [this, requestId, reply, tooLarge]() {
		CloudFileUploadResult result;
		const QByteArray responseBody = reply->readAll();
		const int status = reply->attribute(
			QNetworkRequest::HttpStatusCodeAttribute).toInt();
		QJsonObject object;
		QString parseError;
		const bool validObject = parseObject(responseBody, &object, &parseError);
		if (*tooLarge) {
			result.error = localError(
				QStringLiteral("invalid_response"),
				QStringLiteral("Cloud Board returned an unexpectedly large upload response."));
			result.error.httpStatus = status;
			result.error.networkError = reply->error();
		} else if (status == 201 && reply->error() == QNetworkReply::NoError && validObject &&
			object.value(QStringLiteral("fileId")).isString() &&
			object.value(QStringLiteral("url")).isString() &&
			object.value(QStringLiteral("mimeType")).isString() &&
			object.value(QStringLiteral("size")).isDouble() &&
			object.value(QStringLiteral("createdAt")).isString()) {
			const double numericSize = object.value(QStringLiteral("size")).toDouble(-1.0);
			if (!std::isfinite(numericSize) || numericSize < 0.0 ||
				std::floor(numericSize) != numericSize ||
				numericSize > static_cast<double>(MaximumFileBytes)) {
				result.error = invalidResponseError(reply, {});
				reply->deleteLater();
				emit fileUploaded(requestId, result);
				return;
			}
			result.file.fileId = object.value(QStringLiteral("fileId")).toString();
			result.file.url = QUrl(object.value(QStringLiteral("url")).toString());
			result.file.mimeType = object.value(QStringLiteral("mimeType")).toString();
			result.file.size = static_cast<qint64>(numericSize);
			result.file.createdAt = object.value(QStringLiteral("createdAt")).toString();
			result.ok = !result.file.fileId.isEmpty() &&
				result.file.url.isValid() &&
				result.file.size >= 0 &&
				result.file.size <= MaximumFileBytes;
			if (!result.ok)
				result.error = invalidResponseError(reply, {});
		} else if (status == 201 && reply->error() == QNetworkReply::NoError) {
			result.error = invalidResponseError(reply, parseError);
		} else {
			result.error = responseError(reply, object);
		}
		reply->deleteLater();
		emit fileUploaded(requestId, result);
	});
	return requestId;
}

quint64 CloudBoardClient::downloadFile(
	const QString& fileId,
	const QString& destination)
{
	const quint64 requestId = nextRequestId();
	CloudFileDownloadResult immediate;
	immediate.fileId = fileId;
	immediate.destination = destination;
	if (!validateCredentials(&immediate.error)) {
		QTimer::singleShot(0, this, [this, requestId, immediate]() {
			emit fileDownloaded(requestId, immediate);
		});
		return requestId;
	}
	if (fileId.trimmed().isEmpty()) {
		immediate.error = localError(
			QStringLiteral("invalid_file_id"),
			QStringLiteral("The Cloud Board file ID is empty."));
	} else if (destination.trimmed().isEmpty()) {
		immediate.error = localError(
			QStringLiteral("invalid_destination"),
			QStringLiteral("Choose where to save the downloaded image."));
	}
	if (!immediate.error.code.isEmpty()) {
		QTimer::singleShot(0, this, [this, requestId, immediate]() {
			emit fileDownloaded(requestId, immediate);
		});
		return requestId;
	}

	QNetworkReply* reply = sendGet(endpoint(
		QStringLiteral("files/%1").arg(encodedPathSegment(fileId))));
	const QSharedPointer<bool> tooLarge(new bool(false));
	connect(reply, &QNetworkReply::metaDataChanged, this, [reply, tooLarge]() {
		const QVariant length = reply->header(QNetworkRequest::ContentLengthHeader);
		if (length.isValid() && length.toLongLong() > CloudBoardClient::MaximumFileBytes) {
			*tooLarge = true;
			reply->abort();
		}
	});
	connect(
		reply,
		&QNetworkReply::downloadProgress,
		this,
		[reply, tooLarge](qint64 received, qint64) {
			if (received > CloudBoardClient::MaximumFileBytes) {
				*tooLarge = true;
				reply->abort();
			}
		});
	connect(reply, &QNetworkReply::finished, this,
		[this, requestId, reply, fileId, destination, tooLarge]() {
		CloudFileDownloadResult result;
		result.fileId = fileId;
		result.destination = destination;
		const QByteArray body = reply->readAll();
		const int status = reply->attribute(
			QNetworkRequest::HttpStatusCodeAttribute).toInt();
		if (*tooLarge || body.size() > MaximumFileBytes) {
			result.error = localError(
				QStringLiteral("file_too_large"),
				QStringLiteral("The downloaded image exceeds the 10 MB limit."));
			result.error.httpStatus = status;
			result.error.networkError = reply->error();
		} else if (status == 200 && reply->error() == QNetworkReply::NoError) {
			QSaveFile output(destination);
			if (!output.open(QIODevice::WriteOnly) ||
				output.write(body) != body.size() ||
				!output.commit()) {
				output.cancelWriting();
				result.error = localError(
					QStringLiteral("file_write_failed"),
					QStringLiteral("The downloaded image could not be saved."));
			} else {
				result.ok = true;
				result.size = body.size();
			}
		} else {
			QJsonObject object;
			parseObject(body, &object);
			result.error = responseError(reply, object);
		}
		reply->deleteLater();
		emit fileDownloaded(requestId, result);
	});
	return requestId;
}
