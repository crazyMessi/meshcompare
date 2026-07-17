#include "camera_pose_uuid.h"

#include <QRegularExpression>

namespace {

const QRegularExpression ExactUuidPattern(QStringLiteral("^[0-9a-f]{32}$"));
const QRegularExpression EmbeddedUuidPattern(QStringLiteral(
	"(?<![0-9A-Fa-f])([0-9A-Fa-f]{8}(?:-[0-9A-Fa-f]{4}){3}-"
	"[0-9A-Fa-f]{12}|[0-9A-Fa-f]{32})(?![0-9A-Fa-f])"));

}

namespace meshcompare {

QString normalizeCameraPoseUuid(const QString& value)
{
	QString normalized = value.trimmed();
	if (normalized.startsWith(QLatin1Char('{'))
		&& normalized.endsWith(QLatin1Char('}'))) {
		normalized = normalized.mid(1, normalized.size() - 2);
	}
	normalized.remove(QLatin1Char('-'));
	normalized = normalized.toLower();
	return ExactUuidPattern.match(normalized).hasMatch() ? normalized : QString();
}

QStringList extractCameraPoseUuidCandidates(const QStringList& values)
{
	QStringList candidates;
	for (const QString& value : values) {
		QRegularExpressionMatchIterator matches = EmbeddedUuidPattern.globalMatch(value);
		while (matches.hasNext()) {
			const QString candidate = normalizeCameraPoseUuid(matches.next().captured(1));
			if (!candidate.isEmpty() && !candidates.contains(candidate))
				candidates.push_back(candidate);
		}
	}
	return candidates;
}

}
