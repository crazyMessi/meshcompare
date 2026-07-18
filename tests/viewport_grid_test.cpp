#include <QtTest>

#include <functional>
#include <initializer_list>
#include <memory>
#include <vector>

#include <QPointer>
#include <QHash>
#include <QWidget>

#include <common/ml_document/mesh_document.h>
#include <common/ml_shared_data_context/ml_scene_gl_shared_data_context.h>
#include <common/parameters/rich_parameter_list.h>
#include <wrap/qt/qt_thread_safe_memory_info.h>

#include "renderer/meshlab/viewport_grid.h"

namespace
{
CameraPose pose(const QString& value)
{
    return {value};
}

SceneDescriptor scene(int meshCount)
{
    SceneDescriptor descriptor;
    descriptor.generation = 1;
    for (int index = 0; index < meshCount; ++index) {
        const MeshId meshId = static_cast<MeshId>(101 + index);
        descriptor.meshes.append(
            {meshId,
             static_cast<MeshResourceId>(201 + index),
             QStringLiteral("mesh-%1").arg(meshId),
             index == 0});
    }
    if (!descriptor.meshes.isEmpty())
        descriptor.referenceId = descriptor.meshes.front().id;
    return descriptor;
}

ColorPresentation distancePresentation(
    double maximum,
    DistanceColorMapping mapping)
{
    ColorPresentation presentation;
    presentation.mode = ColorMode::VertexColor;
    presentation.colorLegend.kind = ColorLegendKind::Distance;
    presentation.colorLegend.distanceMapping = mapping;
    presentation.colorLegend.maximum = maximum;
    return presentation;
}

class TestRenderScene
{
public:
    TestRenderScene()
        : memoryInfo_(0), sharedContext_(document_, memoryInfo_, false, 1)
    {
    }

    ViewportGridSceneDependencies dependencies()
    {
        return {
            document_,
            sharedContext_,
            settings_,
            [](MeshId meshId) { return static_cast<int>(meshId); }};
    }

private:
    MeshDocument document_;
    vcg::QtThreadSafeMemoryInfo memoryInfo_;
    MLSceneGLSharedDataContext sharedContext_;
    RichParameterList settings_;
};

class RecordingCallbacks final : public IViewportCallbacks
{
public:
    void viewportActivated(int viewportId) override
    {
        ++activationCount;
        activatedViewportId = viewportId;
    }

    void cameraChanged(int viewportId, const CameraPose& value) override
    {
        ++cameraChangeCount;
        cameraViewportId = viewportId;
        cameraPose = value;
    }

    void rendererError(int viewportId, const QString& message) override
    {
        ++errorCount;
        errorViewportId = viewportId;
        errorMessage = message;
    }

    int activationCount = 0;
    int activatedViewportId = -1;
    int cameraChangeCount = 0;
    int cameraViewportId = -1;
    CameraPose cameraPose;
    int errorCount = 0;
    int errorViewportId = -1;
    QString errorMessage;
};

class GridFakeViewport final : public IViewport
{
public:
    GridFakeViewport(
        QWidget* parent,
        IViewportCallbacks& callbacks,
        int viewportId,
        int& liveCount,
        int& initializationCount,
        QString initializationError,
        bool createWidget,
        bool reference)
        : widget_(createWidget ? new QWidget(parent) : nullptr),
          parentAtCreation_(parent),
          callbacks_(callbacks),
          viewportId_(viewportId),
          liveCount_(liveCount),
          initializationCount_(initializationCount),
          initializationError_(std::move(initializationError)),
          reference_(reference)
    {
        ++liveCount_;
    }

    ~GridFakeViewport() override
    {
        delete widget_.data();
        --liveCount_;
    }

