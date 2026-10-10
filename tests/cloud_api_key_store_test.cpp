#include "services/cloud_api_key_store.h"

#include <QTest>
#include <QUuid>

class CloudApiKeyStoreTest final : public QObject
{
	Q_OBJECT

private slots:
	void usesApplicationSpecificDefaultService()
	{
		QCOMPARE(
			CloudApiKeyStore::defaultService(),
			QStringLiteral("org.vcg.meshcompare.cloud-board"));
	}

	void rejectsEmptyKeys()
	{
		CloudApiKeyStore store(
			QStringLiteral("org.vcg.meshcompare.cloud-board.test.empty." ) +
			QUuid::createUuid().toString(QUuid::WithoutBraces));
		const OperationResult result = store.save(QStringLiteral("   "));
		QVERIFY(!result.ok);
	}

	void storesUpdatesAndClearsExplicitly()
	{
#ifdef Q_OS_MACOS
		const QString service =
			QStringLiteral("org.vcg.meshcompare.cloud-board.test.") +
			QUuid::createUuid().toString(QUuid::WithoutBraces);
		CloudApiKeyStore store(service);
		struct Cleanup {
			CloudApiKeyStore* store = nullptr;
			~Cleanup() { store->clear(); }
		} cleanup{&store};

		QVERIFY(store.clear().ok);
		CloudApiKeyLoadResult loaded = store.load();
		QVERIFY(loaded.ok);
		QVERIFY(!loaded.found);
		QVERIFY(loaded.apiKey.isEmpty());

		QVERIFY(store.save(QStringLiteral("test-only-key-one")).ok);
		loaded = store.load();
		QVERIFY(loaded.ok);
		QVERIFY(loaded.found);
		QCOMPARE(loaded.apiKey, QStringLiteral("test-only-key-one"));

		QVERIFY(store.save(QStringLiteral("test-only-key-two")).ok);
		loaded = store.load();
		QVERIFY(loaded.ok);
		QVERIFY(loaded.found);
		QCOMPARE(loaded.apiKey, QStringLiteral("test-only-key-two"));

		QVERIFY(store.clear().ok);
		loaded = store.load();
		QVERIFY(loaded.ok);
		QVERIFY(!loaded.found);
		QVERIFY(loaded.apiKey.isEmpty());
#else
		QSKIP("macOS Keychain is required for this test.");
#endif
	}
};

QTEST_GUILESS_MAIN(CloudApiKeyStoreTest)

#include "cloud_api_key_store_test.moc"
