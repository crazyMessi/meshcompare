#include <QtTest>

#include <array>
#include <initializer_list>
#include <stdexcept>

#include <GL/glew.h>
#include <QGLWidget>
#include <QWidget>

#include <common/ml_shared_data_context/ml_shared_data_context.h>
#include <common/ml_shared_data_context/ml_scene_gl_shared_data_context.h>
#include <common/ml_document/mesh_document.h>
#include <common/ml_document/mesh_model.h>
#include <common/parameters/rich_parameter_list.h>

#include "core/mesh_resource_provider.h"
#include "renderer/meshlab/mesh_lab_renderer_adapter.h"
#include "renderer/meshlab/render_scene_context.h"
#include "fakes/fake_viewport_factory.h"
#include "fakes/fake_viewport_callbacks.h"

namespace
{
class FakeGeometryView final : public IMeshGeometryView
{
public:
    explicit FakeGeometryView(bool hasVertexColors = false)
        : hasVertexColors_(hasVertexColors)
    {
    }

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
    bool hasVertexColors() const override { return hasVertexColors_; }
    QColor vertexColor(int index) const override
    {
        static const QColor colors[] = {
            QColor(Qt::red), QColor(Qt::green), QColor(Qt::blue)};
        return colors[index];
    }
    int faceCount() const override { return 1; }
    std::array<int, 3> faceVertexIndices(int) const override { return {{0, 1, 2}}; }

private:
    bool hasVertexColors_ = false;
};

class FakeResourceProvider final : public IMeshResourceProvider
{
public:
    explicit FakeResourceProvider(bool hasVertexColors = false)
        : geometry_(hasVertexColors)
    {
        for (MeshResourceId id = 1; id <= 8; ++id)
            available_.append(id);
    }

    const IMeshGeometryView* geometry(MeshResourceId id) const override
    {
        return available_.contains(id) ? &geometry_ : nullptr;
    }

    void remove(MeshResourceId id) { available_.removeAll(id); }

private:
    FakeGeometryView geometry_;
    QVector<MeshResourceId> available_;
};

SceneDescriptor scene(
    quint64 generation,
    std::initializer_list<MeshResourceId> resourceIds)
{
    SceneDescriptor descriptor;
    descriptor.generation = generation;
    MeshId meshId = 1;
    for (const MeshResourceId resourceId : resourceIds) {
        const bool isReference = descriptor.meshes.isEmpty();
        descriptor.meshes.append(
            {meshId, resourceId, QStringLiteral("mesh-%1").arg(meshId), isReference});
        if (isReference)
            descriptor.referenceId = meshId;
        ++meshId;
    }
    return descriptor;
}

ColorPresentation uniformPresentation(const QColor& color)
{
    ColorPresentation presentation;
    presentation.mode = ColorMode::UniformColor;
    presentation.uniformColor = color;
    return presentation;
}

ColorPresentation analysisPresentation(ColorMode mode, const QColor& color)
{
    ColorPresentation presentation;
    presentation.mode = mode;
    presentation.faceColors = {color};
    return presentation;
}

ColorPresentation vertexAnalysisPresentation(
    ColorMode mode,
    const QVector<QColor>& colors)
{
    ColorPresentation presentation;
    presentation.mode = mode;
    presentation.vertexColors = colors;
    return presentation;
}

ColorPresentation distancePresentation(
    double maximum,
    DistanceColorMapping mapping)
{
    ColorPresentation presentation = vertexAnalysisPresentation(
        ColorMode::VertexColor,
        {QColor(Qt::red), QColor(Qt::green), QColor(Qt::blue)});
    presentation.colorLegend.kind = ColorLegendKind::Distance;
    presentation.colorLegend.distanceMapping = mapping;
    presentation.colorLegend.maximum = maximum;
    return presentation;
}

MeshModel* modelFor(RenderSceneContext& context, MeshId meshId)
{
    return context.document().getMesh(context.modelIdFor(meshId));
}

QColor firstLiveFaceColor(const MeshModel& model)
{
    for (const CFaceO& face : model.cm.face) {
        if (!face.IsD()) {
            return QColor(
                int(face.C()[0]),
                int(face.C()[1]),
                int(face.C()[2]),
                int(face.C()[3]));
        }
    }
    return {};
}

QVector<QColor> liveVertexColors(const MeshModel& model)
{
    QVector<QColor> colors;
    colors.reserve(model.cm.vn);
    for (const CVertexO& vertex : model.cm.vert) {
        if (vertex.IsD())
            continue;
        colors.append(QColor(
            int(vertex.C()[0]),
            int(vertex.C()[1]),
            int(vertex.C()[2]),
            int(vertex.C()[3])));
    }
    return colors;
}

void verifySolidUsesOnlyExpectedColor(
    const MLRenderingData& renderingData,
    bool faceColor)
{
    MLRenderingData::RendAtts attributes;
    QVERIFY(renderingData.get(MLRenderingData::PR_SOLID, attributes));
    QVERIFY(attributes[MLRenderingData::ATT_NAMES::ATT_VERTPOSITION]);
    QVERIFY(attributes[MLRenderingData::ATT_NAMES::ATT_VERTNORMAL] ||
            attributes[MLRenderingData::ATT_NAMES::ATT_FACENORMAL]);
    QVERIFY(!attributes[MLRenderingData::ATT_NAMES::ATT_VERTCOLOR]);
    QCOMPARE(
        attributes[MLRenderingData::ATT_NAMES::ATT_FACECOLOR],
        faceColor);
    QVERIFY(!attributes[MLRenderingData::ATT_NAMES::ATT_VERTTEXTURE]);
    QVERIFY(!attributes[MLRenderingData::ATT_NAMES::ATT_WEDGETEXTURE]);
}

void verifySolidUsesVertexColors(const MLRenderingData& renderingData)
{
    MLRenderingData::RendAtts attributes;
    QVERIFY(renderingData.get(MLRenderingData::PR_SOLID, attributes));
    QVERIFY(attributes[MLRenderingData::ATT_NAMES::ATT_VERTPOSITION]);
    QVERIFY(attributes[MLRenderingData::ATT_NAMES::ATT_VERTNORMAL] ||
            attributes[MLRenderingData::ATT_NAMES::ATT_FACENORMAL]);
    QVERIFY(attributes[MLRenderingData::ATT_NAMES::ATT_VERTCOLOR]);
    QVERIFY(!attributes[MLRenderingData::ATT_NAMES::ATT_FACECOLOR]);
    QVERIFY(!attributes[MLRenderingData::ATT_NAMES::ATT_VERTTEXTURE]);
    QVERIFY(!attributes[MLRenderingData::ATT_NAMES::ATT_WEDGETEXTURE]);
}

struct RenderingSignature
{
    bool solidAvailable = false;
    bool vertexPosition = false;
    bool vertexNormal = false;
    bool faceNormal = false;
    bool vertexColor = false;
    bool faceColor = false;
    bool vertexTexture = false;
    bool wedgeTexture = false;
    bool fixedColorEnabled = false;
    bool meshColorEnabled = false;
    vcg::Color4b fixedColor;
};

RenderingSignature renderingSignature(const MLRenderingData& renderingData)
{
    RenderingSignature signature;
    MLRenderingData::RendAtts attributes;
    signature.solidAvailable =
        renderingData.get(MLRenderingData::PR_SOLID, attributes);
    if (signature.solidAvailable) {
        signature.vertexPosition =
            attributes[MLRenderingData::ATT_NAMES::ATT_VERTPOSITION];
        signature.vertexNormal =
            attributes[MLRenderingData::ATT_NAMES::ATT_VERTNORMAL];
        signature.faceNormal =
            attributes[MLRenderingData::ATT_NAMES::ATT_FACENORMAL];
        signature.vertexColor =
            attributes[MLRenderingData::ATT_NAMES::ATT_VERTCOLOR];
        signature.faceColor =
            attributes[MLRenderingData::ATT_NAMES::ATT_FACECOLOR];
        signature.vertexTexture =
            attributes[MLRenderingData::ATT_NAMES::ATT_VERTTEXTURE];
        signature.wedgeTexture =
            attributes[MLRenderingData::ATT_NAMES::ATT_WEDGETEXTURE];
    }
    MLPerViewGLOptions options;
    if (renderingData.get(options)) {
        signature.fixedColorEnabled =
            options._persolid_fixed_color_enabled;
        signature.meshColorEnabled =
            options._persolid_mesh_color_enabled;
        signature.fixedColor = options._persolid_fixed_color;
    }
    return signature;
}

void verifySameRenderingSignature(
    const RenderingSignature& actual,
    const RenderingSignature& expected)
{
    QCOMPARE(actual.solidAvailable, expected.solidAvailable);
    QCOMPARE(actual.vertexPosition, expected.vertexPosition);
    QCOMPARE(actual.vertexNormal, expected.vertexNormal);
    QCOMPARE(actual.faceNormal, expected.faceNormal);
    QCOMPARE(actual.vertexColor, expected.vertexColor);
    QCOMPARE(actual.faceColor, expected.faceColor);
    QCOMPARE(actual.vertexTexture, expected.vertexTexture);
    QCOMPARE(actual.wedgeTexture, expected.wedgeTexture);
    QCOMPARE(actual.fixedColorEnabled, expected.fixedColorEnabled);
    QCOMPARE(actual.meshColorEnabled, expected.meshColorEnabled);
    QCOMPARE(actual.fixedColor, expected.fixedColor);
}

class ThrowOnSecondPresentationPublisher final : public IRenderPresentationPublisher
{
public:
    void arm()
    {
        recording_ = true;
        throwPending_ = true;
        armedCallCount_ = 0;
        modelIds_.clear();
        uploads_.clear();
    }