    QWidget* widget() const override { return widget_.data(); }
    OperationResult initializeForScenePreparation() override
    {
        ++initializationCount_;
        if (!initializationError_.isEmpty())
            return OperationResult::failure(initializationError_);
        return OperationResult::success();
    }
    CameraPose captureCamera() const override { return restoredPose_; }
    OperationResult restoreCamera(const CameraPose& value) override
    {
        ++restoreCount_;
        if (!nextRestoreError_.isEmpty()) {
            const QString error = nextRestoreError_;
            nextRestoreError_.clear();
            return OperationResult::failure(error);
        }
        restoredPose_ = value;
        if (emitCameraChangedOnRestore_) {
            emitCameraChangedOnRestore_ = false;
            callbacks_.cameraChanged(viewportId_, value);
        }
        return OperationResult::success();
    }
    void resetCamera() override { restoredPose_ = {}; }
    void setLabel(QString label) override { label_ = std::move(label); }
    void setSelected(bool selected) override { selected_ = selected; }
    void setReference(bool reference) override { reference_ = reference; }
    void setMeshVisible(int meshModelId, bool visible) override
    {
        meshVisibility_[meshModelId] = visible;
    }
    void setScoreLabel(QString label) override { scoreLabel_ = std::move(label); }
    void setColorLegend(ColorLegendSpec legend) override
    {
        colorLegend_ = std::move(legend);
    }
    void setDiagnostic(DiagnosticFlag flag, bool enabled) override
    {
        switch (flag) {
        case DiagnosticFlag::Orthographic:
            orthographic_ = enabled;
            break;
        case DiagnosticFlag::Wireframe:
            wireframe_ = enabled;
            break;
        case DiagnosticFlag::Normals:
            normals_ = enabled;
            break;
        }
    }
    void requestRepaint() override { ++repaintCount_; }

    void emitCameraChanged(const CameraPose& value)
    {
        callbacks_.cameraChanged(viewportId_, value);
    }
    void emitActivated() { callbacks_.viewportActivated(viewportId_); }
    QWidget* parentAtCreation() const { return parentAtCreation_; }
    const CameraPose& restoredPose() const { return restoredPose_; }
    int restoreCount() const { return restoreCount_; }
    bool selected() const { return selected_; }
    bool reference() const { return reference_; }
    const QString& label() const { return label_; }
    bool meshVisible(int meshModelId) const
    {
        return meshVisibility_.value(meshModelId, true);
    }
    const QString& scoreLabel() const { return scoreLabel_; }
    const ColorLegendSpec& colorLegend() const { return colorLegend_; }
    int repaintCount() const { return repaintCount_; }
    bool diagnosticEnabled(DiagnosticFlag flag) const
    {
        switch (flag) {
        case DiagnosticFlag::Orthographic:
            return orthographic_;
        case DiagnosticFlag::Wireframe:
            return wireframe_;
        case DiagnosticFlag::Normals:
            return normals_;
        }
        return false;
    }
    void setEmitCameraChangedOnRestore(bool enabled)
    {
        emitCameraChangedOnRestore_ = enabled;
    }
    void setPose(const CameraPose& value) { restoredPose_ = value; }
    void failNextRestore(const QString& error) { nextRestoreError_ = error; }

private:
    QPointer<QWidget> widget_;
    QWidget* parentAtCreation_ = nullptr;
    IViewportCallbacks& callbacks_;
    int viewportId_ = 0;
    int& liveCount_;
    int& initializationCount_;
    QString initializationError_;
    CameraPose restoredPose_;
    int restoreCount_ = 0;
    bool selected_ = false;
    bool reference_ = false;
    QString label_;
    QHash<int, bool> meshVisibility_;
    QString scoreLabel_;
    ColorLegendSpec colorLegend_;
    int repaintCount_ = 0;
    bool orthographic_ = false;
    bool wireframe_ = false;
    bool normals_ = false;
    bool emitCameraChangedOnRestore_ = false;
    QString nextRestoreError_;
};

class GridFakeViewportFactory final : public IViewportFactory
{
public:
    OperationResult createViewport(
        QWidget* parent,
        const ViewportDependencies& dependencies,
        std::unique_ptr<IViewport>& viewport) override
    {
        ++creationCount_;
        QVector<int> modelIds{dependencies.meshModelId};
        modelIds += dependencies.additionalMeshModelIds;
        boundModelIds_.push_back(modelIds);
        QString initializationError;
        if (creationCount_ == failingInitializationIndex_)
            initializationError = QStringLiteral("candidate initialization failed");
        std::unique_ptr<GridFakeViewport> candidate(new GridFakeViewport(
            parent,
            dependencies.callbacks,
            dependencies.viewportId,
            liveCount_,
            initializationCount_,
            initializationError,
            creationCount_ != nullWidgetIndex_,
            dependencies.reference));
        viewports_.push_back(candidate.get());
        viewport = std::move(candidate);
        return OperationResult::success();
    }

