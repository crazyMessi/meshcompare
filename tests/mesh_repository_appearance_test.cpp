#include <QtTest>
#include <QDir>
#include <QTemporaryDir>

#include <limits>
#include <memory>

#include <common/ml_document/mesh_model.h>
#include <common/globals.h>
#include <common/plugins/plugin_manager.h>
#include <vcg/complex/allocate.h>

#include "infrastructure/meshlab/meshlab_mesh_repository.h"
#include "infrastructure/meshlab/meshlab_mesh_loader.h"

namespace
{
MeshModel* triangle(MeshLabMeshRepository& repository)
{
    MeshModel* model = repository.allocateMesh(
        QStringLiteral("appearance.obj"), QStringLiteral("appearance"));
    vcg::tri::Allocator<CMeshO>::AddVertices(model->cm, 3);
    model->cm.vert[0].P() = CMeshO::CoordType(0, 0, 0);
    model->cm.vert[1].P() = CMeshO::CoordType(1, 0, 0);
    model->cm.vert[2].P() = CMeshO::CoordType(0, 1, 0);
    for (CVertexO& vertex : model->cm.vert)
        vertex.N() = CMeshO::CoordType(0, 0, 1);
    vcg::tri::Allocator<CMeshO>::AddFaces(model->cm, 1);
    for (int corner = 0; corner < 3; ++corner)
        model->cm.face[0].V(corner) = &model->cm.vert[corner];
    model->cm.Tr.SetIdentity();
    return model;
}

QImage texture(const QColor& color)
{
    QImage image(2, 2, QImage::Format_ARGB32);
    image.fill(color);
    return image;
}
} // namespace