    void publish(
        MLSceneGLSharedDataContext& sharedContext,
        int modelId,
        const MLRenderingData& renderingData,
        RenderPresentationUpload upload) override
    {
        sharedContext.setRenderingDataPerAllMeshViews(modelId, renderingData);
        if (upload.faceColorsChanged || upload.vertexColorsChanged) {
            MLRenderingData::RendAtts changed;
            changed[MLRenderingData::ATT_NAMES::ATT_FACECOLOR] =
                upload.faceColorsChanged;
            changed[MLRenderingData::ATT_NAMES::ATT_VERTCOLOR] =
                upload.vertexColorsChanged;
            sharedContext.meshAttributesUpdated(modelId, false, changed);
        }
        sharedContext.manageBuffers(modelId);
        latestSignatures_.insert(modelId, renderingSignature(renderingData));
        if (!recording_)
            return;
        modelIds_.append(modelId);
        uploads_.append(upload);
        ++armedCallCount_;
        if (throwPending_ && armedCallCount_ == 2) {
            throwPending_ = false;
            throw std::runtime_error("scripted second presentation publish failure");
        }
    }

    const QVector<int>& modelIds() const { return modelIds_; }
    const QVector<RenderPresentationUpload>& uploads() const { return uploads_; }
    RenderingSignature latestSignature(int modelId) const
    {
        return latestSignatures_.value(modelId);
    }

private:
    bool recording_ = false;
    bool throwPending_ = false;
    int armedCallCount_ = 0;
    QVector<int> modelIds_;
    QVector<RenderPresentationUpload> uploads_;
    QHash<int, RenderingSignature> latestSignatures_;
};
} // namespace

class MeshLabRendererAdapterTest : public QObject
{
    Q_OBJECT

private slots:
    void failedPreparationKeepsCommittedScene()
    {
        FakeViewportFactory factory;
        QWidget host;
        MeshLabRendererAdapter adapter(factory);
        QVERIFY(adapter.mount(&host).ok);
        FakeResourceProvider resources;

        QVERIFY2(adapter.prepareScene(scene(1, {1, 2}), resources).ok, "first scene prepares");
        adapter.commitPreparedScene();
        QCOMPARE(adapter.committedGeneration(), quint64(1));

        factory.failNextCreation(QStringLiteral("OpenGL context unavailable"));
        const OperationResult result = adapter.prepareScene(scene(2, {3, 4}), resources);

        QVERIFY(!result.ok);
        QCOMPARE(result.error, QStringLiteral("OpenGL context unavailable"));
        QCOMPARE(adapter.committedGeneration(), quint64(1));
    }

    void delayedViewportInitializationFailureKeepsCommittedScene()
    {
        FakeViewportFactory factory;
        QWidget host;
        MeshLabRendererAdapter adapter(factory);
        QVERIFY(adapter.mount(&host).ok);
        FakeResourceProvider resources;

        QVERIFY(adapter.prepareScene(scene(1, {1, 2}), resources).ok);
        adapter.commitPreparedScene();
        factory.clearLifecycle();

        factory.failNextInitialization(QStringLiteral("OpenGL context became unavailable"));
        const OperationResult result = adapter.prepareScene(scene(2, {3, 4}), resources);

        QVERIFY(!result.ok);
        QCOMPARE(result.error, QStringLiteral("OpenGL context became unavailable"));
        QCOMPARE(factory.initializationCount(), 3);
        QCOMPARE(adapter.preparedGeneration(), quint64(0));
        QCOMPARE(adapter.committedGeneration(), quint64(1));
        QCOMPARE(factory.lifecycle(), QStringList({
                                        QStringLiteral("viewports:destroy"),
                                        QStringLiteral("context:destroy")}));
    }