    GridFakeViewport& viewport(int index) { return *viewports_.at(index); }
    int creationCount() const { return creationCount_; }
    int initializationCount() const { return initializationCount_; }
    int liveCount() const { return liveCount_; }
    const QVector<int>& boundModelIds(int index) const
    {
        return boundModelIds_.at(static_cast<std::size_t>(index));
    }
    void failInitializationAt(int oneBasedIndex)
    {
        failingInitializationIndex_ = oneBasedIndex;
    }
    void returnNullWidgetAt(int oneBasedIndex)
    {
        nullWidgetIndex_ = oneBasedIndex;
    }

private:
    std::vector<GridFakeViewport*> viewports_;
    std::vector<QVector<int>> boundModelIds_;
    int creationCount_ = 0;
    int initializationCount_ = 0;
    int liveCount_ = 0;
    int failingInitializationIndex_ = -1;
    int nullWidgetIndex_ = -1;
};

} // namespace

class ViewportGridTest : public QObject
{
    Q_OBJECT

private slots:
    void createsOnePreparedViewportPerMeshInImportOrder()
    {
        QWidget host;
        host.resize(1000, 600);
        TestRenderScene renderScene;
        GridFakeViewportFactory factory;
        RecordingCallbacks callbacks;
        ViewportGrid grid(factory, callbacks);
        const SceneDescriptor descriptor = scene(5);

        const OperationResult result = grid.create(
            &host,
            descriptor,
            renderScene.dependencies(),
            descriptor.meshes.at(2).id);

        QVERIFY2(result.ok, qPrintable(result.error));
        QCOMPARE(factory.creationCount(), 5);
        QCOMPARE(factory.initializationCount(), 5);
        QCOMPARE(factory.liveCount(), 5);
        QCOMPARE(grid.viewportCount(), 5);
        for (int index = 0; index < 5; ++index) {
            QCOMPARE(
                grid.meshIdForViewport(index + 1),
                descriptor.meshes.at(index).id);
        }
        QVERIFY(factory.viewport(2).selected());
        QVERIFY(!factory.viewport(0).selected());
    }

    void overlayCreatesOneViewportBoundToEveryMeshInImportOrder()
    {
        QWidget host;
        host.resize(1000, 600);
        TestRenderScene renderScene;
        GridFakeViewportFactory factory;
        RecordingCallbacks callbacks;
        ViewportGrid grid(factory, callbacks);
        SceneDescriptor descriptor = scene(3);
        descriptor.layoutMode = SceneLayoutMode::Overlay;
        descriptor.meshes[1].visible = false;

        const OperationResult result = grid.create(
            &host,
            descriptor,
            renderScene.dependencies(),
            descriptor.referenceId);

        QVERIFY2(result.ok, qPrintable(result.error));
        QCOMPARE(factory.creationCount(), 1);
        QCOMPARE(factory.initializationCount(), 1);
        QCOMPARE(grid.viewportCount(), 1);
        QCOMPARE(
            factory.boundModelIds(0),
            (QVector<int>{101, 102, 103}));
        QCOMPARE(grid.meshIdForViewport(1), MeshId(0));
        QVERIFY(!factory.viewport(0).meshVisible(102));
        QCOMPARE(
            factory.viewport(0).label(),
            QStringLiteral("Overlay · 2/3 visible"));
    }

