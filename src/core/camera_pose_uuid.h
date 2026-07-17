#pragma once

#include <QString>
#include <QStringList>

namespace meshcompare {

QString normalizeCameraPoseUuid(const QString& value);
QStringList extractCameraPoseUuidCandidates(const QStringList& values);

}
