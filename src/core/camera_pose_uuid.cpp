#include "camera_pose_uuid.h"

#include <QFileInfo>
#include <QRegularExpression>

namespace {

const QRegularExpression ExactUuidPattern(QStringLiteral("^[0-9a-f]{32}$"));
const QRegularExpression EmbeddedUuidPattern(QStringLiteral(
	"(?<![0-9A-Fa-f])([0-9A-Fa-f]{8}(?:-[0-9A-Fa-f]{4}){3}-"
	"[0-9A-Fa-f]{12}|[0-9A-Fa-f]{32})(?![0-9A-Fa-f])"));
const QRegularExpression NormalizedMeshUidPattern(
	QStringLiteral("^(.+)_(?:gt|hy|s2)_norm$"),
	QRegularExpression::CaseInsensitiveOption);

bool containsUnsupportedCharacter(const QString& value)
{
	for (const QChar character : value) {
		if (character.isNull() || character.category() == QChar::Other_Control)
			return true;
	}
	return false;
}

}

namespace meshcompare {

QString normalizeCameraPoseUuid(const QString& value)
{
	QString normalized = value.trimmed();
	if (normalized.isEmpty() || normalized.size() > 255
		|| containsUnsupportedCharacter(normalized)) {
		return {};
	}

	const bool startsWithBrace = normalized.startsWith(QLatin1Char('{'));
	const bool endsWithBrace = normalized.endsWith(QLatin1Char('}'));
	if (startsWithBrace != endsWithBrace)
		return {};

	QString uuidCandidate = normalized;
	if (startsWithBrace)
		uuidCandidate = uuidCandidate.mid(1, uuidCandidate.size() - 2);
	uuidCandidate.remove(QLatin1Char('-'));
	uuidCandidate = uuidCandidate.toLower();
	if (ExactUuidPattern.match(uuidCandidate).hasMatch())
		return uuidCandidate;

	// Camera-pose collections are workspace-scoped identifiers rather than
	// protocol UUIDs. Preserve standard UUID canonicalization above, while
	// accepting short, long, numeric, and filename-derived UIDs here.
	return normalized.toCaseFolded();
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

		const QString stem = QFileInfo(value).completeBaseName();
		const QRegularExpressionMatch normalizedMesh =
			NormalizedMeshUidPattern.match(stem);
		if (!normalizedMesh.hasMatch())
			continue;
		const QString candidate =
			normalizeCameraPoseUuid(normalizedMesh.captured(1));
		if (!candidate.isEmpty() && !candidates.contains(candidate))
			candidates.push_back(candidate);
	}
	return candidates;
}

}
