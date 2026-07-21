#pragma once

#include <QImage>
#include <QSize>
#include <QString>

#include "app/grid_render_size.h"
#include "core/meshcompare_types.h"

struct GridRenderRequest
{
    QString projectPath;
    QString outputDirectory;
    QSize outputSize = defaultGridRenderSize();
};

QImage normalizeGridRenderImage(QImage image, const QSize& outputSize);

OperationResult renderComparisonGrid(
    const GridRenderRequest& request,
    QString* outputPath = nullptr);