    void overlayVisibilityControlsAssignedMeshesAndScoreSummary()
    {
        QWidget host;
        TestRenderScene renderScene;
        GridFakeViewportFactory factory;
        RecordingCallbacks callbacks;
        ViewportGrid grid(factory, callbacks);
        SceneDescriptor descriptor = scene(3);
        descriptor.layoutMode = SceneLayoutMode::Overlay;
        QVERIFY(grid.create(
                    &host,
                    descriptor,
                    renderScene.dependencies(),
                    descriptor.referenceId)
                    .ok);
        QVERIFY(grid
                    .setAnalysisOverlays(
                        {{101, QStringLiteral("P 0.900")},
                         {102, QStringLiteral("P 0.800")}})
                    .ok);

        QVERIFY(grid.setMeshVisible(102, false).ok);

        QVERIFY(factory.viewport(0).meshVisible(101));
        QVERIFY(!factory.viewport(0).meshVisible(102));
        QVERIFY(factory.viewport(0).meshVisible(103));
        QCOMPARE(
            factory.viewport(0).label(),
            QStringLiteral("Overlay · 2/3 visible"));
        QCOMPARE(
            factory.viewport(0).scoreLabel(),
            QStringLiteral("mesh-101: P 0.900"));
        QVERIFY(!grid.setMeshVisible(999, false).ok);
    }

    void colorLegendsFollowGridPresentationsAndOverlayVisibility()
    {
        QWidget gridHost;
        TestRenderScene gridScene;
        GridFakeViewportFactory gridFactory;
        RecordingCallbacks gridCallbacks;
        ViewportGrid grid(gridFactory, gridCallbacks);
        SceneDescriptor gridDescriptor = scene(3);
        gridDescriptor.meshes[1].presentation =
            distancePresentation(0.04, DistanceColorMapping::SquareRoot);
        QVERIFY(grid.create(
                    &gridHost,
                    gridDescriptor,
                    gridScene.dependencies(),
                    gridDescriptor.referenceId)
                    .ok);

        QCOMPARE(
            gridFactory.viewport(0).colorLegend().kind,
            ColorLegendKind::None);
        QCOMPARE(
            gridFactory.viewport(1).colorLegend(),
            gridDescriptor.meshes[1].presentation.colorLegend);
        grid.setColorLegends(
            {{gridDescriptor.meshes[1].id,
              distancePresentation(
                  0.08,
                  DistanceColorMapping::Linear)}});
        QCOMPARE(
            gridFactory.viewport(1).colorLegend().distanceMapping,
            DistanceColorMapping::Linear);
        QCOMPARE(gridFactory.viewport(1).colorLegend().maximum, 0.08);

        QWidget overlayHost;
        TestRenderScene overlayScene;
        GridFakeViewportFactory overlayFactory;
        RecordingCallbacks overlayCallbacks;
        ViewportGrid overlayGrid(overlayFactory, overlayCallbacks);
        SceneDescriptor overlayDescriptor = scene(3);
        overlayDescriptor.layoutMode = SceneLayoutMode::Overlay;
        overlayDescriptor.meshes[1].presentation =
            distancePresentation(0.04, DistanceColorMapping::SquareRoot);
        overlayDescriptor.meshes[2].presentation =
            distancePresentation(0.08, DistanceColorMapping::Linear);
        QVERIFY(overlayGrid.create(
                    &overlayHost,
                    overlayDescriptor,
                    overlayScene.dependencies(),
                    overlayDescriptor.referenceId)
                    .ok);

        QCOMPARE(
            overlayFactory.viewport(0).colorLegend().kind,
            ColorLegendKind::None);
        QVERIFY(overlayGrid
                    .setMeshVisible(overlayDescriptor.meshes[2].id, false)
                    .ok);
        QCOMPARE(
            overlayFactory.viewport(0).colorLegend(),
            overlayDescriptor.meshes[1].presentation.colorLegend);
        QVERIFY(overlayGrid
                    .setMeshVisible(overlayDescriptor.meshes[1].id, false)
                    .ok);
        QCOMPARE(
            overlayFactory.viewport(0).colorLegend().kind,
            ColorLegendKind::None);
    }

