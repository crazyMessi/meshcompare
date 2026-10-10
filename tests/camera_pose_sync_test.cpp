#include <QtTest>

#include <QCryptographicHash>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

#include "services/camera_pose_sync.h"

namespace {

meshcompare::CameraPoseSyncView view(
	const QString& workspace,
	const QString& id,
	const QString& note = QString())
{
	meshcompare::CameraPoseSyncView result;
	result.workspaceUuid = workspace;
	result.viewId = id;
	result.savedAtUtc = QStringLiteral("2026-10-10T01:02:03.004Z");
	result.tags = QStringList{QStringLiteral("inspection")};
	result.note = note;
	result.poseXml = QStringLiteral("<VCGCamera FocalMm=\"35\"/>");
	return result;
}

void addScreenshot(
	meshcompare::CameraPoseSyncView* value,
	const QChar& checksumCharacter = QLatin1Char('a'))
{
	value->screenshot.sha256 = QString(64, checksumCharacter);
	value->screenshot.mimeType = QStringLiteral("image/png");
	value->screenshot.size = 123;
}

QByteArray serialized(const meshcompare::CameraPoseSyncDocument& document)
{
	QByteArray bytes;
	const OperationResult result =
		meshcompare::serializeCameraPoseSyncDocument(document, &bytes);
	if (!result.ok)
		qWarning() << result.error;
	return bytes;
}

const meshcompare::CameraPoseSyncView* findView(
	const meshcompare::CameraPoseSyncDocument& document,
	const QString& workspace,
	const QString& id)
{
	for (const auto& candidate : document.views) {
		if (candidate.workspaceUuid == workspace && candidate.viewId == id)
			return &candidate;
	}
	return nullptr;
}

} // namespace

class CameraPoseSyncTest final : public QObject
{
	Q_OBJECT

private slots:
	void buildHashesScreenshotsAndNormalizesLocalRecords();
	void serializationIsStableAndOmitsLocalPaths();
	void generationIdRoundTripsAndIsValidated();
	void parsingRejectsCorruptionAndClearsTheDestination();
	void contentEqualityIgnoresOnlyTransportFields();
	void firstSyncAddsDistinctViewsAndCombinesEquivalentTransport();
	void concurrentAdditionsProduceTheSameConflictCopyInEitherDirection();
	void unchangedViewsHonorDeletionAndSingleSidedEdits();
	void concurrentEditsConvergeAndPreserveBothVersions();
	void deleteVersusEditPreservesTheEditAsAStableConflictCopy();
	void conflictIdsStayValidForMaximumLengthViewIds();
};

void CameraPoseSyncTest::buildHashesScreenshotsAndNormalizesLocalRecords()
{
	QTemporaryDir directory;
	QVERIFY(directory.isValid());
	const QString screenshotPath = directory.filePath(QStringLiteral("capture.png"));
	const QByteArray png = QByteArray::fromBase64(
		"iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR4nGNgYAAAAAMA"
		"ASsJTYQAAAAASUVORK5CYII=");
	QFile screenshot(screenshotPath);
	QVERIFY(screenshot.open(QIODevice::WriteOnly));
	QCOMPARE(screenshot.write(png), static_cast<qint64>(png.size()));
	screenshot.close();

	SavedCameraPose source;
	source.uuid = QStringLiteral("{AAAAAAAA-AAAA-AAAA-AAAA-AAAAAAAAAAAA}");
	source.viewId = QStringLiteral("view_002");
	source.savedAtUtc = QStringLiteral("2026-10-10T01:02:03.004Z");
	source.tags = QStringList{
		QStringLiteral(" zeta "), QStringLiteral("alpha"), QStringLiteral("alpha")};
	source.screenshotPath = screenshotPath;
	source.pose.viewStateXml = QStringLiteral("<camera/>");
	source.note = QString::fromUtf8("同步备注");

	meshcompare::CameraPoseSyncDocument document;
	const OperationResult result =
		meshcompare::buildCameraPoseSyncDocument({source}, &document);
	QVERIFY2(result.ok, qPrintable(result.error));
	QCOMPARE(document.views.size(), 1);
	const auto& built = document.views.front();
	QCOMPARE(built.workspaceUuid, QString(32, QLatin1Char('a')));
	QCOMPARE(built.tags, QStringList({QStringLiteral("alpha"), QStringLiteral("zeta")}));
	QCOMPARE(built.note, source.note);
	QCOMPARE(built.screenshot.sha256, QString::fromLatin1(
		QCryptographicHash::hash(png, QCryptographicHash::Sha256).toHex()));
	QCOMPARE(built.screenshot.mimeType, QStringLiteral("image/png"));
	QCOMPARE(built.screenshot.size, static_cast<qint64>(png.size()));
	QCOMPARE(built.screenshot.sourcePath, screenshotPath);
}

