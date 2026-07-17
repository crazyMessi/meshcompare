#pragma once

#include <QString>

namespace camera_pose_store_detail {

enum class PublishResult {
	Published,
	DestinationExists,
	Failed
};

PublishResult publishFileNoReplace(
	const QString& stagedPath,
	const QString& destinationPath,
	QString* error = nullptr);

}
