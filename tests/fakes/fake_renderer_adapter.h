#pragma once

#include <functional>
#include <utility>

#include <QHash>
#include <QSet>
#include <QStringList>

#include "core/renderer_adapter.h"

class FakeRendererAdapter : public IRendererAdapter {
public:
    OperationResult mount(QWidget*) override
    {
        ++m_mountCount;
        appendTrace(QStringLiteral("mount"));
        if (!m_nextMountError.isEmpty()) {
            const QString error = m_nextMountError;
            m_nextMountError.clear();
            return OperationResult::failure(error);
        }
        return OperationResult::success();
    }

    void setEvents(RendererEvents events) override
    {
        const bool connected = events.selectedMeshChanged || events.cameraChanged ||
                               events.resourceProgress || events.rendererError;
        appendTrace(connected ? QStringLiteral("events-connect")
                              : QStringLiteral("events-disconnect"));
        if (!connected)
            ++m_eventDisconnectCount;
        m_events = std::move(events);
    }

    OperationResult prepareScene(
        const SceneDescriptor& scene,
        const IMeshResourceProvider& resources) override
    {
        return prepare(scene, &resources);
    }

    OperationResult prepareScene(const SceneDescriptor& scene)
    {
        return prepare(scene, nullptr);
    }

    void commitPreparedScene() override
    {
        appendTrace(QStringLiteral("renderer-commit"));
        if (m_commitObserver)
            m_commitObserver();
        m_committedProvider = m_preparedProvider;
        m_preparedProvider = nullptr;
        m_committedGeneration = m_preparedGeneration;
        m_preparedGeneration = 0;
        m_presentations.clear();
        m_analysisOverlays.clear();
        for (const SceneMesh& mesh : m_lastPreparedScene.meshes) {
            m_presentations.insert(mesh.id, ColorPresentation{});
            m_analysisOverlays.insert(mesh.id, QString());
        }
    }
    void discardPreparedScene() override
    {
        ++m_discardCount;
        appendTrace(QStringLiteral("discard"));
        m_preparedProvider = nullptr;
        m_preparedGeneration = 0;
    }
    void clearScene() override
    {
        appendTrace(QStringLiteral("clear"));
        ++m_clearCount;
        if (m_clearObserver)
            m_clearObserver();
        if (m_committedProvider != nullptr && m_clearProbeResourceId != 0) {
            m_resourceAvailableDuringClear =
                m_committedProvider->geometry(m_clearProbeResourceId) != nullptr;
        }
        m_preparedGeneration = 0;
        m_committedGeneration = 0;
        m_preparedProvider = nullptr;
        m_committedProvider = nullptr;
        m_presentations.clear();
        m_analysisOverlays.clear();
    }
    void setSelectedMesh(MeshId id) override
    {
        m_selectedMeshId = id;
        appendTrace(QStringLiteral("selection-update"));
        if (m_selectionObserver)
            m_selectionObserver();
    }
    void setReferenceMesh(MeshId id) override
    {
        m_referenceMeshId = id;
        appendTrace(QStringLiteral("reference-update"));
        if (m_referenceObserver)
            m_referenceObserver();
    }
    OperationResult setColorPresentations(
        const QVector<MeshColorPresentationUpdate>& updates) override
    {
        ++m_presentationAttemptCount;
        QHash<MeshId, ColorPresentation> staged = m_presentations;
        QSet<MeshId> ids;
        for (const MeshColorPresentationUpdate& update : updates) {
            if (!staged.contains(update.meshId) || ids.contains(update.meshId)) {
                return OperationResult::failure(
                    QStringLiteral("Fake renderer rejected the presentation batch."));
            }
            ids.insert(update.meshId);
            staged[update.meshId] = update.presentation;
        }
        if (m_presentationObserver)
            m_presentationObserver();
        if (!m_nextPresentationError.isEmpty()) {
            const QString error = m_nextPresentationError;
            m_nextPresentationError.clear();
            return OperationResult::failure(error);
        }
        m_presentations = staged;
        m_lastPresentationBatch = updates;
        ++m_presentationUpdateCount;
        return OperationResult::success();
    }
    OperationResult setAnalysisOverlays(
        const QVector<MeshAnalysisOverlayUpdate>& updates) override
    {
        ++m_overlayAttemptCount;
        if (updates.isEmpty()) {
            return OperationResult::failure(
                QStringLiteral("Fake renderer rejected the analysis overlay batch."));
        }

        QHash<MeshId, QString> staged = m_analysisOverlays;
        QSet<MeshId> ids;
        for (const MeshAnalysisOverlayUpdate& update : updates) {
            if (!staged.contains(update.meshId) || ids.contains(update.meshId)) {
                return OperationResult::failure(
                    QStringLiteral("Fake renderer rejected the analysis overlay batch."));
            }
            ids.insert(update.meshId);
            staged[update.meshId] = update.label;
        }
        if (m_overlayObserver)
            m_overlayObserver();
        if (!m_nextOverlayError.isEmpty()) {
            const QString error = m_nextOverlayError;
            m_nextOverlayError.clear();
            return OperationResult::failure(error);
        }
        m_analysisOverlays = std::move(staged);
        m_lastOverlayBatch = updates;
        ++m_overlayUpdateCount;
        return OperationResult::success();
    }
    CameraPose captureCamera() const override { return m_camera; }
    OperationResult restoreCamera(const CameraPose& pose) override
    {
        ++m_restoreCount;
        m_lastRestoreAttempt = pose;
        appendTrace(QStringLiteral("restore:%1").arg(pose.viewStateXml));
        if (m_restoreObserver)
            m_restoreObserver();
        if (!m_nextRestoreError.isEmpty()) {
            const QString error = m_nextRestoreError;
            m_nextRestoreError.clear();
            return OperationResult::failure(error);
        }
        m_camera = pose;
        return OperationResult::success();
    }
    void resetCamera() override { m_camera = {}; }
    void setDiagnostic(DiagnosticFlag, bool) override {}
    RendererDiagnostics diagnostics() const override { return {}; }

