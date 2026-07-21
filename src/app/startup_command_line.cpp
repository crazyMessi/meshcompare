#include "startup_command_line.h"

#include <QFileInfo>

namespace
{
bool isMeshLabProject(const QString& path)
{
    return QFileInfo(path).suffix().compare(
               QStringLiteral("mlp"), Qt::CaseInsensitive) == 0;
}
} // namespace

StartupCommandLine parseStartupCommandLine(const QStringList& arguments)
{
    StartupCommandLine command;
    bool optionsEnded = false;
    for (int index = 1; index < arguments.size(); ++index) {
        const QString argument = arguments.at(index);
        if (!optionsEnded && argument == QStringLiteral("--")) {
            optionsEnded = true;
            continue;
        }
        if (!optionsEnded && argument == QStringLiteral("--grid")) {
            command.startInComparisonGrid = true;
            continue;
        }
        if (!optionsEnded &&
            (argument == QStringLiteral("--help") ||
             argument == QStringLiteral("-h"))) {
            command.showHelp = true;
            continue;
        }
        command.inputPaths.append(argument);
    }

    if (command.showHelp) {
        command.startInComparisonGrid = false;
        command.inputPaths.clear();
        return command;
    }

    if (command.startInComparisonGrid &&
        (command.inputPaths.size() != 1 ||
         !isMeshLabProject(command.inputPaths.front()))) {
        command.ok = false;
        command.error =
            QStringLiteral("--grid requires exactly one MeshLab project (*.mlp).");
    }
    return command;
}

QString startupCommandLineUsage()
{
    return QStringLiteral(
        "Usage:\n"
        "  meshcompare [mesh ...]\n"
        "  meshcompare --grid comparison.mlp\n"
        "\n"
        "--grid  Open one MeshLab project in the linked comparison grid.\n"
        "--help   Show this help text.\n");
}