class MeshRepositoryAppearanceTest final : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase()
    {
        PluginManager& plugins = meshlab::pluginManagerInstance();
        if (plugins.inputMeshPlugin(QStringLiteral("obj")) != nullptr)
            return;
        const QDir pluginDirectory(meshlab::defaultPluginPath());
        const QStringList candidates = pluginDirectory.entryList(
            {QStringLiteral("*io_base*")}, QDir::Files);
        QVERIFY2(!candidates.isEmpty(), qPrintable(pluginDirectory.absolutePath()));
        plugins.loadPlugin(pluginDirectory.absoluteFilePath(candidates.front()));
        QVERIFY(plugins.inputMeshPlugin(QStringLiteral("obj")) != nullptr);
    }

    void objPreservesPlainMaterialBesideATexturedMaterial()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        QFile material(directory.filePath(QStringLiteral("mixed.mtl")));
        QVERIFY(material.open(QIODevice::WriteOnly));
        QVERIFY(material.write(
            "newmtl textured\nKd 1 1 1\nmap_Kd base.png\n"
            "newmtl plain\nKd 0 1 0\n") > 0);
        material.close();
        QVERIFY(texture(Qt::red).save(directory.filePath(QStringLiteral("base.png"))));
        const QString path = directory.filePath(QStringLiteral("mixed.obj"));
        QFile object(path);
        QVERIFY(object.open(QIODevice::WriteOnly));
        QVERIFY(object.write(
            "mtllib mixed.mtl\nv 0 0 0\nv 1 0 0\nv 0 1 0\nv 1 1 0\n"
            "vt 0 0\nvt 1 0\nvt 0 1\nvt 1 1\n"
            "usemtl textured\nf 1/1 2/2 3/3\n"
            "usemtl plain\nf 2/2 4/4 3/3\n") > 0);
        object.close();

        MeshLabMeshRepository repository;
        MeshLabMeshLoader loader;
        QVector<LoadedMesh> loaded;
        const OperationResult result = loader.loadFile(path, repository, &loaded);
        QVERIFY2(result.ok, qPrintable(result.error));
        QCOMPARE(loaded.size(), 1);
        const IMeshGeometryView* geometry = repository.geometry(loaded.front().resourceId);
        QVERIFY(geometry != nullptr);
        QVERIFY(geometry->hasFaceColors());
        QCOMPARE(geometry->faceCount(), 2);
        QCOMPARE(geometry->faceColor(0), QColor(Qt::white));
        QCOMPARE(geometry->faceColor(1), QColor(Qt::green));
        QCOMPARE(geometry->textureCount(), 1);
        QCOMPARE(geometry->textureImage(0).pixelColor(0, 0), QColor(Qt::red));
        for (const MeshTexCoord& coordinate : geometry->faceTexCoords(0))
            QCOMPARE(coordinate.textureIndex, 0);
        for (const MeshTexCoord& coordinate : geometry->faceTexCoords(1))
            QCOMPARE(coordinate.textureIndex, -1);
    }

    void faceColorsAndWedgesUseDenseLiveFaceOrder()
    {
        MeshLabMeshRepository repository;
        MeshModel* model = triangle(repository);
        model->updateDataMask(MeshModel::MM_FACECOLOR | MeshModel::MM_WEDGTEXCOORD);
        vcg::tri::Allocator<CMeshO>::AddFaces(model->cm, 2);
        for (int index = 0; index < 3; ++index) {
            CFaceO& face = model->cm.face[index];
            for (int corner = 0; corner < 3; ++corner) {
                face.V(corner) = &model->cm.vert[corner];
                face.WT(corner) = vcg::TexCoord2<float>(float(corner) / 2, float(index) / 2);
                face.WT(corner).N() = index == 2 ? 2 : 0;
            }
        }
        model->cm.face[0].C() = vcg::Color4b(10, 20, 30, 40);
        model->cm.face[1].C() = vcg::Color4b(50, 60, 70, 80);
        model->cm.face[2].C() = vcg::Color4b(90, 100, 110, 120);
        vcg::tri::Allocator<CMeshO>::DeleteFace(model->cm, model->cm.face[1]);

        // Names deliberately sort differently from the face texture indices.
        model->addTexture("z-first.png", texture(Qt::red));
        model->cm.textures.push_back("missing.png");
        model->addTexture("a-third.png", texture(Qt::blue));

        const IMeshGeometryView* geometry = repository.geometry(repository.resourceIdFor(*model));
        QVERIFY(geometry != nullptr);
        QCOMPARE(geometry->faceCount(), 2);
        QVERIFY(geometry->hasFaceColors());
        QCOMPARE(geometry->faceColor(0), QColor(10, 20, 30, 40));
        QCOMPARE(geometry->faceColor(1), QColor(90, 100, 110, 120));
        QCOMPARE(geometry->textureCount(), 3);
        QCOMPARE(geometry->textureImage(0).pixelColor(0, 0), QColor(Qt::red));
        QVERIFY(geometry->textureImage(1).isNull());
        QCOMPARE(geometry->textureImage(2).pixelColor(0, 0), QColor(Qt::blue));
        QVERIFY(geometry->hasFaceTexCoords());
        const auto coordinates = geometry->faceTexCoords(1);
        QCOMPARE(coordinates[1].u, 0.5);
        QCOMPARE(coordinates[1].v, 1.0);
        QCOMPARE(coordinates[1].textureIndex, 2);

        model->cm.face[2].C() = vcg::Color4b(0, 0, 0, 0);
        model->cm.face[2].WT(1).U() = 0;
        model->setTexture("a-third.png", texture(Qt::green));
        QCOMPARE(geometry->faceColor(1), QColor(90, 100, 110, 120));
        QCOMPARE(geometry->faceTexCoords(1)[1].u, 0.5);
        QCOMPARE(geometry->textureImage(2).pixelColor(0, 0), QColor(Qt::blue));
    }

    void vertexTextureCoordinatesHaveAMissingTextureFallback()
    {
        MeshLabMeshRepository repository;
        MeshModel* model = triangle(repository);
        model->updateDataMask(MeshModel::MM_VERTTEXCOORD);
        model->addTexture("base.png", texture(Qt::yellow));
        for (int corner = 0; corner < 3; ++corner)
            model->cm.vert[corner].T() = vcg::TexCoord2<float>(float(corner) / 2, 0.25f);
        model->cm.vert[1].T().N() = 8;
        model->cm.vert[2].T().V() = std::numeric_limits<float>::quiet_NaN();

        const IMeshGeometryView* geometry = repository.geometry(repository.resourceIdFor(*model));
        QVERIFY(geometry != nullptr);
        QVERIFY(!geometry->hasFaceColors());
        QVERIFY(geometry->hasFaceTexCoords());
        const auto coordinates = geometry->faceTexCoords(0);
        QCOMPARE(coordinates[0].u, 0.0);
        QCOMPARE(coordinates[0].v, 0.25);
        QCOMPARE(coordinates[0].textureIndex, -1);
        QCOMPARE(coordinates[1].textureIndex, -1);
        QCOMPARE(coordinates[2].textureIndex, -1);
        QCOMPARE(coordinates[2].v, 0.0);
        QCOMPARE(geometry->textureWrapS(0), 10497);
        QCOMPARE(geometry->textureWrapT(0), 10497);
    }

    void retainedResourcesKeepAppearanceMetadataAndImages()
    {
        std::unique_ptr<MeshLabMeshRepository> source(new MeshLabMeshRepository);
        MeshModel* model = triangle(*source);
        model->addTexture("base.png", texture(Qt::cyan));
        const MeshResourceId sourceId = source->resourceIdFor(*model);
        source->setAppearanceMetadata(sourceId, true, {33071}, {33648});

        MeshLabMeshRepository destination;
        MeshResourceId retainedId = 0;
        QVERIFY(destination.retainMesh(*source, sourceId, &retainedId).ok);
        source.reset();

        const IMeshGeometryView* geometry = destination.geometry(retainedId);
        QVERIFY(geometry != nullptr);
        QVERIFY(geometry->isUnlit());
        QCOMPARE(geometry->textureCount(), 1);
        QCOMPARE(geometry->textureImage(0).pixelColor(0, 0), QColor(Qt::cyan));
        QCOMPARE(geometry->textureWrapS(0), 33071);
        QCOMPARE(geometry->textureWrapT(0), 33648);
    }
};

QTEST_GUILESS_MAIN(MeshRepositoryAppearanceTest)
#include "mesh_repository_appearance_test.moc"
