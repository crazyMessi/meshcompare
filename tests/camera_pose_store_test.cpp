#include "services/camera_pose_store.h"
#include "services/camera_pose_store_file_ops.h"
#include "core/camera_pose_uuid.h"

#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QThread>
#include <QtTest>

#ifdef Q_OS_UNIX
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace {

const QString UuidA = QStringLiteral("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
const QString UuidB = QStringLiteral("bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb");

#ifdef Q_OS_UNIX
bool openFifoWriterWithTimeout(
	QFile* writer,
	const QString& path,
	int timeoutMilliseconds = 5000)
{
	QElapsedTimer timer;
	timer.start();
	const QByteArray encodedPath = QFile::encodeName(path);
	while (timer.elapsed() < timeoutMilliseconds) {
		const int descriptor =
			::open(encodedPath.constData(), O_WRONLY | O_NONBLOCK);
		if (descriptor >= 0) {
			if (writer->open(
					descriptor,
					QIODevice::WriteOnly,
					QFileDevice::AutoCloseHandle)) {
				return true;
			}
			::close(descriptor);
			return false;
		}
		if (errno != ENXIO && errno != ENOENT)
			return false;
		QThread::msleep(1);
	}
	return false;
}

void rescueBlockedFifoReader(const QString& path)
{
	const QByteArray encodedPath = QFile::encodeName(path);
	const int descriptor =
		::open(encodedPath.constData(), O_RDWR | O_NONBLOCK);
	if (descriptor < 0)
		return;
	const char fallback[] = "{}";
	::write(descriptor, fallback, sizeof(fallback) - 1);
	::close(descriptor);
}

bool waitForMigrationThread(QThread* thread, const QString& fifoPath)
{
	if (thread->wait(5000))
		return true;
	rescueBlockedFifoReader(fifoPath);
	if (thread->wait(5000))
		return true;
	qFatal("Migration test worker did not stop after FIFO rescue.");
	return false;
}
#endif

CameraPose pose(const QString& xml)
{
	return CameraPose{xml};
}

QJsonObject poseObject(
	const QString& viewId,
	const QString& xml,
	const QString& savedAtUtc = QStringLiteral("2026-07-14T09:10:11.123Z"))
{
	QJsonObject value;
	if (!viewId.isNull())
		value.insert(QStringLiteral("view_id"), viewId);
	if (!savedAtUtc.isNull())
		value.insert(QStringLiteral("saved_at_utc"), savedAtUtc);
	value.insert(QStringLiteral("parameters"), QJsonObject());
	value.insert(QStringLiteral("meshlab_view_state_xml"), xml);
	return value;
}

QByteArray schema2Bytes(const QJsonObject& poses, const QJsonObject& extra = QJsonObject())
{
	QJsonObject root = extra;
	root.insert(QStringLiteral("schema"), 2);
	root.insert(QStringLiteral("poses"), poses);
	return QJsonDocument(root).toJson(QJsonDocument::Indented);
}

bool writeBytes(const QString& path, const QByteArray& bytes)
{
	const QFileInfo info(path);
	if (!QDir().mkpath(info.absolutePath()))
		return false;
	QFile file(path);
	if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
		return false;
	return file.write(bytes) == bytes.size();
}

QByteArray readBytes(const QString& path)
{
	QFile file(path);
	if (!file.open(QIODevice::ReadOnly))
		return QByteArray();
	return file.readAll();
}

QJsonObject readRoot(const QString& path)
{
	QJsonParseError error;
	const QJsonDocument document = QJsonDocument::fromJson(readBytes(path), &error);
	if (error.error != QJsonParseError::NoError || !document.isObject())
		return QJsonObject();
	return document.object();
}

QByteArray onePoseLibrary(
	const QString& uuid = UuidA,
	const QString& viewId = QStringLiteral("view_001"),
	const QString& xml = QStringLiteral("<camera/>"))
{
	QJsonArray views;
	views.append(poseObject(viewId, xml));
	QJsonObject poses;
	poses.insert(uuid, views);
	return schema2Bytes(poses);
}

}

class CameraPoseStoreTest : public QObject
{
	Q_OBJECT

private slots:
	void normalizeUuid_data();
	void normalizeUuid();
	void extractsVariableLengthUidFromNormalizedMeshName();
	void missingLibraryIsAnEmptySuccess();
	void invalidUuidAndEmptyStoragePathFailWithoutMutatingOutputs();
	void saveCreatesNestedSchema2Library();
	void poseTagsCanBeAssignedAndLegacyPosesReceiveHoleTag();
	void migrationCopiesLegacyOnlyWhenCurrentIsAbsent();
	void deletionIsAtomicAndScopedToUuid();
	void malformedLibraryFailsEveryOperationWithoutChangingBytes_data();
	void malformedLibraryFailsEveryOperationWithoutChangingBytes();
	void unrelatedCorruptionInvalidatesUuidScopedReads();
	void readsSingleObjectAndPreservesOpaqueXml();
	void listIsUuidScopedAndSortedByTimestampThenViewId();
	void operationOutputsAreAlwaysOverwrittenOnlyAsDocumented();
	void savePreservesFieldsAndAllocatesMonotonicIds();
	void saveAllocatesIdsBeyondMachineIntegerRange();
	void saveWritesUtcMillisecondTimestampAndExactUnicodeXml();
	void saveDerivesLegacyCompatibleParametersWhenViewStateXmlProvidesThem();
	void missingAndInvalidRemoveRequestsAreByteIdenticalNoOps();
	void failedInitialAndReplacementWritesLeaveNoPartialLibrary();
	void migrationRejectsCorruptionAndExistingCurrentAlwaysWins();
	void migrationRejectsStructurallyCorruptJson();
	void migrationMissingLegacyIsIdempotentNoOp();
	void migrationCopyFailureLeavesLegacyUntouched();
	void exclusivePublisherNeverReplacesExistingDestination();
	void migrationCopyFailureAfterValidationLeavesNoArtifacts();
	void migrationConcurrentDestinationWinsWithoutBeingRemoved();
	void migrationValidatesCopiedBytesBeforePublishing();
	void migrationTreatsDanglingDestinationSymlinkAsExisting();
};

void CameraPoseStoreTest::normalizeUuid_data()
{
	QTest::addColumn<QString>("input");
	QTest::addColumn<QString>("expected");

	QTest::newRow("canonical") << UuidA << UuidA;
	QTest::newRow("trim-braces-hyphens-uppercase")
		<< QStringLiteral("  {AAAAAAAA-AAAA-AAAA-AAAA-AAAAAAAAAAAA}  ") << UuidA;
	QTest::newRow("hyphens-anywhere")
		<< QStringLiteral("aaaa-aaaaaa-aaaaaaaaaa-aaaaaaaaaaaa") << UuidA;
	QTest::newRow("short-numeric-uid")
		<< QStringLiteral("  2048  ") << QStringLiteral("2048");
	QTest::newRow("opaque-project-uid")
		<< QStringLiteral("  Comparison_Run-42  ")
		<< QStringLiteral("comparison_run-42");
	QTest::newRow("unpaired-brace")
		<< QStringLiteral("{aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa") << QString();
	QTest::newRow("wrong-length")
		<< QStringLiteral("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa")
		<< QStringLiteral("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
	QTest::newRow("non-hex")
		<< QStringLiteral("gaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa")
		<< QStringLiteral("gaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
	QTest::newRow("empty") << QStringLiteral(" \t\n ") << QString();
	QTest::newRow("embedded-control")
		<< QStringLiteral("run\n42") << QString();
	QTest::newRow("too-long")
		<< QString(256, QLatin1Char('a')) << QString();
}

void CameraPoseStoreTest::normalizeUuid()
{
	QFETCH(QString, input);
	QFETCH(QString, expected);
	QCOMPARE(CameraPoseStore::normalizeUuid(input), expected);
}

void CameraPoseStoreTest::extractsVariableLengthUidFromNormalizedMeshName()
{
	QCOMPARE(
		meshcompare::extractCameraPoseUuidCandidates({
			QStringLiteral("/tmp/Comparison_Run-42_gt_norm.ply"),
			QStringLiteral("Comparison_Run-42_hy_norm.ply"),
			QStringLiteral("Comparison_Run-42_s2_norm.ply")}),
		QStringList({QStringLiteral("comparison_run-42")}));
}

void CameraPoseStoreTest::missingLibraryIsAnEmptySuccess()
{
	QTemporaryDir dir;
	QVERIFY(dir.isValid());
	CameraPoseStore store(dir.filePath(QStringLiteral("missing/poses.json")), QString());
	OperationResult result = OperationResult::failure(QStringLiteral("stale"));
	QVERIFY(store.list(UuidA, &result).isEmpty());
	QVERIFY(result.ok);
	QVERIFY(result.error.isEmpty());
	QVERIFY(store.listAll(&result).isEmpty());
	QVERIFY(result.ok);
}

void CameraPoseStoreTest::invalidUuidAndEmptyStoragePathFailWithoutMutatingOutputs()
{
	QTemporaryDir dir;
	QVERIFY(dir.isValid());
	CameraPoseStore store(dir.filePath(QStringLiteral("poses.json")), QString());
	QString viewId = QStringLiteral("unchanged");
	QVERIFY(!store.save(QString(), pose(QStringLiteral("<camera/>")), &viewId).ok);
	QCOMPARE(viewId, QStringLiteral("unchanged"));

	OperationResult listResult = OperationResult::success();
	QVERIFY(store.list(QString(), &listResult).isEmpty());
	QVERIFY(!listResult.ok);

	CameraPose destination{QStringLiteral("unchanged")};
	QVERIFY(!store.load(UuidA, QStringLiteral("view_001"), nullptr).ok);
	QVERIFY(!store.load(QString(), QStringLiteral("view_001"), &destination).ok);
	QCOMPARE(destination.viewStateXml, QStringLiteral("unchanged"));

	CameraPoseStore emptyPathStore{QString(), QString()};
	QVERIFY(!emptyPathStore.save(UuidA, pose(QStringLiteral("<camera/>"))).ok);
	QVERIFY(!emptyPathStore.migrateLegacyIfNeeded().ok);
	listResult = OperationResult::success();
	QVERIFY(emptyPathStore.listAll(&listResult).isEmpty());
	QVERIFY(!listResult.ok);
}

void CameraPoseStoreTest::saveCreatesNestedSchema2Library()
{
	QTemporaryDir dir;
	QVERIFY(dir.isValid());
	const QString current = dir.filePath(QStringLiteral("deep/current/poses.json"));
	CameraPoseStore store(current, QString());
	QString viewId = QStringLiteral("stale");
	QVERIFY(store.save(
		UuidA,
		pose(QStringLiteral("<camera>one</camera>")),
		&viewId,
		{QStringLiteral(" inspection "), QStringLiteral("hole"),
		 QStringLiteral("inspection")}).ok);
	QCOMPARE(viewId, QStringLiteral("view_001"));
	QVERIFY(QFileInfo::exists(current));

	QJsonParseError error;
	const QJsonDocument document = QJsonDocument::fromJson(readBytes(current), &error);
	QCOMPARE(error.error, QJsonParseError::NoError);
	QVERIFY(document.isObject());
	QCOMPARE(document.object().value(QStringLiteral("schema")).toInt(), 2);
	const QJsonObject poses = document.object().value(QStringLiteral("poses")).toObject();
	QVERIFY(poses.contains(UuidA));
	QCOMPARE(
		poses.value(UuidA).toArray().at(0).toObject().value(QStringLiteral("tags")).toArray(),
		QJsonArray({QStringLiteral("inspection"), QStringLiteral("hole")}));
}

void CameraPoseStoreTest::poseTagsCanBeAssignedAndLegacyPosesReceiveHoleTag()
{
	QTemporaryDir dir;
	QVERIFY(dir.isValid());
	const QString current = dir.filePath(QStringLiteral("poses.json"));
	QJsonArray viewsA;
	viewsA.append(poseObject(QStringLiteral("view_001"), QStringLiteral("<first/>")));
	QJsonArray viewsB;
	QJsonObject second = poseObject(
		QStringLiteral("view_002"), QStringLiteral("<second/>"));
	second.insert(
		QStringLiteral("tags"), QJsonArray({QStringLiteral("edge")}));
	viewsB.append(second);
	QJsonObject poses;
	poses.insert(UuidA, viewsA);
	poses.insert(UuidB, viewsB);
	QVERIFY(writeBytes(current, schema2Bytes(poses)));

	CameraPoseStore store(current, QString());
	QVERIFY(store.migrateLegacyIfNeeded().ok);
	QCOMPARE(store.list(UuidA).front().tags, QStringList({QStringLiteral("hole")}));
	QCOMPARE(
		store.list(UuidB).front().tags,
		QStringList({QStringLiteral("edge"), QStringLiteral("hole")}));

	QVERIFY(store.setTags(
		UuidA,
		QStringLiteral("view_001"),
		{QStringLiteral("inspection"), QStringLiteral("edge")}).ok);
	QCOMPARE(
		store.list(UuidA).front().tags,
		QStringList({QStringLiteral("inspection"), QStringLiteral("edge")}));
	QCOMPARE(
		store.list(UuidB).front().tags,
		QStringList({QStringLiteral("edge"), QStringLiteral("hole")}));
}

void CameraPoseStoreTest::migrationCopiesLegacyOnlyWhenCurrentIsAbsent()
{
	QTemporaryDir dir;
	QVERIFY(dir.isValid());
	const QString legacy = dir.filePath(QStringLiteral("legacy.json"));
	const QString current = dir.filePath(QStringLiteral("current/poses.json"));
	QJsonArray views;
	views.append(poseObject(QStringLiteral("view_001"), QStringLiteral("<legacy/>")));
	QJsonObject poses;
	poses.insert(UuidA, views);
	const QByteArray legacyBytes = schema2Bytes(poses, {{QStringLiteral("kept"), 17}});
	QVERIFY(writeBytes(legacy, legacyBytes));
	const QDateTime legacyMtime = QFileInfo(legacy).lastModified();

	CameraPoseStore store(current, legacy);
	QVERIFY(store.migrateLegacyIfNeeded().ok);
	QCOMPARE(readBytes(legacy), legacyBytes);
	QCOMPARE(QFileInfo(legacy).lastModified(), legacyMtime);
	const QVector<SavedCameraPose> migrated = store.list(UuidA);
	QCOMPARE(migrated.size(), 1);
	QCOMPARE(migrated.front().tags, QStringList({QStringLiteral("hole")}));
	QCOMPARE(readRoot(current).value(QStringLiteral("kept")).toInt(), 17);

	const QByteArray replacement = schema2Bytes(QJsonObject());
	QVERIFY(writeBytes(current, replacement));
	QVERIFY(store.migrateLegacyIfNeeded().ok);
	QCOMPARE(readBytes(current), replacement);
}

void CameraPoseStoreTest::deletionIsAtomicAndScopedToUuid()
{
	QTemporaryDir dir;
	QVERIFY(dir.isValid());
	const QString current = dir.filePath(QStringLiteral("poses.json"));
	CameraPoseStore store(current, QString());
	QVERIFY(store.save(UuidA, pose(QStringLiteral("a"))).ok);
	QVERIFY(store.save(UuidB, pose(QStringLiteral("b"))).ok);
	QVERIFY(store.remove(UuidA, QStringLiteral("view_001")).ok);
	QCOMPARE(store.list(UuidA).size(), 0);
	QCOMPARE(store.list(UuidB).size(), 1);

	const QJsonDocument document = QJsonDocument::fromJson(readBytes(current));
	const QJsonObject poses = document.object().value(QStringLiteral("poses")).toObject();
	QVERIFY(!poses.contains(UuidA));
	QVERIFY(poses.contains(UuidB));
}

void CameraPoseStoreTest::malformedLibraryFailsEveryOperationWithoutChangingBytes_data()
{
	QTest::addColumn<QByteArray>("bytes");

	QTest::newRow("invalid-json") << QByteArray("{");
	QTest::newRow("non-object-root") << QJsonDocument(QJsonArray()).toJson();

	QJsonObject root;
	root.insert(QStringLiteral("poses"), QJsonObject());
	QTest::newRow("missing-schema") << QJsonDocument(root).toJson();
	root.insert(QStringLiteral("schema"), 1);
	QTest::newRow("unsupported-schema") << QJsonDocument(root).toJson();
	root.insert(QStringLiteral("schema"), 2.5);
	QTest::newRow("non-integer-schema") << QJsonDocument(root).toJson();
	root.insert(QStringLiteral("schema"), QStringLiteral("2"));
	QTest::newRow("non-numeric-schema") << QJsonDocument(root).toJson();
	root = {{QStringLiteral("schema"), 2}};
	QTest::newRow("missing-poses") << QJsonDocument(root).toJson();
	root.insert(QStringLiteral("poses"), QJsonArray());
	QTest::newRow("non-object-poses") << QJsonDocument(root).toJson();

	QJsonObject poses;
	poses.insert(QString(), QJsonArray());
	QTest::newRow("invalid-uuid-key") << schema2Bytes(poses);
	poses = QJsonObject();
	poses.insert(UuidA, QJsonArray());
	poses.insert(QStringLiteral("aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa"), QJsonArray());
	QTest::newRow("duplicate-normalized-uuid") << schema2Bytes(poses);
	poses = QJsonObject();
	poses.insert(UuidA, 17);
	QTest::newRow("scalar-uuid-entry") << schema2Bytes(poses);

	QJsonArray views;
	views.append(17);
	poses = QJsonObject();
	poses.insert(UuidA, views);
	QTest::newRow("scalar-view") << schema2Bytes(poses);

	QJsonObject invalidPose;
	invalidPose.insert(QStringLiteral("view_id"), QStringLiteral("view_001"));
	views = QJsonArray();
	views.append(invalidPose);
	poses = QJsonObject();
	poses.insert(UuidA, views);
	QTest::newRow("missing-xml") << schema2Bytes(poses);
	invalidPose.insert(QStringLiteral("meshlab_view_state_xml"), QString());
	views[0] = invalidPose;
	poses.insert(UuidA, views);
	QTest::newRow("empty-xml") << schema2Bytes(poses);
	invalidPose.insert(QStringLiteral("meshlab_view_state_xml"), 42);
	views[0] = invalidPose;
	poses.insert(UuidA, views);
	QTest::newRow("non-string-xml") << schema2Bytes(poses);

	invalidPose = poseObject(QStringLiteral("view_001"), QStringLiteral("<x/>"));
	invalidPose.insert(QStringLiteral("view_id"), 1);
	views[0] = invalidPose;
	poses.insert(UuidA, views);
	QTest::newRow("non-string-view-id") << schema2Bytes(poses);
	invalidPose = poseObject(
		QStringLiteral("view_001"),
		QStringLiteral("<x/>"),
		QStringLiteral("not-a-time"));
	views[0] = invalidPose;
	poses.insert(UuidA, views);
	QTest::newRow("invalid-timestamp") << schema2Bytes(poses);
	invalidPose = poseObject(QStringLiteral("view_001"), QStringLiteral("<x/>"));
	invalidPose.insert(QStringLiteral("saved_at_utc"), 123);
	views[0] = invalidPose;
	poses.insert(UuidA, views);
	QTest::newRow("non-string-timestamp") << schema2Bytes(poses);

	views = QJsonArray();
	views.append(poseObject(QStringLiteral("same"), QStringLiteral("<one/>")));
	views.append(poseObject(QStringLiteral("same"), QStringLiteral("<two/>")));
	poses = QJsonObject();
	poses.insert(UuidA, views);
	QTest::newRow("duplicate-explicit-view-id") << schema2Bytes(poses);
	views = QJsonArray();
	views.append(poseObject(QString(), QStringLiteral("<one/>")));
	views.append(poseObject(QStringLiteral("view_001"), QStringLiteral("<two/>")));
	poses.insert(UuidA, views);
	QTest::newRow("fallback-collides-with-explicit-id") << schema2Bytes(poses);
}

void CameraPoseStoreTest::malformedLibraryFailsEveryOperationWithoutChangingBytes()
{
	QFETCH(QByteArray, bytes);
	QTemporaryDir dir;
	QVERIFY(dir.isValid());
	const QString current = dir.filePath(QStringLiteral("poses.json"));
	QVERIFY(writeBytes(current, bytes));
	CameraPoseStore store(current, QString());

	OperationResult result = OperationResult::success();
	QVERIFY(store.listAll(&result).isEmpty());
	QVERIFY(!result.ok);
	QVERIFY(!result.error.isEmpty());
	QCOMPARE(readBytes(current), bytes);

	CameraPose destination{QStringLiteral("unchanged")};
	QVERIFY(!store.load(UuidA, QStringLiteral("view_001"), &destination).ok);
	QCOMPARE(destination.viewStateXml, QStringLiteral("unchanged"));
	QCOMPARE(readBytes(current), bytes);

	QString viewId = QStringLiteral("unchanged");
	QVERIFY(!store.save(UuidA, pose(QStringLiteral("<new/>")), &viewId).ok);
	QCOMPARE(viewId, QStringLiteral("unchanged"));
	QCOMPARE(readBytes(current), bytes);

	QVERIFY(!store.remove(UuidA, QStringLiteral("view_001")).ok);
	QCOMPARE(readBytes(current), bytes);
}

void CameraPoseStoreTest::unrelatedCorruptionInvalidatesUuidScopedReads()
{
	QTemporaryDir dir;
	QVERIFY(dir.isValid());
	const QString current = dir.filePath(QStringLiteral("poses.json"));
	QJsonArray validViews;
	validViews.append(poseObject(QStringLiteral("view_001"), QStringLiteral("<valid/>")));
	QJsonArray invalidViews;
	invalidViews.append(QJsonObject{{QStringLiteral("view_id"), QStringLiteral("view_001")}});
	QJsonObject poses;
	poses.insert(UuidA, validViews);
	poses.insert(UuidB, invalidViews);
	QVERIFY(writeBytes(current, schema2Bytes(poses)));

	CameraPoseStore store(current, QString());
	OperationResult result = OperationResult::success();
	QVERIFY(store.list(UuidA, &result).isEmpty());
	QVERIFY(!result.ok);
	CameraPose destination{QStringLiteral("unchanged")};
	QVERIFY(!store.load(UuidA, QStringLiteral("view_001"), &destination).ok);
	QCOMPARE(destination.viewStateXml, QStringLiteral("unchanged"));
}

void CameraPoseStoreTest::readsSingleObjectAndPreservesOpaqueXml()
{
	QTemporaryDir dir;
	QVERIFY(dir.isValid());
	const QString current = dir.filePath(QStringLiteral("poses.json"));
	const QString xml = QString::fromUtf8("not parsed as XML:\n相机 & <opaque");
	QJsonObject single = poseObject(QString(), xml, QString());
	single.insert(QStringLiteral("future_pose_field"), QJsonArray{1, 2, 3});
	QJsonObject poses;
	poses.insert(UuidA, single);
	QVERIFY(writeBytes(current, schema2Bytes(poses)));

	CameraPoseStore store(current, QString());
	OperationResult result = OperationResult::failure(QStringLiteral("stale"));
	const QVector<SavedCameraPose> saved = store.list(
		QStringLiteral("{AAAAAAAA-AAAA-AAAA-AAAA-AAAAAAAAAAAA}"), &result);
	QVERIFY(result.ok);
	QCOMPARE(saved.size(), 1);
	QCOMPARE(saved.front().uuid, UuidA);
	QCOMPARE(saved.front().viewId, QStringLiteral("view_001"));
	QVERIFY(saved.front().savedAtUtc.isEmpty());
	QCOMPARE(saved.front().pose.viewStateXml, xml);

	CameraPose loaded{QStringLiteral("stale")};
	QVERIFY(store.load(UuidA, QStringLiteral("view_001"), &loaded).ok);
	QCOMPARE(loaded.viewStateXml, xml);
	QCOMPARE(store.listAll(&result).size(), 1);
	QVERIFY(result.ok);
}

void CameraPoseStoreTest::listIsUuidScopedAndSortedByTimestampThenViewId()
{
	QTemporaryDir dir;
	QVERIFY(dir.isValid());
	const QString current = dir.filePath(QStringLiteral("poses.json"));
	QJsonArray viewsA;
	viewsA.append(poseObject(
		QStringLiteral("view_b"),
		QStringLiteral("b"),
		QStringLiteral("2026-07-14T09:10:11.124Z")));
	viewsA.append(poseObject(QStringLiteral("view_z"), QStringLiteral("z"), QString()));
	viewsA.append(poseObject(QStringLiteral("view_y"), QStringLiteral("y"), QStringLiteral("")));
	viewsA.append(poseObject(
		QStringLiteral("view_c"),
		QStringLiteral("c"),
		QStringLiteral("2026-07-14T09:10:11.123Z")));
	viewsA.append(poseObject(
		QStringLiteral("view_a"),
		QStringLiteral("a"),
		QStringLiteral("2026-07-14T09:10:11.123Z")));
	QJsonArray viewsB;
	viewsB.append(poseObject(QStringLiteral("other"), QStringLiteral("other"), QString()));
	QJsonObject poses;
	poses.insert(UuidA, viewsA);
	poses.insert(UuidB, viewsB);
	QVERIFY(writeBytes(current, schema2Bytes(poses)));

	CameraPoseStore store(current, QString());
	OperationResult result = OperationResult::failure(QStringLiteral("stale"));
	const QVector<SavedCameraPose> saved = store.list(UuidA, &result);
	QVERIFY(result.ok);
	QCOMPARE(saved.size(), 5);
	QCOMPARE(saved.at(0).viewId, QStringLiteral("view_y"));
	QCOMPARE(saved.at(1).viewId, QStringLiteral("view_z"));
	QCOMPARE(saved.at(2).viewId, QStringLiteral("view_a"));
	QCOMPARE(saved.at(3).viewId, QStringLiteral("view_c"));
	QCOMPARE(saved.at(4).viewId, QStringLiteral("view_b"));
	for (const SavedCameraPose& value : saved)
		QCOMPARE(value.uuid, UuidA);
	QCOMPARE(store.list(UuidB, &result).size(), 1);
	QVERIFY(result.ok);
	QCOMPARE(store.list(QStringLiteral("cccccccccccccccccccccccccccccccc"), &result).size(), 0);
	QVERIFY(result.ok);
	result = OperationResult::failure(QStringLiteral("stale-list-all-result"));
	const QVector<SavedCameraPose> all = store.listAll(&result);
	QCOMPARE(all.size(), 6);
	QVERIFY(result.ok);
	QVERIFY(result.error.isEmpty());
	QCOMPARE(all.at(0).uuid, UuidA);
	QCOMPARE(all.at(0).viewId, QStringLiteral("view_y"));
	QCOMPARE(all.at(1).uuid, UuidA);
	QCOMPARE(all.at(1).viewId, QStringLiteral("view_z"));
	QCOMPARE(all.at(2).uuid, UuidA);
	QCOMPARE(all.at(2).viewId, QStringLiteral("view_a"));
	QCOMPARE(all.at(3).uuid, UuidA);
	QCOMPARE(all.at(3).viewId, QStringLiteral("view_c"));
	QCOMPARE(all.at(4).uuid, UuidA);
	QCOMPARE(all.at(4).viewId, QStringLiteral("view_b"));
	QCOMPARE(all.at(5).uuid, UuidB);
	QCOMPARE(all.at(5).viewId, QStringLiteral("other"));
}

void CameraPoseStoreTest::operationOutputsAreAlwaysOverwrittenOnlyAsDocumented()
{
	QTemporaryDir dir;
	QVERIFY(dir.isValid());
	const QString current = dir.filePath(QStringLiteral("poses.json"));
	CameraPoseStore store(current, QString());

	OperationResult result = OperationResult::failure(QStringLiteral("stale"));
	QVERIFY(store.list(UuidA, &result).isEmpty());
	QVERIFY(result.ok);
	QVERIFY(result.error.isEmpty());
	result = OperationResult::success();
	QVERIFY(store.list(QString(), &result).isEmpty());
	QVERIFY(!result.ok);
	QVERIFY(!result.error.isEmpty());

	CameraPose destination{QStringLiteral("unchanged")};
	QVERIFY(!store.load(UuidA, QStringLiteral("view_001"), &destination).ok);
	QCOMPARE(destination.viewStateXml, QStringLiteral("unchanged"));
	QVERIFY(!store.load(UuidA, QStringLiteral("view_001"), nullptr).ok);

	QString savedViewId = QStringLiteral("unchanged");
	QVERIFY(!store.save(UuidA, pose(QString()), &savedViewId).ok);
	QCOMPARE(savedViewId, QStringLiteral("unchanged"));
	QVERIFY(!QFileInfo::exists(current));
	QVERIFY(store.save(UuidA, pose(QStringLiteral("<saved/>")), &savedViewId).ok);
	QCOMPARE(savedViewId, QStringLiteral("view_001"));
	QVERIFY(store.load(UuidA, savedViewId, &destination).ok);
	QCOMPARE(destination.viewStateXml, QStringLiteral("<saved/>"));
	const QByteArray savedBytes = readBytes(current);
	savedViewId = QStringLiteral("unchanged-again");
	QVERIFY(!store.save(UuidA, pose(QString()), &savedViewId).ok);
	QCOMPARE(savedViewId, QStringLiteral("unchanged-again"));
	QCOMPARE(readBytes(current), savedBytes);
	QVERIFY(!store.save(QString(), pose(QStringLiteral("<x/>")), &savedViewId).ok);
	QCOMPARE(savedViewId, QStringLiteral("unchanged-again"));
	QCOMPARE(readBytes(current), savedBytes);
}

void CameraPoseStoreTest::savePreservesFieldsAndAllocatesMonotonicIds()
{
	QTemporaryDir dir;
	QVERIFY(dir.isValid());
	const QString current = dir.filePath(QStringLiteral("poses.json"));
	QJsonObject first = poseObject(QString(), QStringLiteral("first"), QString());
	first.insert(QStringLiteral("unknown_first"), true);
	QJsonObject second = poseObject(QString(), QStringLiteral("second"));
	QJsonObject parameters{{QStringLiteral("future_parameter"), 91}};
	QJsonObject tenth = poseObject(QStringLiteral("view_010"), QStringLiteral("tenth"));
	tenth.insert(QStringLiteral("parameters"), parameters);
	tenth.insert(QStringLiteral("future_pose_field"), QStringLiteral("keep"));
	QJsonObject custom = poseObject(QStringLiteral("favorite"), QStringLiteral("custom"));
	QJsonArray viewsA{first, second, tenth, custom};
	QJsonObject singleB = poseObject(QString(), QStringLiteral("other"));
	singleB.insert(QStringLiteral("unknown_other"), QJsonObject{{QStringLiteral("x"), 1}});
	QJsonObject poses;
	poses.insert(QStringLiteral("aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa"), viewsA);
	poses.insert(UuidB, singleB);
	const QJsonObject rootExtra{
		{QStringLiteral("future_root"), QJsonObject{{QStringLiteral("nested"), QStringLiteral("keep")}}}};
	QVERIFY(writeBytes(current, schema2Bytes(poses, rootExtra)));

	CameraPoseStore store(current, QString());
	QString savedViewId = QStringLiteral("stale");
	QVERIFY(store.save(
		QStringLiteral("{AAAAAAAA-AAAA-AAAA-AAAA-AAAAAAAAAAAA}"),
		pose(QStringLiteral("new")),
		&savedViewId).ok);
	QCOMPARE(savedViewId, QStringLiteral("view_011"));

	QJsonObject root = readRoot(current);
	QCOMPARE(
		root.value(QStringLiteral("future_root")),
		rootExtra.value(QStringLiteral("future_root")));
	QJsonObject storedPoses = root.value(QStringLiteral("poses")).toObject();
	QVERIFY(!storedPoses.contains(QStringLiteral("aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa")));
	QVERIFY(storedPoses.contains(UuidA));
	QCOMPARE(storedPoses.value(UuidB), QJsonValue(singleB));
	QJsonArray storedA = storedPoses.value(UuidA).toArray();
	QCOMPARE(storedA.size(), 5);
	QCOMPARE(
		storedA.at(0).toObject().value(QStringLiteral("view_id")).toString(),
		QStringLiteral("view_001"));
	QCOMPARE(
		storedA.at(1).toObject().value(QStringLiteral("view_id")).toString(),
		QStringLiteral("view_002"));
	QCOMPARE(storedA.at(0).toObject().value(QStringLiteral("unknown_first")).toBool(), true);
	QCOMPARE(storedA.at(2).toObject().value(QStringLiteral("parameters")), QJsonValue(parameters));
	QCOMPARE(
		storedA.at(2).toObject().value(QStringLiteral("future_pose_field")).toString(),
		QStringLiteral("keep"));

	QVERIFY(store.remove(UuidA, QStringLiteral("view_002")).ok);
	QVERIFY(store.save(UuidA, pose(QStringLiteral("after-hole")), &savedViewId).ok);
	QCOMPARE(savedViewId, QStringLiteral("view_012"));
	QCOMPARE(store.list(UuidB).size(), 1);
}

void CameraPoseStoreTest::saveAllocatesIdsBeyondMachineIntegerRange()
{
	QTemporaryDir dir;
	QVERIFY(dir.isValid());
	const QString current = dir.filePath(QStringLiteral("poses.json"));
	QJsonArray views;
	views.append(poseObject(
		QStringLiteral("view_18446744073709551616"),
		QStringLiteral("huge")));
	QJsonObject poses;
	poses.insert(UuidA, views);
	QVERIFY(writeBytes(current, schema2Bytes(poses)));

	CameraPoseStore store(current, QString());
	QString viewId = QStringLiteral("unchanged");
	QVERIFY(store.save(UuidA, pose(QStringLiteral("next")), &viewId).ok);
	QCOMPARE(viewId, QStringLiteral("view_18446744073709551617"));
}

void CameraPoseStoreTest::saveWritesUtcMillisecondTimestampAndExactUnicodeXml()
{
	QTemporaryDir dir;
	QVERIFY(dir.isValid());
	const QString current = dir.filePath(QStringLiteral("poses.json"));
	CameraPoseStore store(current, QString());
	const QString xml = QString::fromUtf8("opaque line 1\n相机 & not parsed <line 2");
	const QDateTime before = QDateTime::currentDateTimeUtc();
	QString viewId;
	QVERIFY(store.save(UuidA, pose(xml), &viewId).ok);
	const QDateTime after = QDateTime::currentDateTimeUtc();

	const QVector<SavedCameraPose> saved = store.list(UuidA);
	QCOMPARE(saved.size(), 1);
	QCOMPARE(saved.front().pose.viewStateXml, xml);
	QVERIFY(QRegularExpression(
		QStringLiteral("^\\d{4}-\\d{2}-\\d{2}T\\d{2}:\\d{2}:\\d{2}\\.\\d{3}Z$"))
		.match(saved.front().savedAtUtc).hasMatch());
	const QDateTime parsed =
		QDateTime::fromString(saved.front().savedAtUtc, Qt::ISODateWithMs).toUTC();
	QVERIFY(parsed.isValid());
	QVERIFY(parsed >= before.addMSecs(-1));
	QVERIFY(parsed <= after.addMSecs(1));
	CameraPose loaded;
	QVERIFY(store.load(UuidA, viewId, &loaded).ok);
	QCOMPARE(loaded.viewStateXml, xml);

	const QJsonObject root = readRoot(current);
	QCOMPARE(root.value(QStringLiteral("schema")).toInt(), 2);
	const QJsonArray views = root.value(QStringLiteral("poses")).toObject().value(UuidA).toArray();
	QCOMPARE(views.size(), 1);
	QVERIFY(views.at(0).toObject().value(QStringLiteral("parameters")).isObject());
	QCOMPARE(views.at(0).toObject().value(QStringLiteral("meshlab_view_state_xml")).toString(), xml);
}

void CameraPoseStoreTest::saveDerivesLegacyCompatibleParametersWhenViewStateXmlProvidesThem()
{
	QTemporaryDir dir;
	QVERIFY(dir.isValid());
	const QString current = dir.filePath(QStringLiteral("poses.json"));
	CameraPoseStore store(current, QString());
	const QString xml = QStringLiteral(
		"<!DOCTYPE ViewState><project>"
		"<VCGCamera TranslationVector=\"1 2 3 1\" "
		"RotationMatrix=\"1 0 0 0 0 1 0 0 0 0 1 0 4 5 6 1\" "
		"CameraType=\"0\" FocalMm=\"35.5\" LensDistortion=\"0.1 0.2\" "
		"PixelSizeMm=\"0.01 0.02\" ViewportPx=\"800 600\" CenterPx=\"400 300\"/>"
		"<ViewSettings TrackScale=\"2.5\" NearPlane=\"0.01\" FarPlane=\"1000\"/>"
		"</project>");
	QVERIFY(store.save(UuidA, pose(xml)).ok);

	const QJsonObject parameters = readRoot(current)
		.value(QStringLiteral("poses")).toObject()
		.value(UuidA).toArray().at(0).toObject()
		.value(QStringLiteral("parameters")).toObject();
	QCOMPARE(parameters.value(QStringLiteral("translation_vector")).toArray(), QJsonArray({1, 2, 3}));
	QCOMPARE(parameters.value(QStringLiteral("rotation_matrix")).toArray().size(), 4);
	QCOMPARE(
		parameters.value(QStringLiteral("rotation_matrix")).toArray().at(0).toArray(),
		QJsonArray({1, 0, 0, 0}));
	QCOMPARE(parameters.value(QStringLiteral("camera_type")).toDouble(), 0.0);
	QCOMPARE(parameters.value(QStringLiteral("focal_mm")).toDouble(), 35.5);
	QCOMPARE(parameters.value(QStringLiteral("lens_distortion")).toArray(), QJsonArray({0.1, 0.2}));
	QCOMPARE(parameters.value(QStringLiteral("pixel_size_mm")).toArray(), QJsonArray({0.01, 0.02}));
	QCOMPARE(parameters.value(QStringLiteral("viewport_px")).toArray(), QJsonArray({800, 600}));
	QCOMPARE(parameters.value(QStringLiteral("center_px")).toArray(), QJsonArray({400, 300}));
	QCOMPARE(parameters.value(QStringLiteral("track_scale")).toDouble(), 2.5);
	QCOMPARE(parameters.value(QStringLiteral("near_plane")).toDouble(), 0.01);
	QCOMPARE(parameters.value(QStringLiteral("far_plane")).toDouble(), 1000.0);
}

void CameraPoseStoreTest::missingAndInvalidRemoveRequestsAreByteIdenticalNoOps()
{
	QTemporaryDir dir;
	QVERIFY(dir.isValid());
	const QString current = dir.filePath(QStringLiteral("poses.json"));
	QJsonArray viewsA;
	viewsA.append(poseObject(QStringLiteral("view_001"), QStringLiteral("one")));
	viewsA.append(poseObject(QStringLiteral("view_002"), QStringLiteral("two")));
	QJsonArray viewsB;
	viewsB.append(poseObject(QStringLiteral("view_001"), QStringLiteral("other")));
	QJsonObject poses;
	poses.insert(UuidA, viewsA);
	poses.insert(UuidB, viewsB);
	const QByteArray original = schema2Bytes(
		poses,
		{{QStringLiteral("future_root"), QStringLiteral("keep")}});
	QVERIFY(writeBytes(current, original));
	CameraPoseStore store(current, QString());

	QVERIFY(!store.remove(QString(), QStringLiteral("view_001")).ok);
	QCOMPARE(readBytes(current), original);
	QVERIFY(!store.remove(
		QStringLiteral("cccccccccccccccccccccccccccccccc"),
		QStringLiteral("view_001"))
			.ok);
	QCOMPARE(readBytes(current), original);
	QVERIFY(!store.remove(UuidA, QStringLiteral("missing")).ok);
	QCOMPARE(readBytes(current), original);
	QVERIFY(!store.remove(UuidA, QString()).ok);
	QCOMPARE(readBytes(current), original);

	QVERIFY(store.remove(UuidA, QStringLiteral("view_001")).ok);
	QCOMPARE(store.list(UuidA).size(), 1);
	QCOMPARE(store.list(UuidB).size(), 1);
	QCOMPARE(
		readRoot(current).value(QStringLiteral("future_root")).toString(),
		QStringLiteral("keep"));
	QVERIFY(store.remove(UuidA, QStringLiteral("view_002")).ok);
	QVERIFY(!readRoot(current).value(QStringLiteral("poses")).toObject().contains(UuidA));
	QVERIFY(readRoot(current).value(QStringLiteral("poses")).toObject().contains(UuidB));
}

void CameraPoseStoreTest::failedInitialAndReplacementWritesLeaveNoPartialLibrary()
{
	QTemporaryDir initialDir;
	QVERIFY(initialDir.isValid());
	const QString blocker = initialDir.filePath(QStringLiteral("blocker"));
	QVERIFY(writeBytes(blocker, QByteArray("regular-file")));
	const QString impossible = QDir(blocker).filePath(QStringLiteral("poses.json"));
	CameraPoseStore initialStore(impossible, QString());
	QVERIFY(!initialStore.save(UuidA, pose(QStringLiteral("new"))).ok);
	QVERIFY(!QFileInfo::exists(impossible));
	QCOMPARE(readBytes(blocker), QByteArray("regular-file"));

	QTemporaryDir replacementDir;
	QVERIFY(replacementDir.isValid());
	const QString current = replacementDir.filePath(QStringLiteral("poses.json"));
	const QByteArray original = onePoseLibrary();
	QVERIFY(writeBytes(current, original));
	CameraPoseStore replacementStore(current, QString());
	const QFileDevice::Permissions originalPermissions = QFile::permissions(replacementDir.path());
	const QFileDevice::Permissions readOnly =
		QFileDevice::ReadOwner | QFileDevice::ExeOwner |
		QFileDevice::ReadGroup | QFileDevice::ExeGroup |
		QFileDevice::ReadOther | QFileDevice::ExeOther;
	QVERIFY(QFile::setPermissions(replacementDir.path(), readOnly));
	QFile permissionProbe(replacementDir.filePath(QStringLiteral("probe")));
	if (permissionProbe.open(QIODevice::WriteOnly)) {
		permissionProbe.close();
		QFile::remove(permissionProbe.fileName());
		QVERIFY(QFile::setPermissions(replacementDir.path(), originalPermissions));
		QSKIP("The current user can bypass directory write permissions.");
	}
	QString savedViewId = QStringLiteral("unchanged-after-write-failure");
	const OperationResult saveResult = replacementStore.save(
		UuidA,
		pose(QStringLiteral("replacement")),
		&savedViewId);
	const OperationResult removeResult = replacementStore.remove(UuidA, QStringLiteral("view_001"));
	const bool restoredPermissions = QFile::setPermissions(replacementDir.path(), originalPermissions);
	QVERIFY(restoredPermissions);
	QVERIFY(!saveResult.ok);
	QVERIFY(!removeResult.ok);
	QCOMPARE(savedViewId, QStringLiteral("unchanged-after-write-failure"));
	QCOMPARE(readBytes(current), original);
	QCOMPARE(
		QDir(replacementDir.path()).entryList(
			QDir::Files | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System),
		QStringList{QStringLiteral("poses.json")});
}

void CameraPoseStoreTest::migrationRejectsCorruptionAndExistingCurrentAlwaysWins()
{
	QTemporaryDir corruptLegacyDir;
	QVERIFY(corruptLegacyDir.isValid());
	const QString badLegacy = corruptLegacyDir.filePath(QStringLiteral("legacy.json"));
	const QString absentCurrent = corruptLegacyDir.filePath(QStringLiteral("new/poses.json"));
	const QByteArray corrupt("{not-json");
	QVERIFY(writeBytes(badLegacy, corrupt));
	CameraPoseStore corruptLegacyStore(absentCurrent, badLegacy);
	QVERIFY(!corruptLegacyStore.migrateLegacyIfNeeded().ok);
	QCOMPARE(readBytes(badLegacy), corrupt);
	QVERIFY(!QFileInfo::exists(absentCurrent));

	QTemporaryDir currentWinsDir;
	QVERIFY(currentWinsDir.isValid());
	const QString legacy = currentWinsDir.filePath(QStringLiteral("legacy.json"));
	const QString current = currentWinsDir.filePath(QStringLiteral("current.json"));
	const QByteArray legacyBytes = onePoseLibrary();
	const QByteArray currentBytes("corrupt-current-wins");
	QVERIFY(writeBytes(legacy, legacyBytes));
	QVERIFY(writeBytes(current, currentBytes));
	CameraPoseStore currentWinsStore(current, legacy);
	QVERIFY(currentWinsStore.migrateLegacyIfNeeded().ok);
	QCOMPARE(readBytes(current), currentBytes);
	QCOMPARE(readBytes(legacy), legacyBytes);

	QTemporaryDir bothCorruptDir;
	QVERIFY(bothCorruptDir.isValid());
	const QString corruptLegacy =
		bothCorruptDir.filePath(QStringLiteral("legacy.json"));
	const QString corruptCurrent =
		bothCorruptDir.filePath(QStringLiteral("current.json"));
	const QByteArray corruptLegacyBytes("corrupt-legacy-must-not-be-read");
	const QByteArray corruptCurrentBytes("corrupt-current-still-wins");
	QVERIFY(writeBytes(corruptLegacy, corruptLegacyBytes));
	QVERIFY(writeBytes(corruptCurrent, corruptCurrentBytes));
	CameraPoseStore bothCorruptStore(corruptCurrent, corruptLegacy);
	QVERIFY(bothCorruptStore.migrateLegacyIfNeeded().ok);
	QCOMPARE(readBytes(corruptCurrent), corruptCurrentBytes);
	QCOMPARE(readBytes(corruptLegacy), corruptLegacyBytes);

	QTemporaryDir idempotentDir;
	QVERIFY(idempotentDir.isValid());
	const QString idempotentLegacy = idempotentDir.filePath(QStringLiteral("legacy.json"));
	const QString idempotentCurrent = idempotentDir.filePath(QStringLiteral("new/poses.json"));
	QVERIFY(writeBytes(idempotentLegacy, legacyBytes));
	CameraPoseStore idempotentStore(idempotentCurrent, idempotentLegacy);
	QVERIFY(idempotentStore.migrateLegacyIfNeeded().ok);
	QVERIFY(writeBytes(idempotentLegacy, onePoseLibrary(UuidB)));
	QVERIFY(idempotentStore.migrateLegacyIfNeeded().ok);
	const QVector<SavedCameraPose> idempotent = idempotentStore.list(UuidA);
	QCOMPARE(idempotent.size(), 1);
	QCOMPARE(idempotent.front().tags, QStringList({QStringLiteral("hole")}));
	QVERIFY(idempotentStore.list(UuidB).isEmpty());
}

void CameraPoseStoreTest::migrationRejectsStructurallyCorruptJson()
{
	QTemporaryDir dir;
	QVERIFY(dir.isValid());
	const QString legacy = dir.filePath(QStringLiteral("legacy.json"));
	const QString current = dir.filePath(QStringLiteral("current/poses.json"));
	QJsonArray invalidViews;
	invalidViews.append(QJsonObject{
		{QStringLiteral("view_id"), QStringLiteral("view_001")}});
	QJsonObject poses;
	poses.insert(UuidA, invalidViews);
	const QByteArray invalidButParseable = schema2Bytes(poses);
	QVERIFY(writeBytes(legacy, invalidButParseable));
	const QDateTime legacyMtime = QFileInfo(legacy).lastModified();

	CameraPoseStore store(current, legacy);
	QVERIFY(!store.migrateLegacyIfNeeded().ok);
	QCOMPARE(readBytes(legacy), invalidButParseable);
	QCOMPARE(QFileInfo(legacy).lastModified(), legacyMtime);
	QVERIFY(!QFileInfo::exists(current));
}

void CameraPoseStoreTest::migrationMissingLegacyIsIdempotentNoOp()
{
	QTemporaryDir dir;
	QVERIFY(dir.isValid());
	const QString current = dir.filePath(QStringLiteral("not-created/poses.json"));
	CameraPoseStore emptyLegacyStore(current, QString());
	QVERIFY(emptyLegacyStore.migrateLegacyIfNeeded().ok);
	QVERIFY(!QFileInfo::exists(current));
	QVERIFY(!QFileInfo::exists(QFileInfo(current).absolutePath()));

	CameraPoseStore missingLegacyStore(current, dir.filePath(QStringLiteral("missing.json")));
	QVERIFY(missingLegacyStore.migrateLegacyIfNeeded().ok);
	QVERIFY(missingLegacyStore.migrateLegacyIfNeeded().ok);
	QVERIFY(!QFileInfo::exists(current));

	const QString emptyLegacy = dir.filePath(QStringLiteral("empty.json"));
	QVERIFY(writeBytes(emptyLegacy, QByteArray()));
	CameraPoseStore emptyFileStore(current, emptyLegacy);
	QVERIFY(!emptyFileStore.migrateLegacyIfNeeded().ok);
	QVERIFY(!QFileInfo::exists(current));
}

void CameraPoseStoreTest::migrationCopyFailureLeavesLegacyUntouched()
{
	QTemporaryDir dir;
	QVERIFY(dir.isValid());
	const QString legacy = dir.filePath(QStringLiteral("legacy.json"));
	const QByteArray legacyBytes = onePoseLibrary();
	QVERIFY(writeBytes(legacy, legacyBytes));
	const QDateTime legacyMtime = QFileInfo(legacy).lastModified();
	const QString blocker = dir.filePath(QStringLiteral("blocker"));
	QVERIFY(writeBytes(blocker, QByteArray("not-a-directory")));
	const QString current = QDir(blocker).filePath(QStringLiteral("poses.json"));

	CameraPoseStore store(current, legacy);
	QVERIFY(!store.migrateLegacyIfNeeded().ok);
	QCOMPARE(readBytes(legacy), legacyBytes);
	QCOMPARE(QFileInfo(legacy).lastModified(), legacyMtime);
	QVERIFY(!QFileInfo::exists(current));
	QCOMPARE(readBytes(blocker), QByteArray("not-a-directory"));
}

void CameraPoseStoreTest::exclusivePublisherNeverReplacesExistingDestination()
{
	QTemporaryDir dir;
	QVERIFY(dir.isValid());
	const QString staged = dir.filePath(QStringLiteral("staged.json"));
	const QString current = dir.filePath(QStringLiteral("current.json"));
	const QByteArray stagedBytes = onePoseLibrary(UuidA);
	const QByteArray concurrentCurrentBytes = onePoseLibrary(UuidB);
	QVERIFY(writeBytes(staged, stagedBytes));
	QVERIFY(writeBytes(current, concurrentCurrentBytes));

	QString error = QStringLiteral("stale");
	QCOMPARE(
		camera_pose_store_detail::publishFileNoReplace(staged, current, &error),
		camera_pose_store_detail::PublishResult::DestinationExists);
	QCOMPARE(readBytes(current), concurrentCurrentBytes);
	QCOMPARE(readBytes(staged), stagedBytes);
	QCOMPARE(error, QStringLiteral("stale"));

	QVERIFY(QFile::remove(current));
	QCOMPARE(
		camera_pose_store_detail::publishFileNoReplace(staged, current, &error),
		camera_pose_store_detail::PublishResult::Published);
	QCOMPARE(readBytes(current), stagedBytes);
	QCOMPARE(readBytes(staged), stagedBytes);
}

void CameraPoseStoreTest::migrationCopyFailureAfterValidationLeavesNoArtifacts()
{
#ifdef Q_OS_UNIX
	QTemporaryDir dir;
	QVERIFY(dir.isValid());
	const QString legacy = dir.filePath(QStringLiteral("legacy.fifo"));
	const QByteArray encodedLegacyPath = QFile::encodeName(legacy);
	QVERIFY(::mkfifo(encodedLegacyPath.constData(), S_IRUSR | S_IWUSR) == 0);
	const QString current = dir.filePath(QStringLiteral("current/poses.json"));
	const QString currentDirectory = QFileInfo(current).absolutePath();
	const QByteArray validLegacyBytes = onePoseLibrary();
	CameraPoseStore store(current, legacy);
	OperationResult migrationResult = OperationResult::failure(QStringLiteral("not run"));
	QThread* migrationThread = QThread::create([&]() {
		migrationResult = store.migrateLegacyIfNeeded();
	});
	migrationThread->start();

	QFile validationWriter;
	const bool writerOpened =
		openFifoWriterWithTimeout(&validationWriter, legacy);
	bool legacyWritten = false;
	if (writerOpened) {
		legacyWritten =
			validationWriter.write(validLegacyBytes) == validLegacyBytes.size()
			&& validationWriter.flush();
		validationWriter.close();
	}
	else {
		rescueBlockedFifoReader(legacy);
	}

	QString stagingDirectoryPath;
	QElapsedTimer timer;
	timer.start();
	while (stagingDirectoryPath.isEmpty() && timer.elapsed() < 5000) {
		const QStringList directories = QDir(currentDirectory).entryList(
			QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System);
		for (const QString& name : directories) {
			if (name.startsWith(QStringLiteral(".meshcompare-camera-migration-"))) {
				stagingDirectoryPath = QDir(currentDirectory).filePath(name);
				break;
			}
		}
		if (stagingDirectoryPath.isEmpty())
			QThread::msleep(1);
	}

	// QFile::copy is blocked reopening the legacy FIFO. Connect its source,
	// occupy the staged destination while the stream is empty, then deliver EOF.
	// The copy must fail without deleting the still-existing legacy path.
	QFile copyWriter;
	const bool copyWriterOpened =
		openFifoWriterWithTimeout(&copyWriter, legacy);
	// A connected writer proves QFile::copy passed its destination precheck and
	// opened the FIFO source. Keep the stream empty while installing the blocker
	// so the worker cannot finish the copy first.
	const bool stagedDestinationBlocked = copyWriterOpened
		&& !stagingDirectoryPath.isEmpty()
		&& QDir().mkpath(
			QDir(stagingDirectoryPath).filePath(QStringLiteral("camera_poses.json")));
	if (copyWriterOpened) {
		// Closing supplies EOF if the worker is already reading. Do not write:
		// once the blocker is observed, QFile::copy may close its reader and a
		// subsequent FIFO write would raise SIGPIPE in the test process.
		copyWriter.close();
	}
	else {
		rescueBlockedFifoReader(legacy);
	}
	const bool threadFinished = waitForMigrationThread(migrationThread, legacy);
	delete migrationThread;

	QVERIFY(threadFinished);
	QVERIFY(writerOpened);
	QVERIFY(legacyWritten);
	QVERIFY(!stagingDirectoryPath.isEmpty());
	QVERIFY(stagedDestinationBlocked);
	QVERIFY(copyWriterOpened);
	QVERIFY(!migrationResult.ok);
	QVERIFY(QFileInfo::exists(legacy));
	QVERIFY(!QFileInfo::exists(current));
	QCOMPARE(
		QDir(currentDirectory).entryList(
			QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System),
		QStringList());
#else
	QSKIP("This copy-failure harness requires a POSIX FIFO.");
#endif
}

void CameraPoseStoreTest::migrationConcurrentDestinationWinsWithoutBeingRemoved()
{
#ifdef Q_OS_UNIX
	QTemporaryDir dir;
	QVERIFY(dir.isValid());
	const QString legacy = dir.filePath(QStringLiteral("legacy.fifo"));
	const QByteArray encodedLegacyPath = QFile::encodeName(legacy);
	QVERIFY(::mkfifo(encodedLegacyPath.constData(), S_IRUSR | S_IWUSR) == 0);
	const QString current = dir.filePath(QStringLiteral("current/poses.json"));
	const QByteArray legacyBytes = onePoseLibrary(UuidA);
	const QByteArray concurrentCurrentBytes = onePoseLibrary(UuidB);
	CameraPoseStore store(current, legacy);
	OperationResult migrationResult = OperationResult::failure(QStringLiteral("not run"));
	QThread* migrationThread = QThread::create([&]() {
		migrationResult = store.migrateLegacyIfNeeded();
	});
	migrationThread->start();

	QFile writer;
	const bool writerOpened = openFifoWriterWithTimeout(&writer, legacy);
	bool currentWritten = false;
	bool legacyWritten = false;
	bool legacyUnlinked = false;
	if (writerOpened) {
		// Opening the FIFO writer proves migration already passed its initial
		// destination check and is blocked validating the legacy stream.
		currentWritten = writeBytes(current, concurrentCurrentBytes);
		legacyWritten = writer.write(legacyBytes) == legacyBytes.size()
			&& writer.flush();
		legacyUnlinked = QFile::remove(legacy);
		writer.close();
	}
	else {
		rescueBlockedFifoReader(legacy);
	}
	const bool threadFinished = waitForMigrationThread(migrationThread, legacy);
	delete migrationThread;

	QVERIFY(threadFinished);
	QVERIFY(writerOpened);
	QVERIFY(currentWritten);
	QVERIFY(legacyWritten);
	QVERIFY(legacyUnlinked);
	QVERIFY2(migrationResult.ok, qPrintable(migrationResult.error));
	const QVector<SavedCameraPose> concurrent =
		CameraPoseStore(current, QString()).list(UuidB);
	QCOMPARE(concurrent.size(), 1);
	QCOMPARE(concurrent.front().tags, QStringList({QStringLiteral("hole")}));
#else
	QSKIP("This race harness requires a POSIX FIFO.");
#endif
}

void CameraPoseStoreTest::migrationValidatesCopiedBytesBeforePublishing()
{
#ifdef Q_OS_UNIX
	QTemporaryDir dir;
	QVERIFY(dir.isValid());
	const QString legacy = dir.filePath(QStringLiteral("legacy.fifo"));
	const QByteArray encodedLegacyPath = QFile::encodeName(legacy);
	QVERIFY(::mkfifo(encodedLegacyPath.constData(), S_IRUSR | S_IWUSR) == 0);
	const QString current = dir.filePath(QStringLiteral("current/poses.json"));
	const QString currentDirectory = QFileInfo(current).absolutePath();
	const QByteArray validLegacyBytes = onePoseLibrary(UuidA);
	const QByteArray corruptCopiedBytes("{not-json");
	const QString replacement = dir.filePath(QStringLiteral("replacement.json"));
	QVERIFY(writeBytes(replacement, corruptCopiedBytes));
	CameraPoseStore store(current, legacy);
	OperationResult migrationResult = OperationResult::failure(QStringLiteral("not run"));
	QThread* migrationThread = QThread::create([&]() {
		migrationResult = store.migrateLegacyIfNeeded();
	});
	migrationThread->start();

	QFile validationWriter;
	const bool validationWriterOpened =
		openFifoWriterWithTimeout(&validationWriter, legacy);
	bool validationBytesWritten = false;
	bool legacyPathReplaced = false;
	if (validationWriterOpened) {
		validationBytesWritten =
			validationWriter.write(validLegacyBytes) == validLegacyBytes.size()
			&& validationWriter.flush();
		// The Store is still reading the already-open FIFO. Replace only its
		// pathname before delivering EOF, so the subsequent QFile::copy opens
		// different bytes than the ones that passed initial validation.
		legacyPathReplaced = QFile::remove(legacy)
			&& QFile::rename(replacement, legacy);
		validationWriter.close();
	}
	else {
		rescueBlockedFifoReader(legacy);
	}
	const bool threadFinished = waitForMigrationThread(migrationThread, legacy);
	delete migrationThread;

	QVERIFY(threadFinished);
	QVERIFY(validationWriterOpened);
	QVERIFY(validationBytesWritten);
	QVERIFY(legacyPathReplaced);
	QVERIFY(!migrationResult.ok);
	QCOMPARE(readBytes(legacy), corruptCopiedBytes);
	QVERIFY(!QFileInfo::exists(current));
	QCOMPARE(
		QDir(currentDirectory).entryList(
			QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System),
		QStringList());
#else
	QSKIP("This staged-copy harness requires a POSIX FIFO.");
#endif
}

void CameraPoseStoreTest::migrationTreatsDanglingDestinationSymlinkAsExisting()
{
#ifdef Q_OS_UNIX
	QTemporaryDir dir;
	QVERIFY(dir.isValid());
	const QString legacy = dir.filePath(QStringLiteral("legacy.json"));
	const QByteArray legacyBytes = onePoseLibrary();
	QVERIFY(writeBytes(legacy, legacyBytes));
	const QString missingTarget =
		dir.filePath(QStringLiteral("missing-target.json"));
	const QString current = dir.filePath(QStringLiteral("current.json"));
	QVERIFY(QFile::link(missingTarget, current));
	QVERIFY(QFileInfo(current).isSymLink());

	CameraPoseStore store(current, legacy);
	QVERIFY(store.migrateLegacyIfNeeded().ok);
	QVERIFY(QFileInfo(current).isSymLink());
	QVERIFY(!QFileInfo::exists(missingTarget));
	QCOMPARE(readBytes(legacy), legacyBytes);
#else
	QSKIP("This path-entry test requires a POSIX symlink.");
#endif
}

QTEST_APPLESS_MAIN(CameraPoseStoreTest)

#include "camera_pose_store_test.moc"
