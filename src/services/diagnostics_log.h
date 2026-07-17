#pragma once

#include <QString>
#include <QStringList>
#include <QJsonObject>

#include "../core/meshcompare_types.h"

class DiagnosticsLog final
{
public:
    static constexpr qint64 RotationLimitBytes = 2 * 1024 * 1024;

    explicit DiagnosticsLog(
        QString applicationDataRoot,
        qint64 rotationLimitBytes = RotationLimitBytes);

    QString filePath() const;
    QString rotatedFilePath() const;

    OperationResult recordSessionStart();
    OperationResult recordSessionEnd();
    OperationResult recordImportOutcome(
        bool succeeded,
        int meshCount,
        const QStringList& displayNames,
        const QString& error = {});
    OperationResult recordAnalysisOutcome(
        const QString& metric,
        bool succeeded,
        const QString& error = {});
    OperationResult recordCameraSave(
        bool succeeded,
        const QString& error = {});
    OperationResult recordCameraApply(
        bool succeeded,
        const QString& error = {});
    OperationResult recordFatalRendererError(const QString& error);

    static QString redactDisplayName(const QString& displayName);

private:
    OperationResult appendEvent(
        const QString& event,
        QJsonObject fields = {});

    QString applicationDataRoot_;
    qint64 rotationLimitBytes_ = RotationLimitBytes;
};
