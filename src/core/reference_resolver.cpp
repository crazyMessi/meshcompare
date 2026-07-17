#include "reference_resolver.h"

ReferenceResolution resolveReference(const QVector<MeshEntry>& meshes)
{
    QVector<MeshId> matches;
    for (const MeshEntry& mesh : meshes) {
        if (mesh.sourcePath.contains(QStringLiteral("gt"), Qt::CaseInsensitive) ||
            mesh.displayName.contains(QStringLiteral("gt"), Qt::CaseInsensitive))
            matches.push_back(mesh.id);
    }

    if (matches.size() == 1)
        return {matches.front(), {}};
    if (matches.size() > 1)
        return {matches.front(),
                QStringLiteral("Multiple gt meshes found; using the first import.")};
    return meshes.isEmpty()
        ? ReferenceResolution{}
        : ReferenceResolution{meshes.front().id,
                              QStringLiteral("No gt mesh found; using the first import.")};
}
