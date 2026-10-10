#include <QtTest>

#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

#include "services/camera_sync_state.h"

class CameraSyncStateTest final : public QObject
{
    Q_OBJECT

private slots:
    void missingAndDifferentBoardAreEmptySuccesses()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        meshcompare::CameraSyncStateStore store(
            directory.filePath(QStringLiteral("sync-state.json")));
        meshcompare::CameraSyncPersistedState loaded;
        bool found = true;
        QVERIFY(store.load(QStringLiteral("board-a"), &loaded, &found).ok);
        QVERIFY(!found);

        meshcompare::CameraSyncPersistedState saved;
        saved.boardKey = QStringLiteral("board-a");
        saved.boardVersion = 3;
        saved.syncedAtUtc = QStringLiteral("2026-10-10T01:02:03.004Z");
        QVERIFY(store.save(saved).ok);
        QVERIFY(store.load(QStringLiteral("board-b"), &loaded, &found).ok);
        QVERIFY(!found);
    }

    void roundTripPreservesTheAcknowledgedBase()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        meshcompare::CameraSyncStateStore store(
            directory.filePath(QStringLiteral("sync-state.json")));
        meshcompare::CameraSyncPersistedState saved;
        saved.boardKey = QStringLiteral("meshcompare-camera-views");
        saved.boardVersion = 7;
        saved.syncedAtUtc = QStringLiteral("2026-10-10T01:02:03.004Z");
        meshcompare::CameraPoseSyncView view;
        view.workspaceUuid = QStringLiteral("workspace-a");
        view.viewId = QStringLiteral("view_001");
        view.savedAtUtc = QStringLiteral("2026-10-10T00:00:00.000Z");
        view.tags = QStringList{QStringLiteral("inspection")};
        view.note = QString::fromUtf8("云端备注");
        view.poseXml = QStringLiteral("<camera/>");
        view.screenshot.sha256 = QString(64, QLatin1Char('a'));
        view.screenshot.fileId = QStringLiteral("img_001");
        view.screenshot.mimeType = QStringLiteral("image/png");
        view.screenshot.size = 123;
        saved.base.views.append(view);
        QVERIFY(store.save(saved).ok);

        meshcompare::CameraSyncPersistedState loaded;
        bool found = false;
        const OperationResult result = store.load(saved.boardKey, &loaded, &found);
        QVERIFY2(result.ok, qPrintable(result.error));
        QVERIFY(found);
        QCOMPARE(loaded.boardKey, saved.boardKey);
        QCOMPARE(loaded.boardVersion, 7);
        QCOMPARE(loaded.syncedAtUtc, saved.syncedAtUtc);
        QCOMPARE(loaded.base.views.size(), 1);
        QCOMPARE(loaded.base.views.front().note, view.note);
        QCOMPARE(loaded.base.views.front().screenshot.fileId, QStringLiteral("img_001"));
        QVERIFY(loaded.base.views.front().screenshot.sourcePath.isEmpty());
    }

    void roundTripPreservesVersionsBeyondThirtyTwoBits()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        meshcompare::CameraSyncStateStore store(
            directory.filePath(QStringLiteral("sync-state.json")));
        meshcompare::CameraSyncPersistedState saved;
        saved.boardKey = QStringLiteral("board-a");
        saved.boardVersion = qint64(1) << 40;
        QVERIFY(store.save(saved).ok);

        meshcompare::CameraSyncPersistedState loaded;
        bool found = false;
        const OperationResult result = store.load(saved.boardKey, &loaded, &found);
        QVERIFY2(result.ok, qPrintable(result.error));
        QVERIFY(found);
        QCOMPARE(loaded.boardVersion, saved.boardVersion);
    }

    void switchingBoardsPreservesEachAcknowledgedBase()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(QStringLiteral("sync-state.json"));
        meshcompare::CameraSyncStateStore store(path);

        meshcompare::CameraSyncPersistedState boardA;
        boardA.boardKey = QStringLiteral("board-a");
        boardA.boardVersion = 2;
        boardA.syncedAtUtc = QStringLiteral("2026-10-10T01:00:00.000Z");
        meshcompare::CameraPoseSyncView viewA;
        viewA.workspaceUuid = QStringLiteral("workspace-a");
        viewA.viewId = QStringLiteral("view-a");
        viewA.savedAtUtc = QStringLiteral("2026-10-10T00:00:00.000Z");
        viewA.poseXml = QStringLiteral("<camera-a/>");
        boardA.base.views.append(viewA);
        QVERIFY(store.save(boardA).ok);

        meshcompare::CameraSyncPersistedState boardB;
        boardB.boardKey = QStringLiteral("board-b");
        boardB.boardVersion = 8;
        boardB.syncedAtUtc = QStringLiteral("2026-10-10T02:00:00.000Z");
        meshcompare::CameraPoseSyncView viewB = viewA;
        viewB.workspaceUuid = QStringLiteral("workspace-b");
        viewB.viewId = QStringLiteral("view-b");
        viewB.poseXml = QStringLiteral("<camera-b/>");
        boardB.base.views.append(viewB);
        QVERIFY(store.save(boardB).ok);

        boardA.boardVersion = 3;
        boardA.syncedAtUtc = QStringLiteral("2026-10-10T03:00:00.000Z");
        boardA.base.views.front().note = QStringLiteral("updated a");
        QVERIFY(store.save(boardA).ok);

        meshcompare::CameraSyncPersistedState loaded;
        bool found = false;
        QVERIFY(store.load(QStringLiteral("board-b"), &loaded, &found).ok);
        QVERIFY(found);
        QCOMPARE(loaded.boardVersion, qint64(8));
        QCOMPARE(loaded.base.views.front().viewId, QStringLiteral("view-b"));

        QVERIFY(store.load(QStringLiteral("board-a"), &loaded, &found).ok);
        QVERIFY(found);
        QCOMPARE(loaded.boardVersion, qint64(3));
        QCOMPARE(loaded.base.views.front().note, QStringLiteral("updated a"));

        QFile input(path);
        QVERIFY(input.open(QIODevice::ReadOnly));
        const QJsonObject root = QJsonDocument::fromJson(input.readAll()).object();
        QCOMPARE(root.value(QStringLiteral("schema")).toInt(), 2);
        QCOMPARE(root.value(QStringLiteral("boards")).toObject().size(), 2);
    }

    void boundsBoardHistoryAndKeepsTheCurrentBoard()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(QStringLiteral("sync-state.json"));
        meshcompare::CameraSyncStateStore store(path);
        for (int index = 0; index < 33; ++index) {
            meshcompare::CameraSyncPersistedState state;
            state.boardKey = QStringLiteral("board-%1").arg(index, 2, 10, QLatin1Char('0'));
            state.boardVersion = 1;
            state.syncedAtUtc = QStringLiteral("2026-10-%1T00:00:00.000Z")
                .arg(index + 1, 2, 10, QLatin1Char('0'));
            QVERIFY(store.save(state).ok);
        }

        QFile input(path);
        QVERIFY(input.open(QIODevice::ReadOnly));
        const QJsonObject boards = QJsonDocument::fromJson(input.readAll())
            .object().value(QStringLiteral("boards")).toObject();
        QCOMPARE(boards.size(), 32);
        QVERIFY(!boards.contains(QStringLiteral("board-00")));
        QVERIFY(boards.contains(QStringLiteral("board-32")));

        meshcompare::CameraSyncPersistedState loaded;
        bool found = false;
        QVERIFY(store.load(QStringLiteral("board-32"), &loaded, &found).ok);
        QVERIFY(found);
    }

    void legacyStateLoadsAndMigratesWithoutLosingItsBoard()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(QStringLiteral("sync-state.json"));
        QFile output(path);
        QVERIFY(output.open(QIODevice::WriteOnly));
        const QByteArray legacy = QByteArrayLiteral(
            "{\"schema\":1,\"boardKey\":\"legacy-board\",\"boardVersion\":4,"
            "\"syncedAtUtc\":\"2026-10-09T00:00:00.000Z\","
            "\"base\":{\"app\":\"meshcompare\",\"schema\":1,\"views\":[]}}");
        QCOMPARE(output.write(legacy), qint64(legacy.size()));
        output.close();

        meshcompare::CameraSyncStateStore store(path);
        meshcompare::CameraSyncPersistedState loaded;
        bool found = false;
        QVERIFY(store.load(QStringLiteral("legacy-board"), &loaded, &found).ok);
        QVERIFY(found);
        QCOMPARE(loaded.boardVersion, qint64(4));

        meshcompare::CameraSyncPersistedState added;
        added.boardKey = QStringLiteral("new-board");
        added.boardVersion = 1;
        added.syncedAtUtc = QStringLiteral("2026-10-10T00:00:00.000Z");
        QVERIFY(store.save(added).ok);

        QFile input(path);
        QVERIFY(input.open(QIODevice::ReadOnly));
        const QJsonObject root = QJsonDocument::fromJson(input.readAll()).object();
        QCOMPARE(root.value(QStringLiteral("schema")).toInt(), 2);
        const QJsonObject boards = root.value(QStringLiteral("boards")).toObject();
        QCOMPARE(boards.size(), 2);
        QVERIFY(boards.contains(QStringLiteral("legacy-board")));
        QVERIFY(boards.contains(QStringLiteral("new-board")));

        QVERIFY(store.load(QStringLiteral("legacy-board"), &loaded, &found).ok);
        QVERIFY(found);
        QCOMPARE(loaded.boardVersion, qint64(4));
        QVERIFY(store.load(QStringLiteral("new-board"), &loaded, &found).ok);
        QVERIFY(found);
        QCOMPARE(loaded.boardVersion, qint64(1));
    }

    void rejectsFractionalBoardVersions()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(QStringLiteral("sync-state.json"));
        QFile output(path);
        QVERIFY(output.open(QIODevice::WriteOnly));
        const QByteArray state = QByteArrayLiteral(
            "{\"schema\":1,\"boardKey\":\"board-a\",\"boardVersion\":1.5,"
            "\"base\":{\"app\":\"meshcompare\",\"schema\":1,\"views\":[]}}");
        QCOMPARE(output.write(state), qint64(state.size()));
        output.close();

        meshcompare::CameraSyncStateStore store(path);
        meshcompare::CameraSyncPersistedState loaded;
        bool found = true;
        const OperationResult result = store.load(
            QStringLiteral("board-a"), &loaded, &found);
        QVERIFY(!result.ok);
        QVERIFY(!found);
    }

    void malformedStateFailsWithoutReturningStaleData()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(QStringLiteral("sync-state.json"));
        QFile output(path);
        QVERIFY(output.open(QIODevice::WriteOnly));
        QVERIFY(output.write("{broken") > 0);
        output.close();
        meshcompare::CameraSyncStateStore store(path);
        meshcompare::CameraSyncPersistedState loaded;
        loaded.boardKey = QStringLiteral("stale");
        bool found = true;
        const OperationResult result =
            store.load(QStringLiteral("board"), &loaded, &found);
        QVERIFY(!result.ok);
        QVERIFY(!found);
        QVERIFY(loaded.boardKey.isEmpty());
    }
};

QTEST_APPLESS_MAIN(CameraSyncStateTest)
#include "camera_sync_state_test.moc"
