#include "meshlab_mesh_loader.h"

#include "core/camera_pose_uuid.h"
#include "glb_mesh_loader.h"

#include <cmath>
#include <exception>
#include <list>

#include <QDir>
#include <QDomDocument>
#include <QFile>
#include <QFileInfo>

#include <common/globals.h>
#include <common/mlexception.h>
#include <common/ml_document/mesh_model.h>
#include <common/plugins/plugin_manager.h>
#include <common/utilities/load_save.h>

namespace
{
class CurrentDirectoryRestore final
{
public:
    CurrentDirectoryRestore()
        : path_(QDir::currentPath())
    {
    }

    ~CurrentDirectoryRestore()
    {
        QDir::setCurrent(path_);
    }

private:
    QString path_;
};

struct MeshLabProjectLayer
{
    QString sourcePath;
    QString label;
    int idInFile = -1;
    Matrix44m transform;
};

void removeModels(
    MeshLabMeshRepository& repository,
    const QVector<MeshModel*>& models)
{
    for (MeshModel* model : models) {
        if (model != nullptr)
            repository.removeMesh(repository.resourceIdFor(*model));
    }
}

OperationResult loadMeshModels(
    const QString& path,
    MeshLabMeshRepository& destination,
    QVector<MeshModel*>* loadedModels)
{
    if (loadedModels == nullptr) {
        return OperationResult::failure(
            QStringLiteral("No loaded mesh model output was provided."));
    }
    loadedModels->clear();

    const QFileInfo info(path);
    if (!info.exists())
        return OperationResult::failure(QStringLiteral("File does not exist."));
    if (!info.isReadable())
        return OperationResult::failure(QStringLiteral("File is not readable."));

    const QString suffix = info.suffix();
    if (suffix.compare(QStringLiteral("glb"), Qt::CaseInsensitive) == 0)
        return loadGlbMeshModels(path, destination, loadedModels);
    IOPlugin* plugin = meshlab::pluginManagerInstance().inputMeshPlugin(suffix);
    if (plugin == nullptr) {
        return OperationResult::failure(
            QStringLiteral("No MeshLab input plugin supports .%1.").arg(suffix));
    }

    std::list<MeshModel*> models;
    QVector<MeshModel*> allocated;
    CurrentDirectoryRestore restoreCurrentDirectory;
    try {
        RichParameterList parameters = plugin->initPreOpenParameter(suffix);
        parameters.join(meshlab::defaultGlobalParameterList());
        const unsigned int contained = plugin->numberMeshesContainedInFile(
            suffix, path, parameters);
        if (contained == 0) {
            return OperationResult::failure(
                QStringLiteral("The mesh file contains no mesh layers."));
        }
        for (unsigned int index = 0; index < contained; ++index) {
            MeshModel* model = destination.allocateMesh(path, info.fileName());
            if (contained != 1)
                model->setIdInFile(static_cast<int>(index));
            models.push_back(model);
            allocated.push_back(model);
        }

        std::list<int> masks;
        meshlab::loadMesh(path, plugin, parameters, models, masks, nullptr);
    }
    catch (const MLException& exception) {
        removeModels(destination, allocated);
        return OperationResult::failure(
            QString::fromLocal8Bit(exception.what()));
    }
    catch (const std::exception& exception) {
        removeModels(destination, allocated);
        return OperationResult::failure(
            QString::fromLocal8Bit(exception.what()));
    }
    catch (...) {
        removeModels(destination, allocated);
        return OperationResult::failure(
            QStringLiteral("The mesh file could not be loaded."));
    }

    loadedModels->reserve(static_cast<int>(models.size()));
    for (MeshModel* model : models)
        loadedModels->append(model);
    return OperationResult::success();
}

LoadedMesh loadedMeshFor(
    MeshLabMeshRepository& repository,
    const MeshModel& model)
{
    LoadedMesh mesh;
    mesh.resourceId = repository.resourceIdFor(model);
    mesh.sourcePath = model.fullName();
    mesh.displayName = model.label();
    mesh.uuidCandidates = meshcompare::extractCameraPoseUuidCandidates(
        {mesh.sourcePath, mesh.displayName});
    return mesh;
}

QString normalizedSourcePath(const QString& path, const QDir& projectDirectory)
{
    const QFileInfo info(path);
    const QString absolutePath = info.isAbsolute()
                                     ? info.absoluteFilePath()
                                     : projectDirectory.absoluteFilePath(path);
    const QString canonicalPath = QFileInfo(absolutePath).canonicalFilePath();
    return QDir::cleanPath(
        canonicalPath.isEmpty() ? absolutePath : canonicalPath);
}

OperationResult readProjectLayers(
    const QString& projectPath,
    QVector<MeshLabProjectLayer>* layers)
{
    if (layers == nullptr) {
        return OperationResult::failure(
            QStringLiteral("No MeshLab project layer output was provided."));
    }
    layers->clear();

    QFile file(projectPath);
    if (!file.open(QIODevice::ReadOnly))
        return OperationResult::failure(QStringLiteral("File is not readable."));

    QDomDocument document(QStringLiteral("MeshLabDocument"));
    QString parseError;
    int parseErrorLine = 0;
    int parseErrorColumn = 0;
    if (!document.setContent(
            &file,
            &parseError,
            &parseErrorLine,
            &parseErrorColumn)) {
        return OperationResult::failure(
            QStringLiteral("Invalid MeshLab project XML at %1:%2: %3")
                .arg(parseErrorLine)
                .arg(parseErrorColumn)
                .arg(parseError));
    }

    const QDomElement root = document.documentElement();
    const QDomElement meshGroup = root.firstChildElement(
        QStringLiteral("MeshGroup"));
    const QDir projectDirectory = QFileInfo(projectPath).absoluteDir();
    for (QDomElement mesh = meshGroup.firstChildElement(
             QStringLiteral("MLMesh"));
         !mesh.isNull();
         mesh = mesh.nextSiblingElement(QStringLiteral("MLMesh"))) {
        const QString filename = mesh.attribute(QStringLiteral("filename"));
        if (filename.isEmpty()) {
            return OperationResult::failure(
                QStringLiteral("A MeshLab project layer has no source filename."));
        }

        MeshLabProjectLayer layer;
        layer.sourcePath = normalizedSourcePath(filename, projectDirectory);
        layer.label = mesh.attribute(
            QStringLiteral("label"), QFileInfo(filename).fileName());
        layer.transform.SetIdentity();
        if (mesh.hasAttribute(QStringLiteral("idInFile"))) {
            bool validId = false;
            layer.idInFile = mesh.attribute(
                QStringLiteral("idInFile")).toInt(&validId);
            if (!validId) {
                return OperationResult::failure(
                    QStringLiteral("A MeshLab project layer has an invalid idInFile value."));
            }
        }

        const QDomElement matrix = mesh.firstChildElement(
            QStringLiteral("MLMatrix44"));
        if (!matrix.isNull()) {
            const QStringList values = matrix.text()
                                           .simplified()
                                           .split(QLatin1Char(' '), Qt::SkipEmptyParts);
            if (values.size() != 16) {
                return OperationResult::failure(
                    QStringLiteral("A MeshLab project layer has an invalid 4x4 transform."));
            }
            Scalarm* destination = layer.transform.V();
            for (int index = 0; index < values.size(); ++index) {
                bool validValue = false;
                const double value = values.at(index).toDouble(&validValue);
                if (!validValue || !std::isfinite(value)) {
                    return OperationResult::failure(
                        QStringLiteral("A MeshLab project layer has a non-finite transform."));
                }
                destination[index] = static_cast<Scalarm>(value);
            }
        }
        layers->append(layer);
    }
    return OperationResult::success();
}

OperationResult loadProjectLayerModels(
    const QVector<MeshLabProjectLayer>& layers,
    MeshLabMeshRepository& destination,
    QVector<MeshModel*>* orderedModels)
{
    if (orderedModels == nullptr) {
        return OperationResult::failure(
            QStringLiteral("No ordered MeshLab project layer output was provided."));
    }
    orderedModels->clear();
    orderedModels->reserve(layers.size());
    for (const MeshLabProjectLayer& layer : layers) {
        QVector<MeshModel*> sourceModels;
        const OperationResult loaded = loadMeshModels(
            layer.sourcePath, destination, &sourceModels);
        if (!loaded.ok) {
            removeModels(destination, *orderedModels);
            orderedModels->clear();
            return OperationResult::failure(
                QStringLiteral("%1: %2").arg(layer.label, loaded.error));
        }

        MeshModel* selected = nullptr;
        if (layer.idInFile < 0) {
            if (sourceModels.size() == 1)
                selected = sourceModels.front();
        }
        else {
            for (MeshModel* model : sourceModels) {
                if (model != nullptr && model->idInFile() == layer.idInFile) {
                    selected = model;
                    break;
                }
            }
            if (selected == nullptr && layer.idInFile == 0 &&
                sourceModels.size() == 1 &&
                sourceModels.front()->idInFile() == -1) {
                selected = sourceModels.front();
            }
        }

        if (selected == nullptr) {
            removeModels(destination, sourceModels);
            removeModels(destination, *orderedModels);
            orderedModels->clear();
            return OperationResult::failure(QStringLiteral(
                "%1 could not be matched to source mesh layer %2.")
                    .arg(layer.label)
                    .arg(layer.idInFile));
        }

        for (MeshModel* model : sourceModels) {
            if (model != selected)
                destination.removeMesh(destination.resourceIdFor(*model));
        }
        selected->setLabel(layer.label);
        selected->cm.Tr = layer.transform;
        orderedModels->append(selected);
    }
    return OperationResult::success();
}
} // namespace