void CameraPoseSyncTest::serializationIsStableAndOmitsLocalPaths()
{
	auto first = view(QStringLiteral("workspace-b"), QStringLiteral("view_002"));
	first.tags = QStringList{QStringLiteral("zeta"), QStringLiteral("alpha")};
	addScreenshot(&first);
	first.screenshot.fileId = QStringLiteral("cloud-file-1");
	first.screenshot.sourcePath = QStringLiteral("/private/local/capture.png");
	auto second = view(QStringLiteral("workspace-a"), QStringLiteral("view_001"));

	meshcompare::CameraPoseSyncDocument one;
	one.views = {first, second};
	meshcompare::CameraPoseSyncDocument two;
	two.views = {second, first};
	const QByteArray firstBytes = serialized(one);
	const QByteArray secondBytes = serialized(two);
	QVERIFY(!firstBytes.isEmpty());
	QCOMPARE(firstBytes, secondBytes);
	QVERIFY(!firstBytes.contains("/private/local"));

	meshcompare::CameraPoseSyncDocument parsed;
	const OperationResult parseResult =
		meshcompare::parseCameraPoseSyncDocument(firstBytes, &parsed);
	QVERIFY2(parseResult.ok, qPrintable(parseResult.error));
	QCOMPARE(parsed.views.size(), 2);
	QCOMPARE(parsed.views.front().workspaceUuid, QStringLiteral("workspace-a"));
	QCOMPARE(parsed.views.back().tags,
		QStringList({QStringLiteral("alpha"), QStringLiteral("zeta")}));
	QVERIFY(parsed.views.back().screenshot.sourcePath.isEmpty());
	QCOMPARE(parsed.views.back().screenshot.fileId, QStringLiteral("cloud-file-1"));
}

void CameraPoseSyncTest::generationIdRoundTripsAndIsValidated()
{
	const QString generation =
		QStringLiteral("123e4567-e89b-42d3-a456-426614174000");
	meshcompare::CameraPoseSyncDocument document;
	document.generationId = generation;
	document.views.append(
		view(QStringLiteral("workspace"), QStringLiteral("view_001")));

	const QByteArray bytes = serialized(document);
	QVERIFY(!bytes.isEmpty());
	QCOMPARE(
		QJsonDocument::fromJson(bytes).object()
			.value(QStringLiteral("generationId")).toString(),
		generation);
	meshcompare::CameraPoseSyncDocument parsed;
	const OperationResult parsedResult =
		meshcompare::parseCameraPoseSyncDocument(bytes, &parsed);
	QVERIFY2(parsedResult.ok, qPrintable(parsedResult.error));
	QCOMPARE(parsed.generationId, generation);

	parsed.generationId = QStringLiteral("not-a-generation");
	QVERIFY(!meshcompare::validateCameraPoseSyncDocument(parsed).ok);
}

