#pragma once

#include <QStringList>

namespace meshcompare {

inline QStringList normalizeCameraPoseTags(const QStringList& tags)
{
    QStringList normalized;
    for (const QString& rawTag : tags) {
        const QString tag = rawTag.trimmed();
        if (!tag.isEmpty() && !normalized.contains(tag))
            normalized.append(tag);
    }
    return normalized;
}

} // namespace meshcompare