    void clearDestroysViewportsBeforeSharedContext()
    {
        FakeViewportFactory factory;
        QWidget host;
        MeshLabRendererAdapter adapter(factory);
        QVERIFY(adapter.mount(&host).ok);
        FakeResourceProvider resources;

        QVERIFY(adapter.prepareScene(scene(1, {1, 2}), resources).ok);
        adapter.commitPreparedScene();
        factory.clearLifecycle();

        adapter.clearScene();

        QCOMPARE(factory.lifecycle(), QStringList({
                                        QStringLiteral("viewports:destroy"),
                                        QStringLiteral("viewports:destroy"),
                                        QStringLiteral("context:destroy")}));
    }

    void discardingPreparedSceneKeepsCommittedScene()
    {
        FakeViewportFactory factory;
        QWidget host;
        MeshLabRendererAdapter adapter(factory);
        QVERIFY(adapter.mount(&host).ok);
        FakeResourceProvider resources;

        QVERIFY(adapter.prepareScene(scene(1, {1, 2}), resources).ok);
        adapter.commitPreparedScene();
        QVERIFY(adapter.prepareScene(scene(2, {3, 4}), resources).ok);

        adapter.discardPreparedScene();

        QCOMPARE(adapter.committedGeneration(), quint64(1));
    }

    void missingResourceKeepsCommittedScene()
    {
        FakeViewportFactory factory;
        QWidget host;
        MeshLabRendererAdapter adapter(factory);
        QVERIFY(adapter.mount(&host).ok);
        FakeResourceProvider resources;

        QVERIFY(adapter.prepareScene(scene(1, {1, 2}), resources).ok);
        adapter.commitPreparedScene();
        resources.remove(3);

        const OperationResult result = adapter.prepareScene(scene(2, {3, 4}), resources);

        QVERIFY(!result.ok);
        QCOMPARE(adapter.committedGeneration(), quint64(1));
    }

    void createsOneViewportForEverySceneMesh()
    {
        FakeViewportFactory factory;
        QWidget host;
        MeshLabRendererAdapter adapter(factory);
        QVERIFY(adapter.mount(&host).ok);
        FakeResourceProvider resources;

        QVERIFY(adapter.prepareScene(scene(1, {1, 2, 3}), resources).ok);

        QCOMPARE(factory.creationCount(), 3);
        QCOMPARE(factory.initializationCount(), 3);
        QCOMPARE(factory.viewportCount(), 3);
        QVERIFY(factory.lastParent() != static_cast<QWidget*>(&host));
        QVERIFY(host.isAncestorOf(factory.lastParent()));
    }

    void preparedLayoutRestoresItsPresentationCameraAndAnalysisLabels()
    {
        FakeViewportFactory factory;
        QWidget host;
        MeshLabRendererAdapter adapter(factory);
        QVERIFY(adapter.mount(&host).ok);
        FakeResourceProvider resources;
        SceneDescriptor descriptor = scene(1, {1, 2});
        descriptor.initialCamera = {QStringLiteral("<camera-state/>")};
        descriptor.meshes[1].presentation =
            uniformPresentation(QColor(QStringLiteral("#3d7eff")));
        descriptor.meshes[1].analysisLabel = QStringLiteral("P 0.942");

        const OperationResult result =
            adapter.prepareScene(descriptor, resources);

        QVERIFY2(result.ok, qPrintable(result.error));
        QCOMPARE(
            factory.viewport(0).camera().viewStateXml,
            QStringLiteral("<camera-state/>"));
        QCOMPARE(
            factory.viewport(1).camera().viewStateXml,
            QStringLiteral("<camera-state/>"));
        QCOMPARE(factory.viewport(0).scoreLabel(), QString());
        QCOMPARE(
            factory.viewport(1).scoreLabel(),
            QStringLiteral("P 0.942"));
        QCOMPARE(factory.viewport(0).repaintCount(), 1);
        QCOMPARE(factory.viewport(1).repaintCount(), 1);
    }

    void preparedOverlayRestoresHiddenLayers()
    {
        FakeViewportFactory factory;
        QWidget host;
        MeshLabRendererAdapter adapter(factory);
        QVERIFY(adapter.mount(&host).ok);
        FakeResourceProvider resources;
        SceneDescriptor descriptor = scene(1, {1, 2});
        descriptor.layoutMode = SceneLayoutMode::Overlay;
        descriptor.meshes[1].visible = false;
        descriptor.meshes[1].analysisLabel =
            QStringLiteral("P 0.942");

        const OperationResult result =
            adapter.prepareScene(descriptor, resources);

        QVERIFY2(result.ok, qPrintable(result.error));
        QCOMPARE(factory.viewportCount(), 1);
        QCOMPARE(factory.viewport(0).hiddenMeshCount(), 1);
        QCOMPARE(
            factory.viewport(0).label(),
            QStringLiteral("Overlay · 1/2 visible"));
        QCOMPARE(factory.viewport(0).scoreLabel(), QString());
    }

    void viewportActivationEmitsTheMappedMeshId()
    {
        FakeViewportFactory factory;
        QWidget host;
        MeshLabRendererAdapter adapter(factory);
        QVERIFY(adapter.mount(&host).ok);
        FakeResourceProvider resources;
        MeshId selectedMeshId = 0;
        RendererEvents events;
        events.selectedMeshChanged = [&](MeshId id) { selectedMeshId = id; };
        adapter.setEvents(events);
        QVERIFY(adapter.prepareScene(scene(1, {1, 2, 3}), resources).ok);
        adapter.commitPreparedScene();

        factory.viewport(1).emitActivated();

        QCOMPARE(selectedMeshId, MeshId(2));
        QVERIFY(!factory.viewport(0).selected());
        QVERIFY(factory.viewport(1).selected());
        QVERIFY(!factory.viewport(2).selected());
    }

    void referenceBadgeUpdatesWithoutRecreatingViewports()
    {
        FakeViewportFactory factory;
        QWidget host;
        MeshLabRendererAdapter adapter(factory);
        QVERIFY(adapter.mount(&host).ok);
        FakeResourceProvider resources;
        QVERIFY(adapter.prepareScene(scene(1, {1, 2}), resources).ok);
        adapter.commitPreparedScene();
        const int creationCount = factory.creationCount();

        adapter.setReferenceMesh(2);

        QCOMPARE(factory.creationCount(), creationCount);
        QVERIFY(!factory.viewport(0).reference());
        QVERIFY(factory.viewport(1).reference());
    }

