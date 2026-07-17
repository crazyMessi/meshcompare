#include "camera_pose_store.h"
#include "camera_pose_store_file_ops.h"

#include "core/camera_pose_uuid.h"

#include <QDateTime>
#include <QDir>
#include <QDomDocument>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QTemporaryDir>

#include <algorithm>
#include <utility>

namespace {

const QString SchemaKey = QStringLiteral("schema");
const QString PosesKey = QStringLiteral("poses");
const QString ViewIdKey = QStringLiteral("view_id");
const QString SavedAtUtcKey = QStringLiteral("saved_at_utc");
const QString ParametersKey = QStringLiteral("parameters");
const QString ViewStateXmlKey = QStringLiteral("meshlab_view_state_xml");

const QRegularExpression NumericViewIdPattern(QStringLiteral("^view_(\\d+)$"));

struct ParsedView
{
	QString uuid;
	QString viewId;
	QString savedAtUtc;
	QDateTime savedAt;
	bool hasSavedAt = false;
	CameraPose pose;
};

struct ParsedCollection
{
	QString sourceKey;
	QJsonValue sourceValue;
	QVector<ParsedView> views;
};

struct ParsedLibrary
{
	QJsonObject root;
	QJsonObject poses;
	QHash<QString, ParsedCollection> collections;
};

OperationResult failure(const QString& message)
{
	return OperationResult::failure(message);
}

void setResult(OperationResult* destination, const OperationResult& result)
{
	if (destination != nullptr)
		*destination = result;
}

ParsedLibrary emptyLibrary()
{
	ParsedLibrary library;
	library.root.insert(SchemaKey, 2);
	library.root.insert(PosesKey, QJsonObject());
	return library;
}

QString fallbackViewId(int index)
{
	return QStringLiteral("view_%1").arg(index + 1, 3, 10, QLatin1Char('0'));
}

QDateTime parseTimestamp(const QString& value)
{
	QDateTime parsed = QDateTime::fromString(value, Qt::ISODateWithMs);
	if (!parsed.isValid())
		parsed = QDateTime::fromString(value, Qt::ISODate);
	return parsed;
}

OperationResult parseLibraryRoot(const QJsonObject& root, ParsedLibrary* destination)
{
	const QJsonValue schema = root.value(SchemaKey);
	if (!schema.isDouble() || schema.toDouble() != 2.0)
		return failure(QStringLiteral("The camera-pose library schema is not supported."));

	const QJsonValue posesValue = root.value(PosesKey);
	if (!posesValue.isObject())
		return failure(QStringLiteral("The camera-pose library has an invalid poses section."));

	ParsedLibrary parsed;
	parsed.root = root;
	parsed.poses = posesValue.toObject();

	for (auto poseIt = parsed.poses.constBegin(); poseIt != parsed.poses.constEnd(); ++poseIt) {
		const QString normalizedUuid = CameraPoseStore::normalizeUuid(poseIt.key());
		if (normalizedUuid.isEmpty())
			return failure(QStringLiteral("The camera-pose library contains an invalid UUID key."));
		if (parsed.collections.contains(normalizedUuid))
			return failure(QStringLiteral("The camera-pose library contains duplicate UUID keys."));

		QJsonArray values;
		if (poseIt.value().isArray()) {
			values = poseIt.value().toArray();
		}
		else if (poseIt.value().isObject()) {
			values.append(poseIt.value());
		}
		else {
			return failure(QStringLiteral("The camera-pose library contains an invalid UUID entry."));
		}

		ParsedCollection collection;
		collection.sourceKey = poseIt.key();
		collection.sourceValue = poseIt.value();
		QSet<QString> viewIds;
		for (int index = 0; index < values.size(); ++index) {
			const QJsonValue viewValue = values.at(index);
			if (!viewValue.isObject())
				return failure(QStringLiteral("The camera-pose library contains an invalid view entry."));
			const QJsonObject viewObject = viewValue.toObject();

			QString viewId;
			if (viewObject.contains(ViewIdKey)) {
				const QJsonValue idValue = viewObject.value(ViewIdKey);
				if (!idValue.isString())
					return failure(QStringLiteral("The camera-pose library contains an invalid view ID."));
				viewId = idValue.toString();
			}
			if (viewId.isEmpty())
				viewId = fallbackViewId(index);
			if (viewIds.contains(viewId))
				return failure(QStringLiteral("The camera-pose library contains duplicate view IDs."));
			viewIds.insert(viewId);

			const QJsonValue xmlValue = viewObject.value(ViewStateXmlKey);
			if (!xmlValue.isString() || xmlValue.toString().isEmpty())
				return failure(QStringLiteral(
					"The camera-pose library contains a view without MeshLab camera data."));

			ParsedView view;
			view.uuid = normalizedUuid;
			view.viewId = viewId;
			view.pose.viewStateXml = xmlValue.toString();
			if (viewObject.contains(SavedAtUtcKey)) {
				const QJsonValue timestampValue = viewObject.value(SavedAtUtcKey);
				if (!timestampValue.isString())
					return failure(QStringLiteral("The camera-pose library contains an invalid saved time."));
				view.savedAtUtc = timestampValue.toString();
				if (!view.savedAtUtc.isEmpty()) {
					view.savedAt = parseTimestamp(view.savedAtUtc);
					if (!view.savedAt.isValid())
						return failure(QStringLiteral("The camera-pose library contains an invalid saved time."));
					view.hasSavedAt = true;
				}
			}
			collection.views.push_back(view);
		}
		parsed.collections.insert(normalizedUuid, collection);
	}

	if (destination != nullptr)
		*destination = parsed;
	return OperationResult::success();
}

OperationResult readLibraryFile(
	const QString& path,
	bool missingIsEmpty,
	ParsedLibrary* destination)
{
	if (path.isEmpty())
		return failure(QStringLiteral("The camera-pose library path is empty."));

	QFile input(path);
	if (!input.exists()) {
		if (!missingIsEmpty)
			return failure(QStringLiteral("The camera-pose library does not exist."));
		if (destination != nullptr)
			*destination = emptyLibrary();
		return OperationResult::success();
	}
	if (!input.open(QIODevice::ReadOnly))
		return failure(input.errorString());

	QJsonParseError parseError;
	const QJsonDocument document = QJsonDocument::fromJson(input.readAll(), &parseError);
	if (parseError.error != QJsonParseError::NoError || !document.isObject())
		return failure(QStringLiteral("The camera-pose library is not valid JSON."));
	return parseLibraryRoot(document.object(), destination);
}

OperationResult ensureParentDirectory(const QString& path)
{
	if (path.isEmpty())
		return failure(QStringLiteral("The camera-pose library path is empty."));
	const QString directory = QFileInfo(path).absolutePath();
	if (directory.isEmpty() || !QDir().mkpath(directory))
		return failure(QStringLiteral("Unable to create the camera-pose library directory."));
	return OperationResult::success();
}

bool pathEntryExists(const QString& path)
{
	const QFileInfo info(path);
	return info.exists() || info.isSymLink();
}

OperationResult writeLibraryFile(const QString& path, const QJsonObject& root)
{
	const OperationResult directoryResult = ensureParentDirectory(path);
	if (!directoryResult.ok)
		return directoryResult;

	QSaveFile output(path);
	if (!output.open(QIODevice::WriteOnly))
		return failure(output.errorString());
	const QByteArray json = QJsonDocument(root).toJson(QJsonDocument::Indented);
	if (output.write(json) != json.size()) {
		const QString message = output.errorString();
		output.cancelWriting();
		return failure(message.isEmpty()
			? QStringLiteral("Unable to write the camera-pose library.")
			: message);
	}
	if (!output.commit()) {
		const QString message = output.errorString();
		return failure(message.isEmpty()
			? QStringLiteral("Unable to replace the camera-pose library.")
			: message);
	}
	return OperationResult::success();
}

QJsonArray numberArray(const QString& text, int maximumCount = -1)
{
	QJsonArray result;
	const QStringList values = text.split(QLatin1Char(' '), Qt::SkipEmptyParts);
	for (const QString& value : values) {
		bool ok = false;
		const double number = value.toDouble(&ok);
		if (ok)
			result.append(number);
		if (maximumCount >= 0 && result.size() >= maximumCount)
			break;
	}
	return result;
}

QJsonArray matrixArray(const QString& text)
{
	const QJsonArray values = numberArray(text, 16);
	QJsonArray matrix;
	if (values.size() != 16)
		return matrix;
	for (int row = 0; row < 4; ++row) {
		QJsonArray matrixRow;
		for (int column = 0; column < 4; ++column)
			matrixRow.append(values.at(row * 4 + column));
		matrix.append(matrixRow);
	}
	return matrix;
}

void addNumberArray(
	QJsonObject& object,
	const QString& key,
	const QString& text,
	int maximumCount = -1)
{
	const QJsonArray values = numberArray(text, maximumCount);
	if (!values.isEmpty())
		object.insert(key, values);
}

void addNumber(QJsonObject& object, const QString& key, const QString& text)
{
	bool ok = false;
	const double value = text.toDouble(&ok);
	if (ok)
		object.insert(key, value);
}

QJsonObject poseParameters(const QString& viewStateXml)
{
	QDomDocument document;
	if (!document.setContent(viewStateXml))
		return QJsonObject();

	const QDomElement root = document.documentElement();
	const QDomElement camera = root.firstChildElement(QStringLiteral("VCGCamera"));
	if (camera.isNull())
		return QJsonObject();

	QJsonObject parameters;
	addNumberArray(
		parameters,
		QStringLiteral("translation_vector"),
		camera.attribute(QStringLiteral("TranslationVector")),
		3);
	const QJsonArray rotationMatrix =
		matrixArray(camera.attribute(QStringLiteral("RotationMatrix")));
	if (!rotationMatrix.isEmpty())
		parameters.insert(QStringLiteral("rotation_matrix"), rotationMatrix);
	addNumber(
		parameters,
		QStringLiteral("camera_type"),
		camera.attribute(QStringLiteral("CameraType")));
	addNumber(parameters, QStringLiteral("focal_mm"), camera.attribute(QStringLiteral("FocalMm")));
	addNumberArray(
		parameters,
		QStringLiteral("lens_distortion"),
		camera.attribute(QStringLiteral("LensDistortion")),
		2);
	addNumberArray(
		parameters,
		QStringLiteral("pixel_size_mm"),
		camera.attribute(QStringLiteral("PixelSizeMm")),
		2);
	addNumberArray(
		parameters,
		QStringLiteral("viewport_px"),
		camera.attribute(QStringLiteral("ViewportPx")),
		2);
	addNumberArray(
		parameters,
		QStringLiteral("center_px"),
		camera.attribute(QStringLiteral("CenterPx")),
		2);

	const QDomElement settings = root.firstChildElement(QStringLiteral("ViewSettings"));
	if (!settings.isNull()) {
		addNumber(
			parameters,
			QStringLiteral("track_scale"),
			settings.attribute(QStringLiteral("TrackScale")));
		addNumber(
			parameters,
			QStringLiteral("near_plane"),
			settings.attribute(QStringLiteral("NearPlane")));
		addNumber(
			parameters,
			QStringLiteral("far_plane"),
			settings.attribute(QStringLiteral("FarPlane")));
	}
	return parameters;
}

bool parsedViewLess(const ParsedView& left, const ParsedView& right)
{
	if (left.hasSavedAt != right.hasSavedAt)
		return !left.hasSavedAt;
	if (left.hasSavedAt && left.savedAt != right.savedAt)
		return left.savedAt < right.savedAt;
	return left.viewId < right.viewId;
}

SavedCameraPose savedPose(const ParsedView& view)
{
	SavedCameraPose result;
	result.uuid = view.uuid;
	result.viewId = view.viewId;
	result.savedAtUtc = view.savedAtUtc;
	result.pose = view.pose;
	return result;
}

QJsonArray collectionAsArray(const ParsedCollection& collection)
{
	QJsonArray values;
	if (collection.sourceValue.isArray())
		values = collection.sourceValue.toArray();
	else
		values.append(collection.sourceValue);

	// Once a compatible legacy collection is rewritten, persist its logical
	// fallback IDs. Otherwise removing an earlier entry would renumber every
	// later missing ID and make previously listed poses impossible to address.
	for (int index = 0; index < values.size(); ++index) {
		QJsonObject object = values.at(index).toObject();
		if (object.value(ViewIdKey).toString().isEmpty()) {
			object.insert(ViewIdKey, collection.views.at(index).viewId);
			values[index] = object;
		}
	}
	return values;
}

QString canonicalDecimal(QString digits)
{
	int firstNonZero = 0;
	while (firstNonZero + 1 < digits.size()
		&& digits.at(firstNonZero) == QLatin1Char('0')) {
		++firstNonZero;
	}
	return digits.mid(firstNonZero);
}

bool decimalMagnitudeLess(const QString& left, const QString& right)
{
	if (left.size() != right.size())
		return left.size() < right.size();
	return left < right;
}

QString incrementDecimal(QString digits)
{
	for (int index = digits.size() - 1; index >= 0; --index) {
		if (digits.at(index) != QLatin1Char('9')) {
			digits[index] = QChar(digits.at(index).unicode() + 1);
			return digits;
		}
		digits[index] = QLatin1Char('0');
	}
	return QLatin1Char('1') + digits;
}

OperationResult invalidUuidResult()
{
	return failure(QStringLiteral("UUID must contain exactly 32 hexadecimal characters."));
}

}

