#include "camera_pose_sync.h"

#include "core/camera_pose_tags.h"
#include "core/camera_pose_uuid.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QMap>
#include <QMimeDatabase>
#include <QRegularExpression>
#include <QSet>

#include <algorithm>
#include <cmath>
#include <utility>

namespace meshcompare {
namespace {

constexpr int SyncSchema = 1;
constexpr int MaximumViewCount = 10000;
constexpr qint64 MaximumDocumentBytes = 2 * 1024 * 1024;
constexpr qint64 MaximumScreenshotBytes = 10 * 1024 * 1024;

const QString AppName = QStringLiteral("meshcompare");
const QString ConflictTag = QStringLiteral("sync-conflict");
const QRegularExpression Sha256Pattern(QStringLiteral("^[0-9a-f]{64}$"));
const QRegularExpression GenerationIdPattern(QStringLiteral(
	"^[0-9a-f]{8}-[0-9a-f]{4}-[1-5][0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$"));

OperationResult failure(const QString& message)
{
	return OperationResult::failure(message);
}

bool hasUnsupportedIdentityCharacter(const QString& value)
{
	for (const QChar character : value) {
		if (character.isNull() || character.category() == QChar::Other_Control)
			return true;
	}
	return false;
}

bool isSupportedImageMimeType(const QString& mimeType)
{
	return mimeType == QStringLiteral("image/png")
		|| mimeType == QStringLiteral("image/jpeg")
		|| mimeType == QStringLiteral("image/webp")
		|| mimeType == QStringLiteral("image/gif");
}

bool isValidSavedAt(const QString& value)
{
	if (value.isEmpty())
		return true;
	QDateTime parsed = QDateTime::fromString(value, Qt::ISODateWithMs);
	if (!parsed.isValid())
		parsed = QDateTime::fromString(value, Qt::ISODate);
	return parsed.isValid();
}

QStringList canonicalTags(const QStringList& tags)
{
	QStringList result = normalizeCameraPoseTags(tags);
	std::sort(result.begin(), result.end(), [](const QString& left, const QString& right) {
		const int folded = QString::compare(left, right, Qt::CaseInsensitive);
		return folded < 0 || (folded == 0 && left < right);
	});
	return result;
}

QJsonObject screenshotObject(const CameraPoseSyncScreenshot& screenshot)
{
	QJsonObject object;
	object.insert(QStringLiteral("sha256"), screenshot.sha256);
	if (!screenshot.fileId.isEmpty())
		object.insert(QStringLiteral("fileId"), screenshot.fileId);
	object.insert(QStringLiteral("mimeType"), screenshot.mimeType);
	object.insert(QStringLiteral("size"), static_cast<double>(screenshot.size));
	return object;
}

QJsonObject viewObject(
	const CameraPoseSyncView& view,
	bool includeTransport)
{
	QJsonObject object;
	object.insert(QStringLiteral("workspaceUuid"), view.workspaceUuid);
	object.insert(QStringLiteral("viewId"), view.viewId);
	object.insert(QStringLiteral("savedAtUtc"), view.savedAtUtc);
	object.insert(
		QStringLiteral("tags"),
		QJsonArray::fromStringList(canonicalTags(view.tags)));
	object.insert(QStringLiteral("note"), view.note);
	object.insert(QStringLiteral("poseXml"), view.poseXml);
	if (!view.screenshot.sha256.isEmpty()) {
		CameraPoseSyncScreenshot screenshot = view.screenshot;
		if (!includeTransport)
			screenshot.fileId.clear();
		object.insert(QStringLiteral("screenshot"), screenshotObject(screenshot));
	}
	return object;
}

QByteArray semanticBytes(const CameraPoseSyncView& view)
{
	return QJsonDocument(viewObject(view, false)).toJson(QJsonDocument::Compact);
}

bool viewLess(const CameraPoseSyncView& left, const CameraPoseSyncView& right)
{
	if (left.workspaceUuid != right.workspaceUuid)
		return left.workspaceUuid < right.workspaceUuid;
	return left.viewId < right.viewId;
}

void sortDocument(CameraPoseSyncDocument* document)
{
	std::sort(document->views.begin(), document->views.end(), viewLess);
}

CameraPoseSyncView mergeTransport(
	const CameraPoseSyncView& preferredContent,
	const CameraPoseSyncView& other,
	bool preferOtherFileId)
{
	CameraPoseSyncView result = preferredContent;
	if (result.screenshot.sha256.isEmpty()
		|| result.screenshot.sha256 != other.screenshot.sha256) {
		return result;
	}

	if (!other.screenshot.sourcePath.isEmpty()
		&& result.screenshot.sourcePath.isEmpty()) {
		result.screenshot.sourcePath = other.screenshot.sourcePath;
	}
	if (preferOtherFileId && !other.screenshot.fileId.isEmpty())
		result.screenshot.fileId = other.screenshot.fileId;
	else if (result.screenshot.fileId.isEmpty())
		result.screenshot.fileId = other.screenshot.fileId;
	return result;
}

using ViewMap = QMap<QString, CameraPoseSyncView>;

ViewMap mapFor(const CameraPoseSyncDocument& document)
{
	ViewMap result;
	for (const CameraPoseSyncView& view : document.views)
		result.insert(cameraPoseSyncIdentity(view), view);
	return result;
}

bool identityAvailableFor(
	const QString& identity,
	const CameraPoseSyncView& candidate,
	const ViewMap& base,
	const ViewMap& local,
	const ViewMap& remote,
	const ViewMap& merged)
{
	const ViewMap* maps[] = {&base, &local, &remote, &merged};
	for (const ViewMap* map : maps) {
		const auto it = map->constFind(identity);
		if (it != map->constEnd()
			&& !cameraPoseSyncContentEqual(it.value(), candidate)) {
			return false;
		}
	}
	return true;
}

QString conflictViewId(
	const CameraPoseSyncView& original,
	CameraPoseSyncView* conflict,
	const ViewMap& base,
	const ViewMap& local,
	const ViewMap& remote,
	const ViewMap& merged)
{
	const QString digest = cameraPoseSyncContentDigest(original);
	QStringList tags = normalizeCameraPoseTags(conflict->tags);
	if (!tags.contains(ConflictTag))
		tags.append(ConflictTag);
	conflict->tags = canonicalTags(tags);
	const auto candidateWithSuffix = [&original](const QString& suffix) {
		const int baseLength = std::max(0, 1024 - suffix.size());
		return original.viewId.left(baseLength) + suffix;
	};

	for (int prefixLength = 12; prefixLength <= digest.size(); prefixLength += 4) {
		const QString candidateId = candidateWithSuffix(
			QStringLiteral("~sync-conflict-") + digest.left(prefixLength));
		conflict->viewId = candidateId;
		const QString identity = cameraPoseSyncIdentity(*conflict);
		if (identityAvailableFor(
				identity, *conflict, base, local, remote, merged)) {
			return candidateId;
		}
	}

	for (int suffix = 2; ; ++suffix) {
		const QString candidateId = candidateWithSuffix(
			QStringLiteral("~sync-conflict-") + digest
			+ QStringLiteral("-%1").arg(suffix));
		conflict->viewId = candidateId;
		const QString identity = cameraPoseSyncIdentity(*conflict);
		if (identityAvailableFor(
				identity, *conflict, base, local, remote, merged)) {
			return candidateId;
		}
	}
}

void insertMerged(ViewMap* merged, const CameraPoseSyncView& candidate)
{
	const QString identity = cameraPoseSyncIdentity(candidate);
	const auto existing = merged->find(identity);
	if (existing == merged->end()) {
		merged->insert(identity, candidate);
		return;
	}
	if (cameraPoseSyncContentEqual(existing.value(), candidate))
		existing.value() = mergeTransport(existing.value(), candidate, true);
}

void preserveConflict(
	CameraPoseSyncConflictKind kind,
	const CameraPoseSyncView* first,
	const CameraPoseSyncView* second,
	const ViewMap& base,
	const ViewMap& local,
	const ViewMap& remote,
	ViewMap* merged,
	QVector<CameraPoseSyncConflict>* conflicts)
{
	const CameraPoseSyncView* primary = first;
	const CameraPoseSyncView* losing = second;
	if (second != nullptr) {
		const QByteArray firstBytes = semanticBytes(*first);
		const QByteArray secondBytes = semanticBytes(*second);
		if (secondBytes < firstBytes) {
			primary = second;
			losing = first;
		}
		insertMerged(merged, mergeTransport(*primary, *losing, true));
	} else {
		primary = nullptr;
		losing = first;
	}

	CameraPoseSyncView conflict = *losing;
	if (second != nullptr)
		conflict = mergeTransport(conflict, *primary, true);
	const QString originalViewId = losing->viewId;
	const QString copyId = conflictViewId(
		*losing, &conflict, base, local, remote, *merged);
	insertMerged(merged, conflict);

	CameraPoseSyncConflict record;
	record.kind = kind;
	record.workspaceUuid = losing->workspaceUuid;
	record.originalViewId = originalViewId;
	record.conflictViewId = copyId;
	switch (kind) {
	case CameraPoseSyncConflictKind::ConcurrentAddition:
		record.description = QStringLiteral(
			"Both sides added different saved views with the same ID.");
		break;
	case CameraPoseSyncConflictKind::ConcurrentEdit:
		record.description = QStringLiteral(
			"Both sides edited the saved view differently.");
		break;
	case CameraPoseSyncConflictKind::DeleteVsEdit:
		record.description = QStringLiteral(
			"One side deleted the saved view while the other edited it.");
		break;
	}
	conflicts->append(record);
}

OperationResult parseScreenshot(
	const QJsonValue& value,
	CameraPoseSyncScreenshot* destination)
{
	if (!value.isObject())
		return failure(QStringLiteral("A cloud screenshot entry is invalid."));
	const QJsonObject object = value.toObject();
	const QJsonValue sha = object.value(QStringLiteral("sha256"));
	const QJsonValue mime = object.value(QStringLiteral("mimeType"));
	const QJsonValue size = object.value(QStringLiteral("size"));
	if (!sha.isString() || !mime.isString() || !size.isDouble())
		return failure(QStringLiteral("A cloud screenshot entry is incomplete."));
	const double numericSize = size.toDouble(-1.0);
	if (!std::isfinite(numericSize)
		|| numericSize < 1.0
		|| numericSize != std::floor(numericSize)
		|| numericSize > static_cast<double>(MaximumScreenshotBytes)) {
		return failure(QStringLiteral("A cloud screenshot has an invalid size."));
	}
	CameraPoseSyncScreenshot parsed;
	parsed.sha256 = sha.toString();
	parsed.mimeType = mime.toString();
	parsed.size = static_cast<qint64>(numericSize);
	if (object.contains(QStringLiteral("fileId"))) {
		const QJsonValue fileId = object.value(QStringLiteral("fileId"));
		if (!fileId.isString())
			return failure(QStringLiteral("A cloud screenshot has an invalid file ID."));
		parsed.fileId = fileId.toString();
	}
	*destination = parsed;
	return OperationResult::success();
}

OperationResult parseView(
	const QJsonValue& value,
	CameraPoseSyncView* destination)
{
	if (!value.isObject())
		return failure(QStringLiteral("A cloud saved-view entry is invalid."));
	const QJsonObject object = value.toObject();
	const QJsonValue workspace = object.value(QStringLiteral("workspaceUuid"));
	const QJsonValue viewId = object.value(QStringLiteral("viewId"));
	const QJsonValue savedAt = object.value(QStringLiteral("savedAtUtc"));
	const QJsonValue tags = object.value(QStringLiteral("tags"));
	const QJsonValue note = object.value(QStringLiteral("note"));
	const QJsonValue pose = object.value(QStringLiteral("poseXml"));
	if (!workspace.isString() || !viewId.isString()
		|| !savedAt.isString() || !tags.isArray()
		|| !note.isString() || !pose.isString()) {
		return failure(QStringLiteral("A cloud saved-view entry is incomplete."));
	}

	CameraPoseSyncView parsed;
	parsed.workspaceUuid = workspace.toString();
	parsed.viewId = viewId.toString();
	parsed.savedAtUtc = savedAt.toString();
	parsed.note = note.toString();
	parsed.poseXml = pose.toString();
	for (const QJsonValue& tag : tags.toArray()) {
		if (!tag.isString())
			return failure(QStringLiteral("A cloud saved view contains an invalid tag."));
		parsed.tags.append(tag.toString());
	}
	if (object.contains(QStringLiteral("screenshot"))) {
		const OperationResult screenshot = parseScreenshot(
			object.value(QStringLiteral("screenshot")), &parsed.screenshot);
		if (!screenshot.ok)
			return screenshot;
	}
	*destination = parsed;
	return OperationResult::success();
}

} // namespace

QString cameraPoseSyncIdentity(const CameraPoseSyncView& view)
{
	return view.workspaceUuid + QChar(0x1f) + view.viewId;
}

bool cameraPoseSyncContentEqual(
	const CameraPoseSyncView& left,
	const CameraPoseSyncView& right)
{
	return semanticBytes(left) == semanticBytes(right);
}

QString cameraPoseSyncContentDigest(const CameraPoseSyncView& view)
{
	return QString::fromLatin1(
		QCryptographicHash::hash(semanticBytes(view), QCryptographicHash::Sha256)
			.toHex());
}

OperationResult buildCameraPoseSyncDocument(
	const QVector<SavedCameraPose>& poses,
	CameraPoseSyncDocument* destination)
{
	if (destination == nullptr)
		return failure(QStringLiteral("No destination was provided for cloud camera data."));
	*destination = {};

	CameraPoseSyncDocument built;
	built.views.reserve(poses.size());
	QMimeDatabase mimeDatabase;
	for (const SavedCameraPose& source : poses) {
		CameraPoseSyncView view;
		view.workspaceUuid = normalizeCameraPoseUuid(source.uuid);
		view.viewId = source.viewId;
		view.savedAtUtc = source.savedAtUtc;
		view.tags = canonicalTags(source.tags);
		view.note = source.note;
		view.poseXml = source.pose.viewStateXml;

		if (!source.screenshotPath.isEmpty()) {
			const QFileInfo info(source.screenshotPath);
			if (!info.isFile() || info.isSymLink()) {
				return failure(QStringLiteral(
					"A saved-view screenshot is unavailable for cloud sync."));
			}
			if (info.size() <= 0 || info.size() > MaximumScreenshotBytes) {
				return failure(QStringLiteral(
					"A saved-view screenshot has an unsupported size."));
			}
			QFile input(info.absoluteFilePath());
			if (!input.open(QIODevice::ReadOnly)) {
				return failure(QStringLiteral(
					"A saved-view screenshot could not be read for cloud sync."));
			}
			const qint64 originalSize = input.size();
			QCryptographicHash hash(QCryptographicHash::Sha256);
			if (!hash.addData(&input) || input.size() != originalSize) {
				return failure(QStringLiteral(
					"A saved-view screenshot changed while cloud sync was preparing it."));
			}
			QString mimeType = mimeDatabase.mimeTypeForFile(
				info.absoluteFilePath(), QMimeDatabase::MatchContent).name();
			if (!isSupportedImageMimeType(mimeType)) {
				mimeType = mimeDatabase.mimeTypeForFile(
					info.absoluteFilePath(), QMimeDatabase::MatchExtension).name();
			}
			if (!isSupportedImageMimeType(mimeType)) {
				return failure(QStringLiteral(
					"A saved-view screenshot uses an unsupported image format."));
			}
			view.screenshot.sha256 = QString::fromLatin1(hash.result().toHex());
			view.screenshot.mimeType = mimeType;
			view.screenshot.size = originalSize;
			view.screenshot.sourcePath = info.absoluteFilePath();
		}
		built.views.append(view);
	}

	const OperationResult valid = validateCameraPoseSyncDocument(built);
	if (!valid.ok)
		return valid;
	sortDocument(&built);
	*destination = std::move(built);
	return OperationResult::success();
}

OperationResult validateCameraPoseSyncDocument(
	const CameraPoseSyncDocument& document)
{
	if (!document.generationId.isEmpty()
		&& !GenerationIdPattern.match(document.generationId).hasMatch()) {
		return failure(QStringLiteral(
			"Cloud camera data has an invalid generation ID."));
	}
	if (document.views.size() > MaximumViewCount)
		return failure(QStringLiteral("Cloud camera data contains too many saved views."));
	QSet<QString> identities;
	for (const CameraPoseSyncView& view : document.views) {
		if (view.workspaceUuid.isEmpty()
			|| normalizeCameraPoseUuid(view.workspaceUuid) != view.workspaceUuid
			|| hasUnsupportedIdentityCharacter(view.workspaceUuid)) {
			return failure(QStringLiteral("A cloud saved view has an invalid workspace ID."));
		}
		if (view.viewId.isEmpty() || view.viewId.size() > 1024
			|| hasUnsupportedIdentityCharacter(view.viewId)) {
			return failure(QStringLiteral("A cloud saved view has an invalid view ID."));
		}
		const QString identity = cameraPoseSyncIdentity(view);
		if (identities.contains(identity))
			return failure(QStringLiteral("Cloud camera data contains a duplicate view ID."));
		identities.insert(identity);
		if (!isValidSavedAt(view.savedAtUtc))
			return failure(QStringLiteral("A cloud saved view has an invalid saved time."));
		if (view.poseXml.isEmpty())
			return failure(QStringLiteral("A cloud saved view is missing its camera data."));
		if (normalizeCameraPoseTags(view.tags) != view.tags)
			return failure(QStringLiteral("A cloud saved view contains invalid or duplicate tags."));

		const CameraPoseSyncScreenshot& screenshot = view.screenshot;
		if (screenshot.sha256.isEmpty()) {
			if (!screenshot.fileId.isEmpty() || !screenshot.mimeType.isEmpty()
				|| screenshot.size != 0) {
				return failure(QStringLiteral("A cloud screenshot entry is incomplete."));
			}
			continue;
		}
		if (!Sha256Pattern.match(screenshot.sha256).hasMatch())
			return failure(QStringLiteral("A cloud screenshot has an invalid checksum."));
		if (!isSupportedImageMimeType(screenshot.mimeType))
			return failure(QStringLiteral("A cloud screenshot has an unsupported image format."));
		if (screenshot.size <= 0 || screenshot.size > MaximumScreenshotBytes)
			return failure(QStringLiteral("A cloud screenshot has an unsupported size."));
		if (screenshot.fileId.size() > 1024
			|| hasUnsupportedIdentityCharacter(screenshot.fileId)) {
			return failure(QStringLiteral("A cloud screenshot has an invalid file ID."));
		}
	}
	return OperationResult::success();
}

OperationResult serializeCameraPoseSyncDocument(
	const CameraPoseSyncDocument& document,
	QByteArray* destination)
{
	if (destination == nullptr)
		return failure(QStringLiteral("No destination was provided for cloud camera data."));
	destination->clear();
	const OperationResult valid = validateCameraPoseSyncDocument(document);
	if (!valid.ok)
		return valid;

	CameraPoseSyncDocument sorted = document;
	sortDocument(&sorted);
	QJsonArray views;
	for (const CameraPoseSyncView& view : sorted.views)
		views.append(viewObject(view, true));
	QJsonObject root;
	root.insert(QStringLiteral("app"), AppName);
	root.insert(QStringLiteral("schema"), SyncSchema);
	if (!sorted.generationId.isEmpty())
		root.insert(QStringLiteral("generationId"), sorted.generationId);
	root.insert(QStringLiteral("views"), views);
	const QByteArray bytes = QJsonDocument(root).toJson(QJsonDocument::Compact);
	if (bytes.size() > MaximumDocumentBytes)
		return failure(QStringLiteral("Cloud camera data exceeds the 2 MB limit."));
	*destination = bytes;
	return OperationResult::success();
}

OperationResult parseCameraPoseSyncDocument(
	const QByteArray& bytes,
	CameraPoseSyncDocument* destination)
{
	if (destination == nullptr)
		return failure(QStringLiteral("No destination was provided for cloud camera data."));
	*destination = {};
	if (bytes.size() > MaximumDocumentBytes)
		return failure(QStringLiteral("Cloud camera data exceeds the 2 MB limit."));
	QJsonParseError parseError;
	const QJsonDocument json = QJsonDocument::fromJson(bytes, &parseError);
	if (parseError.error != QJsonParseError::NoError || !json.isObject())
		return failure(QStringLiteral("Cloud camera data is not valid JSON."));
	const QJsonObject root = json.object();
	const QJsonValue app = root.value(QStringLiteral("app"));
	const QJsonValue schema = root.value(QStringLiteral("schema"));
	const QJsonValue views = root.value(QStringLiteral("views"));
	if (!app.isString() || app.toString() != AppName
		|| !schema.isDouble() || schema.toDouble() != SyncSchema
		|| !views.isArray()) {
		return failure(QStringLiteral("Cloud camera data has an unsupported format."));
	}

	CameraPoseSyncDocument parsed;
	if (root.contains(QStringLiteral("generationId"))) {
		const QJsonValue generation = root.value(QStringLiteral("generationId"));
		if (!generation.isString()) {
			return failure(QStringLiteral(
				"Cloud camera data has an invalid generation ID."));
		}
		parsed.generationId = generation.toString();
	}
	parsed.views.reserve(views.toArray().size());
	for (const QJsonValue& value : views.toArray()) {
		CameraPoseSyncView view;
		const OperationResult result = parseView(value, &view);
		if (!result.ok)
			return result;
		parsed.views.append(view);
	}
	const OperationResult valid = validateCameraPoseSyncDocument(parsed);
	if (!valid.ok)
		return valid;
	sortDocument(&parsed);
	*destination = std::move(parsed);
	return OperationResult::success();
}

OperationResult mergeCameraPoseSyncDocuments(
	const CameraPoseSyncDocument* baseDocument,
	const CameraPoseSyncDocument& localDocument,
	const CameraPoseSyncDocument& remoteDocument,
	CameraPoseSyncMergeResult* destination)
{
	if (destination == nullptr)
		return failure(QStringLiteral("No destination was provided for merged camera data."));
	*destination = {};
	if (baseDocument != nullptr) {
		const OperationResult validBase = validateCameraPoseSyncDocument(*baseDocument);
		if (!validBase.ok)
			return failure(QStringLiteral("The saved sync base is invalid: %1").arg(validBase.error));
	}
	const OperationResult validLocal = validateCameraPoseSyncDocument(localDocument);
	if (!validLocal.ok)
		return failure(QStringLiteral("Local saved views are invalid: %1").arg(validLocal.error));
	const OperationResult validRemote = validateCameraPoseSyncDocument(remoteDocument);
	if (!validRemote.ok)
		return failure(QStringLiteral("Cloud saved views are invalid: %1").arg(validRemote.error));

	const ViewMap base = baseDocument != nullptr ? mapFor(*baseDocument) : ViewMap();
	const ViewMap local = mapFor(localDocument);
	const ViewMap remote = mapFor(remoteDocument);
	QSet<QString> identities;
	for (auto it = base.constBegin(); it != base.constEnd(); ++it)
		identities.insert(it.key());
	for (auto it = local.constBegin(); it != local.constEnd(); ++it)
		identities.insert(it.key());
	for (auto it = remote.constBegin(); it != remote.constEnd(); ++it)
		identities.insert(it.key());
	QStringList orderedIdentities = identities.values();
	std::sort(orderedIdentities.begin(), orderedIdentities.end());

	ViewMap merged;
	QVector<CameraPoseSyncConflict> conflicts;
	for (const QString& identity : orderedIdentities) {
		const auto baseIt = base.constFind(identity);
		const auto localIt = local.constFind(identity);
		const auto remoteIt = remote.constFind(identity);
		const bool hasBase = baseIt != base.constEnd();
		const bool hasLocal = localIt != local.constEnd();
		const bool hasRemote = remoteIt != remote.constEnd();

		if (!hasBase) {
			if (hasLocal && hasRemote) {
				if (cameraPoseSyncContentEqual(localIt.value(), remoteIt.value())) {
					insertMerged(&merged,
						mergeTransport(localIt.value(), remoteIt.value(), true));
				} else {
					preserveConflict(
						CameraPoseSyncConflictKind::ConcurrentAddition,
						&localIt.value(), &remoteIt.value(), base, local, remote,
						&merged, &conflicts);
				}
			} else if (hasLocal) {
				insertMerged(&merged, localIt.value());
			} else if (hasRemote) {
				insertMerged(&merged, remoteIt.value());
			}
			continue;
		}

		if (!hasLocal && !hasRemote)
			continue;
		if (!hasLocal) {
			if (!cameraPoseSyncContentEqual(baseIt.value(), remoteIt.value())) {
				preserveConflict(
					CameraPoseSyncConflictKind::DeleteVsEdit,
					&remoteIt.value(), nullptr, base, local, remote,
					&merged, &conflicts);
			}
			continue;
		}
		if (!hasRemote) {
			if (!cameraPoseSyncContentEqual(baseIt.value(), localIt.value())) {
				preserveConflict(
					CameraPoseSyncConflictKind::DeleteVsEdit,
					&localIt.value(), nullptr, base, local, remote,
					&merged, &conflicts);
			}
			continue;
		}

		const bool localChanged =
			!cameraPoseSyncContentEqual(baseIt.value(), localIt.value());
		const bool remoteChanged =
			!cameraPoseSyncContentEqual(baseIt.value(), remoteIt.value());
		if (!localChanged && !remoteChanged) {
			insertMerged(&merged,
				mergeTransport(localIt.value(), remoteIt.value(), true));
		} else if (localChanged && !remoteChanged) {
			insertMerged(&merged,
				mergeTransport(localIt.value(), remoteIt.value(), true));
		} else if (!localChanged && remoteChanged) {
			insertMerged(&merged,
				mergeTransport(remoteIt.value(), localIt.value(), false));
		} else if (cameraPoseSyncContentEqual(localIt.value(), remoteIt.value())) {
			insertMerged(&merged,
				mergeTransport(localIt.value(), remoteIt.value(), true));
		} else {
			preserveConflict(
				CameraPoseSyncConflictKind::ConcurrentEdit,
				&localIt.value(), &remoteIt.value(), base, local, remote,
				&merged, &conflicts);
		}
	}

	CameraPoseSyncMergeResult result;
	result.document.generationId = !remoteDocument.generationId.isEmpty()
		? remoteDocument.generationId
		: (!localDocument.generationId.isEmpty()
			? localDocument.generationId
			: (baseDocument != nullptr ? baseDocument->generationId : QString()));
	result.document.views.reserve(merged.size());
	for (auto it = merged.constBegin(); it != merged.constEnd(); ++it)
		result.document.views.append(it.value());
	result.conflicts = std::move(conflicts);
	const OperationResult validMerged =
		validateCameraPoseSyncDocument(result.document);
	if (!validMerged.ok) {
		return failure(QStringLiteral("Merged saved views are invalid: %1")
			.arg(validMerged.error));
	}
	*destination = std::move(result);
	return OperationResult::success();
}

} // namespace meshcompare
