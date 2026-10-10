#include <QtTest>

#include <cstring>
#include <cmath>

#include "fakes/fake_mesh_loader.h"
#include "infrastructure/meshlab/meshlab_mesh_loader.h"

#include <QDir>
#include <QBuffer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QtEndian>
#include <common/ml_document/mesh_model.h>
#include <common/globals.h>
#include <common/plugins/plugin_manager.h>
#include <vcg/complex/allocate.h>

namespace {
void appendUInt32(QByteArray* bytes, quint32 value)
{
    const quint32 littleEndian = qToLittleEndian(value);
    bytes->append(
        reinterpret_cast<const char*>(&littleEndian),
        sizeof(littleEndian));
}

void appendUInt16(QByteArray* bytes, quint16 value)
{
    const quint16 littleEndian = qToLittleEndian(value);
    bytes->append(
        reinterpret_cast<const char*>(&littleEndian),
        sizeof(littleEndian));
}

void appendFloat(QByteArray* bytes, float value)
{
    quint32 bits = 0;
    static_assert(sizeof(bits) == sizeof(value), "float must be 32-bit");
    std::memcpy(&bits, &value, sizeof(bits));
    appendUInt32(bytes, bits);
}

bool writeGlb(const QString& path, const QJsonObject& root, QByteArray binary)
{
    QByteArray json = QJsonDocument(root).toJson(QJsonDocument::Compact);
    while (json.size() % 4 != 0)
        json.append(' ');
    while (binary.size() % 4 != 0)
        binary.append('\0');
    QByteArray glb;
    appendUInt32(&glb, 0x46546c67);
    appendUInt32(&glb, 2);
    appendUInt32(&glb, quint32(12 + 8 + json.size() + 8 + binary.size()));
    appendUInt32(&glb, quint32(json.size()));
    appendUInt32(&glb, 0x4e4f534a);
    glb.append(json);
    appendUInt32(&glb, quint32(binary.size()));
    appendUInt32(&glb, 0x004e4942);
    glb.append(binary);
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(glb) == glb.size();
}

bool writeSceneGlb(const QString& path, int nodeCount = 2)
{
    QByteArray binary;
    for (float value : {
             0.0f, 0.0f, 0.0f,
             1.0f, 0.0f, 0.0f,
             0.0f, 1.0f, 0.0f})
        appendFloat(&binary, value);
    for (quint8 value : {
             quint8(255), quint8(0), quint8(0), quint8(255),
             quint8(0), quint8(255), quint8(0), quint8(128),
             quint8(0), quint8(0), quint8(255), quint8(64)})
        binary.append(char(value));
    appendUInt16(&binary, 0);
    appendUInt16(&binary, 1);
    appendUInt16(&binary, 2);
    while (binary.size() % 4 != 0)
        binary.append('\0');

    QByteArray json = QByteArrayLiteral(
        R"({"asset":{"version":"2.0"},"scene":0,"scenes":[{"nodes":[0,1]}],)"
        R"("nodes":[{"name":"Reference","mesh":0},{"name":"Candidate","mesh":0,"translation":[10,20,30],"scale":[-1,1,1]}],)"
        R"("materials":[{"alphaMode":"BLEND"}],)"
        R"("meshes":[{"primitives":[{"attributes":{"POSITION":0,"COLOR_0":1},"indices":2,"material":0}]}],)"
        R"("accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3"},)"
        R"({"bufferView":1,"componentType":5121,"normalized":true,"count":3,"type":"VEC4"},)"
        R"({"bufferView":2,"componentType":5123,"count":3,"type":"SCALAR"}],)"
        R"("bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":36},)"
        R"({"buffer":0,"byteOffset":36,"byteLength":12},)"
        R"({"buffer":0,"byteOffset":48,"byteLength":6}],)"
        R"("buffers":[{"byteLength":56}]})");
    QJsonObject scene = QJsonDocument::fromJson(json).object();
    QJsonArray nodes = scene.value(QStringLiteral("nodes")).toArray();
    QJsonArray rootNodes{0, 1};
    for (int index = 2; index < nodeCount; ++index) {
        nodes.append(QJsonObject{
            {QStringLiteral("name"), QStringLiteral("Part %1").arg(index + 1)},
            {QStringLiteral("mesh"), 0},
            {QStringLiteral("translation"), QJsonArray{index * 10, 0, 0}},
        });
        rootNodes.append(index);
    }
    scene.insert(QStringLiteral("nodes"), nodes);
    scene.insert(
        QStringLiteral("scenes"),
        QJsonArray{QJsonObject{{QStringLiteral("nodes"), rootNodes}}});
    return writeGlb(path, scene, binary);
}

struct AppearanceGlbFixture
{
    QJsonObject root;
    QByteArray binary;
    QByteArray imageBytes;

    AppearanceGlbFixture()
    {
        for (float value : {0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 1.f, 0.f})
            appendFloat(&binary, value);
        for (quint8 value : {255, 0, 0, 255, 0, 255, 0, 128, 0, 0, 255, 64})
            binary.append(char(value));
        for (quint16 value : {0, 0, 65535, 0, 0, 65535})
            appendUInt16(&binary, value);
        QImage image(2, 2, QImage::Format_RGBA8888);
        image.setPixelColor(0, 0, QColor(255, 0, 0, 64));
        image.setPixelColor(1, 0, Qt::green);
        image.setPixelColor(0, 1, Qt::blue);
        image.setPixelColor(1, 1, Qt::yellow);
        QBuffer output(&imageBytes);
        output.open(QIODevice::WriteOnly);
        image.save(&output, "PNG");
        root = QJsonDocument::fromJson(QByteArrayLiteral(
            R"({"asset":{"version":"2.0"},"scene":0,"scenes":[{"nodes":[0]}],)"
            R"("nodes":[{"mesh":0,"scale":[-1,1,1]}],)"
            R"("extensionsRequired":["KHR_materials_unlit","KHR_texture_transform"],)"
            R"("meshes":[{"primitives":[{"attributes":{"POSITION":0,"COLOR_0":1,"TEXCOORD_1":2},"material":0},)"
            R"({"attributes":{"POSITION":0},"material":1}]}],)"
            R"("materials":[{"alphaMode":"BLEND","extensions":{"KHR_materials_unlit":{}},"pbrMetallicRoughness":{)"
            R"("baseColorFactor":[0.5,0.25,1,0.5],"baseColorTexture":{"index":0,"texCoord":0,"extensions":{)"
            R"("KHR_texture_transform":{"texCoord":1,"offset":[0.25,0.125],"scale":[0.5,0.25],"rotation":1.5707963267948966}}}}},)"
            R"({"extensions":{"KHR_materials_unlit":{}},"pbrMetallicRoughness":{"baseColorFactor":[0,0,1,1]}}],)"
            R"("textures":[{"source":0,"sampler":0}],"samplers":[{"wrapS":33071,"wrapT":33648}],)"
            R"("accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3"},)"
            R"({"bufferView":1,"componentType":5121,"normalized":true,"count":3,"type":"VEC4"},)"
            R"({"bufferView":2,"componentType":5123,"normalized":true,"count":3,"type":"VEC2"}],)"
            R"("bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":36},)"
            R"({"buffer":0,"byteOffset":36,"byteLength":12},{"buffer":0,"byteOffset":48,"byteLength":12}],)"
            R"("buffers":[{"byteLength":60}]})")).object();
    }

