#pragma once

#include <QImage>
#include <QSize>
#include <QString>

#include "app/grid_render_camera.h"
#include "app/grid_render_size.h"
#include "core/meshcompare_types.h"

struct GridRenderRequest
{
    QString projectPath;
    QString outputDirectory;
    QSize outputSize = defaultGridRenderSize();
    GridRenderCamera camera;
    QString initialCameraViewStateXml;
};

QImage normalizeGridRenderImage(QImage image, const QSize& outputSize);
QSize gridRenderHostSizeForPixelOutput(
    const QSize& outputSize,
    qreal devicePixelRatio);

OperationResult renderComparisonGrid(
    const GridRenderRequest& request,
    QString* outputPath = nullptr);
