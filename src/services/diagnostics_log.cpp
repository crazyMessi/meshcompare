#include "diagnostics_log.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <utility>

constexpr qint64 DiagnosticsLog::RotationLimitBytes;

namespace
{
constexpr auto LogFileName = "meshcompare-diagnostics.jsonl";
constexpr auto RotatedLogFileName = "meshcompare-diagnostics.previous.jsonl";

QJsonObject outcomeFields(bool succeeded, const QString& error)
{
    QJsonObject fields;
    fields.insert(
        QStringLiteral("outcome"),
        succeeded ? QStringLiteral("success") : QStringLiteral("failure"));
    if (!error.isEmpty())
        fields.insert(QStringLiteral("error"), error);
    return fields;
}
} // namespace

DiagnosticsLog::DiagnosticsLog(
    QString applicationDataRoot,
    qint64 rotationLimitBytes)
    : applicationDataRoot_(std::move(applicationDataRoot)),
      rotationLimitBytes_(qMax<qint64>(1, rotationLimitBytes))
{
}

QString DiagnosticsLog::filePath() const
{
    if (applicationDataRoot_.isEmpty())
        return {};
    return QDir(applicationDataRoot_).filePath(QString::fromLatin1(LogFileName));
}

QString DiagnosticsLog::rotatedFilePath() const
{
    if (applicationDataRoot_.isEmpty())
        return {};
    return QDir(applicationDataRoot_).filePath(
        QString::fromLatin1(RotatedLogFileName));
}

OperationResult DiagnosticsLog::recordSessionStart()
{
    return appendEvent(QStringLiteral("session_start"));
}

OperationResult DiagnosticsLog::recordSessionEnd()
{
    return appendEvent(QStringLiteral("session_end"));
}

OperationResult DiagnosticsLog::recordImportOutcome(
    bool succeeded,
    int meshCount,
    const QStringList& displayNames,
    const QString& error)
{
    QJsonObject fields = outcomeFields(succeeded, error);
    fields.insert(QStringLiteral("mesh_count"), meshCount);
    QJsonArray names;
    for (const QString& displayName : displayNames)
        names.append(redactDisplayName(displayName));
    fields.insert(QStringLiteral("display_names"), names);
    return appendEvent(QStringLiteral("import_outcome"), fields);
}

OperationResult DiagnosticsLog::recordAnalysisOutcome(
    const QString& metric,
    bool succeeded,
    const QString& error)
{
    QJsonObject fields = outcomeFields(succeeded, error);
    fields.insert(QStringLiteral("metric"), metric);
    return appendEvent(QStringLiteral("analysis_outcome"), fields);
}

OperationResult DiagnosticsLog::recordCameraSave(
    bool succeeded,
    const QString& error)
{
    return appendEvent(
        QStringLiteral("camera_save"), outcomeFields(succeeded, error));
}

OperationResult DiagnosticsLog::recordCameraApply(
    bool succeeded,
    const QString& error)
{
    return appendEvent(
        QStringLiteral("camera_apply"), outcomeFields(succeeded, error));
}

OperationResult DiagnosticsLog::recordFatalRendererError(const QString& error)
{
    QJsonObject fields;
    fields.insert(QStringLiteral("error"), error);
    return appendEvent(QStringLiteral("fatal_renderer_error"), fields);
}

QString DiagnosticsLog::redactDisplayName(const QString& displayName)
{
    if (displayName.contains(QLatin1Char('/')) ||
        displayName.contains(QLatin1Char('\\'))) {
        return QStringLiteral("path_like_name_omitted");
    }
    return displayName;
}

OperationResult DiagnosticsLog::appendEvent(
    const QString& event,
    QJsonObject fields)
{
    const QString currentPath = filePath();
    if (currentPath.isEmpty()) {
        return OperationResult::failure(
            QStringLiteral("The diagnostics log directory is unavailable."));
    }
    if (!QDir().mkpath(applicationDataRoot_)) {
        return OperationResult::failure(
            QStringLiteral("Unable to create the diagnostics log directory."));
    }

    fields.insert(QStringLiteral("event"), event);
    fields.insert(
        QStringLiteral("timestamp_utc"),
        QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
    QByteArray line = QJsonDocument(fields).toJson(QJsonDocument::Compact);
    line.append('\n');

    const QFileInfo currentInfo(currentPath);
    if (currentInfo.exists() && currentInfo.size() > 0 &&
        currentInfo.size() + line.size() > rotationLimitBytes_) {
        const QString previousPath = rotatedFilePath();
        if (QFileInfo::exists(previousPath) && !QFile::remove(previousPath)) {
            return OperationResult::failure(
                QStringLiteral("Unable to replace the previous diagnostics log."));
        }
        if (!QFile::rename(currentPath, previousPath)) {
            return OperationResult::failure(
                QStringLiteral("Unable to rotate the diagnostics log."));
        }
    }

    QFile output(currentPath);
    if (!output.open(QIODevice::WriteOnly | QIODevice::Append))
        return OperationResult::failure(output.errorString());
    if (output.write(line) != line.size()) {
        const QString error = output.errorString();
        return OperationResult::failure(
            error.isEmpty() ? QStringLiteral("Unable to write the diagnostics log.")
                            : error);
    }
    if (!output.flush()) {
        const QString error = output.errorString();
        return OperationResult::failure(
            error.isEmpty() ? QStringLiteral("Unable to flush the diagnostics log.")
                            : error);
    }
    return OperationResult::success();
}