    void embedImage(const QString& mimeType = QStringLiteral("image/png"))
    {
        QJsonArray views = root.value(QStringLiteral("bufferViews")).toArray();
        views.append(QJsonObject{{QStringLiteral("buffer"), 0},
                                 {QStringLiteral("byteOffset"), binary.size()},
                                 {QStringLiteral("byteLength"), imageBytes.size()}});
        root.insert(QStringLiteral("bufferViews"), views);
        root.insert(QStringLiteral("images"), QJsonArray{QJsonObject{
            {QStringLiteral("bufferView"), views.size() - 1},
            {QStringLiteral("mimeType"), mimeType}}});
        binary.append(imageBytes);
        root.insert(QStringLiteral("buffers"), QJsonArray{QJsonObject{
            {QStringLiteral("byteLength"), binary.size()}}});
    }
};

bool loadIoBasePlugin()
{
    PluginManager& plugins = meshlab::pluginManagerInstance();
    if (plugins.numberIOPlugins() != 0)
        return true;

    QDir pluginDirectory(meshlab::defaultPluginPath());
    const QStringList candidates = pluginDirectory.entryList(
        {QStringLiteral("*io_base*")}, QDir::Files);
    if (candidates.isEmpty())
        return false;
    try {
        plugins.loadPlugin(pluginDirectory.absoluteFilePath(candidates.front()));
    }
    catch (...) {
        return false;
    }
    return plugins.numberIOPlugins() != 0;
}

LoadedMesh fakeLoadedMesh(MeshResourceId resourceId, const QString& name)
{
    LoadedMesh mesh;
    mesh.resourceId = resourceId;
    mesh.sourcePath = name;
    mesh.displayName = name;
    return mesh;
}

class InvalidTopologyMeshLoader final : public IMeshLoader
{
public:
    OperationResult loadFile(
        const QString& path,
        MeshLabMeshRepository& destination,
        QVector<LoadedMesh>* loaded) override
    {
        for (int index = 0; index < 2; ++index) {
            const QString name = index == 0 ? QStringLiteral("valid") : QStringLiteral("broken");
            MeshModel* model = destination.allocateMesh(path, name);
            vcg::tri::Allocator<CMeshO>::AddVertices(model->cm, 3);
            model->cm.vert[0].P() = CMeshO::CoordType(0, 0, 0);
            model->cm.vert[1].P() = CMeshO::CoordType(1, 0, 0);
            model->cm.vert[2].P() = index == 0
                                        ? CMeshO::CoordType(0, 1, 0)
                                        : CMeshO::CoordType(2, 0, 0);
            vcg::tri::Allocator<CMeshO>::AddFaces(model->cm, 1);
            model->cm.face[0].V(0) = &model->cm.vert[0];
            model->cm.face[0].V(1) = &model->cm.vert[1];
            model->cm.face[0].V(2) = &model->cm.vert[2];
            if (index == 1)
                vcg::tri::Allocator<CMeshO>::DeleteVertex(model->cm, model->cm.vert[0]);

            LoadedMesh result;
            result.resourceId = destination.resourceIdFor(*model);
            result.sourcePath = path;
            result.displayName = name;
            loaded->push_back(result);
        }
        return OperationResult::success();
    }
};
}

class MeshImportServiceTest : public QObject
{
    Q_OBJECT

private slots:
    void oneFailureRejectsTheEntireBatch()
    {
        FakeMeshLoader loader;
        loader.succeed(QStringLiteral("a.obj"), {fakeLoadedMesh(1, QStringLiteral("a.obj"))});
        loader.fail(QStringLiteral("bad.obj"), QStringLiteral("bad.obj is corrupt"));
        MeshImportService service(loader);

        StagedWorkspace staged = service.stage({QStringLiteral("a.obj"), QStringLiteral("bad.obj")});

        QVERIFY(!staged.result.ok);
        QVERIFY(staged.repository == nullptr);
        QCOMPARE(staged.fileErrors.size(), 1);
    }

    void validatesTotalLoadedMeshCountNotFileCount()
    {
        FakeMeshLoader loader;
        loader.succeed(
            QStringLiteral("multi.obj"),
            {fakeLoadedMesh(1, QStringLiteral("a")), fakeLoadedMesh(2, QStringLiteral("b"))});
        MeshImportService service(loader);

        StagedWorkspace staged = service.stage({QStringLiteral("multi.obj")});

        QVERIFY(staged.result.ok);
        QCOMPARE(staged.entries.size(), 2);
    }

    void keepsAllImportedLayersWithDefaultVisibility_data()
    {
        QTest::addColumn<int>("count");
        QTest::addColumn<int>("visibleCount");
        QTest::addColumn<bool>("project");
        const QVector<QPair<int, int>> counts{
            {1, 1}, {2, 2}, {3, 3}, {4, 4}, {5, 5}, {6, 6},
            {7, 7}, {8, 8}, {9, 8}, {12, 8}, {32, 8},
        };
        for (const auto& count : counts) {
            for (const bool project : {false, true}) {
                const QByteArray name =
                    QStringLiteral("%1-%2")
                        .arg(project ? QStringLiteral("project") : QStringLiteral("mesh"))
                        .arg(count.first)
                        .toLatin1();
                QTest::newRow(name.constData())
                    << count.first << count.second << project;
            }
        }
    }

    void keepsAllImportedLayersWithDefaultVisibility()
    {
        QFETCH(int, count);
        QFETCH(int, visibleCount);
        QFETCH(bool, project);
        FakeMeshLoader loader;
        QVector<LoadedMesh> meshes;
        for (int index = 0; index < count; ++index) {
            LoadedMesh mesh =
                fakeLoadedMesh(index + 1, QStringLiteral("mesh-%1").arg(index));
            mesh.uuidCandidates = QStringList{QStringLiteral("uuid-%1").arg(index)};
            meshes.push_back(mesh);
        }
        const QString path = project ? QStringLiteral("comparison.mlp")
                                     : QStringLiteral("comparison.obj");
        loader.succeed(path, meshes);
        MeshImportService service(loader);

        StagedWorkspace staged = service.stage({path});

        QVERIFY2(staged.result.ok, qPrintable(staged.result.error));
        QVERIFY(staged.repository != nullptr);
        QCOMPARE(staged.entries.size(), count);
        QCOMPARE(staged.layoutMode, project ? SceneLayoutMode::Overlay
                                           : SceneLayoutMode::ComparisonGrid);
        for (int index = 0; index < count; ++index) {
            const MeshEntry& entry = staged.entries.at(index);
            QCOMPARE(entry.id, MeshId(index + 1));
            QCOMPARE(entry.visible, index < visibleCount);
            QCOMPARE(entry.displayName, meshes.at(index).displayName);
            QCOMPARE(entry.sourcePath, meshes.at(index).sourcePath);
            QCOMPARE(entry.uuidCandidates, meshes.at(index).uuidCandidates);
            QVERIFY(staged.repository->geometry(entry.resourceId) != nullptr);
            for (int previous = 0; previous < index; ++previous)
                QVERIFY(entry.resourceId != staged.entries.at(previous).resourceId);
        }
    }

