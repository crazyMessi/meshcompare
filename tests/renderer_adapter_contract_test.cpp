#include <QtTest>
#include "core/renderer_adapter.h"
#include "fakes/fake_renderer_adapter.h"

class RendererAdapterContractTest : public QObject
{
    Q_OBJECT
private slots:
    void prepareCommitAndDiscardAreExplicit()
    {
        FakeRendererAdapter renderer;
        SceneDescriptor scene;
        scene.generation = 7;
        scene.meshes = {
            {11, 101, QStringLiteral("GT"), true},
            {12, 102, QStringLiteral("Result"), false}
        };
        scene.referenceId = 11;

        QCOMPARE(renderer.prepareScene(scene).ok, true);
        QCOMPARE(renderer.preparedGeneration(), quint64(7));
        renderer.commitPreparedScene();
        QCOMPARE(renderer.committedGeneration(), quint64(7));

        scene.generation = 8;
        QCOMPARE(renderer.prepareScene(scene).ok, true);
        renderer.discardPreparedScene();
        QCOMPARE(renderer.committedGeneration(), quint64(7));
    }
};

QTEST_GUILESS_MAIN(RendererAdapterContractTest)
#include "renderer_adapter_contract_test.moc"
