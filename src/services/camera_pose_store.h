#pragma once

#include "core/meshcompare_types.h"

#include <QString>
#include <QVector>

struct SavedCameraPose {
	QString uuid;
	QString viewId;
	QString savedAtUtc;
	CameraPose pose;
};

class ICameraPoseStore
{
public:
	virtual ~ICameraPoseStore() = default;

	virtual OperationResult migrateLegacyIfNeeded() = 0;
	virtual OperationResult save(
		const QString& uuid,
		const CameraPose& pose,
		QString* viewId = nullptr) = 0;
	virtual QVector<SavedCameraPose> list(
		const QString& uuid,
		OperationResult* result = nullptr) const = 0;
	virtual OperationResult load(
		const QString& uuid,
		const QString& viewId,
		CameraPose* pose) const = 0;
	virtual OperationResult remove(const QString& uuid, const QString& viewId) = 0;
};

class CameraPoseStore final : public ICameraPoseStore
{
public:
	CameraPoseStore(QString storagePath, QString legacyPath);

	OperationResult migrateLegacyIfNeeded() override;
	OperationResult save(
		const QString& uuid,
		const CameraPose& pose,
		QString* viewId = nullptr) override;
	QVector<SavedCameraPose> list(
		const QString& uuid,
		OperationResult* result = nullptr) const override;
	OperationResult load(
		const QString& uuid,
		const QString& viewId,
		CameraPose* pose) const override;
	OperationResult remove(const QString& uuid, const QString& viewId) override;

	QVector<SavedCameraPose> listAll(OperationResult* result = nullptr) const;

	static QString normalizeUuid(const QString& value);

private:
	QString storagePath_;
	QString legacyPath_;
};