    void gridRejectsOverlayVisibilityUpdates()
    {
        QWidget host;
        TestRenderScene renderScene;
        GridFakeViewportFactory factory;
        RecordingCallbacks callbacks;
        ViewportGrid grid(factory, callbacks);
        const SceneDescriptor descriptor = scene(3);
        QVERIFY(grid.create(
                    &host,
                    descriptor,
                    renderScene.dependencies(),
                    descriptor.referenceId)
                    .ok);

        QVERIFY(!grid.setMeshVisible(102, false).ok);
    }

    void cameraChangePropagatesWithoutFeedbackLoop()
    {
        QWidget host;
        TestRenderScene renderScene;
        GridFakeViewportFactory factory;
        RecordingCallbacks callbacks;
        ViewportGrid grid(factory, callbacks);
        const SceneDescriptor descriptor = scene(3);
        QVERIFY(grid.create(
                    &host,
                    descriptor,
                    renderScene.dependencies(),
                    descriptor.referenceId)
                    .ok);
        factory.viewport(1).setEmitCameraChangedOnRestore(true);
        factory.viewport(2).setEmitCameraChangedOnRestore(true);

        factory.viewport(0).emitCameraChanged(pose(QStringLiteral("camera-a")));

        QCOMPARE(factory.viewport(0).restoreCount(), 0);
        QCOMPARE(factory.viewport(1).restoredPose().viewStateXml, QStringLiteral("camera-a"));
        QCOMPARE(factory.viewport(2).restoredPose().viewStateXml, QStringLiteral("camera-a"));
        QCOMPARE(factory.viewport(1).restoreCount(), 1);
        QCOMPARE(factory.viewport(2).restoreCount(), 1);
        QCOMPARE(callbacks.cameraChangeCount, 1);
        QCOMPARE(callbacks.cameraViewportId, 1);
    }

    void diagnosticsAreForwardedToEveryViewport()
    {
        QWidget host;
        TestRenderScene renderScene;
        GridFakeViewportFactory factory;
        RecordingCallbacks callbacks;
        ViewportGrid grid(factory, callbacks);
        const SceneDescriptor descriptor = scene(3);
        QVERIFY(grid.create(
                    &host,
                    descriptor,
                    renderScene.dependencies(),
                    descriptor.referenceId)
                    .ok);

        grid.setDiagnostic(DiagnosticFlag::Orthographic, true);
        grid.setDiagnostic(DiagnosticFlag::Wireframe, true);
        grid.setDiagnostic(DiagnosticFlag::Normals, true);
        grid.setDiagnostic(DiagnosticFlag::Wireframe, false);

        for (int index = 0; index < 3; ++index) {
            QVERIFY(factory.viewport(index).diagnosticEnabled(
                DiagnosticFlag::Orthographic));
            QVERIFY(!factory.viewport(index).diagnosticEnabled(
                DiagnosticFlag::Wireframe));
            QVERIFY(factory.viewport(index).diagnosticEnabled(
                DiagnosticFlag::Normals));
        }
    }

    void explicitCameraRestoreRollsBackEveryEarlierViewportOnFailure()
    {
        QWidget host;
        TestRenderScene renderScene;
        GridFakeViewportFactory factory;
        RecordingCallbacks callbacks;
        ViewportGrid grid(factory, callbacks);
        const SceneDescriptor descriptor = scene(3);
        QVERIFY(grid.create(
                    &host,
                    descriptor,
                    renderScene.dependencies(),
                    descriptor.referenceId)
                    .ok);
        factory.viewport(0).setPose(pose(QStringLiteral("original-a")));
        factory.viewport(1).setPose(pose(QStringLiteral("original-b")));
        factory.viewport(2).setPose(pose(QStringLiteral("original-c")));
        factory.viewport(1).failNextRestore(
            QStringLiteral("viewport rejected camera"));

        const OperationResult result =
            grid.restoreCamera(pose(QStringLiteral("replacement")));

        QVERIFY(!result.ok);
        QCOMPARE(result.error, QStringLiteral("viewport rejected camera"));
        QCOMPARE(
            factory.viewport(0).restoredPose().viewStateXml,
            QStringLiteral("original-a"));
        QCOMPARE(
            factory.viewport(1).restoredPose().viewStateXml,
            QStringLiteral("original-b"));
        QCOMPARE(
            factory.viewport(2).restoredPose().viewStateXml,
            QStringLiteral("original-c"));
        QCOMPARE(callbacks.cameraChangeCount, 0);
    }

