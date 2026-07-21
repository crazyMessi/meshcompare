#include "startup_command_line.h"

#include <QFileInfo>

#include "app/grid_render_size.h"

namespace
{
bool isMeshLabProject(const QString& path)
{
    return QFileInfo(path).suffix().compare(
               QStringLiteral("mlp"), Qt::CaseInsensitive) == 0;
}

bool parseOutputSize(const QString& value, QSize* outputSize)
{
    const QStringList dimensions = value.toLower().split(
        QLatin1Char('x'), Qt::KeepEmptyParts);
    if (dimensions.size() != 2)
        return false;

    bool widthOk = false;
    bool heightOk = false;
    const int width = dimensions.at(0).toInt(&widthOk);
    const int height = dimensions.at(1).toInt(&heightOk);
    if (!widthOk || !heightOk || width <= 0 || height <= 0)
        return false;

    const QSize parsedSize(width, height);
    if (!isSupportedGridRenderSize(parsedSize))
        return false;

    *outputSize = parsedSize;
    return true;
}
} // namespace

StartupCommandLine parseStartupCommandLine(const QStringList& arguments)
{
    StartupCommandLine command;
    bool optionsEnded = false;
    bool invalidOutputSize = false;
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
        if (!optionsEnded && argument == QStringLiteral("--size")) {
            if (index + 1 >= arguments.size() ||
                !parseOutputSize(arguments.at(++index), &command.outputSize)) {
                invalidOutputSize = true;
            }
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
        command.outputSize = QSize();
        return command;
    }

    if (invalidOutputSize) {
        command.ok = false;
        command.error =
            QStringLiteral(
                "--size must use positive WIDTHxHEIGHT dimensions, at most 16384 per side and 67108864 pixels.");
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
    else if (!command.renderComparisonGrid &&
             (!command.outputDirectory.isEmpty() || command.outputSize.isValid())) {
        command.ok = false;
        command.error = QStringLiteral("--output-dir and --size require --render-grid.");
    }
    return command;
}

QString startupCommandLineUsage()
{
    return QStringLiteral(
        "Usage:\n"
        "  meshcompare [mesh ...]\n"
        "  meshcompare --render-grid comparison.mlp --output-dir output [--size WIDTHxHEIGHT]\n"
        "\n"
        "--render-grid  Render one MeshLab project to a PNG without opening a window.\n"
        "--output-dir   Directory that receives <project>.grid.png.\n"
        "--size         Output PNG WIDTHxHEIGHT; defaults to 2048x1152 (max 16384 per side, 64 MP).\n"
        "--help         Show this help text.\n");
}