    void rejectsImportWithoutAnyLayers()
    {
        FakeMeshLoader loader;
        loader.succeed(QStringLiteral("empty.obj"), {});
        MeshImportService service(loader);

        StagedWorkspace staged = service.stage({QStringLiteral("empty.obj")});

        QVERIFY(!staged.result.ok);
        QVERIFY(staged.repository == nullptr);
        QVERIFY(staged.entries.isEmpty());
        QVERIFY(staged.fileErrors.isEmpty());
    }

    void selectsTheFirstEightLayersAcrossAnEntireFileBatch()
    {
        FakeMeshLoader loader;
        QStringList paths;
        for (int index = 0; index < 12; ++index) {
            const QString path = QStringLiteral("mesh-%1.obj").arg(index);
            paths.append(path);
            loader.succeed(path, {fakeLoadedMesh(index + 1, path)});
        }
        MeshImportService service(loader);

        StagedWorkspace staged = service.stage(paths);

        QVERIFY2(staged.result.ok, qPrintable(staged.result.error));
        QCOMPARE(staged.entries.size(), paths.size());
        for (int index = 0; index < paths.size(); ++index) {
            QCOMPARE(staged.entries.at(index).sourcePath, paths.at(index));
            QCOMPARE(staged.entries.at(index).visible, index < 8);
        }
    }

    void rejectsInvalidGeometryBeyondTheInitiallyVisibleLayers()
    {
        FakeMeshLoader loader;
        QVector<LoadedMesh> meshes;
        for (int index = 0; index < 8; ++index)
            meshes.push_back(fakeLoadedMesh(index + 1, QStringLiteral("valid-%1").arg(index)));
        loader.succeed(QStringLiteral("valid.obj"), meshes);
        loader.succeedWithoutPositiveArea(
            QStringLiteral("hidden.obj"), {fakeLoadedMesh(9, QStringLiteral("hidden"))});
        MeshImportService service(loader);

        StagedWorkspace staged = service.stage({QStringLiteral("valid.obj"), QStringLiteral("hidden.obj")});

        QVERIFY(!staged.result.ok);
        QCOMPARE(staged.result.error, QStringLiteral("hidden has no positive-area triangles."));
        QVERIFY(staged.repository == nullptr);
        QVERIFY(staged.entries.isEmpty());
    }

    void rejectsMeshWithoutPositiveAreaFaces()
    {
        FakeMeshLoader loader;
        loader.succeed(QStringLiteral("valid.obj"), {fakeLoadedMesh(1, QStringLiteral("valid"))});
        loader.succeedWithoutPositiveArea(
            QStringLiteral("flat.obj"), {fakeLoadedMesh(2, QStringLiteral("flat"))});
        MeshImportService service(loader);

        StagedWorkspace staged = service.stage({QStringLiteral("valid.obj"), QStringLiteral("flat.obj")});

        QVERIFY(!staged.result.ok);
        QCOMPARE(staged.result.error, QStringLiteral("flat has no positive-area triangles."));
        QVERIFY(staged.repository == nullptr);
        QVERIFY(staged.entries.isEmpty());
    }

    void importsRealObjFixturesWithoutUi()
    {
        QVERIFY(loadIoBasePlugin());

        MeshLabMeshLoader loader;
        MeshImportService service(loader);
        const QString triangle = QFINDTESTDATA("fixtures/triangle_a.obj");
        const QString groundTruth = QFINDTESTDATA("fixtures/triangle_gt.obj");
        QVERIFY(QFileInfo::exists(triangle));
        QVERIFY(QFileInfo::exists(groundTruth));

        StagedWorkspace staged = service.stage({triangle, groundTruth});

        QVERIFY2(staged.result.ok, qPrintable(staged.result.error));
        QCOMPARE(staged.entries.size(), 2);
        QVERIFY(staged.repository != nullptr);
    }

    void importsGlbSceneNodesWithWorldTransforms()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path =
            directory.filePath(QStringLiteral("comparison.GLB"));
        QVERIFY(writeSceneGlb(path));

        MeshLabMeshLoader loader;
        MeshImportService service(loader);
        StagedWorkspace staged = service.stage({path});

