#include <QtTest>

#include <cstring>

#include "fakes/fake_mesh_loader.h"
#include "infrastructure/meshlab/meshlab_mesh_loader.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
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

bool writeTwoNodeGlb(const QString& path)
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
        R"("meshes":[{"primitives":[{"attributes":{"POSITION":0,"COLOR_0":1},"indices":2}]}],)"
        R"("accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3"},)"
        R"({"bufferView":1,"componentType":5121,"normalized":true,"count":3,"type":"VEC4"},)"
        R"({"bufferView":2,"componentType":5123,"count":3,"type":"SCALAR"}],)"
        R"("bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":36},)"
        R"({"buffer":0,"byteOffset":36,"byteLength":12},)"
        R"({"buffer":0,"byteOffset":48,"byteLength":6}],)"
        R"("buffers":[{"byteLength":56}]})");
    while (json.size() % 4 != 0)
        json.append(' ');

    QByteArray glb;
    appendUInt32(&glb, 0x46546c67);
    appendUInt32(&glb, 2);
    appendUInt32(
        &glb,
        quint32(12 + 8 + json.size() + 8 + binary.size()));
    appendUInt32(&glb, quint32(json.size()));
    appendUInt32(&glb, 0x4e4f534a);
    glb.append(json);
    appendUInt32(&glb, quint32(binary.size()));
    appendUInt32(&glb, 0x004e4942);
    glb.append(binary);

    QFile file(path);
    return file.open(QIODevice::WriteOnly) &&
           file.write(glb) == glb.size();
}

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

    void acceptsOneLoadedLayerAndRejectsZeroOrNine()
    {
        {
            FakeMeshLoader loader;
            loader.succeed(
                QStringLiteral("single.obj"),
                {fakeLoadedMesh(1, QStringLiteral("single"))});
            MeshImportService service(loader);

            StagedWorkspace staged =
                service.stage({QStringLiteral("single.obj")});

            QVERIFY2(staged.result.ok, qPrintable(staged.result.error));
            QCOMPARE(staged.entries.size(), 1);
            QVERIFY(staged.repository != nullptr);
        }

        for (const int count : {0, 9}) {
            FakeMeshLoader loader;
            QVector<LoadedMesh> meshes;
            for (int index = 0; index < count; ++index)
                meshes.push_back(fakeLoadedMesh(index + 1, QStringLiteral("mesh-%1").arg(index)));
            loader.succeed(QStringLiteral("count-%1.obj").arg(count), meshes);
            MeshImportService service(loader);

            StagedWorkspace staged = service.stage({QStringLiteral("count-%1.obj").arg(count)});

            QVERIFY2(!staged.result.ok, qPrintable(staged.result.error));
            QVERIFY(staged.repository == nullptr);
            QVERIFY(staged.entries.isEmpty());
            QVERIFY(staged.fileErrors.isEmpty());
        }
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
        QVERIFY(writeTwoNodeGlb(path));

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

    void importsGlbLayersReferencedByMeshLabProject()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString glbPath =
            directory.filePath(QStringLiteral("comparison.glb"));
        QVERIFY(writeTwoNodeGlb(glbPath));

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
