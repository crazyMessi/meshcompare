#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QTextStream>

#include <common/globals.h>
#include <common/plugins/plugin_manager.h>

#include "infrastructure/meshlab/meshlab_mesh_loader.h"
#include "plugins/controllable_hole_filling/controllable_hole_filling.h"

namespace
{

bool loadIoBasePlugin(QString* error)
{
    PluginManager& plugins = meshlab::pluginManagerInstance();
    if (plugins.numberIOPlugins() != 0)
        return true;
    QDir pluginDirectory(meshlab::defaultPluginPath());
    const QStringList candidates = pluginDirectory.entryList(
        {QStringLiteral("*io_base*")}, QDir::Files);
    if (candidates.isEmpty()) {
        *error = QStringLiteral("io_base plugin was not found in %1")
                     .arg(pluginDirectory.absolutePath());
        return false;
    }
    try {
        plugins.loadPlugin(
            pluginDirectory.absoluteFilePath(candidates.front()));
    }
    catch (const std::exception& exception) {
        *error = QString::fromLocal8Bit(exception.what());
        return false;
    }
    return plugins.numberIOPlugins() != 0;
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    QTextStream output(stdout);
    QTextStream errors(stderr);
    if (argc < 2 || argc > 5) {
        errors << "usage: " << argv[0]
               << " INPUT_DIR [r=512] [ccl-iter=3] [eps=2]\n";
        return 64;
    }

    bool ok = false;
    controllable_hole_filling::FillConfig config;
    if (argc >= 3) {
        config.resolution = QString::fromLocal8Bit(argv[2]).toInt(&ok);
        if (!ok) {
            errors << "invalid r\n";
            return 64;
        }
    }
    if (argc >= 4) {
        config.cclIterations =
            QString::fromLocal8Bit(argv[3]).toInt(&ok);
        if (!ok) {
            errors << "invalid ccl-iter\n";
            return 64;
        }
    }
    if (argc >= 5) {
        config.epsFactor =
            QString::fromLocal8Bit(argv[4]).toDouble(&ok);
        if (!ok) {
            errors << "invalid eps\n";
            return 64;
        }
    }

    QString pluginError;
    if (!loadIoBasePlugin(&pluginError)) {
        errors << pluginError << '\n';
        return 1;
    }

    const QDir inputDirectory(QString::fromLocal8Bit(argv[1]));
    const QFileInfoList inputs = inputDirectory.entryInfoList(
        {QStringLiteral("*.ply")},
        QDir::Files,
        QDir::Name);
    if (inputs.isEmpty()) {
        errors << "no PLY inputs found\n";
        return 1;
    }

    output << "mesh,load_ms,fill_ms,total_ms,vertices,faces,active_cells,status\n";
    bool failed = false;
    MeshLabMeshLoader loader;
    for (const QFileInfo& input : inputs) {
        QElapsedTimer totalTimer;
        QElapsedTimer loadTimer;
        totalTimer.start();
        loadTimer.start();
        MeshLabMeshRepository repository;
        QVector<LoadedMesh> loaded;
        const OperationResult load = loader.loadFile(
            input.absoluteFilePath(), repository, &loaded);
        const qint64 loadMilliseconds = loadTimer.elapsed();
        if (!load.ok || loaded.size() != 1) {
            output << input.fileName() << ',' << loadMilliseconds
                   << ",0," << totalTimer.elapsed()
                   << ",0,0,0,\""
                   << (load.ok
                           ? QStringLiteral("expected one mesh")
                           : load.error)
                   << "\"\n";
            failed = true;
            continue;
        }

        const IMeshGeometryView* geometry = nullptr;
        const OperationResult snapshot = repository.snapshotGeometry(
            loaded.front().resourceId, &geometry);
        if (!snapshot.ok || geometry == nullptr) {
            output << input.fileName() << ',' << loadMilliseconds
                   << ",0," << totalTimer.elapsed()
                   << ",0,0,0,\""
                   << (snapshot.ok
                           ? QStringLiteral("missing geometry")
                           : snapshot.error)
                   << "\"\n";
            failed = true;
            continue;
        }

        const controllable_hole_filling::FillResult result =
            controllable_hole_filling::Engine().fill(*geometry, config);
        const qint64 totalMilliseconds = totalTimer.elapsed();
        output << input.fileName() << ','
               << loadMilliseconds << ','
               << result.elapsedMilliseconds << ','
               << totalMilliseconds << ','
               << result.mesh.vertices.size() << ','
               << result.mesh.faces.size() << ','
               << result.activeCellCount << ",\""
               << (result.result.ok
                       ? QStringLiteral("ok")
                       : result.result.error)
               << "\"\n";
        output.flush();
        if (!result.result.ok ||
            result.elapsedMilliseconds > 4000 ||
            totalMilliseconds > 4000) {
            failed = true;
        }
    }
    return failed ? 2 : 0;
}