        QVERIFY2(staged.result.ok, qPrintable(staged.result.error));
        QCOMPARE(staged.entries.size(), 2);
        QCOMPARE(staged.entries.at(0).displayName, QStringLiteral("Reference"));
        QCOMPARE(staged.entries.at(1).displayName, QStringLiteral("Candidate"));
        const IMeshGeometryView* reference =
            staged.repository->geometry(staged.entries.at(0).resourceId);
        const IMeshGeometryView* candidate =
            staged.repository->geometry(staged.entries.at(1).resourceId);
        QVERIFY(reference != nullptr);
        QVERIFY(candidate != nullptr);
        QCOMPARE(reference->vertexPosition(0), (MeshPoint3D{{0.0, 0.0, 0.0}}));
        QCOMPARE(candidate->vertexPosition(0), (MeshPoint3D{{10.0, 20.0, 30.0}}));
        QCOMPARE(candidate->vertexPosition(1), (MeshPoint3D{{9.0, 20.0, 30.0}}));
        QCOMPARE(candidate->faceVertexIndices(0), (std::array<int, 3>{{0, 2, 1}}));
        QVERIFY(candidate->hasVertexColors());
        QCOMPARE(candidate->vertexColor(0), QColor(255, 0, 0, 255));
        QCOMPARE(candidate->vertexColor(1), QColor(0, 255, 0, 128));
        QCOMPARE(candidate->vertexColor(2), QColor(0, 0, 255, 64));
    }

    void importsGlbMaterialColorsAndTextures_data()
    {
        QTest::addColumn<QString>("source");
        QTest::newRow("embedded-png") << QStringLiteral("embedded");
        QTest::newRow("data-uri-png") << QStringLiteral("data-uri");
        QTest::newRow("local-png") << QStringLiteral("local");
        QTest::newRow("embedded-jpeg") << QStringLiteral("jpeg");
    }

    void importsGlbMaterialColorsAndTextures()
    {
        QFETCH(QString, source);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        AppearanceGlbFixture fixture;
        QVERIFY(!fixture.root.isEmpty());
        QVERIFY(!fixture.imageBytes.isEmpty());
        if (source == QStringLiteral("embedded") || source == QStringLiteral("jpeg")) {
            if (source == QStringLiteral("jpeg")) {
                QImage image = QImage::fromData(fixture.imageBytes);
                fixture.imageBytes.clear();
                QBuffer output(&fixture.imageBytes);
                QVERIFY(output.open(QIODevice::WriteOnly));
                QVERIFY(image.save(&output, "JPEG"));
            }
            fixture.embedImage(source == QStringLiteral("jpeg")
                                   ? QStringLiteral("image/jpeg")
                                   : QStringLiteral("image/png"));
        }
        else {
            QString uri;
            if (source == QStringLiteral("data-uri"))
                uri = QStringLiteral("data:image/png;base64,") + QString::fromLatin1(fixture.imageBytes.toBase64());
            else {
                QFile imageFile(directory.filePath(QStringLiteral("original color.png")));
                QVERIFY(imageFile.open(QIODevice::WriteOnly));
                QCOMPARE(imageFile.write(fixture.imageBytes), qint64(fixture.imageBytes.size()));
                imageFile.close();
                uri = QStringLiteral("original%20color.png");
            }
            fixture.root.insert(QStringLiteral("images"), QJsonArray{QJsonObject{{QStringLiteral("uri"), uri}}});
        }
        const QString path = directory.filePath(QStringLiteral("appearance.glb"));
        QVERIFY(writeGlb(path, fixture.root, fixture.binary));
        MeshLabMeshLoader loader;
        MeshImportService service(loader);
        StagedWorkspace staged = service.stage({path});
        QVERIFY2(staged.result.ok, qPrintable(staged.result.error));
        QCOMPARE(staged.entries.size(), 1);
        const IMeshGeometryView* mesh = staged.repository->geometry(staged.entries[0].resourceId);
        QVERIFY(mesh != nullptr);
        QVERIFY(mesh->hasVertexColors());
        QCOMPARE(mesh->vertexColor(0), QColor(128, 0, 0, 128));
        QCOMPARE(mesh->vertexColor(1), QColor(0, 64, 0, 64));
        QCOMPARE(mesh->vertexColor(2), QColor(0, 0, 255, 32));
        QCOMPARE(mesh->vertexColor(3), QColor(0, 0, 255, 255));
        QCOMPARE(mesh->textureCount(), 1);
        QCOMPARE(mesh->textureImage(0).size(), QSize(2, 2));
        if (source != QStringLiteral("jpeg")) {
            QCOMPARE(mesh->textureImage(0).pixelColor(0, 0), QColor(255, 0, 0, 64));
            QCOMPARE(mesh->textureImage(0).pixelColor(0, 1), QColor(Qt::blue));
        }
        QCOMPARE(mesh->textureWrapS(0), 33071);
        QCOMPARE(mesh->textureWrapT(0), 33648);
        QVERIFY(mesh->isUnlit());
        QVERIFY(mesh->hasFaceTexCoords());
        const auto coordinates = mesh->faceTexCoords(0);
        for (const MeshTexCoord& coordinate : coordinates)
            QCOMPARE(coordinate.textureIndex, 0);
        QVERIFY(std::abs(coordinates[0].u - 0.25) < 1e-6);
        QVERIFY(std::abs(coordinates[0].v - 0.875) < 1e-6);
        // Mirrored nodes reverse winding, including the corresponding UV corners.
        QVERIFY(std::abs(coordinates[1].u) < 1e-6);
        QVERIFY(std::abs(coordinates[1].v - 0.875) < 1e-6);
        QVERIFY(std::abs(coordinates[2].u - 0.25) < 1e-6);
        QVERIFY(std::abs(coordinates[2].v - 0.375) < 1e-6);
        for (const MeshTexCoord& coordinate : mesh->faceTexCoords(1))
            QCOMPARE(coordinate.textureIndex, -1);
    }

    void preservesMultipleGlbMaterialTextures()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        AppearanceGlbFixture fixture;
        fixture.embedImage();
        QImage secondImage(1, 1, QImage::Format_RGBA8888);
        secondImage.fill(Qt::cyan);
        QByteArray secondImageBytes;
        QBuffer output(&secondImageBytes);
        QVERIFY(output.open(QIODevice::WriteOnly));
        QVERIFY(secondImage.save(&output, "PNG"));
        QJsonArray images = fixture.root.value(QStringLiteral("images")).toArray();
        images.append(QJsonObject{{QStringLiteral("uri"), QStringLiteral("data:image/png;base64,") +
            QString::fromLatin1(secondImageBytes.toBase64())}});
        fixture.root.insert(QStringLiteral("images"), images);
        QJsonArray textures = fixture.root.value(QStringLiteral("textures")).toArray();
        textures.append(QJsonObject{{QStringLiteral("source"), 1}});
        fixture.root.insert(QStringLiteral("textures"), textures);
        QJsonArray materials = fixture.root.value(QStringLiteral("materials")).toArray();
        const QJsonObject secondTexture{{QStringLiteral("index"), 1},
                                        {QStringLiteral("texCoord"), 1}};
        const QJsonObject secondPbr{{QStringLiteral("baseColorTexture"), secondTexture}};
        materials[1] = QJsonObject{{QStringLiteral("pbrMetallicRoughness"), secondPbr}};
        fixture.root.insert(QStringLiteral("materials"), materials);
        QJsonArray meshes = fixture.root.value(QStringLiteral("meshes")).toArray();
        QJsonObject mesh = meshes[0].toObject();
        QJsonArray primitives = mesh.value(QStringLiteral("primitives")).toArray();
        QJsonObject primitive = primitives[1].toObject();
        primitive.insert(QStringLiteral("attributes"), QJsonObject{
            {QStringLiteral("POSITION"), 0}, {QStringLiteral("TEXCOORD_1"), 2}});
        primitives[1] = primitive;
        mesh.insert(QStringLiteral("primitives"), primitives);
        meshes[0] = mesh;
        fixture.root.insert(QStringLiteral("meshes"), meshes);
        const QString path = directory.filePath(QStringLiteral("two-materials.glb"));
        QVERIFY(writeGlb(path, fixture.root, fixture.binary));
        MeshLabMeshLoader loader;
        MeshImportService service(loader);
        StagedWorkspace staged = service.stage({path});
        QVERIFY2(staged.result.ok, qPrintable(staged.result.error));
        const IMeshGeometryView* geometry = staged.repository->geometry(staged.entries[0].resourceId);
        QCOMPARE(geometry->textureCount(), 2);
        QCOMPARE(geometry->faceTexCoords(0)[0].textureIndex, 0);
        QCOMPARE(geometry->faceTexCoords(1)[0].textureIndex, 1);
        QCOMPARE(geometry->textureImage(1).pixelColor(0, 0), QColor(Qt::cyan));
        QCOMPARE(geometry->textureWrapS(1), 10497);
        QCOMPARE(geometry->textureWrapT(1), 10497);
        QVERIFY(!geometry->isUnlit());
        QCOMPARE(geometry->vertexColor(3), QColor(Qt::white));
    }

    void opaqueGlbMaterialsIgnoreAlphaWithoutChangingSharedBlendTextures_data()
    {
        QTest::addColumn<bool>("explicitMode");
        QTest::newRow("default-opaque") << false;
        QTest::newRow("explicit-opaque") << true;
    }

    void opaqueGlbMaterialsIgnoreAlphaWithoutChangingSharedBlendTextures()
    {
        QFETCH(bool, explicitMode);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        AppearanceGlbFixture fixture;
        fixture.embedImage();
        QJsonArray materials = fixture.root.value(QStringLiteral("materials")).toArray();
        QJsonObject opaqueMaterial = materials[0].toObject();
        if (explicitMode)
            opaqueMaterial.insert(QStringLiteral("alphaMode"), QStringLiteral("OPAQUE"));
        else
            opaqueMaterial.remove(QStringLiteral("alphaMode"));
        materials[1] = opaqueMaterial;
        fixture.root.insert(QStringLiteral("materials"), materials);
        QJsonArray meshes = fixture.root.value(QStringLiteral("meshes")).toArray();
        QJsonObject mesh = meshes[0].toObject();
        QJsonArray primitives = mesh.value(QStringLiteral("primitives")).toArray();
        QJsonObject opaquePrimitive = primitives[0].toObject();
        opaquePrimitive.insert(QStringLiteral("material"), 1);
        primitives[1] = opaquePrimitive;
        QJsonObject defaultPrimitive = opaquePrimitive;
        defaultPrimitive.remove(QStringLiteral("material"));
        primitives.append(defaultPrimitive);
        mesh.insert(QStringLiteral("primitives"), primitives);
        meshes[0] = mesh;
        fixture.root.insert(QStringLiteral("meshes"), meshes);
        const QString path = directory.filePath(QStringLiteral("shared-alpha.glb"));
        QVERIFY(writeGlb(path, fixture.root, fixture.binary));
        MeshLabMeshLoader loader;
        MeshImportService service(loader);
        StagedWorkspace staged = service.stage({path});
        QVERIFY2(staged.result.ok, qPrintable(staged.result.error));
        const IMeshGeometryView* geometry = staged.repository->geometry(staged.entries[0].resourceId);
        QCOMPARE(geometry->vertexColor(0), QColor(128, 0, 0, 128));
        QCOMPARE(geometry->vertexColor(3), QColor(128, 0, 0, 255));
        QCOMPARE(geometry->vertexColor(4), QColor(0, 64, 0, 255));
        QCOMPARE(geometry->vertexColor(7), QColor(0, 255, 0, 255));
        QCOMPARE(geometry->textureCount(), 2);
        QCOMPARE(geometry->faceTexCoords(0)[0].textureIndex, 0);
        QCOMPARE(geometry->faceTexCoords(1)[0].textureIndex, 1);
        QCOMPARE(geometry->faceTexCoords(2)[0].textureIndex, -1);
        QCOMPARE(geometry->textureImage(0).pixelColor(0, 0), QColor(255, 0, 0, 64));
        QCOMPARE(geometry->textureImage(1).pixelColor(0, 0), QColor(255, 0, 0, 255));
        QCOMPARE(geometry->textureWrapS(1), 33071);
        QCOMPARE(geometry->textureWrapT(1), 33648);
    }

    void rejectsBrokenGlbAppearance_data()
    {
        QTest::addColumn<QString>("problem");
        QTest::newRow("invalid-image") << QStringLiteral("image");
        QTest::newRow("missing-uv") << QStringLiteral("uv");
        QTest::newRow("image-out-of-bounds") << QStringLiteral("bounds");
    }

    void rejectsBrokenGlbAppearance()
    {
        QFETCH(QString, problem);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        AppearanceGlbFixture fixture;
        if (problem == QStringLiteral("image"))
            fixture.imageBytes = QByteArrayLiteral("not an image");
        fixture.embedImage();
        if (problem == QStringLiteral("uv")) {
            QJsonArray meshes = fixture.root.value(QStringLiteral("meshes")).toArray();
            QJsonObject mesh = meshes[0].toObject();
            QJsonArray primitives = mesh.value(QStringLiteral("primitives")).toArray();
            QJsonObject primitive = primitives[0].toObject();
            QJsonObject attributes = primitive.value(QStringLiteral("attributes")).toObject();
            attributes.remove(QStringLiteral("TEXCOORD_1"));
            primitive.insert(QStringLiteral("attributes"), attributes);
            primitives[0] = primitive;
            mesh.insert(QStringLiteral("primitives"), primitives);
            meshes[0] = mesh;
            fixture.root.insert(QStringLiteral("meshes"), meshes);
        }
        else if (problem == QStringLiteral("bounds")) {
            QJsonArray views = fixture.root.value(QStringLiteral("bufferViews")).toArray();
            QJsonObject imageView = views.last().toObject();
            imageView.insert(QStringLiteral("byteLength"), fixture.binary.size() + 1);
            views[views.size() - 1] = imageView;
            fixture.root.insert(QStringLiteral("bufferViews"), views);
        }
        const QString path = directory.filePath(QStringLiteral("broken.glb"));
        QVERIFY(writeGlb(path, fixture.root, fixture.binary));
        MeshLabMeshLoader loader;
        MeshImportService service(loader);
        StagedWorkspace staged = service.stage({path});
        QVERIFY(!staged.result.ok);
        QVERIFY(staged.repository == nullptr);
        QVERIFY(staged.entries.isEmpty());
    }

    void importsGlbLayersReferencedByMeshLabProject()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString glbPath =
            directory.filePath(QStringLiteral("comparison.glb"));
        QVERIFY(writeSceneGlb(glbPath));

        const QString projectPath =
            directory.filePath(QStringLiteral("comparison.mlp"));
        QFile project(projectPath);
        QVERIFY(project.open(QIODevice::WriteOnly | QIODevice::Text));
        const QByteArray projectXml = QByteArrayLiteral(
            "<!DOCTYPE MeshLabDocument>\n"
            "<MeshLabProject><MeshGroup>\n"
            "  <MLMesh label=\"GLB Candidate\" filename=\"comparison.glb\" idInFile=\"1\" />\n"
            "  <MLMesh label=\"GLB Reference\" filename=\"comparison.glb\" idInFile=\"0\" />\n"
            "</MeshGroup></MeshLabProject>\n");
        QCOMPARE(project.write(projectXml), projectXml.size());
        project.close();

        MeshLabMeshLoader loader;
        MeshImportService service(loader);
        StagedWorkspace staged = service.stage({projectPath});

        QVERIFY2(staged.result.ok, qPrintable(staged.result.error));
        QCOMPARE(staged.layoutMode, SceneLayoutMode::Overlay);
        QCOMPARE(staged.entries.size(), 2);
        QCOMPARE(staged.entries.at(0).displayName, QStringLiteral("GLB Candidate"));
        QCOMPARE(staged.entries.at(1).displayName, QStringLiteral("GLB Reference"));
    }

    void importsMoreThanEightRealGlbLayers_data()
    {
        QTest::addColumn<bool>("project");
        QTest::newRow("glb-scene") << false;
        QTest::newRow("mlp-project") << true;
    }

    void importsMoreThanEightRealGlbLayers()
    {
        QFETCH(bool, project);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString glbPath = directory.filePath(QStringLiteral("comparison.glb"));
        QVERIFY(writeSceneGlb(glbPath, 12));

        QString path = glbPath;
        if (project) {
            path = directory.filePath(QStringLiteral("comparison.mlp"));
            QByteArray xml = QByteArrayLiteral(
                "<!DOCTYPE MeshLabDocument>\n<MeshLabProject><MeshGroup>\n");
            for (int index = 11; index >= 0; --index) {
                xml.append(
                    QStringLiteral(
                        "<MLMesh label=\"Project part %1\" filename=\"comparison.glb\" idInFile=\"%1\" />\n")
                        .arg(index)
                        .toUtf8());
            }
            xml.append("</MeshGroup></MeshLabProject>\n");
            QFile file(path);
            QVERIFY(file.open(QIODevice::WriteOnly));
            QCOMPARE(file.write(xml), xml.size());
        }

        MeshLabMeshLoader loader;
        MeshImportService service(loader);
        StagedWorkspace staged = service.stage({path});

        QVERIFY2(staged.result.ok, qPrintable(staged.result.error));
        QVERIFY(staged.repository != nullptr);
        QCOMPARE(staged.entries.size(), 12);
        QCOMPARE(staged.layoutMode, project ? SceneLayoutMode::Overlay
                                           : SceneLayoutMode::ComparisonGrid);
        for (int index = 0; index < staged.entries.size(); ++index) {
            const MeshEntry& entry = staged.entries.at(index);
            const int nodeIndex = project ? 11 - index : index;
            const QString nodeName =
                nodeIndex == 0 ? QStringLiteral("Reference")
                : nodeIndex == 1 ? QStringLiteral("Candidate")
                                 : QStringLiteral("Part %1").arg(nodeIndex + 1);
            QCOMPARE(entry.id, MeshId(index + 1));
            QCOMPARE(entry.visible, index < 8);
            QCOMPARE(
                entry.displayName,
                project ? QStringLiteral("Project part %1").arg(nodeIndex) : nodeName);
            const IMeshGeometryView* geometry =
                staged.repository->geometry(entry.resourceId);
            QVERIFY(geometry != nullptr);
            const MeshPoint3D expectedPosition =
                nodeIndex == 1 ? MeshPoint3D{{10.0, 20.0, 30.0}}
                               : MeshPoint3D{{double(nodeIndex * 10), 0.0, 0.0}};
            QCOMPARE(geometry->vertexPosition(0), expectedPosition);
        }
    }

    void importsPlyVertexColorsIntoGeometrySnapshot()
    {
        QVERIFY(loadIoBasePlugin());

        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path =
            directory.filePath(QStringLiteral("colored-triangle.ply"));
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Text));
        const QByteArray ply = QByteArrayLiteral(
            "ply\n"
            "format ascii 1.0\n"
            "element vertex 3\n"
            "property float x\n"
            "property float y\n"
            "property float z\n"
            "property uchar red\n"
            "property uchar green\n"
            "property uchar blue\n"
            "property uchar alpha\n"
            "element face 1\n"
            "property list uchar int vertex_indices\n"
            "end_header\n"
            "0 0 0 255 0 0 255\n"
            "1 0 0 0 255 0 128\n"
            "0 1 0 0 0 255 64\n"
            "3 0 1 2\n");
        QCOMPARE(file.write(ply), ply.size());
        file.close();

        MeshLabMeshLoader loader;
        MeshLabMeshRepository repository;
        QVector<LoadedMesh> loaded;
        const OperationResult result =
            loader.loadFile(path, repository, &loaded);

        QVERIFY2(result.ok, qPrintable(result.error));
        QCOMPARE(loaded.size(), 1);
        const IMeshGeometryView* geometry =
            repository.geometry(loaded.front().resourceId);
        QVERIFY(geometry != nullptr);
        QVERIFY(geometry->hasVertexColors());
        QCOMPARE(geometry->vertexColor(0), QColor(255, 0, 0, 255));
        QCOMPARE(geometry->vertexColor(1), QColor(0, 255, 0, 128));
        QCOMPARE(geometry->vertexColor(2), QColor(0, 0, 255, 64));
    }

    void importsMeshLabProjectAsAnOrderedOverlayInWorldSpace()
    {
        QVERIFY(loadIoBasePlugin());

        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString firstMesh = directory.filePath(QStringLiteral("first.obj"));
        const QString secondMesh = directory.filePath(QStringLiteral("second.obj"));
        QVERIFY(QFile::copy(QFINDTESTDATA("fixtures/triangle_a.obj"), firstMesh));
        QVERIFY(QFile::copy(QFINDTESTDATA("fixtures/triangle_gt.obj"), secondMesh));

        const QString projectPath = directory.filePath(QStringLiteral("comparison.mlp"));
        QFile project(projectPath);
        QVERIFY(project.open(QIODevice::WriteOnly | QIODevice::Text));
        const QByteArray projectXml = QByteArrayLiteral(
            "<!DOCTYPE MeshLabDocument>\n"
            "<MeshLabProject><MeshGroup>\n"
            "  <MLMesh label=\"Candidate\" filename=\"first.obj\">\n"
            "    <MLMatrix44>\n"
            "      1 0 0 10\n"
            "      0 1 0 20\n"
            "      0 0 1 30\n"
            "      0 0 0 1\n"
            "    </MLMatrix44>\n"
            "  </MLMesh>\n"
            "  <MLMesh label=\"Ground Truth\" filename=\"second.obj\">\n"
            "    <MLMatrix44>\n"
            "      1 0 0 0\n"
            "      0 1 0 0\n"
            "      0 0 1 5\n"
            "      0 0 0 1\n"
            "    </MLMatrix44>\n"
            "  </MLMesh>\n"
            "</MeshGroup></MeshLabProject>\n");
        QCOMPARE(project.write(projectXml), projectXml.size());
        project.close();

        MeshLabMeshLoader loader;
        MeshImportService service(loader);
        StagedWorkspace staged = service.stage({projectPath});

        QVERIFY2(staged.result.ok, qPrintable(staged.result.error));
        QCOMPARE(staged.layoutMode, SceneLayoutMode::Overlay);
        QCOMPARE(staged.entries.size(), 2);
        QCOMPARE(staged.entries.at(0).displayName, QStringLiteral("Candidate"));
        QCOMPARE(staged.entries.at(1).displayName, QStringLiteral("Ground Truth"));

        const IMeshGeometryView* candidate =
            staged.repository->geometry(staged.entries.at(0).resourceId);
        const IMeshGeometryView* groundTruth =
            staged.repository->geometry(staged.entries.at(1).resourceId);
        QVERIFY(candidate != nullptr);
        QVERIFY(groundTruth != nullptr);
        QCOMPARE(candidate->vertexPosition(0), (MeshPoint3D{{10.0, 20.0, 30.0}}));
        QCOMPARE(groundTruth->vertexPosition(0), (MeshPoint3D{{0.0, 0.0, 6.0}}));
    }

    void projectLayerWithUnmatchedFileIdFailsWithoutLeavingAModel()
    {
        QVERIFY(loadIoBasePlugin());

        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString source = directory.filePath(QStringLiteral("single.obj"));
        QVERIFY(QFile::copy(QFINDTESTDATA("fixtures/triangle_a.obj"), source));

        const QString projectPath = directory.filePath(QStringLiteral("subset.mlp"));
        QFile project(projectPath);
        QVERIFY(project.open(QIODevice::WriteOnly | QIODevice::Text));
        const QByteArray projectXml = QByteArrayLiteral(
            "<!DOCTYPE MeshLabDocument>\n"
            "<MeshLabProject><MeshGroup>\n"
            "  <MLMesh label=\"Second child\" filename=\"single.obj\" idInFile=\"1\">\n"
            "    <MLMatrix44>1 0 0 0  0 1 0 0  0 0 1 0  0 0 0 1</MLMatrix44>\n"
            "  </MLMesh>\n"
            "</MeshGroup></MeshLabProject>\n");
        QCOMPARE(project.write(projectXml), projectXml.size());
        project.close();

        MeshLabMeshLoader loader;
        MeshLabMeshRepository repository;
        QVector<LoadedMesh> loaded;
        const OperationResult result = loader.loadFile(
            projectPath, repository, &loaded);

        QVERIFY(!result.ok);
        QVERIFY(result.error.contains(QStringLiteral("source mesh layer 1")));
        QVERIFY(loaded.isEmpty());
        QVERIFY(repository.geometry(1) == nullptr);
    }

    void concreteLoaderExtractsNormalizedUuidCandidatesWithoutDuplicates()
    {
        QVERIFY(loadIoBasePlugin());

        const QString fixture = QFINDTESTDATA("fixtures/triangle_a.obj");
        QVERIFY(QFileInfo::exists(fixture));
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(QStringLiteral(
            "mesh_{550E8400-E29B-41D4-A716-446655440000}_"
            "550e8400e29b41d4a716446655440000_"
            "123e4567-e89b-12d3-a456-426614174000_"
            "00112233445566778899aabbccddeeff_"
            "f00112233445566778899aabbccddeeff.obj"));
        QVERIFY(QFile::copy(fixture, path));

        MeshLabMeshLoader loader;
        MeshLabMeshRepository repository;
        QVector<LoadedMesh> loaded;

        const OperationResult result = loader.loadFile(path, repository, &loaded);

        QVERIFY2(result.ok, qPrintable(result.error));
        QCOMPARE(loaded.size(), 1);
        QCOMPARE(
            loaded.front().uuidCandidates,
            QStringList({
                QStringLiteral("550e8400e29b41d4a716446655440000"),
                QStringLiteral("123e4567e89b12d3a456426614174000"),
                QStringLiteral("00112233445566778899aabbccddeeff"),
            }));
    }

    void rollsBackAllocatedModelsWhenConcreteLoaderThrows()
    {
        QVERIFY(loadIoBasePlugin());

        MeshLabMeshLoader loader;
        MeshLabMeshRepository repository;
        QVector<LoadedMesh> loaded;
        const QString corrupt = QFINDTESTDATA("fixtures/corrupt.obj");
        QVERIFY(QFileInfo::exists(corrupt));
        const QString workingDirectory = QDir::currentPath();

        const OperationResult result = loader.loadFile(corrupt, repository, &loaded);

        QVERIFY(!result.ok);
        QVERIFY(loaded.isEmpty());
        QVERIFY(repository.geometry(1) == nullptr);
        QCOMPARE(QDir::currentPath(), workingDirectory);
    }

    void repositoryRejectsLiveFaceReferencingDeletedVertex()
    {
        MeshLabMeshRepository repository;
        MeshModel* model = repository.allocateMesh(
            QStringLiteral("/tmp/broken.obj"),
            QStringLiteral("broken.obj"));
        vcg::tri::Allocator<CMeshO>::AddVertices(model->cm, 3);
        model->cm.vert[0].P() = CMeshO::CoordType(0, 0, 0);
        model->cm.vert[1].P() = CMeshO::CoordType(1, 0, 0);
        model->cm.vert[2].P() = CMeshO::CoordType(0, 1, 0);
        vcg::tri::Allocator<CMeshO>::AddFaces(model->cm, 1);
        model->cm.face[0].V(0) = &model->cm.vert[0];
        model->cm.face[0].V(1) = &model->cm.vert[1];
        model->cm.face[0].V(2) = &model->cm.vert[2];
        vcg::tri::Allocator<CMeshO>::DeleteVertex(model->cm, model->cm.vert[0]);

        const IMeshGeometryView* geometry =
            repository.geometry(repository.resourceIdFor(*model));

        QVERIFY(geometry == nullptr);
    }

    void repositoryAcceptsAPlanarMeshWithASingularWorldTransform()
    {
        MeshLabMeshRepository repository;
        MeshModel* model = repository.allocateMesh(
            QStringLiteral("/tmp/planar.obj"),
            QStringLiteral("planar.obj"));
        vcg::tri::Allocator<CMeshO>::AddVertices(model->cm, 3);
        model->cm.vert[0].P() = CMeshO::CoordType(0, 0, 0);
        model->cm.vert[1].P() = CMeshO::CoordType(1, 0, 0);
        model->cm.vert[2].P() = CMeshO::CoordType(0, 1, 0);
        vcg::tri::Allocator<CMeshO>::AddFaces(model->cm, 1);
        model->cm.face[0].V(0) = &model->cm.vert[0];
        model->cm.face[0].V(1) = &model->cm.vert[1];
        model->cm.face[0].V(2) = &model->cm.vert[2];
        model->cm.Tr.SetIdentity();
        model->cm.Tr[2][2] = 0;

        const MeshResourceId resourceId = repository.resourceIdFor(*model);
        const IMeshGeometryView* geometry = repository.geometry(resourceId);

        QVERIFY(geometry != nullptr);
        QVERIFY(repository.hasPositiveAreaFaces(resourceId));
        QCOMPARE(geometry->vertexNormal(0), (MeshPoint3D{{0.0, 0.0, 1.0}}));
    }

    void stageRejectsLiveFaceReferencingDeletedVertex()
    {
        InvalidTopologyMeshLoader loader;
        MeshImportService service(loader);

        StagedWorkspace staged = service.stage({QStringLiteral("broken.obj")});

        QVERIFY(!staged.result.ok);
        QCOMPARE(
            staged.result.error,
            QStringLiteral(
                "broken: Mesh contains a live face that references a deleted or unmapped "
                "vertex."));
        QVERIFY(staged.repository == nullptr);
        QVERIFY(staged.entries.isEmpty());
        QVERIFY(staged.fileErrors.isEmpty());
    }

    void repositoryGeometryIsADenseOwningSnapshotAcrossDeletedSlots()
    {
        MeshLabMeshRepository repository;
        MeshModel* model = repository.allocateMesh(
            QStringLiteral("/tmp/deleted.obj"),
            QStringLiteral("deleted.obj"));
        vcg::tri::Allocator<CMeshO>::AddVertices(model->cm, 4);
        const Scalarm precisionSensitiveX = Scalarm(16777217.25);
        model->cm.vert[0].P() = CMeshO::CoordType(precisionSensitiveX, 0, 0);
        model->cm.vert[1].P() = CMeshO::CoordType(20, 0, 0);
        model->cm.vert[2].P() = CMeshO::CoordType(30, 0, 0);
        model->cm.vert[3].P() = CMeshO::CoordType(40, 0, 0);
        for (CVertexO& vertex : model->cm.vert)
            vertex.N() = CMeshO::CoordType(0, 0, 1);
        vcg::tri::Allocator<CMeshO>::AddFaces(model->cm, 2);
        model->cm.face[0].V(0) = &model->cm.vert[0];
        model->cm.face[0].V(1) = &model->cm.vert[2];
        model->cm.face[0].V(2) = &model->cm.vert[3];
        model->cm.face[1].V(0) = &model->cm.vert[0];
        model->cm.face[1].V(1) = &model->cm.vert[1];
        model->cm.face[1].V(2) = &model->cm.vert[2];
        vcg::tri::Allocator<CMeshO>::DeleteFace(model->cm, model->cm.face[1]);
        vcg::tri::Allocator<CMeshO>::DeleteVertex(model->cm, model->cm.vert[1]);

        const IMeshGeometryView* geometry =
            repository.geometry(repository.resourceIdFor(*model));

        QVERIFY(geometry != nullptr);
        QCOMPARE(geometry->vertexCount(), 3);
        QCOMPARE(geometry->faceCount(), 1);
        QCOMPARE(geometry->vertexPosition(0)[0], double(precisionSensitiveX));
        QCOMPARE(geometry->vertexPosition(1)[0], 30.0);
        QCOMPARE(geometry->vertexPosition(2)[0], 40.0);
        QCOMPARE(geometry->faceVertexIndices(0), (std::array<int, 3>{{0, 1, 2}}));

        model->cm.vert[0].P()[0] = 99;
        QCOMPARE(geometry->vertexPosition(0)[0], double(precisionSensitiveX));
    }

    void retainedResourcesKeepTransformsAndColorsAfterSourceIsDestroyed()
    {
        QTemporaryDir temporary;
        const QString path = temporary.filePath(QStringLiteral("colored.glb"));
        QVERIFY(writeSceneGlb(path));
        MeshLabMeshLoader loader;
        std::unique_ptr<MeshLabMeshRepository> source(new MeshLabMeshRepository);
        QVector<LoadedMesh> loaded;
        QVERIFY(loader.loadFile(path, *source, &loaded).ok);
        QCOMPARE(loaded.size(), 2);

        MeshLabMeshRepository destination;
        MeshModel* existing = destination.allocateMesh(
            QStringLiteral("existing.obj"), QStringLiteral("existing"));
        const MeshResourceId existingId = destination.resourceIdFor(*existing);
        MeshResourceId retainedId = 0;
        QVERIFY(destination.retainMesh(
            *source, loaded.back().resourceId, &retainedId).ok);
        QVERIFY(retainedId != existingId);

        const IMeshGeometryView* original =
            source->geometry(loaded.back().resourceId);
        QVERIFY(original != nullptr);
        QVector<MeshPoint3D> positions;
        QVector<MeshPoint3D> normals;
        QVector<QColor> colors;
        QVector<std::array<int, 3>> faces;
        QVERIFY(original->hasVertexColors());
        for (int index = 0; index < original->vertexCount(); ++index) {
            positions.append(original->vertexPosition(index));
            normals.append(original->vertexNormal(index));
            colors.append(original->vertexColor(index));
        }
        for (int index = 0; index < original->faceCount(); ++index)
            faces.append(original->faceVertexIndices(index));

        source.reset();

        const IMeshGeometryView* retained = destination.geometry(retainedId);
        QVERIFY(retained != nullptr);
        QCOMPARE(retained->vertexCount(), positions.size());
        QCOMPARE(retained->faceCount(), faces.size());
        QVERIFY(retained->hasVertexColors());
        for (int index = 0; index < retained->vertexCount(); ++index) {
            QCOMPARE(retained->vertexPosition(index), positions[index]);
            QCOMPARE(retained->vertexNormal(index), normals[index]);
            QCOMPARE(retained->vertexColor(index), colors[index]);
        }
        for (int index = 0; index < retained->faceCount(); ++index)
            QCOMPARE(retained->faceVertexIndices(index), faces[index]);
        QCOMPARE(destination.resourceIdFor(*existing), existingId);
        QVERIFY(destination.hasPositiveAreaFaces(retainedId));
    }

    void retainedResourceRollbackAndAliasesDoNotRemoveOtherOwners()
    {
        const QVector<MeshPoint3D> vertices{
            {{0, 0, 0}}, {{1, 0, 0}}, {{0, 1, 0}}};
        const QVector<std::array<int, 3>> faces{{{0, 1, 2}}};
        MeshLabMeshRepository source;
        MeshResourceId sourceId = 0;
        QVERIFY(source.createMesh({}, QStringLiteral("source"), vertices, faces, &sourceId).ok);
        MeshLabMeshRepository destination;
        MeshResourceId existingId = 0;
        QVERIFY(destination.createMesh(
            {}, QStringLiteral("existing"), vertices, faces, &existingId).ok);
        QCOMPARE(sourceId, existingId);

        MeshResourceId rolledBackId = 0;
        QVERIFY(destination.retainMesh(source, sourceId, &rolledBackId).ok);
        QVERIFY(rolledBackId != existingId);
        destination.removeMesh(rolledBackId);
        QVERIFY(destination.geometry(rolledBackId) == nullptr);
        QVERIFY(source.hasPositiveAreaFaces(sourceId));
        QVERIFY(destination.hasPositiveAreaFaces(existingId));

        MeshResourceId retainedId = 0;
        MeshResourceId aliasId = 0;
        QVERIFY(destination.retainMesh(source, sourceId, &retainedId).ok);
        QVERIFY(retainedId > rolledBackId);
        QVERIFY(destination.retainMesh(destination, retainedId, &aliasId).ok);
        QVERIFY(aliasId != retainedId);
        source.removeMesh(sourceId);
        destination.removeMesh(retainedId);
        QVERIFY(destination.hasPositiveAreaFaces(aliasId));

        MeshResourceId generatedId = 0;
        QVERIFY(destination.createMesh(
            {}, QStringLiteral("generated"), vertices, faces, &generatedId).ok);
        QVERIFY(generatedId > aliasId);
        QVERIFY(destination.hasPositiveAreaFaces(generatedId));
        QVERIFY(destination.hasPositiveAreaFaces(existingId));
        MeshResourceId invalidId = 99;
        QVERIFY(!destination.retainMesh(source, sourceId, &invalidId).ok);
        QCOMPARE(invalidId, MeshResourceId(0));
        QVERIFY(!destination.retainMesh(destination, aliasId, nullptr).ok);
    }
};

