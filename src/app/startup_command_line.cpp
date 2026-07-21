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
        if (!optionsEnded && argument == QStringLiteral("--render-grid")) {
            command.renderComparisonGrid = true;
            if (index + 1 < arguments.size())
                command.inputPaths.append(arguments.at(++index));
            continue;
        }
        if (!optionsEnded && argument == QStringLiteral("--output-dir")) {
            if (index + 1 < arguments.size())
                command.outputDirectory = arguments.at(++index);
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
        command.renderComparisonGrid = false;
        command.inputPaths.clear();
        command.outputDirectory.clear();
        return command;
    }

    if (command.renderComparisonGrid &&
        (command.inputPaths.size() != 1 ||
         !isMeshLabProject(command.inputPaths.front()) ||
         command.outputDirectory.isEmpty())) {
        command.ok = false;
        command.error =
            QStringLiteral(
                "--render-grid requires exactly one MeshLab project (*.mlp) and --output-dir.");
    }
    else if (!command.renderComparisonGrid && !command.outputDirectory.isEmpty()) {
        command.ok = false;
        command.error = QStringLiteral("--output-dir requires --render-grid.");
    }
    return command;
}

QString startupCommandLineUsage()
{
    return QStringLiteral(
        "Usage:\n"
        "  meshcompare [mesh ...]\n"
        "  meshcompare --render-grid comparison.mlp --output-dir output\n"
        "\n"
        "--render-grid  Render one MeshLab project to a PNG without opening a window.\n"
        "--output-dir   Directory that receives <project>.grid.png.\n"
        "--help         Show this help text.\n");
}