    void diagnosticsApplyToCurrentAndFutureViewportGrids()
    {
        FakeViewportFactory factory;
        QWidget host;
        MeshLabRendererAdapter adapter(factory);
        QVERIFY(adapter.mount(&host).ok);
        FakeResourceProvider resources;

        adapter.setDiagnostic(DiagnosticFlag::Orthographic, true);
        QVERIFY(adapter.prepareScene(scene(1, {1, 2}), resources).ok);
        adapter.commitPreparedScene();
        adapter.setDiagnostic(DiagnosticFlag::Wireframe, true);
        adapter.setDiagnostic(DiagnosticFlag::Normals, true);
        adapter.setDiagnostic(DiagnosticFlag::Wireframe, false);

        for (int index = 0; index < 2; ++index) {
            QVERIFY(factory.viewport(index).diagnosticEnabled(
                DiagnosticFlag::Orthographic));
            QVERIFY(!factory.viewport(index).diagnosticEnabled(
                DiagnosticFlag::Wireframe));
            QVERIFY(factory.viewport(index).diagnosticEnabled(
                DiagnosticFlag::Normals));
        }

        QVERIFY(adapter.prepareScene(scene(2, {3, 4}), resources).ok);
        for (int index = 2; index < 4; ++index) {
            QVERIFY(factory.viewport(index).diagnosticEnabled(
                DiagnosticFlag::Orthographic));
            QVERIFY(!factory.viewport(index).diagnosticEnabled(
                DiagnosticFlag::Wireframe));
            QVERIFY(factory.viewport(index).diagnosticEnabled(
                DiagnosticFlag::Normals));
        }
    }

    void analysisOverlayBatchValidatesBeforeUpdatingExistingViewports()
    {
        FakeViewportFactory factory;
        QWidget host;
        MeshLabRendererAdapter adapter(factory);
        QVERIFY(adapter.mount(&host).ok);
        FakeResourceProvider resources;

        QVERIFY(adapter.prepareScene(scene(1, {1, 2, 3}), resources).ok);
        adapter.commitPreparedScene();
        const int creationCount = factory.creationCount();

        QVERIFY(adapter.setAnalysisOverlays(
                    {{2, QStringLiteral("37%")},
                     {3, QStringLiteral("P 0.942")}})
                    .ok);
        QCOMPARE(factory.viewport(0).scoreLabel(), QString());
        QCOMPARE(factory.viewport(1).scoreLabel(), QStringLiteral("37%"));
        QCOMPARE(factory.viewport(2).scoreLabel(), QStringLiteral("P 0.942"));

        QVERIFY(!adapter.setAnalysisOverlays(
                     {{2, QStringLiteral("90%")},
                      {99, QStringLiteral("N 0.917")}})
                     .ok);
        QVERIFY(!adapter.setAnalysisOverlays(
                     {{2, QStringLiteral("90%")},
                      {2, QString()}})
                     .ok);

        QCOMPARE(factory.creationCount(), creationCount);
        QCOMPARE(factory.viewport(0).scoreLabel(), QString());
        QCOMPARE(factory.viewport(1).scoreLabel(), QStringLiteral("37%"));
        QCOMPARE(factory.viewport(2).scoreLabel(), QStringLiteral("P 0.942"));

        QVERIFY(adapter.setAnalysisOverlays(
                    {{2, QString()},
                     {3, QStringLiteral("N 0.917")}})
                    .ok);
        QCOMPARE(factory.creationCount(), creationCount);
        QCOMPARE(factory.viewport(1).scoreLabel(), QString());
        QCOMPARE(factory.viewport(2).scoreLabel(), QStringLiteral("N 0.917"));
    }

    void analysisOverlaysRequireACommittedScene()
    {
        FakeViewportFactory factory;
        MeshLabRendererAdapter adapter(factory);

        const OperationResult result = adapter.setAnalysisOverlays(
            {{1, QStringLiteral("10%")}});

        QVERIFY(!result.ok);
        QCOMPARE(
            result.error,
            QStringLiteral("No committed scene is available for analysis overlays."));
    }

    void failureInALaterViewportKeepsTheCommittedGeneration()
    {
        FakeViewportFactory factory;
        QWidget host;
        MeshLabRendererAdapter adapter(factory);
        QVERIFY(adapter.mount(&host).ok);
        FakeResourceProvider resources;

        QVERIFY(adapter.prepareScene(scene(1, {1, 2}), resources).ok);
        adapter.commitPreparedScene();
        factory.clearLifecycle();
        factory.failInitializationAfter(
            1,
            QStringLiteral("second candidate viewport failed"));

        const OperationResult result = adapter.prepareScene(scene(2, {3, 4, 5}), resources);

        QVERIFY(!result.ok);
        QCOMPARE(result.error, QStringLiteral("second candidate viewport failed"));
        QCOMPARE(adapter.preparedGeneration(), quint64(0));
        QCOMPARE(adapter.committedGeneration(), quint64(1));
        QCOMPARE(factory.lifecycle(), QStringList({
                                        QStringLiteral("viewports:destroy"),
                                        QStringLiteral("viewports:destroy"),
                                        QStringLiteral("context:destroy")}));
    }

    void preparationRequiresMountedViewportHost()
    {
        FakeViewportFactory factory;
        MeshLabRendererAdapter adapter(factory);
        FakeResourceProvider resources;

        const OperationResult result = adapter.prepareScene(scene(1, {1, 2}), resources);

        QVERIFY(!result.ok);
        QCOMPARE(
            result.error,
            QStringLiteral("A viewport host must be mounted before preparing a MeshLab scene."));
        QCOMPARE(factory.creationCount(), 0);
    }

    void concreteFactoryRegistersRenderingBeforeCommit()
    {
        QWidget host;
        FakeResourceProvider resources;
        const SceneDescriptor descriptor = scene(1, {1, 2});
        RenderSceneContext context;
        QVERIFY(context.addResources(descriptor, resources).ok);

        RichParameterList settings;
        FakeViewportCallbacks callbacks;
        const int meshModelId = context.modelIdFor(1);
        ViewportDependencies dependencies{
            context.document(),
            context.sharedContext(),
            settings,
            callbacks,
            1,
            meshModelId,
            1,
            1,
            QStringLiteral("mesh-1"),
            true,
            QString()};
        MeshLabViewportFactory factory;
        std::unique_ptr<IViewport> viewport;

        QVERIFY2(
            factory.createViewport(&host, dependencies, viewport).ok,
            "concrete viewport preparation succeeds");
        const OperationResult initialization = viewport->initializeForScenePreparation();
        QVERIFY2(initialization.ok, qPrintable(initialization.error));
        QVERIFY(viewport->initializeForScenePreparation().ok);
        auto* glWidget = dynamic_cast<QGLWidget*>(viewport->widget());
        QVERIFY(glWidget != nullptr);
        QVERIFY(glWidget->context() != nullptr);

        MLRenderingData renderingData;
        MLPerViewGLOptions options;
        context.sharedContext().getRenderInfoPerMeshView(
            meshModelId,
            glWidget->context(),
            renderingData);
        QVERIFY(renderingData.get(options));
    }

