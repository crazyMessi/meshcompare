#pragma once

#include <QSize>
#include <QString>

#include "core/meshcompare_types.h"

struct GridRenderRequest
{
    QString projectPath;
    QString outputDirectory;
    QSize outputSize = QSize(2048, 1152);
};

OperationResult renderComparisonGrid(
    const GridRenderRequest& request,
    QString* outputPath = nullptr);
