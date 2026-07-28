#include "camera_pose_store.h"
#include "camera_pose_store_file_ops.h"

#include "core/camera_pose_tags.h"
#include "core/camera_pose_uuid.h"

#include <QDateTime>
#include <QCryptographicHash>
#include <QDir>
#include <QDomDocument>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QImage>
#include <QImageWriter>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QTemporaryDir>
#include <QUuid>

#include <algorithm>
#include <utility>

namespace {

const QString SchemaKey = QStringLiteral("schema");
const QString PosesKey = QStringLiteral("poses");
const QString ViewIdKey = QStringLiteral("view_id");
const QString SavedAtUtcKey = QStringLiteral("saved_at_utc");
const QString ParametersKey = QStringLiteral("parameters");
const QString ViewStateXmlKey = QStringLiteral("meshlab_view_state_xml");
const QString TagsKey = QStringLiteral("tags");
const QString ScreenshotPathKey = QStringLiteral("screenshot_path");
const QString ScreenshotDirectoryName =
	QStringLiteral("camera_pose_screenshots");
const QString DefaultPoseTag = QStringLiteral("hole");

const QRegularExpression NumericViewIdPattern(QStringLiteral("^view_(\\d+)$"));

struct ParsedView
{
	QString uuid;
	QString viewId;
	QString savedAtUtc;
	QDateTime savedAt;
	bool hasSavedAt = false;
	QStringList tags;
	QString screenshotRelativePath;
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

QString normalizedScreenshotRelativePath(const QString& path)
{
	const QString normalized =
		QDir::cleanPath(QDir::fromNativeSeparators(path));
	const QString prefix = ScreenshotDirectoryName + QLatin1Char('/');
	if (normalized.isEmpty()
		|| QFileInfo(normalized).isAbsolute()
		|| !normalized.startsWith(prefix)
		|| normalized == QStringLiteral("..")
		|| normalized.startsWith(QStringLiteral("../"))) {
		return {};
	}
	return normalized;
}

OperationResult resolveScreenshotPath(
	const QString& storagePath,
	const QString& relativePath,
	QString* destination)
{
	const QString normalized =
		normalizedScreenshotRelativePath(relativePath);
	if (normalized.isEmpty()) {
		return failure(QStringLiteral(
			"The camera-pose library contains an unsafe screenshot path."));
	}

	const QString storageDirectory =
		QFileInfo(storagePath).absolutePath();
	const QString screenshotRoot =
		QDir::cleanPath(
			QDir(storageDirectory).filePath(ScreenshotDirectoryName));
	const QString absolutePath =
		QDir::cleanPath(QDir(storageDirectory).filePath(normalized));
	const QString pathWithinRoot =
		QDir(screenshotRoot).relativeFilePath(absolutePath);
	if (pathWithinRoot == QStringLiteral("..")
		|| pathWithinRoot.startsWith(QStringLiteral("../"))) {
		return failure(QStringLiteral(
			"The camera-pose screenshot path escapes its storage directory."));
	}

	QString currentPath = screenshotRoot;
	const QStringList components = QDir::fromNativeSeparators(pathWithinRoot)
		.split(QLatin1Char('/'), Qt::SkipEmptyParts);
	if (QFileInfo(currentPath).isSymLink()) {
		return failure(QStringLiteral(
			"The camera-pose screenshot directory must not be a symbolic link."));
	}
	for (const QString& component : components) {
		currentPath = QDir(currentPath).filePath(component);
		if (QFileInfo(currentPath).isSymLink()) {
			return failure(QStringLiteral(
				"A camera-pose screenshot path contains a symbolic link."));
		}
	}
	if (destination != nullptr)
		*destination = absolutePath;
	return OperationResult::success();
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
	QSet<QString> screenshotPaths;

	for (auto poseIt = parsed.poses.constBegin(); poseIt != parsed.poses.constEnd(); ++poseIt) {
		const QString normalizedUuid = CameraPoseStore::normalizeUuid(poseIt.key());
		if (normalizedUuid.isEmpty())
			return failure(QStringLiteral("The camera-pose library contains an invalid UID key."));
		if (parsed.collections.contains(normalizedUuid))
			return failure(QStringLiteral("The camera-pose library contains duplicate UID keys."));

		QJsonArray values;
		if (poseIt.value().isArray()) {
			values = poseIt.value().toArray();
		}
		else if (poseIt.value().isObject()) {
			values.append(poseIt.value());
		}
		else {
			return failure(QStringLiteral("The camera-pose library contains an invalid UID entry."));
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
			if (viewObject.contains(TagsKey)) {
				const QJsonValue tagsValue = viewObject.value(TagsKey);
				if (!tagsValue.isArray())
					return failure(QStringLiteral(
						"The camera-pose library contains invalid pose tags."));
				for (const QJsonValue& tagValue : tagsValue.toArray()) {
					if (!tagValue.isString())
						return failure(QStringLiteral(
							"The camera-pose library contains invalid pose tags."));
					view.tags.append(tagValue.toString());
				}
			}
			if (viewObject.contains(ScreenshotPathKey)) {
				const QJsonValue screenshotValue =
					viewObject.value(ScreenshotPathKey);
				if (!screenshotValue.isString())
					return failure(QStringLiteral(
						"The camera-pose library contains an invalid screenshot path."));
				view.screenshotRelativePath =
					normalizedScreenshotRelativePath(
						screenshotValue.toString());
				if (view.screenshotRelativePath.isEmpty()) {
					return failure(QStringLiteral(
						"The camera-pose library contains an unsafe screenshot path."));
				}
				const QString uniquenessKey =
					view.screenshotRelativePath.toCaseFolded();
				if (screenshotPaths.contains(uniquenessKey)) {
					return failure(QStringLiteral(
						"The camera-pose library maps multiple poses to one screenshot."));
				}
				screenshotPaths.insert(uniquenessKey);
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

QString readablePathComponent(const QString& value)
{
	QString readable;
	const QString normalized = value.normalized(
		QString::NormalizationForm_KC).trimmed();
	readable.reserve(normalized.size());
	bool previousWasSeparator = false;
	for (const QChar character : normalized) {
		const bool safe = character.isLetterOrNumber()
			|| character == QLatin1Char('-')
			|| character == QLatin1Char('_');
		if (safe) {
			readable.append(character);
			previousWasSeparator = false;
		}
		else if (!previousWasSeparator) {
			readable.append(QLatin1Char('_'));
			previousWasSeparator = true;
		}
	}
	while (readable.startsWith(QLatin1Char('_')))
		readable.remove(0, 1);
	while (readable.endsWith(QLatin1Char('_')))
		readable.chop(1);
	if (readable.isEmpty())
		readable = QStringLiteral("item");
	if (readable.size() > 64)
		readable.truncate(64);
	const QString digest = QString::fromLatin1(
		QCryptographicHash::hash(
			normalized.toUtf8(), QCryptographicHash::Sha256).toHex().left(10));
	return QStringLiteral("%1-%2").arg(readable, digest);
}

QString screenshotClassification(const QStringList& tags)
{
	QStringList normalized = meshcompare::normalizeCameraPoseTags(tags);
	std::sort(
		normalized.begin(),
		normalized.end(),
		[](const QString& left, const QString& right) {
			return left.compare(right, Qt::CaseInsensitive) < 0;
		});
	if (normalized.isEmpty())
		return QStringLiteral("untagged");
	return readablePathComponent(normalized.join(QStringLiteral("__")));
}

QString screenshotRelativePath(
	const QString& normalizedUuid,
	const QString& viewId,
	const QStringList& tags)
{
	return QDir(ScreenshotDirectoryName)
		.filePath(
			QDir(readablePathComponent(normalizedUuid))
				.filePath(
					QDir(screenshotClassification(tags))
						.filePath(
							readablePathComponent(viewId)
							+ QStringLiteral(".png"))));
}

QString absoluteScreenshotPath(
	const QString& storagePath,
	const QString& relativePath)
{
	if (relativePath.isEmpty())
		return {};
	QString resolved;
	if (!resolveScreenshotPath(storagePath, relativePath, &resolved).ok)
		return {};
	return resolved;
}

OperationResult writeScreenshot(
	const QString& path,
	const QImage& screenshot)
{
	if (screenshot.isNull())
		return failure(QStringLiteral("The camera-pose screenshot is empty."));
	if (!QDir().mkpath(QFileInfo(path).absolutePath())) {
		return failure(QStringLiteral(
			"Unable to create the camera-pose screenshot directory."));
	}

	QSaveFile output(path);
	if (!output.open(QIODevice::WriteOnly))
		return failure(output.errorString());
	QImageWriter writer(&output, "png");
	if (!writer.write(screenshot)) {
		const QString message = writer.errorString();
		output.cancelWriting();
		return failure(message.isEmpty()
			? QStringLiteral("Unable to encode the camera-pose screenshot.")
			: message);
	}
	if (!output.commit()) {
		const QString message = output.errorString();
		return failure(message.isEmpty()
			? QStringLiteral("Unable to save the camera-pose screenshot.")
			: message);
	}
	return OperationResult::success();
}

SavedCameraPose savedPose(
	const ParsedView& view,
	const QString& storagePath)
{
	SavedCameraPose result;
	result.uuid = view.uuid;
	result.viewId = view.viewId;
	result.savedAtUtc = view.savedAtUtc;
	result.tags = view.tags;
	result.screenshotPath = absoluteScreenshotPath(
		storagePath, view.screenshotRelativePath);
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

OperationResult addDefaultTagsToExistingPoses(const QString& storagePath)
{
	ParsedLibrary library;
	const OperationResult readResult = readLibraryFile(storagePath, true, &library);
	if (!readResult.ok)
		// An already-owned library always wins over the legacy source. Keep the
		// established migration guarantee for corrupt files: do not interpret or
		// replace them while trying to add a default tag.
		return OperationResult::success();

	bool changed = false;
	QVector<QPair<QString, QString>> screenshotMoves;
	const auto rollbackScreenshotMoves = [&screenshotMoves] {
		bool restored = true;
		for (int index = screenshotMoves.size() - 1; index >= 0; --index) {
			if (!QFile::rename(
					screenshotMoves.at(index).second,
					screenshotMoves.at(index).first)) {
				restored = false;
			}
		}
		return restored;
	};
	const auto failMigration = [&rollbackScreenshotMoves](
								   const QString& message) {
		if (rollbackScreenshotMoves())
			return failure(message);
		return failure(message + QStringLiteral(
			" Screenshot rollback also failed; the library was not rewritten."));
	};
	QJsonObject poses = library.poses;
	for (auto collectionIt = library.collections.constBegin();
		 collectionIt != library.collections.constEnd();
		 ++collectionIt) {
		const ParsedCollection& collection = collectionIt.value();
		QJsonArray views = collectionAsArray(collection);
		bool collectionChanged = false;
		for (int index = 0; index < collection.views.size(); ++index) {
			QStringList tags = collection.views.at(index).tags;
			if (tags.contains(DefaultPoseTag))
				continue;
			tags.append(DefaultPoseTag);
			QJsonObject view = views.at(index).toObject();
			const ParsedView& parsedView = collection.views.at(index);
			if (!parsedView.screenshotRelativePath.isEmpty()) {
				QString oldPath;
				const OperationResult oldPathResult = resolveScreenshotPath(
					storagePath,
					parsedView.screenshotRelativePath,
					&oldPath);
				if (!oldPathResult.ok)
					return failMigration(oldPathResult.error);
				if (!QFileInfo(oldPath).isFile()) {
					return failMigration(QStringLiteral(
						"The screenshot mapped to a migrated camera pose no longer exists."));
				}
				const QString newRelativePath = screenshotRelativePath(
					parsedView.uuid, parsedView.viewId, tags);
				QString newPath;
				const OperationResult newPathResult = resolveScreenshotPath(
					storagePath, newRelativePath, &newPath);
				if (!newPathResult.ok)
					return failMigration(newPathResult.error);
				if (newPath != oldPath) {
					if (pathEntryExists(newPath)) {
						return failMigration(QStringLiteral(
							"A migrated screenshot classification already exists."));
					}
					if (!QDir().mkpath(QFileInfo(newPath).absolutePath())) {
						return failMigration(QStringLiteral(
							"Unable to create a migrated screenshot classification."));
					}
					if (!QFile::rename(oldPath, newPath)) {
						return failMigration(QStringLiteral(
							"Unable to move a screenshot during camera-pose migration."));
					}
					screenshotMoves.append({oldPath, newPath});
				}
				view.insert(ScreenshotPathKey, newRelativePath);
			}
			view.insert(TagsKey, QJsonArray::fromStringList(tags));
			views[index] = view;
			collectionChanged = true;
		}
		if (!collectionChanged)
			continue;
		poses.insert(collection.sourceKey, views);
		changed = true;
	}

	if (!changed)
		return OperationResult::success();
	library.root.insert(PosesKey, poses);
	const OperationResult writeResult =
		writeLibraryFile(storagePath, library.root);
	if (!writeResult.ok && !rollbackScreenshotMoves()) {
		return failure(writeResult.error + QStringLiteral(
			" Screenshot rollback also failed; the library still references the original paths."));
	}
	return writeResult;
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
	return failure(QStringLiteral(
		"UID must be a non-empty identifier of at most 255 characters."));
}

struct PreparedPoseSave
{
	ParsedLibrary library;
	QString normalizedUuid;
	QString sourceKey;
	QJsonArray existingViews;
	QJsonObject newView;
	QString newViewId;
};

OperationResult preparePoseSave(
	const QString& storagePath,
	const QString& uuid,
	const CameraPose& pose,
	const QStringList& tags,
	PreparedPoseSave* destination)
{
	const QString normalizedUuid = CameraPoseStore::normalizeUuid(uuid);
	if (normalizedUuid.isEmpty())
		return invalidUuidResult();
	if (pose.viewStateXml.isEmpty())
		return failure(QStringLiteral("The camera pose contains no MeshLab camera data."));

	PreparedPoseSave prepared;
	prepared.normalizedUuid = normalizedUuid;
	const OperationResult readResult =
		readLibraryFile(storagePath, true, &prepared.library);
	if (!readResult.ok)
		return readResult;

	QString maximumId = QStringLiteral("0");
	const auto collectionIt =
		prepared.library.collections.constFind(normalizedUuid);
	if (collectionIt != prepared.library.collections.constEnd()) {
		prepared.sourceKey = collectionIt.value().sourceKey;
		prepared.existingViews = collectionAsArray(collectionIt.value());
		for (const ParsedView& existing : collectionIt.value().views) {
			const QRegularExpressionMatch match =
				NumericViewIdPattern.match(existing.viewId);
			if (!match.hasMatch())
				continue;
			const QString numericId = canonicalDecimal(match.captured(1));
			if (decimalMagnitudeLess(maximumId, numericId))
				maximumId = numericId;
		}
	}

	prepared.newViewId = QStringLiteral("view_%1").arg(
		incrementDecimal(maximumId).rightJustified(3, QLatin1Char('0')));
	prepared.newView.insert(ViewIdKey, prepared.newViewId);
	prepared.newView.insert(
		SavedAtUtcKey,
		QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
	prepared.newView.insert(
		TagsKey,
		QJsonArray::fromStringList(
			meshcompare::normalizeCameraPoseTags(tags)));
	prepared.newView.insert(ParametersKey, poseParameters(pose.viewStateXml));
	prepared.newView.insert(ViewStateXmlKey, pose.viewStateXml);
	if (destination != nullptr)
		*destination = std::move(prepared);
	return OperationResult::success();
}

OperationResult commitPoseSave(
	const QString& storagePath,
	PreparedPoseSave prepared)
{
	prepared.existingViews.append(prepared.newView);
	QJsonObject poses = prepared.library.poses;
	if (!prepared.sourceKey.isEmpty()
		&& prepared.sourceKey != prepared.normalizedUuid) {
		poses.remove(prepared.sourceKey);
	}
	poses.insert(prepared.normalizedUuid, prepared.existingViews);
	prepared.library.root.insert(SchemaKey, 2);
	prepared.library.root.insert(PosesKey, poses);
	return writeLibraryFile(storagePath, prepared.library.root);
}

}

CameraPoseStore::CameraPoseStore(QString storagePath, QString legacyPath)
	: storagePath_(std::move(storagePath)), legacyPath_(std::move(legacyPath))
{
}

QString CameraPoseStore::libraryPath() const
{
	return storagePath_;
}

OperationResult CameraPoseStore::migrateLegacyIfNeeded()
{
	if (storagePath_.isEmpty())
		return failure(QStringLiteral("The camera-pose library path is empty."));
	if (pathEntryExists(storagePath_))
		return addDefaultTagsToExistingPoses(storagePath_);
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
		return addDefaultTagsToExistingPoses(storagePath_);

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
	return addDefaultTagsToExistingPoses(storagePath_);
}

OperationResult CameraPoseStore::saveWithScreenshot(
	const QString& uuid,
	const CameraPose& pose,
	const QImage& screenshot,
	QString* viewId,
	const QStringList& tags,
	QString* screenshotPath)
{
	PreparedPoseSave prepared;
	const OperationResult preparedResult = preparePoseSave(
		storagePath_, uuid, pose, tags, &prepared);
	if (!preparedResult.ok)
		return preparedResult;
	if (screenshot.isNull())
		return failure(QStringLiteral("The camera-pose screenshot is empty."));

	const QString relativePath = ::screenshotRelativePath(
		prepared.normalizedUuid, prepared.newViewId, tags);
	QString absolutePath;
	const OperationResult pathResult = resolveScreenshotPath(
		storagePath_, relativePath, &absolutePath);
	if (!pathResult.ok)
		return pathResult;
	if (pathEntryExists(absolutePath)) {
		return failure(QStringLiteral(
			"A screenshot already exists for the next camera-pose ID."));
	}
	const OperationResult imageResult =
		writeScreenshot(absolutePath, screenshot);
	if (!imageResult.ok)
		return imageResult;

	prepared.newView.insert(ScreenshotPathKey, relativePath);
	const QString newViewId = prepared.newViewId;
	const OperationResult writeResult =
		commitPoseSave(storagePath_, std::move(prepared));
	if (!writeResult.ok) {
		if (!QFile::remove(absolutePath)) {
			return failure(QStringLiteral(
				"The camera pose was not saved and its screenshot cleanup failed: %1")
					.arg(writeResult.error));
		}
		return writeResult;
	}
	if (viewId != nullptr)
		*viewId = newViewId;
	if (screenshotPath != nullptr)
		*screenshotPath = absolutePath;
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
		poses.push_back(savedPose(view, storagePath_));
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
		return failure(QStringLiteral("No saved camera poses exist for this UID."));
	for (const ParsedView& candidate : collectionIt.value().views) {
		if (candidate.viewId == viewId) {
			*pose = candidate.pose;
			return OperationResult::success();
		}
	}
	return failure(QStringLiteral("The selected saved camera pose no longer exists."));
}

OperationResult CameraPoseStore::setTags(
	const QString& uuid,
	const QString& viewId,
	const QStringList& tags)
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
		return failure(QStringLiteral("No saved camera poses exist for this UID."));

	int viewIndex = -1;
	for (int index = 0; index < collectionIt.value().views.size(); ++index) {
		if (collectionIt.value().views.at(index).viewId == viewId) {
			viewIndex = index;
			break;
		}
	}
	if (viewIndex < 0)
		return failure(QStringLiteral("The selected saved camera pose no longer exists."));

	QJsonArray views = collectionAsArray(collectionIt.value());
	QJsonObject view = views.at(viewIndex).toObject();
	const QStringList normalizedTags =
		meshcompare::normalizeCameraPoseTags(tags);
	const QString oldRelativePath =
		collectionIt.value().views.at(viewIndex).screenshotRelativePath;
	QString newRelativePath = oldRelativePath;
	QString oldAbsolutePath;
	QString newAbsolutePath;
	bool screenshotMoved = false;
	if (!oldRelativePath.isEmpty()) {
		const OperationResult oldPathResult = resolveScreenshotPath(
			storagePath_, oldRelativePath, &oldAbsolutePath);
		if (!oldPathResult.ok)
			return oldPathResult;
		if (!QFileInfo(oldAbsolutePath).isFile()) {
			return failure(QStringLiteral(
				"The screenshot mapped to this camera pose no longer exists."));
		}
		newRelativePath = screenshotRelativePath(
			normalizedUuid, viewId, normalizedTags);
		const OperationResult newPathResult = resolveScreenshotPath(
			storagePath_, newRelativePath, &newAbsolutePath);
		if (!newPathResult.ok)
			return newPathResult;
		if (newAbsolutePath != oldAbsolutePath) {
			if (pathEntryExists(newAbsolutePath)) {
				return failure(QStringLiteral(
					"The new screenshot classification already contains this pose."));
			}
			if (!QDir().mkpath(QFileInfo(newAbsolutePath).absolutePath())) {
				return failure(QStringLiteral(
					"Unable to create the new screenshot classification directory."));
			}
			if (!QFile::rename(oldAbsolutePath, newAbsolutePath)) {
				return failure(QStringLiteral(
					"Unable to move the screenshot to its new tag classification."));
			}
			screenshotMoved = true;
		}
		view.insert(ScreenshotPathKey, newRelativePath);
	}
	view.insert(
		TagsKey,
		QJsonArray::fromStringList(normalizedTags));
	views[viewIndex] = view;
	QJsonObject poses = library.poses;
	poses.remove(collectionIt.value().sourceKey);
	poses.insert(normalizedUuid, views);
	library.root.insert(PosesKey, poses);
	const OperationResult writeResult =
		writeLibraryFile(storagePath_, library.root);
	if (!writeResult.ok && screenshotMoved
		&& !QFile::rename(newAbsolutePath, oldAbsolutePath)) {
		return failure(writeResult.error + QStringLiteral(
			" Screenshot rollback also failed; the pose still references its original path."));
	}
	return writeResult;
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
		return failure(QStringLiteral("No saved camera poses exist for this UID."));

	int removeIndex = -1;
	for (int index = 0; index < collectionIt.value().views.size(); ++index) {
		if (collectionIt.value().views.at(index).viewId == viewId) {
			removeIndex = index;
			break;
		}
	}
	if (removeIndex < 0)
		return failure(QStringLiteral("The selected saved camera pose no longer exists."));

	const QString screenshotRelative =
		collectionIt.value().views.at(removeIndex).screenshotRelativePath;
	QString screenshotAbsolute;
	if (!screenshotRelative.isEmpty()) {
		const OperationResult pathResult = resolveScreenshotPath(
			storagePath_, screenshotRelative, &screenshotAbsolute);
		if (!pathResult.ok)
			return pathResult;
	}
	QString stagedScreenshotPath;
	if (!screenshotRelative.isEmpty()
		&& QFileInfo(screenshotAbsolute).isFile()) {
		stagedScreenshotPath = screenshotAbsolute
			+ QStringLiteral(".meshcompare-delete-")
			+ QUuid::createUuid().toString(QUuid::Id128);
		if (!QFile::rename(screenshotAbsolute, stagedScreenshotPath)) {
			return failure(QStringLiteral(
				"Unable to stage the camera-pose screenshot for deletion."));
		}
	}

	const QJsonObject originalRoot = library.root;
	QJsonObject poses = library.poses;
	poses.remove(collectionIt.value().sourceKey);
	QJsonArray remaining = collectionAsArray(collectionIt.value());
	remaining.removeAt(removeIndex);
	if (!remaining.isEmpty())
		poses.insert(normalizedUuid, remaining);
	library.root.insert(PosesKey, poses);
	const OperationResult writeResult =
		writeLibraryFile(storagePath_, library.root);
	if (!writeResult.ok) {
		if (!stagedScreenshotPath.isEmpty()
			&& !QFile::rename(stagedScreenshotPath, screenshotAbsolute)) {
			return failure(writeResult.error + QStringLiteral(
				" Screenshot rollback also failed; the library still references its original path."));
		}
		return writeResult;
	}
	if (!stagedScreenshotPath.isEmpty()
		&& !QFile::remove(stagedScreenshotPath)) {
		const OperationResult libraryRollback =
			writeLibraryFile(storagePath_, originalRoot);
		const bool screenshotRollback =
			QFile::rename(stagedScreenshotPath, screenshotAbsolute);
		if (libraryRollback.ok && screenshotRollback) {
			return failure(QStringLiteral(
				"The camera-pose deletion was rolled back because its screenshot could not be removed."));
		}
		return failure(QStringLiteral(
			"The camera pose deletion could not be fully rolled back after screenshot cleanup failed."));
	}
	return OperationResult::success();
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
		poses.push_back(savedPose(view, storagePath_));
	setResult(result, OperationResult::success());
	return poses;
}

QString CameraPoseStore::normalizeUuid(const QString& value)
{
	return meshcompare::normalizeCameraPoseUuid(value);
}