    void presentationValidationRejectsTheWholeBatchWithoutMutation()
    {
        FakeResourceProvider resources;
        RenderSceneContext context;
        QVERIFY(context.addResources(scene(1, {1, 2}), resources).ok);
        const QColor original(QStringLiteral("#5aaa75"));
        QVERIFY(context.setColorPresentations(
                    {{1, uniformPresentation(original)}}).ok);

        const ColorPresentation replacement =
            uniformPresentation(QColor(QStringLiteral("#d65c5c")));
        QVERIFY(!context.setColorPresentations({}).ok);
        QVERIFY(!context.setColorPresentations(
                     {{1, replacement}, {99, replacement}}).ok);
        QVERIFY(!context.setColorPresentations(
                     {{1, replacement}, {1, replacement}}).ok);
        QVERIFY(!context.setColorPresentations(
                     {{1, analysisPresentation(
                              ColorMode::PrecisionResult,
                              QColor(Qt::red))},
                      {2,
                       ColorPresentation{
                           ColorMode::NormalAgreementResult,
                           {},
                           {QColor(Qt::red), QColor(Qt::blue)}}}}).ok);
        ColorPresentation invalidMode;
        invalidMode.mode = static_cast<ColorMode>(99);
        QVERIFY(!context.setColorPresentations({{1, invalidMode}}).ok);
        ColorPresentation invalidAnalysis;
        invalidAnalysis.mode = ColorMode::PrecisionResult;
        invalidAnalysis.faceColors = {QColor()};
        QVERIFY(!context.setColorPresentations({{1, invalidAnalysis}}).ok);
        ColorPresentation wrongVertexCount = vertexAnalysisPresentation(
            ColorMode::PrecisionResult,
            {QColor(Qt::red), QColor(Qt::green)});
        QVERIFY(!context
                     .setColorPresentations({{1, wrongVertexCount}})
                     .ok);
        ColorPresentation bothColorDomains = analysisPresentation(
            ColorMode::PrecisionResult,
            QColor(Qt::red));
        bothColorDomains.vertexColors = {
            QColor(Qt::red), QColor(Qt::green), QColor(Qt::blue)};
        QVERIFY(!context
                     .setColorPresentations({{1, bothColorDomains}})
                     .ok);
        ColorPresentation invalidVertex = vertexAnalysisPresentation(
            ColorMode::PrecisionResult,
            {QColor(Qt::red), QColor(), QColor(Qt::blue)});
        QVERIFY(!context
                     .setColorPresentations({{1, invalidVertex}})
                     .ok);
        ColorPresentation invalidUniform;
        invalidUniform.mode = ColorMode::UniformColor;
        QVERIFY(!context.setColorPresentations({{1, invalidUniform}}).ok);
        ColorPresentation strayUniform = replacement;
        strayUniform.faceColors = {QColor(Qt::blue)};
        QVERIFY(!context.setColorPresentations({{1, strayUniform}}).ok);
        strayUniform.faceColors.clear();
        strayUniform.vertexColors = {QColor(Qt::blue)};
        QVERIFY(!context.setColorPresentations({{1, strayUniform}}).ok);
        ColorPresentation strayDefault;
        strayDefault.uniformColor = QColor(Qt::red);
        QVERIFY(!context.setColorPresentations({{1, strayDefault}}).ok);
        strayDefault.uniformColor = {};
        strayDefault.vertexColors = {QColor(Qt::red)};
        QVERIFY(!context.setColorPresentations({{1, strayDefault}}).ok);

        const ColorPresentation* committed = context.colorPresentation(1);
        QVERIFY(committed != nullptr);
        QCOMPARE(committed->mode, ColorMode::UniformColor);
        QCOMPARE(committed->uniformColor, original);
        QVERIFY(!modelFor(context, 1)->hasDataMask(MeshModel::MM_FACECOLOR));
        const MLRenderingData* renderingData = context.currentRenderingData(1);
        QVERIFY(renderingData != nullptr);
        MLPerViewGLOptions options;
        QVERIFY(renderingData->get(options));
        QVERIFY(options._persolid_fixed_color_enabled);
        QCOMPARE(int(options._persolid_fixed_color[0]), original.red());
        QCOMPARE(int(options._persolid_fixed_color[1]), original.green());
        QCOMPARE(int(options._persolid_fixed_color[2]), original.blue());
    }

    void uniformPresentationUsesExactFixedSolidColor()
    {
        FakeResourceProvider resources;
        RenderSceneContext context;
        QVERIFY(context.addResources(scene(1, {1, 2}), resources).ok);
        const QColor color(QStringLiteral("#5aaa75"));
        QVERIFY(context.setColorPresentations(
                    {{2,
                      analysisPresentation(
                          ColorMode::PrecisionResult,
                          QColor(Qt::red))}}).ok);
        QVERIFY(modelFor(context, 2)->hasDataMask(MeshModel::MM_FACECOLOR));

        QVERIFY(context.setColorPresentations(
                    {{2, uniformPresentation(color)}}).ok);

        const ColorPresentation* committed = context.colorPresentation(2);
        QVERIFY(committed != nullptr);
        QCOMPARE(committed->mode, ColorMode::UniformColor);
        QCOMPARE(committed->uniformColor, color);
        QVERIFY(!modelFor(context, 2)->hasDataMask(MeshModel::MM_FACECOLOR));
        const MLRenderingData* renderingData = context.currentRenderingData(2);
        QVERIFY(renderingData != nullptr);
        verifySolidUsesOnlyExpectedColor(*renderingData, false);
        MLPerViewGLOptions options;
        QVERIFY(renderingData->get(options));
        QVERIFY(options._persolid_fixed_color_enabled);
        QVERIFY(!options._persolid_mesh_color_enabled);
        QCOMPARE(int(options._persolid_fixed_color[0]), color.red());
        QCOMPARE(int(options._persolid_fixed_color[1]), color.green());
        QCOMPARE(int(options._persolid_fixed_color[2]), color.blue());
        QCOMPARE(int(options._persolid_fixed_color[3]), color.alpha());
    }

    void sourceVertexColorsDoNotChangeTheDefaultLightGrayPresentation()
    {
        FakeResourceProvider resources(true);
        RenderSceneContext context;
        QVERIFY(context.addResources(scene(1, {1, 2}), resources).ok);

        MeshModel* model = modelFor(context, 2);
        QVERIFY(model != nullptr);
        QVERIFY(!model->hasDataMask(MeshModel::MM_VERTCOLOR));
        const MLRenderingData* renderingData =
            context.currentRenderingData(2);
        QVERIFY(renderingData != nullptr);
        verifySolidUsesOnlyExpectedColor(*renderingData, false);
        MLPerViewGLOptions options;
        QVERIFY(renderingData->get(options));
        QVERIFY(options._persolid_fixed_color_enabled);
        QCOMPARE(
            options._persolid_fixed_color,
            vcg::Color4b(vcg::Color4b::LightGray));
    }

