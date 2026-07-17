#include <QtTest>

#include <array>

#include <QWidget>

#include "core/mesh_resource_provider.h"
#include "renderer/meshlab/mesh_lab_renderer_adapter.h"

namespace
{
class TriangleGeometry final : public IMeshGeometryView
{
public:
    int vertexCount() const override { return 3; }
    MeshPoint3D vertexPosition(int index) const override
    {
        static const MeshPoint3D positions[] = {
            {{0.0, 0.0, 0.0}},
            {{1.0, 0.0, 0.0}},
            {{0.0, 1.0, 0.0}}};
        return positions[index];
    }
    MeshPoint3D vertexNormal(int) const override { return {{0.0, 0.0, 1.0}}; }
    int faceCount() const override { return 1; }
    std::array<int, 3> faceVertexIndices(int) const override { return {{0, 1, 2}}; }
};

class TriangleResources final : public IMeshResourceProvider
{
public:
    const IMeshGeometryView* geometry(MeshResourceId id) const override
    {
        return id == 1 || id == 2 ? &geometry_ : nullptr;
    }

private:
    TriangleGeometry geometry_;
};
} // namespace

class MeshLabRendererAdapterOffscreenTest : public QObject
{
    Q_OBJECT

private slots:
    void offscreenPreparationDoesNotCrash()
    {
        MeshLabRendererAdapter adapter;
        QWidget host;
        QVERIFY(adapter.mount(&host).ok);
        TriangleResources resources;
        SceneDescriptor scene;
        scene.generation = 1;
        scene.meshes.append({1, 1, QStringLiteral("triangle"), true});
        scene.meshes.append({2, 2, QStringLiteral("triangle-copy"), false});
        scene.referenceId = 1;

        const OperationResult result = adapter.prepareScene(scene, resources);

        if (result.ok) {
            QCOMPARE(adapter.preparedGeneration(), quint64(1));
            adapter.discardPreparedScene();
            QCOMPARE(adapter.preparedGeneration(), quint64(0));
            QCOMPARE(adapter.committedGeneration(), quint64(0));
            return;
        }

        QVERIFY(result.error.contains(QStringLiteral("OpenGL")));
        QCOMPARE(adapter.preparedGeneration(), quint64(0));
        QCOMPARE(adapter.committedGeneration(), quint64(0));
    }
};

QTEST_MAIN(MeshLabRendererAdapterOffscreenTest)
#include "mesh_lab_renderer_adapter_offscreen_test.moc"
