#pragma once

#include "core/meshcompare_types.h"

#include <QString>
#include <QStringList>
#include <QVector>

class QImage;

struct SavedCameraPose {
	QString uuid;
	QString viewId;
	QString savedAtUtc;
	QStringList tags;
	QString screenshotPath;
	CameraPose pose;
};

class ICameraPoseStore
{
public:
	virtual ~ICameraPoseStore() = default;

	// Creates the owned library from its legacy source when needed, then applies
	// compatible library upgrades such as default pose tags.
	virtual OperationResult migrateLegacyIfNeeded() = 0;
	virtual OperationResult saveWithScreenshot(
		const QString& uuid,
		const CameraPose& pose,
		const QImage& screenshot,
		QString* viewId = nullptr,
		const QStringList& tags = {},
		QString* screenshotPath = nullptr) = 0;
	virtual QVector<SavedCameraPose> list(
		const QString& uuid,
		OperationResult* result = nullptr) const = 0;
	virtual QVector<SavedCameraPose> listAll(
		OperationResult* result = nullptr) const = 0;
	virtual OperationResult load(
		const QString& uuid,
		const QString& viewId,
		CameraPose* pose) const = 0;
	virtual OperationResult setTags(
		const QString& uuid,
		const QString& viewId,
		const QStringList& tags) = 0;
	virtual OperationResult remove(const QString& uuid, const QString& viewId) = 0;
};

class CameraPoseStore final : public ICameraPoseStore
{
public:
	CameraPoseStore(QString storagePath, QString legacyPath);

	OperationResult migrateLegacyIfNeeded() override;
	OperationResult saveWithScreenshot(
		const QString& uuid,
		const CameraPose& pose,
		const QImage& screenshot,
		QString* viewId = nullptr,
		const QStringList& tags = {},
		QString* screenshotPath = nullptr) override;
	QVector<SavedCameraPose> list(
		const QString& uuid,
		OperationResult* result = nullptr) const override;
	OperationResult load(
		const QString& uuid,
		const QString& viewId,
		CameraPose* pose) const override;
	OperationResult setTags(
		const QString& uuid,
		const QString& viewId,
		const QStringList& tags) override;
	OperationResult remove(const QString& uuid, const QString& viewId) override;

	QVector<SavedCameraPose> listAll(
		OperationResult* result = nullptr) const override;

	static QString normalizeUuid(const QString& value);

private:
	QString storagePath_;
	QString legacyPath_;
};