    void precisionAndNormalPresentationsUseRendererPrivateFaceColors()
    {
        FakeResourceProvider resources;
        RenderSceneContext context;
        QVERIFY(context.addResources(scene(1, {1, 2}), resources).ok);
        const QColor precisionColor(QStringLiteral("#d65c5c"));
        const QColor normalColor(QStringLiteral("#5a78d6"));

        QVERIFY(context.setColorPresentations(
                    {{1,
                      analysisPresentation(
                          ColorMode::PrecisionResult,
                          precisionColor)},
                     {2,
                      analysisPresentation(
                          ColorMode::NormalAgreementResult,
                          normalColor)}}).ok);

        for (const auto& expected : {
                 qMakePair(MeshId(1), precisionColor),
                 qMakePair(MeshId(2), normalColor)}) {
            MeshModel* model = modelFor(context, expected.first);
            QVERIFY(model->hasDataMask(MeshModel::MM_FACECOLOR));
            QCOMPARE(firstLiveFaceColor(*model), expected.second);
            const MLRenderingData* renderingData =
                context.currentRenderingData(expected.first);
            QVERIFY(renderingData != nullptr);
            verifySolidUsesOnlyExpectedColor(*renderingData, true);
            MLPerViewGLOptions options;
            QVERIFY(renderingData->get(options));
            QVERIFY(!options._persolid_fixed_color_enabled);
            QVERIFY(!options._persolid_mesh_color_enabled);
        }
        QCOMPARE(
            context.colorPresentation(1)->mode,
            ColorMode::PrecisionResult);
        QCOMPARE(
            context.colorPresentation(2)->mode,
            ColorMode::NormalAgreementResult);
    }

    void analysisPresentationUsesRendererPrivateVertexColors()
    {
        FakeResourceProvider resources;
        RenderSceneContext context;
        QVERIFY(context.addResources(scene(1, {1, 2}), resources).ok);
        const QVector<QColor> expected = {
            QColor(QStringLiteral("#d65c5c")),
            QColor(QStringLiteral("#5aaa75")),
            QColor(QStringLiteral("#5a78d6"))};

        QVERIFY(context
                    .setColorPresentations(
                        {{2,
                          vertexAnalysisPresentation(
                              ColorMode::VertexColor,
                              expected)}})
                    .ok);

        MeshModel* model = modelFor(context, 2);
        QVERIFY(model != nullptr);
        QVERIFY(model->hasDataMask(MeshModel::MM_VERTCOLOR));
        QVERIFY(!model->hasDataMask(MeshModel::MM_FACECOLOR));
        QCOMPARE(liveVertexColors(*model), expected);
        const MLRenderingData* renderingData =
            context.currentRenderingData(2);
        QVERIFY(renderingData != nullptr);
        verifySolidUsesVertexColors(*renderingData);
        MLPerViewGLOptions options;
        QVERIFY(renderingData->get(options));
        QVERIFY(!options._persolid_fixed_color_enabled);
        QVERIFY(!options._persolid_mesh_color_enabled);
        QCOMPARE(
            context.colorPresentation(2)->vertexColors,
            expected);
    }

    void defaultAndUniformPresentationsRemoveTransientVertexColors()
    {
        FakeResourceProvider resources;
        RenderSceneContext context;
        QVERIFY(context.addResources(scene(1, {1, 2}), resources).ok);
        MeshModel* model = modelFor(context, 2);
        QVERIFY(model != nullptr);
        QVERIFY(!model->hasDataMask(MeshModel::MM_VERTCOLOR));
        const RenderingSignature originalDefault =
            renderingSignature(*context.currentRenderingData(2));
        const ColorPresentation vertexColors =
            vertexAnalysisPresentation(
                ColorMode::VertexColor,
                {QColor(Qt::red), QColor(Qt::green), QColor(Qt::blue)});
        QVERIFY(context.setColorPresentations({{2, vertexColors}}).ok);
        QVERIFY(model->hasDataMask(MeshModel::MM_VERTCOLOR));

        QVERIFY(context
                    .setColorPresentations({{2, ColorPresentation{}}})
                    .ok);

        QVERIFY(!model->hasDataMask(MeshModel::MM_VERTCOLOR));
        QCOMPARE(context.colorPresentation(2)->mode, ColorMode::Default);
        verifySameRenderingSignature(
            renderingSignature(*context.currentRenderingData(2)),
            originalDefault);

        QVERIFY(context.setColorPresentations({{2, vertexColors}}).ok);
        const QColor fixed(QStringLiteral("#5aaa75"));
        QVERIFY(context
                    .setColorPresentations(
                        {{2, uniformPresentation(fixed)}})
                    .ok);

        QVERIFY(!model->hasDataMask(MeshModel::MM_VERTCOLOR));
        const MLRenderingData* uniformRendering =
            context.currentRenderingData(2);
        QVERIFY(uniformRendering != nullptr);
        verifySolidUsesOnlyExpectedColor(*uniformRendering, false);
        MLPerViewGLOptions options;
        QVERIFY(uniformRendering->get(options));
        QVERIFY(options._persolid_fixed_color_enabled);
        QCOMPARE(
            options._persolid_fixed_color,
            vcg::Color4b(
                fixed.red(), fixed.green(), fixed.blue(), fixed.alpha()));
    }

    void defaultPresentationRemovesTransientFaceColorAndRestoresDefaultOptions()
    {
        FakeResourceProvider resources;
        RenderSceneContext context;
        QVERIFY(context.addResources(scene(1, {1, 2}), resources).ok);
        const MLRenderingData* originalRendering =
            context.currentRenderingData(2);
        QVERIFY(originalRendering != nullptr);
        const RenderingSignature originalDefault =
            renderingSignature(*originalRendering);
        QVERIFY(context.setColorPresentations(
                    {{2,
                      analysisPresentation(
                          ColorMode::PrecisionResult,
                          QColor(Qt::red))}}).ok);
        QVERIFY(modelFor(context, 2)->hasDataMask(MeshModel::MM_FACECOLOR));

        QVERIFY(context.setColorPresentations(
                    {{2, ColorPresentation{}}}).ok);

        QVERIFY(!modelFor(context, 2)->hasDataMask(MeshModel::MM_FACECOLOR));
        QCOMPARE(context.colorPresentation(2)->mode, ColorMode::Default);
        const MLRenderingData* renderingData = context.currentRenderingData(2);
        QVERIFY(renderingData != nullptr);
        verifySameRenderingSignature(
            renderingSignature(*renderingData), originalDefault);
        verifySolidUsesOnlyExpectedColor(*renderingData, false);
        MLPerViewGLOptions options;
        QVERIFY(renderingData->get(options));
        QVERIFY(options._persolid_fixed_color_enabled);
        QVERIFY(!options._persolid_mesh_color_enabled);
        const vcg::Color4b expected(vcg::Color4b::LightGray);
        QCOMPARE(options._persolid_fixed_color, expected);
    }

