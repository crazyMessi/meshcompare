#pragma once

#include "camera_pose_store.h"

#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QVector>

namespace meshcompare {

struct CameraPoseSyncScreenshot
{
	QString sha256;
	QString fileId;
	QString mimeType;
	qint64 size = 0;

	// Runtime-only. This is used by the upload step and is never serialized.
	QString sourcePath;
};

struct CameraPoseSyncView
{
	QString workspaceUuid;
	QString viewId;
	QString savedAtUtc;
	QStringList tags;
	QString note;
	QString poseXml;
	CameraPoseSyncScreenshot screenshot;
};

struct CameraPoseSyncDocument
{
	// Identifies one lifetime of a remote board. Locally built snapshots leave
	// this empty until the sync coordinator adopts or creates a generation.
	QString generationId;
	QVector<CameraPoseSyncView> views;
};

enum class CameraPoseSyncConflictKind
{
	ConcurrentAddition,
	ConcurrentEdit,
	DeleteVsEdit,
};

struct CameraPoseSyncConflict
{
	CameraPoseSyncConflictKind kind =
		CameraPoseSyncConflictKind::ConcurrentEdit;
	QString workspaceUuid;
	QString originalViewId;
	QString conflictViewId;
	QString description;
};

struct CameraPoseSyncMergeResult
{
	CameraPoseSyncDocument document;
	QVector<CameraPoseSyncConflict> conflicts;
};

// The identity is stable and unambiguous for validated views.
QString cameraPoseSyncIdentity(const CameraPoseSyncView& view);

// Content equality excludes transport-only screenshot fields (fileId and
// sourcePath). The digest uses the same definition and is lowercase SHA-256.
bool cameraPoseSyncContentEqual(
	const CameraPoseSyncView& left,
	const CameraPoseSyncView& right);
QString cameraPoseSyncContentDigest(const CameraPoseSyncView& view);

OperationResult buildCameraPoseSyncDocument(
	const QVector<SavedCameraPose>& poses,
	CameraPoseSyncDocument* destination);
OperationResult validateCameraPoseSyncDocument(
	const CameraPoseSyncDocument& document);
OperationResult serializeCameraPoseSyncDocument(
	const CameraPoseSyncDocument& document,
	QByteArray* destination);
OperationResult parseCameraPoseSyncDocument(
	const QByteArray& bytes,
	CameraPoseSyncDocument* destination);

// A null base performs the additive first sync. Supplying the snapshot saved
// after the previous sync enables deletion propagation and three-way conflict
// detection.
OperationResult mergeCameraPoseSyncDocuments(
	const CameraPoseSyncDocument* base,
	const CameraPoseSyncDocument& local,
	const CameraPoseSyncDocument& remote,
	CameraPoseSyncMergeResult* destination);

} // namespace meshcompare