void CameraPoseSyncTest::parsingRejectsCorruptionAndClearsTheDestination()
{
	meshcompare::CameraPoseSyncDocument valid;
	valid.views.append(view(QStringLiteral("workspace"), QStringLiteral("view_001")));
	QJsonDocument json = QJsonDocument::fromJson(serialized(valid));
	QJsonObject root = json.object();
	QJsonArray duplicateViews = root.value(QStringLiteral("views")).toArray();
	duplicateViews.append(duplicateViews.first());
	root.insert(QStringLiteral("views"), duplicateViews);

	meshcompare::CameraPoseSyncDocument destination;
	destination.views.append(view(QStringLiteral("stale"), QStringLiteral("stale")));
	const OperationResult duplicate = meshcompare::parseCameraPoseSyncDocument(
		QJsonDocument(root).toJson(QJsonDocument::Compact), &destination);
	QVERIFY(!duplicate.ok);
	QVERIFY(destination.views.isEmpty());

	root.insert(QStringLiteral("app"), QStringLiteral("another-app"));
	const OperationResult wrongApp = meshcompare::parseCameraPoseSyncDocument(
		QJsonDocument(root).toJson(QJsonDocument::Compact), &destination);
	QVERIFY(!wrongApp.ok);
	QVERIFY(destination.views.isEmpty());

	root = json.object();
	QJsonArray views = root.value(QStringLiteral("views")).toArray();
	QJsonObject malformedView = views.first().toObject();
	QJsonObject screenshot;
	screenshot.insert(QStringLiteral("sha256"), QString(64, QLatin1Char('a')));
	screenshot.insert(QStringLiteral("mimeType"), QStringLiteral("image/png"));
	screenshot.insert(QStringLiteral("size"), -1e300);
	malformedView.insert(QStringLiteral("screenshot"), screenshot);
	views[0] = malformedView;
	root.insert(QStringLiteral("views"), views);
	const OperationResult outOfRangeSize =
		meshcompare::parseCameraPoseSyncDocument(
			QJsonDocument(root).toJson(QJsonDocument::Compact), &destination);
	QVERIFY(!outOfRangeSize.ok);
	QVERIFY(destination.views.isEmpty());
}

void CameraPoseSyncTest::contentEqualityIgnoresOnlyTransportFields()
{
	auto left = view(QStringLiteral("workspace"), QStringLiteral("view_001"));
	addScreenshot(&left);
	auto right = left;
	right.screenshot.fileId = QStringLiteral("remote-file");
	right.screenshot.sourcePath = QStringLiteral("/local/file.png");
	QVERIFY(meshcompare::cameraPoseSyncContentEqual(left, right));
	QCOMPARE(
		meshcompare::cameraPoseSyncContentDigest(left),
		meshcompare::cameraPoseSyncContentDigest(right));

	right.screenshot.size += 1;
	QVERIFY(!meshcompare::cameraPoseSyncContentEqual(left, right));
	right = left;
	right.note = QStringLiteral("changed");
	QVERIFY(!meshcompare::cameraPoseSyncContentEqual(left, right));
	right = left;
	right.tags = QStringList{QStringLiteral("inspection"), QStringLiteral("second")};
	QVERIFY(!meshcompare::cameraPoseSyncContentEqual(left, right));
}

void CameraPoseSyncTest::firstSyncAddsDistinctViewsAndCombinesEquivalentTransport()
{
	auto sharedLocal = view(QStringLiteral("workspace"), QStringLiteral("shared"));
	addScreenshot(&sharedLocal);
	sharedLocal.screenshot.sourcePath = QStringLiteral("/local/shared.png");
	auto sharedRemote = sharedLocal;
	sharedRemote.screenshot.sourcePath.clear();
	sharedRemote.screenshot.fileId = QStringLiteral("remote-shared");
	meshcompare::CameraPoseSyncDocument local;
	local.views = {
		view(QStringLiteral("workspace"), QStringLiteral("local")), sharedLocal};
	meshcompare::CameraPoseSyncDocument remote;
	remote.views = {
		view(QStringLiteral("workspace"), QStringLiteral("remote")), sharedRemote};

	meshcompare::CameraPoseSyncMergeResult merged;
	const OperationResult result = meshcompare::mergeCameraPoseSyncDocuments(
		nullptr, local, remote, &merged);
	QVERIFY2(result.ok, qPrintable(result.error));
	QCOMPARE(merged.document.views.size(), 3);
	QVERIFY(merged.conflicts.isEmpty());
	const auto* shared = findView(merged.document, QStringLiteral("workspace"),
		QStringLiteral("shared"));
	QVERIFY(shared != nullptr);
	QCOMPARE(shared->screenshot.sourcePath, QStringLiteral("/local/shared.png"));
	QCOMPARE(shared->screenshot.fileId, QStringLiteral("remote-shared"));
}