    void rollbackCleanupRepairsHalfEnabledOptionalFaceColors()
    {
        FakeResourceProvider resources;
        RenderSceneContext context;
        QVERIFY(context.addResources(scene(1, {1, 2}), resources).ok);
        MeshModel* model = modelFor(context, 2);
        QVERIFY(model != nullptr);
        QVERIFY(!model->hasDataMask(MeshModel::MM_FACECOLOR));
        QVERIFY(!model->cm.face.IsColorEnabled());

        // Reproduce the state EnableColor() can leave if CV.resize() throws:
        // the OCF flag is set before MeshModel ORs MM_FACECOLOR.
        model->cm.face.EnableColor();
        QVERIFY(model->cm.face.IsColorEnabled());
        QVERIFY(!model->hasDataMask(MeshModel::MM_FACECOLOR));

        MeshCompareRenderDetail::ensureFaceColorDisabled(*model);

        QVERIFY(!model->cm.face.IsColorEnabled());
        QVERIFY(!model->hasDataMask(MeshModel::MM_FACECOLOR));
        QVERIFY(model->cm.face.CV.empty());
    }

    void secondPublishExceptionRollsBackCurrentAndPreviouslyTouchedMeshes()
    {
        FakeResourceProvider resources;
        ThrowOnSecondPresentationPublisher publisher;
        RenderSceneSettings settings;
        RenderSceneContext context(settings, publisher);
        QVERIFY(context.addResources(scene(1, {1, 2}), resources).ok);
        const QColor originalUniform(QStringLiteral("#5aaa75"));
        const QColor originalFace(QStringLiteral("#e7b45a"));
        QVERIFY(context.setColorPresentations(
                    {{1, uniformPresentation(originalUniform)},
                     {2,
                      analysisPresentation(
                          ColorMode::PrecisionResult,
                          originalFace)}}).ok);
        MeshModel* originalMesh1 = modelFor(context, 1);
        MeshModel* originalMesh2 = modelFor(context, 2);
        QVERIFY(originalMesh1 != nullptr);
        QVERIFY(originalMesh2 != nullptr);
        QVERIFY(!originalMesh1->cm.face.IsColorEnabled());
        QVERIFY(originalMesh2->cm.face.IsColorEnabled());
        const vcg::Color4b* const originalMesh2ColorStorage =
            originalMesh2->cm.face.CV.data();
        const std::size_t originalMesh2ColorCapacity =
            originalMesh2->cm.face.CV.capacity();
        publisher.arm();

        const OperationResult result = context.setColorPresentations(
            {{1,
              analysisPresentation(
                  ColorMode::NormalAgreementResult,
                  QColor(Qt::red))},
             {2, uniformPresentation(QColor(Qt::blue))}});

        QVERIFY(!result.ok);
        QVERIFY(result.error.contains(QStringLiteral("scripted second")));
        QCOMPARE(context.colorPresentation(1)->mode, ColorMode::UniformColor);
        QCOMPARE(context.colorPresentation(1)->uniformColor, originalUniform);
        QCOMPARE(context.colorPresentation(2)->mode, ColorMode::PrecisionResult);
        QCOMPARE(context.colorPresentation(2)->faceColors,
                 QVector<QColor>({originalFace}));
        QVERIFY(!modelFor(context, 1)->hasDataMask(MeshModel::MM_FACECOLOR));
        QVERIFY(modelFor(context, 2)->hasDataMask(MeshModel::MM_FACECOLOR));
        QVERIFY(!modelFor(context, 1)->cm.face.IsColorEnabled());
        QVERIFY(modelFor(context, 2)->cm.face.IsColorEnabled());
        QCOMPARE(
            modelFor(context, 2)->cm.face.CV.data(),
            originalMesh2ColorStorage);
        QCOMPARE(
            modelFor(context, 2)->cm.face.CV.capacity(),
            originalMesh2ColorCapacity);
        QCOMPARE(firstLiveFaceColor(*modelFor(context, 2)), originalFace);
        const MLRenderingData* mesh1Rendering = context.currentRenderingData(1);
        const MLRenderingData* mesh2Rendering = context.currentRenderingData(2);
        QVERIFY(mesh1Rendering != nullptr);
        QVERIFY(mesh2Rendering != nullptr);
        verifySolidUsesOnlyExpectedColor(*mesh1Rendering, false);
        verifySolidUsesOnlyExpectedColor(*mesh2Rendering, true);
        MLPerViewGLOptions mesh1Options;
        QVERIFY(mesh1Rendering->get(mesh1Options));
        QVERIFY(mesh1Options._persolid_fixed_color_enabled);
        QCOMPARE(int(mesh1Options._persolid_fixed_color[0]), originalUniform.red());
        QCOMPARE(int(mesh1Options._persolid_fixed_color[1]), originalUniform.green());
        QCOMPARE(int(mesh1Options._persolid_fixed_color[2]), originalUniform.blue());
        QCOMPARE(
            publisher.modelIds(),
            QVector<int>({context.modelIdFor(1),
                          context.modelIdFor(2),
                          context.modelIdFor(2),
                          context.modelIdFor(1)}));
        const RenderingSignature publishedMesh1 =
            publisher.latestSignature(context.modelIdFor(1));
        QVERIFY(publishedMesh1.solidAvailable);
        QVERIFY(publishedMesh1.vertexPosition);
        QVERIFY(!publishedMesh1.vertexColor);
        QVERIFY(!publishedMesh1.faceColor);
        QVERIFY(!publishedMesh1.vertexTexture);
        QVERIFY(!publishedMesh1.wedgeTexture);
        QVERIFY(publishedMesh1.fixedColorEnabled);
        QVERIFY(!publishedMesh1.meshColorEnabled);
        QCOMPARE(
            publishedMesh1.fixedColor,
            vcg::Color4b(
                originalUniform.red(),
                originalUniform.green(),
                originalUniform.blue(),
                originalUniform.alpha()));
        const RenderingSignature publishedMesh2 =
            publisher.latestSignature(context.modelIdFor(2));
        QVERIFY(publishedMesh2.solidAvailable);
        QVERIFY(publishedMesh2.vertexPosition);
        QVERIFY(!publishedMesh2.vertexColor);
        QVERIFY(publishedMesh2.faceColor);
        QVERIFY(!publishedMesh2.vertexTexture);
        QVERIFY(!publishedMesh2.wedgeTexture);
        QVERIFY(!publishedMesh2.fixedColorEnabled);
        QVERIFY(!publishedMesh2.meshColorEnabled);
    }

