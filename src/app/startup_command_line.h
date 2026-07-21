#pragma once

#include <QString>
#include <QStringList>

struct StartupCommandLine
{
    bool ok = true;
    bool showHelp = false;
    bool renderComparisonGrid = false;
    QStringList inputPaths;
    QString outputDirectory;
    QString error;
};

StartupCommandLine parseStartupCommandLine(const QStringList& arguments);
QString startupCommandLineUsage();