QTEST_GUILESS_MAIN(MeshImportServiceTest)

#include "mesh_import_service_test.moc"

void FakeMeshLoader::succeed(const QString& path, const QVector<LoadedMesh>& meshes)
{
    PlannedLoad load;
    load.succeeds = true;
    load.meshes = meshes;
    loads_.insert(path, load);
}

void FakeMeshLoader::succeedWithoutPositiveArea(
    const QString& path,
    const QVector<LoadedMesh>& meshes)
{
    PlannedLoad load;
    load.succeeds = true;
    load.hasPositiveArea = false;
    load.meshes = meshes;
    loads_.insert(path, load);
}

void FakeMeshLoader::fail(const QString& path, const QString& error)
{
    PlannedLoad load;
    load.error = error;
    loads_.insert(path, load);
}

OperationResult FakeMeshLoader::loadFile(
    const QString& path,
    MeshLabMeshRepository& destination,
    QVector<LoadedMesh>* loaded)
{
    const auto it = loads_.constFind(path);
    if (it == loads_.cend())
        return OperationResult::failure(QStringLiteral("No fake result was configured."));
    if (!it->succeeds)
        return OperationResult::failure(it->error);

    for (LoadedMesh mesh : it->meshes) {
        MeshModel* model = destination.allocateMesh(path, mesh.displayName);
        vcg::tri::Allocator<CMeshO>::AddVertices(model->cm, 3);
        model->cm.vert[0].P() = CMeshO::CoordType(0, 0, 0);
        model->cm.vert[1].P() = CMeshO::CoordType(1, 0, 0);
        model->cm.vert[2].P() = it->hasPositiveArea
                                    ? CMeshO::CoordType(0, 1, 0)
                                    : CMeshO::CoordType(2, 0, 0);
        vcg::tri::Allocator<CMeshO>::AddFaces(model->cm, 1);
        model->cm.face[0].V(0) = &model->cm.vert[0];
        model->cm.face[0].V(1) = &model->cm.vert[1];
        model->cm.face[0].V(2) = &model->cm.vert[2];
        mesh.resourceId = destination.resourceIdFor(*model);
        if (mesh.sourcePath.isEmpty())
            mesh.sourcePath = path;
        if (mesh.displayName.isEmpty())
            mesh.displayName = path;
        loaded->push_back(mesh);
    }
    return OperationResult::success();
}