CameraPoseStore::CameraPoseStore(QString storagePath, QString legacyPath)
	: storagePath_(std::move(storagePath)), legacyPath_(std::move(legacyPath))
{
}

OperationResult CameraPoseStore::migrateLegacyIfNeeded()
{
	if (storagePath_.isEmpty())
		return failure(QStringLiteral("The camera-pose library path is empty."));
	if (pathEntryExists(storagePath_))
		return OperationResult::success();
	if (legacyPath_.isEmpty() || !QFileInfo::exists(legacyPath_))
		return OperationResult::success();

	ParsedLibrary legacy;
	const OperationResult validation = readLibraryFile(legacyPath_, false, &legacy);
	if (!validation.ok)
		return validation;
	const OperationResult directoryResult = ensureParentDirectory(storagePath_);
	if (!directoryResult.ok)
		return directoryResult;
	if (pathEntryExists(storagePath_))
		return OperationResult::success();

	// Copy into an owned staging directory first. The legacy path is opened
	// again by QFile::copy(), so validate the staged bytes as well: the source
	// may have been replaced after the first validation. Exclusive hard-link
	// publication keeps the destination atomic and never deletes a library
	// another process created while this migration was in flight.
	const QString parentDirectory = QFileInfo(storagePath_).absolutePath();
	QTemporaryDir stagingDirectory(
		QDir(parentDirectory).filePath(
			QStringLiteral(".meshcompare-camera-migration-XXXXXX")));
	if (!stagingDirectory.isValid())
		return failure(QStringLiteral("Unable to create a camera-pose migration directory."));
	const QString stagedPath =
		stagingDirectory.filePath(QStringLiteral("camera_poses.json"));
	if (!QFile::copy(legacyPath_, stagedPath))
		return failure(QStringLiteral("Unable to copy the legacy camera-pose library."));

	ParsedLibrary staged;
	const OperationResult stagedValidation = readLibraryFile(stagedPath, false, &staged);
	if (!stagedValidation.ok)
		return stagedValidation;
	QString publishError;
	const camera_pose_store_detail::PublishResult publishResult =
		camera_pose_store_detail::publishFileNoReplace(
			stagedPath, storagePath_, &publishError);
	if (publishResult == camera_pose_store_detail::PublishResult::Failed)
		return failure(publishError);
	return OperationResult::success();
}

