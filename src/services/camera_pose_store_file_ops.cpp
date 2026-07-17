#include "camera_pose_store_file_ops.h"

#include <QFile>
#include <QtGlobal>

#ifdef Q_OS_WIN
#include <qt_windows.h>
#else
#include <cerrno>
#include <unistd.h>
#endif

namespace camera_pose_store_detail {

PublishResult publishFileNoReplace(
	const QString& stagedPath,
	const QString& destinationPath,
	QString* error)
{
#ifdef Q_OS_WIN
	if (::CreateHardLinkW(
			reinterpret_cast<LPCWSTR>(destinationPath.utf16()),
			reinterpret_cast<LPCWSTR>(stagedPath.utf16()),
			nullptr)) {
		return PublishResult::Published;
	}
	const int nativeError = static_cast<int>(::GetLastError());
	if (nativeError == ERROR_FILE_EXISTS || nativeError == ERROR_ALREADY_EXISTS)
		return PublishResult::DestinationExists;
#else
	const QByteArray encodedStagedPath = QFile::encodeName(stagedPath);
	const QByteArray encodedDestinationPath = QFile::encodeName(destinationPath);
	if (::link(encodedStagedPath.constData(), encodedDestinationPath.constData()) == 0)
		return PublishResult::Published;
	const int nativeError = errno;
	if (nativeError == EEXIST)
		return PublishResult::DestinationExists;
#endif

	if (error != nullptr) {
		*error = QStringLiteral("Unable to publish the migrated camera-pose library: %1")
				 .arg(qt_error_string(nativeError));
	}
	return PublishResult::Failed;
}

}
