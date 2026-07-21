#pragma once

#include <QSize>
#include <QString>
#include <QStringList>

#include "app/grid_render_camera.h"
#include "app/grid_render_coloring.h"

struct StartupCommandLine
{
    bool ok = true;
    bool showHelp = false;
    bool renderComparisonGrid = false;
    QStringList inputPaths;
    QString outputDirectory;
    QSize outputSize;
    GridRenderCamera camera;
    QString cameraPoseUuid;
    QString cameraPoseViewId;
    GridRenderColoring coloring = GridRenderColoring::None;
    QString error;
};

StartupCommandLine parseStartupCommandLine(const QStringList& arguments);
QString startupCommandLineUsage();
