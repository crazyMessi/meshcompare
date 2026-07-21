#pragma once

#include <QString>
#include <QStringList>

struct StartupCommandLine
{
    bool ok = true;
    bool showHelp = false;
    bool startInComparisonGrid = false;
    QStringList inputPaths;
    QString error;
};

StartupCommandLine parseStartupCommandLine(const QStringList& arguments);
QString startupCommandLineUsage();