OperationResult MeshLabMeshLoader::loadFile(
    const QString& path,
    MeshLabMeshRepository& destination,
    QVector<LoadedMesh>* loaded)
{
    if (loaded == nullptr)
        return OperationResult::failure(QStringLiteral("No output collection was provided."));

    QFileInfo info(path);
    if (!info.exists())
        return OperationResult::failure(QStringLiteral("File does not exist."));
    if (!info.isReadable())
        return OperationResult::failure(QStringLiteral("File is not readable."));

    const QString suffix = info.suffix();
    const bool meshLabProject = suffix.compare(
                                    QStringLiteral("mlp"),
                                    Qt::CaseInsensitive) == 0;
    if (meshLabProject) {
        try {
            QVector<MeshLabProjectLayer> layers;
            OperationResult projectResult = readProjectLayers(path, &layers);
            if (!projectResult.ok)
                return projectResult;

            QVector<MeshModel*> orderedModels;
            projectResult = loadProjectLayerModels(
                layers, destination, &orderedModels);
            if (!projectResult.ok)
                return projectResult;

            QVector<LoadedMesh> projectMeshes;
            projectMeshes.reserve(orderedModels.size());
            for (MeshModel* model : orderedModels)
                projectMeshes.push_back(loadedMeshFor(destination, *model));
            *loaded += projectMeshes;
            return OperationResult::success();
        }
        catch (const MLException& exception) {
            return OperationResult::failure(
                QString::fromLocal8Bit(exception.what()));
        }
        catch (const std::exception& exception) {
            return OperationResult::failure(
                QString::fromLocal8Bit(exception.what()));
        }
        catch (...) {
            return OperationResult::failure(
                QStringLiteral("The MeshLab project could not be loaded."));
        }
    }

    QVector<MeshModel*> models;
    const OperationResult result = loadMeshModels(path, destination, &models);
    if (!result.ok)
        return result;
    for (MeshModel* model : models)
        loaded->push_back(loadedMeshFor(destination, *model));
    return OperationResult::success();
}
