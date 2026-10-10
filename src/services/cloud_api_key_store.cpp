#include "services/cloud_api_key_store.h"

#include <QByteArray>
#include <QtGlobal>

#include <utility>

#ifdef Q_OS_MACOS
#include <Security/Security.h>
#endif

namespace {

#ifdef Q_OS_MACOS

CFStringRef makeString(const QString& value)
{
	const QByteArray utf8 = value.toUtf8();
	return CFStringCreateWithBytes(
		kCFAllocatorDefault,
		reinterpret_cast<const UInt8*>(utf8.constData()),
		utf8.size(),
		kCFStringEncodingUTF8,
		false);
}

QString fromString(CFStringRef value)
{
	if (!value)
		return {};
	const CFIndex length = CFStringGetLength(value);
	const CFIndex maximum = CFStringGetMaximumSizeForEncoding(
		length, kCFStringEncodingUTF8) + 1;
	QByteArray utf8(static_cast<int>(maximum), '\0');
	if (!CFStringGetCString(
			value, utf8.data(), maximum, kCFStringEncodingUTF8)) {
		return {};
	}
	return QString::fromUtf8(utf8.constData());
}

QString statusDescription(OSStatus status)
{
	CFStringRef description = SecCopyErrorMessageString(status, nullptr);
	const QString text = fromString(description);
	if (description)
		CFRelease(description);
	if (text.isEmpty())
		return QStringLiteral("Keychain error %1.").arg(status);
	return QStringLiteral("Keychain error %1: %2").arg(status).arg(text);
}

CFMutableDictionaryRef makeQuery(
	const QString& service,
	const QString& account)
{
	CFMutableDictionaryRef query = CFDictionaryCreateMutable(
		kCFAllocatorDefault,
		0,
		&kCFTypeDictionaryKeyCallBacks,
		&kCFTypeDictionaryValueCallBacks);
	CFStringRef serviceString = makeString(service);
	CFStringRef accountString = makeString(account);
	CFDictionarySetValue(query, kSecClass, kSecClassGenericPassword);
	CFDictionarySetValue(query, kSecAttrService, serviceString);
	CFDictionarySetValue(query, kSecAttrAccount, accountString);
	CFRelease(serviceString);
	CFRelease(accountString);
	return query;
}

CFDataRef makeData(const QString& value)
{
	const QByteArray utf8 = value.toUtf8();
	return CFDataCreate(
		kCFAllocatorDefault,
		reinterpret_cast<const UInt8*>(utf8.constData()),
		utf8.size());
}

#endif

} // namespace

CloudApiKeyStore::CloudApiKeyStore(QString service, QString account)
	: service_(std::move(service)), account_(std::move(account))
{
}

QString CloudApiKeyStore::defaultService()
{
	return QStringLiteral("org.vcg.meshcompare.cloud-board");
}

CloudApiKeyLoadResult CloudApiKeyStore::load() const
{
	CloudApiKeyLoadResult result;
#ifdef Q_OS_MACOS
	CFMutableDictionaryRef query = makeQuery(service_, account_);
	CFDictionarySetValue(query, kSecReturnData, kCFBooleanTrue);
	CFDictionarySetValue(query, kSecMatchLimit, kSecMatchLimitOne);
	CFTypeRef value = nullptr;
	const OSStatus status = SecItemCopyMatching(query, &value);
	CFRelease(query);

	if (status == errSecItemNotFound)
		return result;
	if (status != errSecSuccess) {
		result.ok = false;
		result.error = statusDescription(status);
		return result;
	}
	if (!value || CFGetTypeID(value) != CFDataGetTypeID()) {
		if (value)
			CFRelease(value);
		result.ok = false;
		result.error = QStringLiteral("Keychain returned an invalid API key value.");
		return result;
	}

	CFDataRef data = static_cast<CFDataRef>(value);
	result.apiKey = QString::fromUtf8(
		reinterpret_cast<const char*>(CFDataGetBytePtr(data)),
		static_cast<int>(CFDataGetLength(data)));
	result.found = true;
	CFRelease(value);
	return result;
#else
	result.ok = false;
	result.error = QStringLiteral(
		"Secure Cloud Board credential storage is unavailable on this platform.");
	return result;
#endif
}

OperationResult CloudApiKeyStore::save(const QString& apiKey)
{
	if (apiKey.trimmed().isEmpty()) {
		return OperationResult::failure(
			QStringLiteral("The Cloud Board API key cannot be empty."));
	}
#ifdef Q_OS_MACOS
	CFMutableDictionaryRef query = makeQuery(service_, account_);
	CFMutableDictionaryRef updates = CFDictionaryCreateMutable(
		kCFAllocatorDefault,
		0,
		&kCFTypeDictionaryKeyCallBacks,
		&kCFTypeDictionaryValueCallBacks);
	CFDataRef data = makeData(apiKey.trimmed());
	CFDictionarySetValue(updates, kSecValueData, data);
	OSStatus status = SecItemUpdate(query, updates);
	CFRelease(updates);

	if (status == errSecItemNotFound) {
		CFDictionarySetValue(query, kSecValueData, data);
		CFDictionarySetValue(
			query, kSecAttrAccessible, kSecAttrAccessibleWhenUnlocked);
		status = SecItemAdd(query, nullptr);
		if (status == errSecDuplicateItem) {
			CFDictionaryRemoveValue(query, kSecAttrAccessible);
			CFMutableDictionaryRef retry = CFDictionaryCreateMutable(
				kCFAllocatorDefault,
				0,
				&kCFTypeDictionaryKeyCallBacks,
				&kCFTypeDictionaryValueCallBacks);
			CFDictionarySetValue(retry, kSecValueData, data);
			CFDictionaryRemoveValue(query, kSecValueData);
			status = SecItemUpdate(query, retry);
			CFRelease(retry);
		}
	}
	CFRelease(data);
	CFRelease(query);
	if (status != errSecSuccess)
		return OperationResult::failure(statusDescription(status));
	return OperationResult::success();
#else
	Q_UNUSED(apiKey)
	return OperationResult::failure(QStringLiteral(
		"Secure Cloud Board credential storage is unavailable on this platform."));
#endif
}

OperationResult CloudApiKeyStore::clear()
{
#ifdef Q_OS_MACOS
	CFMutableDictionaryRef query = makeQuery(service_, account_);
	const OSStatus status = SecItemDelete(query);
	CFRelease(query);
	if (status != errSecSuccess && status != errSecItemNotFound)
		return OperationResult::failure(statusDescription(status));
	return OperationResult::success();
#else
	return OperationResult::failure(QStringLiteral(
		"Secure Cloud Board credential storage is unavailable on this platform."));
#endif
}