    quint64 preparedGeneration() const { return m_preparedGeneration; }
    quint64 committedGeneration() const { return m_committedGeneration; }
    int mountCount() const { return m_mountCount; }
    int prepareCount() const { return m_prepareCount; }
    int discardCount() const { return m_discardCount; }
    int clearCount() const { return m_clearCount; }
    int eventDisconnectCount() const { return m_eventDisconnectCount; }
    MeshId selectedMeshId() const { return m_selectedMeshId; }
    MeshId referenceMeshId() const { return m_referenceMeshId; }
    const SceneDescriptor& lastPreparedScene() const { return m_lastPreparedScene; }
    bool resourceAvailableDuringClear() const { return m_resourceAvailableDuringClear; }
    int presentationAttemptCount() const { return m_presentationAttemptCount; }
    int presentationUpdateCount() const { return m_presentationUpdateCount; }
    ColorPresentation presentation(MeshId id) const
    {
        return m_presentations.value(id);
    }
    const QVector<MeshColorPresentationUpdate>& lastPresentationBatch() const
    {
        return m_lastPresentationBatch;
    }
    QString analysisOverlay(MeshId id) const
    {
        return m_analysisOverlays.value(id);
    }
    int overlayAttemptCount() const { return m_overlayAttemptCount; }
    int overlayUpdateCount() const { return m_overlayUpdateCount; }
    int restoreCount() const { return m_restoreCount; }
    CameraPose camera() const { return m_camera; }
    CameraPose restoredPose() const { return m_camera; }
    CameraPose lastRestoreAttempt() const { return m_lastRestoreAttempt; }
    const QVector<MeshAnalysisOverlayUpdate>& lastOverlayBatch() const
    {
        return m_lastOverlayBatch;
    }