    void activationSelectsExactlyOneViewportAndForwardsItsId()
    {
        QWidget host;
        TestRenderScene renderScene;
        GridFakeViewportFactory factory;
        RecordingCallbacks callbacks;
        ViewportGrid grid(factory, callbacks);
        const SceneDescriptor descriptor = scene(3);
        QVERIFY(grid.create(
                    &host,
                    descriptor,
                    renderScene.dependencies(),
                    descriptor.referenceId)
                    .ok);

        factory.viewport(1).emitActivated();

        QVERIFY(!factory.viewport(0).selected());
        QVERIFY(factory.viewport(1).selected());
        QVERIFY(!factory.viewport(2).selected());
        QCOMPARE(callbacks.activationCount, 1);
        QCOMPARE(callbacks.activatedViewportId, 2);
        QCOMPARE(grid.meshIdForViewport(2), descriptor.meshes.at(1).id);
    }

    void referenceBadgeUpdatesWithoutRebuildingTheGrid()
    {
        QWidget host;
        TestRenderScene renderScene;
        GridFakeViewportFactory factory;
        RecordingCallbacks callbacks;
        ViewportGrid grid(factory, callbacks);
        const SceneDescriptor descriptor = scene(3);
        QVERIFY(grid.create(
                    &host,
                    descriptor,
                    renderScene.dependencies(),
                    descriptor.referenceId)
                    .ok);
        const int creationCount = factory.creationCount();
        QVERIFY(factory.viewport(0).reference());
        QVERIFY(!factory.viewport(1).reference());

        grid.setReferenceMesh(descriptor.meshes.at(1).id);

        QCOMPARE(factory.creationCount(), creationCount);
        QVERIFY(!factory.viewport(0).reference());
        QVERIFY(factory.viewport(1).reference());
        QVERIFY(!factory.viewport(2).reference());
    }

    void overlayBatchValidationIsNonMutating()
    {
        QWidget host;
        TestRenderScene renderScene;
        GridFakeViewportFactory factory;
        RecordingCallbacks callbacks;
        ViewportGrid grid(factory, callbacks);
        const SceneDescriptor descriptor = scene(3);
        QVERIFY(grid.create(
                    &host,
                    descriptor,
                    renderScene.dependencies(),
                    descriptor.referenceId)
                    .ok);
        const int creationCount = factory.creationCount();

        QVERIFY(grid.setAnalysisOverlays(
                    {{descriptor.meshes.at(1).id, QStringLiteral("42%")},
                     {descriptor.meshes.at(2).id, QStringLiteral("P 0.942")}})
                    .ok);
        QCOMPARE(factory.viewport(0).scoreLabel(), QString());
        QCOMPARE(factory.viewport(1).scoreLabel(), QStringLiteral("42%"));
        QCOMPARE(factory.viewport(2).scoreLabel(), QStringLiteral("P 0.942"));

        QVERIFY(!grid.setAnalysisOverlays(
                     {{descriptor.meshes.at(1).id, QStringLiteral("99%")},
                      {MeshId(999), QStringLiteral("N 0.917")}})
                     .ok);
        QVERIFY(!grid.setAnalysisOverlays(
                     {{descriptor.meshes.at(1).id, QStringLiteral("99%")},
                      {descriptor.meshes.at(1).id, QString()}})
                     .ok);

        QCOMPARE(factory.creationCount(), creationCount);
        QCOMPARE(factory.viewport(0).scoreLabel(), QString());
        QCOMPARE(factory.viewport(1).scoreLabel(), QStringLiteral("42%"));
        QCOMPARE(factory.viewport(2).scoreLabel(), QStringLiteral("P 0.942"));

        QVERIFY(grid.setAnalysisOverlays(
                    {{descriptor.meshes.at(1).id, QString()},
                     {descriptor.meshes.at(2).id, QStringLiteral("N 0.917")}})
                    .ok);
        QCOMPARE(factory.creationCount(), creationCount);
        QCOMPARE(factory.viewport(1).scoreLabel(), QString());
        QCOMPARE(factory.viewport(2).scoreLabel(), QStringLiteral("N 0.917"));
    }

