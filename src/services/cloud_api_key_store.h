#pragma once

#include "core/meshcompare_types.h"

#include <QString>

struct CloudApiKeyLoadResult {
	bool ok = true;
	bool found = false;
	QString apiKey;
	QString error;
};

class ICloudApiKeyStore
{
public:
	virtual ~ICloudApiKeyStore() = default;

	virtual CloudApiKeyLoadResult load() const = 0;
	virtual OperationResult save(const QString& apiKey) = 0;
	virtual OperationResult clear() = 0;
};

class CloudApiKeyStore final : public ICloudApiKeyStore
{
public:
	explicit CloudApiKeyStore(
		QString service = defaultService(),
		QString account = QStringLiteral("api-key"));

	CloudApiKeyLoadResult load() const override;
	OperationResult save(const QString& apiKey) override;
	OperationResult clear() override;

	static QString defaultService();

private:
	QString service_;
	QString account_;
};
