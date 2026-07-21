#include "startup_command_line.h"

#include <cmath>

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

bool parseVector3(const QString& value, GridRenderVector3* vector)
{
    const QStringList values = value.split(QLatin1Char(','), Qt::KeepEmptyParts);
    if (values.size() != 3)
        return false;

    bool xOk = false;
    bool yOk = false;
    bool zOk = false;
    const double x = values.at(0).toDouble(&xOk);
    const double y = values.at(1).toDouble(&yOk);
    const double z = values.at(2).toDouble(&zOk);
    if (!xOk || !yOk || !zOk || !std::isfinite(x) || !std::isfinite(y) ||
        !std::isfinite(z)) {
        return false;
    }

    *vector = {x, y, z};
    return true;
}

bool parseFieldOfView(const QString& value, double* fieldOfViewDegrees)
{
    bool valid = false;
    const double parsed = value.toDouble(&valid);
    if (!valid || !std::isfinite(parsed))
        return false;
    *fieldOfViewDegrees = parsed;
    return true;
}

bool parseCameraPoseReference(
    const QString& value,
    QString* uuid,
    QString* viewId)
{
    const int separator = value.indexOf(QLatin1Char(':'));
    if (separator <= 0 || separator >= value.size() - 1)
        return false;

    const QString parsedUuid = value.left(separator).trimmed();
    const QString parsedViewId = value.mid(separator + 1).trimmed();
    if (parsedUuid.isEmpty() || parsedViewId.isEmpty())
        return false;
    *uuid = parsedUuid;
    *viewId = parsedViewId;
    return true;
}
} // namespace

StartupCommandLine parseStartupCommandLine(const QStringList& arguments)
{
    StartupCommandLine command;
    bool optionsEnded = false;
    bool invalidOutputSize = false;
    bool cameraSpecified = false;
    bool lookAtSpecified = false;
    bool customUpSpecified = false;
    bool customFieldOfViewSpecified = false;
    bool cameraPoseSpecified = false;
    bool invalidCameraCoordinates = false;
    bool invalidFieldOfView = false;
    bool invalidCameraPoseReference = false;
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
        if (!optionsEnded && argument == QStringLiteral("--camera")) {
            cameraSpecified = true;
            command.camera.enabled = true;
            if (index + 1 >= arguments.size() ||
                !parseVector3(arguments.at(++index), &command.camera.position)) {
                invalidCameraCoordinates = true;
            }
            continue;
        }
        if (!optionsEnded && argument == QStringLiteral("--look-at")) {
            lookAtSpecified = true;
            if (index + 1 >= arguments.size() ||
                !parseVector3(arguments.at(++index), &command.camera.target)) {
                invalidCameraCoordinates = true;
            }
            continue;
        }
        if (!optionsEnded && argument == QStringLiteral("--up")) {
            customUpSpecified = true;
            if (index + 1 >= arguments.size() ||
                !parseVector3(arguments.at(++index), &command.camera.up)) {
                invalidCameraCoordinates = true;
            }
            continue;
        }
        if (!optionsEnded && argument == QStringLiteral("--fov")) {
            customFieldOfViewSpecified = true;
            if (index + 1 >= arguments.size() ||
                !parseFieldOfView(
                    arguments.at(++index), &command.camera.fieldOfViewDegrees)) {
                invalidFieldOfView = true;
            }
            continue;
        }
        if (!optionsEnded && argument == QStringLiteral("--camera-pose")) {
            cameraPoseSpecified = true;
            if (index + 1 >= arguments.size() ||
                !parseCameraPoseReference(
                    arguments.at(++index),
                    &command.cameraPoseUuid,
                    &command.cameraPoseViewId)) {
                invalidCameraPoseReference = true;
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
        command.camera = GridRenderCamera{};
        command.cameraPoseUuid.clear();
        command.cameraPoseViewId.clear();
        return command;
    }

    if (invalidOutputSize) {
        command.ok = false;
        command.error =
            QStringLiteral(
                "--size must use positive WIDTHxHEIGHT dimensions, at most 16384 per side and 67108864 pixels.");
        return command;
    }

    const bool hasCameraOptions =
        cameraSpecified || lookAtSpecified || customUpSpecified || customFieldOfViewSpecified;
    if (invalidCameraCoordinates) {
        command.ok = false;
        command.error =
            QStringLiteral("--camera, --look-at, and --up require finite X,Y,Z coordinates.");
        return command;
    }
    if (invalidFieldOfView) {
        command.ok = false;
        command.error = QStringLiteral("--fov requires a finite number of degrees.");
        return command;
    }
    if (invalidCameraPoseReference) {
        command.ok = false;
        command.error = QStringLiteral("--camera-pose requires a UID:view_id value.");
        return command;
    }
    if (cameraSpecified != lookAtSpecified) {
        command.ok = false;
        command.error =
            QStringLiteral("--camera and --look-at must be provided together.");
        return command;
    }
    if (hasCameraOptions && !cameraSpecified) {
        command.ok = false;
        command.error =
            QStringLiteral("--up and --fov require --camera and --look-at.");
        return command;
    }
    if (cameraSpecified && !isValidGridRenderCamera(command.camera)) {
        command.ok = false;
        command.error = QStringLiteral(
            "Camera position, target, up vector, and FOV must define a valid perspective view.");
        return command;
    }
    if (cameraPoseSpecified && hasCameraOptions) {
        command.ok = false;
        command.error = QStringLiteral(
            "--camera-pose cannot be combined with --camera, --look-at, --up, or --fov.");
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
             (!command.outputDirectory.isEmpty() || command.outputSize.isValid() ||
              hasCameraOptions || cameraPoseSpecified)) {
        command.ok = false;
        command.error =
            QStringLiteral("--output-dir, --size, and camera options require --render-grid.");
    }
    return command;
}

QString startupCommandLineUsage()
{
    return QStringLiteral(
        "Usage:\n"
        "  meshcompare [mesh ...]\n"
        "  meshcompare --render-grid comparison.mlp --output-dir output [--size WIDTHxHEIGHT] [--camera X,Y,Z --look-at X,Y,Z [--up X,Y,Z] [--fov DEGREES] | --camera-pose UID:view_id]\n"
        "\n"
        "--render-grid  Render one MeshLab project to a PNG without opening a window.\n"
        "--output-dir   Directory that receives <project>.grid.png.\n"
        "--size         Output PNG WIDTHxHEIGHT; defaults to 2048x1152 (max 16384 per side, 64 MP).\n"
        "--camera       Camera position; requires --look-at.\n"
        "--look-at      Point viewed by --camera.\n"
        "--up           Camera up vector; defaults to 0,1,0.\n"
        "--fov          Perspective field of view in degrees; defaults to 60.\n"
        "--camera-pose  Reuse a saved camera pose, addressed as UID:view_id.\n"
        "--help         Show this help text.\n");
}