    void initializationFailureDestroysTheWholeCandidateGrid()
    {
        QWidget host;
        TestRenderScene renderScene;
        GridFakeViewportFactory factory;
        factory.failInitializationAt(2);
        RecordingCallbacks callbacks;
        ViewportGrid grid(factory, callbacks);
        const SceneDescriptor descriptor = scene(3);

        const OperationResult result = grid.create(
            &host,
            descriptor,
            renderScene.dependencies(),
            descriptor.referenceId);

        QVERIFY(!result.ok);
        QCOMPARE(result.error, QStringLiteral("candidate initialization failed"));
        QCOMPARE(factory.creationCount(), 2);
        QCOMPARE(factory.initializationCount(), 2);
        QCOMPARE(factory.liveCount(), 0);
        QCOMPARE(grid.viewportCount(), 0);
    }

    void factorySuccessWithoutAWidgetRejectsTheWholeCandidateGrid()
    {
        QWidget host;
        TestRenderScene renderScene;
        GridFakeViewportFactory factory;
        factory.returnNullWidgetAt(2);
        RecordingCallbacks callbacks;
        ViewportGrid grid(factory, callbacks);
        const SceneDescriptor descriptor = scene(3);

        const OperationResult result = grid.create(
            &host,
            descriptor,
            renderScene.dependencies(),
            descriptor.referenceId);

        QVERIFY(!result.ok);
        QCOMPARE(result.error, QStringLiteral("Viewport creation returned no widget."));
        QCOMPARE(factory.creationCount(), 2);
        QCOMPARE(factory.liveCount(), 0);
        QCOMPARE(grid.viewportCount(), 0);
    }

    void committedContainerKeepsFinalParentAndTracksHostResize()
    {
        QWidget host;
        host.resize(640, 480);
        host.show();
        TestRenderScene renderScene;
        GridFakeViewportFactory factory;
        RecordingCallbacks callbacks;
        ViewportGrid grid(factory, callbacks);
        const SceneDescriptor descriptor = scene(2);
        QVERIFY(grid.create(
                    &host,
                    descriptor,
                    renderScene.dependencies(),
                    descriptor.referenceId)
                    .ok);
        QWidget* viewportHost = factory.viewport(0).widget()->parentWidget();
        QWidget* parentAtCreation = factory.viewport(0).parentAtCreation();
        QWidget* container = viewportHost;
        while (container->parentWidget() != &host)
            container = container->parentWidget();
        QVERIFY(container->isHidden());
        QVERIFY(!container->isVisibleTo(&host));

        grid.showCommitted();
        host.resize(1111, 777);
        QCoreApplication::processEvents();

        QCOMPARE(viewportHost, parentAtCreation);
        QCOMPARE(container->parentWidget(), &host);
        QCOMPARE(container->geometry(), host.rect());
        QVERIFY(viewportHost->isVisibleTo(&host));
        QCOMPARE(
            factory.viewport(0).widget()->height(),
            factory.viewport(1).widget()->height());
        QVERIFY(qAbs(
                    factory.viewport(0).widget()->width() -
                    factory.viewport(1).widget()->width()) <= 1);
    }
};

QTEST_MAIN(ViewportGridTest)
#include "viewport_grid_test.moc"