void CameraPoseSyncTest::concurrentAdditionsProduceTheSameConflictCopyInEitherDirection()
{
	auto alpha = view(QStringLiteral("workspace"), QStringLiteral("view_001"),
		QStringLiteral("alpha"));
	auto beta = view(QStringLiteral("workspace"), QStringLiteral("view_001"),
		QStringLiteral("beta"));
	meshcompare::CameraPoseSyncDocument left;
	left.views.append(alpha);
	meshcompare::CameraPoseSyncDocument right;
	right.views.append(beta);
	meshcompare::CameraPoseSyncMergeResult leftFirst;
	meshcompare::CameraPoseSyncMergeResult rightFirst;
	QVERIFY(meshcompare::mergeCameraPoseSyncDocuments(
		nullptr, left, right, &leftFirst).ok);
	QVERIFY(meshcompare::mergeCameraPoseSyncDocuments(
		nullptr, right, left, &rightFirst).ok);
	QCOMPARE(serialized(leftFirst.document), serialized(rightFirst.document));
	QCOMPARE(leftFirst.document.views.size(), 2);
	QCOMPARE(leftFirst.conflicts.size(), 1);
	QVERIFY(leftFirst.conflicts.front().kind
		== meshcompare::CameraPoseSyncConflictKind::ConcurrentAddition);
	const QString copyId = leftFirst.conflicts.front().conflictViewId;
	QVERIFY(copyId.startsWith(QStringLiteral("view_001~sync-conflict-")));
	const auto* copy = findView(leftFirst.document, QStringLiteral("workspace"), copyId);
	QVERIFY(copy != nullptr);
	QVERIFY(copy->tags.contains(QStringLiteral("sync-conflict")));
}

void CameraPoseSyncTest::unchangedViewsHonorDeletionAndSingleSidedEdits()
{
	auto original = view(QStringLiteral("workspace"), QStringLiteral("view_001"),
		QStringLiteral("original"));
	meshcompare::CameraPoseSyncDocument base;
	base.views.append(original);
	meshcompare::CameraPoseSyncDocument empty;
	meshcompare::CameraPoseSyncDocument unchanged = base;
	meshcompare::CameraPoseSyncMergeResult deletion;
	QVERIFY(meshcompare::mergeCameraPoseSyncDocuments(
		&base, empty, unchanged, &deletion).ok);
	QVERIFY(deletion.document.views.isEmpty());
	QVERIFY(deletion.conflicts.isEmpty());

	auto edited = original;
	edited.note = QStringLiteral("local edit");
	meshcompare::CameraPoseSyncDocument local;
	local.views.append(edited);
	meshcompare::CameraPoseSyncMergeResult oneEdit;
	QVERIFY(meshcompare::mergeCameraPoseSyncDocuments(
		&base, local, unchanged, &oneEdit).ok);
	QCOMPARE(oneEdit.document.views.size(), 1);
	QCOMPARE(oneEdit.document.views.front().note, QStringLiteral("local edit"));
	QCOMPARE(oneEdit.document.views.front().viewId, QStringLiteral("view_001"));
	QVERIFY(oneEdit.conflicts.isEmpty());
}