    void secondPublishExceptionRollsBackVertexColorsAndMasksAtomically()
    {
        FakeResourceProvider resources;
        ThrowOnSecondPresentationPublisher publisher;
        RenderSceneSettings settings;
        RenderSceneContext context(settings, publisher);
        QVERIFY(context.addResources(scene(1, {1, 2}), resources).ok);
        const QVector<QColor> originalVertexColors = {
            QColor(QStringLiteral("#d65c5c")),
            QColor(QStringLiteral("#5aaa75")),
            QColor(QStringLiteral("#5a78d6"))};
        const QColor originalUniform(QStringLiteral("#e7b45a"));
        QVERIFY(context
                    .setColorPresentations(
                        {{1,
                          vertexAnalysisPresentation(
                              ColorMode::VertexColor,
                              originalVertexColors)},
                         {2, uniformPresentation(originalUniform)}})
                    .ok);
        const RenderingSignature originalMesh1Rendering =
            renderingSignature(*context.currentRenderingData(1));
        const RenderingSignature originalMesh2Rendering =
            renderingSignature(*context.currentRenderingData(2));
        publisher.arm();

        const OperationResult result = context.setColorPresentations(
            {{1, uniformPresentation(QColor(Qt::white))},
             {2,
              vertexAnalysisPresentation(
                  ColorMode::VertexColor,
                  {QColor(Qt::cyan),
                   QColor(Qt::magenta),
                   QColor(Qt::yellow)})}});

        QVERIFY(!result.ok);
        QVERIFY(result.error.contains(QStringLiteral("scripted second")));
        QCOMPARE(
            context.colorPresentation(1)->vertexColors,
            originalVertexColors);
        QCOMPARE(
            context.colorPresentation(2)->uniformColor,
            originalUniform);
        MeshModel* mesh1 = modelFor(context, 1);
        MeshModel* mesh2 = modelFor(context, 2);
        QVERIFY(mesh1->hasDataMask(MeshModel::MM_VERTCOLOR));
        QVERIFY(!mesh2->hasDataMask(MeshModel::MM_VERTCOLOR));
        QCOMPARE(liveVertexColors(*mesh1), originalVertexColors);
        verifySameRenderingSignature(
            renderingSignature(*context.currentRenderingData(1)),
            originalMesh1Rendering);
        verifySameRenderingSignature(
            renderingSignature(*context.currentRenderingData(2)),
            originalMesh2Rendering);
        QCOMPARE(
            publisher.modelIds(),
            QVector<int>({context.modelIdFor(1),
                          context.modelIdFor(2),
                          context.modelIdFor(2),
                          context.modelIdFor(1)}));
        QCOMPARE(publisher.uploads().size(), 4);
        QVERIFY(publisher.uploads().at(0).vertexColorsChanged);
        QVERIFY(publisher.uploads().at(1).vertexColorsChanged);
        QVERIFY(publisher.uploads().at(2).vertexColorsChanged);
        QVERIFY(publisher.uploads().at(3).vertexColorsChanged);
    }

    void successfulPresentationRepaintsExistingViewportsWithoutRecreation()
    {
        FakeViewportFactory factory;
        QWidget host;
        MeshLabRendererAdapter adapter(factory);
        QVERIFY(adapter.mount(&host).ok);
        FakeResourceProvider resources;
        QVERIFY(adapter.prepareScene(scene(1, {1, 2}), resources).ok);
        adapter.commitPreparedScene();
        const int creationCount = factory.creationCount();

        QVERIFY(adapter.setColorPresentations(
                    {{2, uniformPresentation(QColor(Qt::green))}}).ok);

        QCOMPARE(factory.creationCount(), creationCount);
        QCOMPARE(factory.viewport(0).repaintCount(), 1);
        QCOMPARE(factory.viewport(1).repaintCount(), 1);
        QVERIFY(!adapter.setColorPresentations(
                     {{99, uniformPresentation(QColor(Qt::red))}}).ok);
        QCOMPARE(factory.creationCount(), creationCount);
        QCOMPARE(factory.viewport(0).repaintCount(), 1);
        QCOMPARE(factory.viewport(1).repaintCount(), 1);
    }

    void distanceLegendRestoresAndUpdatesWithColorPresentations()
    {
        FakeViewportFactory factory;
        QWidget host;
        MeshLabRendererAdapter adapter(factory);
        QVERIFY(adapter.mount(&host).ok);
        FakeResourceProvider resources;
        SceneDescriptor descriptor = scene(1, {1, 2});
        descriptor.meshes[1].presentation =
            distancePresentation(0.04, DistanceColorMapping::SquareRoot);

        QVERIFY(adapter.prepareScene(descriptor, resources).ok);
        QCOMPARE(
            factory.viewport(0).colorLegend().kind,
            ColorLegendKind::None);
        QCOMPARE(
            factory.viewport(1).colorLegend(),
            descriptor.meshes[1].presentation.colorLegend);
        adapter.commitPreparedScene();

        QVERIFY(adapter
                    .setColorPresentations(
                        {{2,
                          distancePresentation(
                              0.08,
                              DistanceColorMapping::Linear)}})
                    .ok);
        QCOMPARE(
            factory.viewport(1).colorLegend().distanceMapping,
            DistanceColorMapping::Linear);
        QCOMPARE(factory.viewport(1).colorLegend().maximum, 0.08);
    }

    void vertexPresentationAppliesThroughCommittedRendererInterface()
    {
        FakeViewportFactory factory;
        QWidget host;
        MeshLabRendererAdapter adapter(factory);
        QVERIFY(adapter.mount(&host).ok);
        FakeResourceProvider resources;
        QVERIFY(adapter.prepareScene(scene(1, {1, 2}), resources).ok);
        adapter.commitPreparedScene();

        const OperationResult result = adapter.setColorPresentations(
            {{2,
              vertexAnalysisPresentation(
                  ColorMode::VertexColor,
                  {QColor(Qt::red), QColor(Qt::green), QColor(Qt::blue)})}});

        QVERIFY2(result.ok, qPrintable(result.error));
        QCOMPARE(factory.viewport(0).repaintCount(), 1);
        QCOMPARE(factory.viewport(1).repaintCount(), 1);
    }
};

QTEST_MAIN(MeshLabRendererAdapterTest)
#include "mesh_lab_renderer_adapter_test.moc"