    void failNextMount(const QString& error) { m_nextMountError = error; }
    void failNextPrepare(const QString& error) { m_nextPrepareError = error; }
    void failNextPresentationBatch(const QString& error)
    {
        m_nextPresentationError = error;
    }
    void failNextOverlayBatch(const QString& error) { m_nextOverlayError = error; }
    void failNextRestore(const QString& error) { m_nextRestoreError = error; }
    void setCamera(const CameraPose& pose) { m_camera = pose; }
    void setTrace(QStringList* trace) { m_trace = trace; }
    void setPrepareObserver(std::function<void()> observer)
    {
        m_prepareObserver = std::move(observer);
    }
    void setCommitObserver(std::function<void()> observer)
    {
        m_commitObserver = std::move(observer);
    }
    void setReferenceObserver(std::function<void()> observer)
    {
        m_referenceObserver = std::move(observer);
    }
    void setSelectionObserver(std::function<void()> observer)
    {
        m_selectionObserver = std::move(observer);
    }
    void setPresentationObserver(std::function<void()> observer)
    {
        m_presentationObserver = std::move(observer);
    }
    void setOverlayObserver(std::function<void()> observer)
    {
        m_overlayObserver = std::move(observer);
    }
    void setClearProbeResource(MeshResourceId id) { m_clearProbeResourceId = id; }
    void setClearObserver(std::function<void()> observer)
    {
        m_clearObserver = std::move(observer);
    }
    void setRestoreObserver(std::function<void()> observer)
    {
        m_restoreObserver = std::move(observer);
    }

    void emitSelectedMeshChanged(MeshId id)
    {
        if (m_events.selectedMeshChanged)
            m_events.selectedMeshChanged(id);
    }
    void emitRendererError(const QString& error)
    {
        if (m_events.rendererError)
            m_events.rendererError(error);
    }

private:
    OperationResult prepare(
        const SceneDescriptor& scene,
        const IMeshResourceProvider* resources)
    {
        ++m_prepareCount;
        appendTrace(QStringLiteral("prepare"));
        if (m_prepareObserver)
            m_prepareObserver();
        if (!m_nextPrepareError.isEmpty()) {
            const QString error = m_nextPrepareError;
            m_nextPrepareError.clear();
            return OperationResult::failure(error);
        }
        m_preparedProvider = resources;
        m_lastPreparedScene = scene;
        m_preparedGeneration = scene.generation;
        return OperationResult::success();
    }
    void appendTrace(const QString& event)
    {
        if (m_trace != nullptr)
            m_trace->append(event);
    }

    RendererEvents m_events;
    CameraPose m_camera;
    QString m_nextMountError;
    QString m_nextPrepareError;
    QString m_nextPresentationError;
    QString m_nextOverlayError;
    QString m_nextRestoreError;
    QStringList* m_trace = nullptr;
    std::function<void()> m_prepareObserver;
    std::function<void()> m_commitObserver;
    std::function<void()> m_referenceObserver;
    std::function<void()> m_selectionObserver;
    std::function<void()> m_presentationObserver;
    std::function<void()> m_overlayObserver;
    std::function<void()> m_clearObserver;
    std::function<void()> m_restoreObserver;
    const IMeshResourceProvider* m_preparedProvider = nullptr;
    const IMeshResourceProvider* m_committedProvider = nullptr;
    SceneDescriptor m_lastPreparedScene;
    QHash<MeshId, ColorPresentation> m_presentations;
    QHash<MeshId, QString> m_analysisOverlays;
    QVector<MeshColorPresentationUpdate> m_lastPresentationBatch;
    QVector<MeshAnalysisOverlayUpdate> m_lastOverlayBatch;
    CameraPose m_lastRestoreAttempt;
    quint64 m_preparedGeneration = 0;
    quint64 m_committedGeneration = 0;
    MeshId m_selectedMeshId = 0;
    MeshId m_referenceMeshId = 0;
    MeshResourceId m_clearProbeResourceId = 0;
    int m_mountCount = 0;
    int m_prepareCount = 0;
    int m_discardCount = 0;
    int m_clearCount = 0;
    int m_eventDisconnectCount = 0;
    int m_presentationAttemptCount = 0;
    int m_presentationUpdateCount = 0;
    int m_overlayAttemptCount = 0;
    int m_overlayUpdateCount = 0;
    int m_restoreCount = 0;
    bool m_resourceAvailableDuringClear = false;
};
