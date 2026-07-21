#pragma once

#include <QSize>
#include <QString>
#include <QStringList>

#include "app/grid_render_camera.h"

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
    QString error;
};

StartupCommandLine parseStartupCommandLine(const QStringList& arguments);
QString startupCommandLineUsage();