OperationResult CameraPoseStore::save(
	const QString& uuid,
	const CameraPose& pose,
	QString* viewId)
{
	const QString normalizedUuid = normalizeUuid(uuid);
	if (normalizedUuid.isEmpty())
		return invalidUuidResult();
	if (pose.viewStateXml.isEmpty())
		return failure(QStringLiteral("The camera pose contains no MeshLab camera data."));

	ParsedLibrary library;
	const OperationResult readResult = readLibraryFile(storagePath_, true, &library);
	if (!readResult.ok)
		return readResult;

	QJsonArray views;
	QString maximumId = QStringLiteral("0");
	const auto collectionIt = library.collections.constFind(normalizedUuid);
	if (collectionIt != library.collections.constEnd()) {
		views = collectionAsArray(collectionIt.value());
		for (const ParsedView& existing : collectionIt.value().views) {
			const QRegularExpressionMatch match = NumericViewIdPattern.match(existing.viewId);
			if (!match.hasMatch())
				continue;
			const QString numericId = canonicalDecimal(match.captured(1));
			if (decimalMagnitudeLess(maximumId, numericId))
				maximumId = numericId;
		}
	}

	const QString newViewId = QStringLiteral("view_%1").arg(
		incrementDecimal(maximumId).rightJustified(3, QLatin1Char('0')));
	QJsonObject newView;
	newView.insert(ViewIdKey, newViewId);
	newView.insert(
		SavedAtUtcKey,
		QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
	newView.insert(ParametersKey, poseParameters(pose.viewStateXml));
	newView.insert(ViewStateXmlKey, pose.viewStateXml);
	views.append(newView);

	QJsonObject poses = library.poses;
	if (collectionIt != library.collections.constEnd()
		&& collectionIt.value().sourceKey != normalizedUuid) {
		poses.remove(collectionIt.value().sourceKey);
	}
	poses.insert(normalizedUuid, views);
	library.root.insert(SchemaKey, 2);
	library.root.insert(PosesKey, poses);

	const OperationResult writeResult = writeLibraryFile(storagePath_, library.root);
	if (!writeResult.ok)
		return writeResult;
	if (viewId != nullptr)
		*viewId = newViewId;
	return OperationResult::success();
}

QVector<SavedCameraPose> CameraPoseStore::list(
	const QString& uuid,
	OperationResult* result) const
{
	const QString normalizedUuid = normalizeUuid(uuid);
	if (normalizedUuid.isEmpty()) {
		setResult(result, invalidUuidResult());
		return QVector<SavedCameraPose>();
	}

	ParsedLibrary library;
	const OperationResult readResult = readLibraryFile(storagePath_, true, &library);
	if (!readResult.ok) {
		setResult(result, readResult);
		return QVector<SavedCameraPose>();
	}

	QVector<ParsedView> parsedViews;
	const auto collectionIt = library.collections.constFind(normalizedUuid);
	if (collectionIt != library.collections.constEnd())
		parsedViews = collectionIt.value().views;
	std::sort(parsedViews.begin(), parsedViews.end(), parsedViewLess);

	QVector<SavedCameraPose> poses;
	poses.reserve(parsedViews.size());
	for (const ParsedView& view : parsedViews)
		poses.push_back(savedPose(view));
	setResult(result, OperationResult::success());
	return poses;
}

OperationResult CameraPoseStore::load(
	const QString& uuid,
	const QString& viewId,
	CameraPose* pose) const
{
	if (pose == nullptr)
		return failure(QStringLiteral("No destination was provided for the camera data."));
	const QString normalizedUuid = normalizeUuid(uuid);
	if (normalizedUuid.isEmpty())
		return invalidUuidResult();
	if (viewId.isEmpty())
		return failure(QStringLiteral("The camera-pose view ID is empty."));

	ParsedLibrary library;
	const OperationResult readResult = readLibraryFile(storagePath_, true, &library);
	if (!readResult.ok)
		return readResult;
	const auto collectionIt = library.collections.constFind(normalizedUuid);
	if (collectionIt == library.collections.constEnd())
		return failure(QStringLiteral("No saved camera poses exist for this UUID."));
	for (const ParsedView& candidate : collectionIt.value().views) {
		if (candidate.viewId == viewId) {
			*pose = candidate.pose;
			return OperationResult::success();
		}
	}
	return failure(QStringLiteral("The selected saved camera pose no longer exists."));
}

OperationResult CameraPoseStore::remove(const QString& uuid, const QString& viewId)
{
	const QString normalizedUuid = normalizeUuid(uuid);
	if (normalizedUuid.isEmpty())
		return invalidUuidResult();
	if (viewId.isEmpty())
		return failure(QStringLiteral("The camera-pose view ID is empty."));

	ParsedLibrary library;
	const OperationResult readResult = readLibraryFile(storagePath_, true, &library);
	if (!readResult.ok)
		return readResult;
	const auto collectionIt = library.collections.constFind(normalizedUuid);
	if (collectionIt == library.collections.constEnd())
		return failure(QStringLiteral("No saved camera poses exist for this UUID."));

	int removeIndex = -1;
	for (int index = 0; index < collectionIt.value().views.size(); ++index) {
		if (collectionIt.value().views.at(index).viewId == viewId) {
			removeIndex = index;
			break;
		}
	}
	if (removeIndex < 0)
		return failure(QStringLiteral("The selected saved camera pose no longer exists."));

	QJsonObject poses = library.poses;
	poses.remove(collectionIt.value().sourceKey);
	QJsonArray remaining = collectionAsArray(collectionIt.value());
	remaining.removeAt(removeIndex);
	if (!remaining.isEmpty())
		poses.insert(normalizedUuid, remaining);
	library.root.insert(PosesKey, poses);
	return writeLibraryFile(storagePath_, library.root);
}

QVector<SavedCameraPose> CameraPoseStore::listAll(OperationResult* result) const
{
	ParsedLibrary library;
	const OperationResult readResult = readLibraryFile(storagePath_, true, &library);
	if (!readResult.ok) {
		setResult(result, readResult);
		return QVector<SavedCameraPose>();
	}

	QVector<ParsedView> parsedViews;
	for (auto collectionIt = library.collections.constBegin();
		 collectionIt != library.collections.constEnd();
		 ++collectionIt) {
		parsedViews += collectionIt.value().views;
	}
	std::sort(
		parsedViews.begin(),
		parsedViews.end(),
		[](const ParsedView& left, const ParsedView& right) {
			if (left.uuid != right.uuid)
				return left.uuid < right.uuid;
			return parsedViewLess(left, right);
		});

	QVector<SavedCameraPose> poses;
	poses.reserve(parsedViews.size());
	for (const ParsedView& view : parsedViews)
		poses.push_back(savedPose(view));
	setResult(result, OperationResult::success());
	return poses;
}

QString CameraPoseStore::normalizeUuid(const QString& value)
{
	return meshcompare::normalizeCameraPoseUuid(value);
}