void CameraPoseSyncTest::concurrentEditsConvergeAndPreserveBothVersions()
{
	auto original = view(QStringLiteral("workspace"), QStringLiteral("view_001"),
		QStringLiteral("base"));
	auto alpha = original;
	alpha.note = QStringLiteral("alpha edit");
	auto beta = original;
	beta.note = QStringLiteral("beta edit");
	meshcompare::CameraPoseSyncDocument base;
	base.views.append(original);
	meshcompare::CameraPoseSyncDocument left;
	left.views.append(alpha);
	meshcompare::CameraPoseSyncDocument right;
	right.views.append(beta);
	meshcompare::CameraPoseSyncMergeResult leftFirst;
	meshcompare::CameraPoseSyncMergeResult rightFirst;
	QVERIFY(meshcompare::mergeCameraPoseSyncDocuments(
		&base, left, right, &leftFirst).ok);
	QVERIFY(meshcompare::mergeCameraPoseSyncDocuments(
		&base, right, left, &rightFirst).ok);
	QCOMPARE(serialized(leftFirst.document), serialized(rightFirst.document));
	QCOMPARE(leftFirst.document.views.size(), 2);
	QCOMPARE(leftFirst.conflicts.size(), 1);
	QVERIFY(leftFirst.conflicts.front().kind
		== meshcompare::CameraPoseSyncConflictKind::ConcurrentEdit);
	QSet<QString> notes;
	for (const auto& candidate : leftFirst.document.views)
		notes.insert(candidate.note);
	QCOMPARE(notes.size(), 2);
	QVERIFY(notes.contains(QStringLiteral("alpha edit")));
	QVERIFY(notes.contains(QStringLiteral("beta edit")));
}

void CameraPoseSyncTest::deleteVersusEditPreservesTheEditAsAStableConflictCopy()
{
	auto original = view(QStringLiteral("workspace"), QStringLiteral("view_001"),
		QStringLiteral("base"));
	auto edited = original;
	edited.note = QStringLiteral("edited remotely");
	meshcompare::CameraPoseSyncDocument base;
	base.views.append(original);
	meshcompare::CameraPoseSyncDocument local;
	meshcompare::CameraPoseSyncDocument remote;
	remote.views.append(edited);
	meshcompare::CameraPoseSyncMergeResult first;
	QVERIFY(meshcompare::mergeCameraPoseSyncDocuments(
		&base, local, remote, &first).ok);
	QCOMPARE(first.document.views.size(), 1);
	QCOMPARE(first.conflicts.size(), 1);
	QVERIFY(first.conflicts.front().kind
		== meshcompare::CameraPoseSyncConflictKind::DeleteVsEdit);
	QVERIFY(findView(first.document, QStringLiteral("workspace"),
		QStringLiteral("view_001")) == nullptr);
	const QString conflictId = first.conflicts.front().conflictViewId;
	const auto* preserved = findView(
		first.document, QStringLiteral("workspace"), conflictId);
	QVERIFY(preserved != nullptr);
	QCOMPARE(preserved->note, QStringLiteral("edited remotely"));
	QVERIFY(preserved->tags.contains(QStringLiteral("sync-conflict")));

	// A stale peer encountering the already-created copy must reuse it instead
	// of producing another conflict identity.
	meshcompare::CameraPoseSyncDocument remoteWithCopy = first.document;
	meshcompare::CameraPoseSyncMergeResult repeated;
	meshcompare::CameraPoseSyncDocument staleEdit;
	staleEdit.views.append(edited);
	QVERIFY(meshcompare::mergeCameraPoseSyncDocuments(
		&base, staleEdit, remoteWithCopy, &repeated).ok);
	QCOMPARE(serialized(repeated.document), serialized(first.document));
}

void CameraPoseSyncTest::conflictIdsStayValidForMaximumLengthViewIds()
{
	const QString maximumId(1024, QLatin1Char('v'));
	auto alpha = view(QStringLiteral("workspace"), maximumId, QStringLiteral("alpha"));
	auto beta = alpha;
	beta.note = QStringLiteral("beta");
	meshcompare::CameraPoseSyncDocument local;
	local.views.append(alpha);
	meshcompare::CameraPoseSyncDocument remote;
	remote.views.append(beta);
	meshcompare::CameraPoseSyncMergeResult merged;
	const OperationResult result = meshcompare::mergeCameraPoseSyncDocuments(
		nullptr, local, remote, &merged);
	QVERIFY2(result.ok, qPrintable(result.error));
	QCOMPARE(merged.document.views.size(), 2);
	QCOMPARE(merged.conflicts.size(), 1);
	QVERIFY(merged.conflicts.front().conflictViewId.size() <= 1024);
	QVERIFY(meshcompare::validateCameraPoseSyncDocument(merged.document).ok);
}

QTEST_APPLESS_MAIN(CameraPoseSyncTest)
#include "camera_pose_sync_test.moc"
