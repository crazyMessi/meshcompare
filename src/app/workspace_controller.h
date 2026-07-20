#pragma once

#include <memory>

#include <QObject>
#include <QPair>
#include <QString>
#include <QStringList>
#include <QVector>

#include "camera_commands.h"
#include "coloring_commands.h"
#include "../core/meshcompare_types.h"
#include "../services/mesh_color_service.h"

class ICameraPoseStore;
class IMeshImportService;
class IRendererAdapter;
class ISurfaceComparer;
class MeshLabMeshRepository;
struct MeshEntry;
class WorkspaceState;

struct WorkspaceImportOutcome
{
    OperationResult result;
    QVector<QPair<QString, QString>> fileErrors;
    QString notice;
};

QString resolveWorkspaceUuid(
    const QVector<MeshEntry>& meshes,
    MeshId referenceId);

class WorkspaceController final : public QObject,
                                  public IColoringCommands,
                                  public ICameraCommands
{
    Q_OBJECT

public:
    WorkspaceController(
        WorkspaceState& state,
        IMeshImportService& importer,
        IRendererAdapter& renderer,
        ISurfaceComparer& comparer,
        ICameraPoseStore& cameraStore,
        QObject* parent = nullptr);
    ~WorkspaceController() override;

    WorkspaceImportOutcome importMeshes(const QStringList& paths);
    OperationResult setLayoutMode(SceneLayoutMode layoutMode);
    OperationResult setMeshVisible(MeshId meshId, bool visible);
    OperationResult selectMesh(MeshId id) override;
    OperationResult setReference(MeshId id) override;
    OperationResult startAnalysis(
        SurfaceComparisonMetric metric,
        const SurfaceComparisonOptions& options) override;
    void cancelAnalysis() override;
    OperationResult setUniformColor(
        MeshId meshId,
        const QColor& color) override;
    OperationResult clearColoring() override;
    CameraPanelSnapshot cameraPanelSnapshot() const override;
    OperationResult setCameraPoseUid(const QString& uid) override;
    OperationResult saveCurrentCameraPose(
        QString* savedViewId = nullptr) override;
    OperationResult saveCurrentCameraPoseWithScreenshot(
        QImage& screenshot,
        QString* savedViewId = nullptr) override;
    OperationResult applyCameraPose(const QString& viewId) override;
    OperationResult setCameraPoseTags(
        const QString& viewId,
        const QStringList& tags) override;
    OperationResult deleteCameraPose(const QString& viewId) override;

signals:
    void statusMessage(const QString& message);
    void workspaceChanged();
    void analysisProgress(
        quint64 generation,
        quint64 batchSerial,
        MeshId meshId,
        int percent,
        const QString& message);
    void analysisFinished(AnalysisBatchResult result);
    void analysisStartFailed(
        SurfaceComparisonMetric metric,
        const QString& error);
    void cameraSaveFinished(bool succeeded, const QString& error);
    void cameraApplyFinished(bool succeeded, const QString& error);

private:
    void connectRendererEvents();
    void disconnectRendererEvents();
    void createColorService();
    void publishStatusMessage(const QString& message);
    void publishWorkspaceChanged();
    OperationResult publishAnalysisStartFailure(
        SurfaceComparisonMetric metric,
        OperationResult result);
    void publishCameraSaveFinished(const OperationResult& result);
    void publishCameraApplyFinished(const OperationResult& result);
    void publishOverlayUpdates(
        const QVector<MeshAnalysisOverlayUpdate>& updates);
    QVector<MeshAnalysisOverlayUpdate> committedAnalysisOverlays() const;
    QString restoreLatestCameraPose();
    OperationResult validateCameraMutation() const;
    OperationResult saveCurrentCameraPoseImpl(
        QImage* screenshot,
        QString* savedViewId);

    WorkspaceState& state_;
    IMeshImportService& importer_;
    IRendererAdapter& renderer_;
    ISurfaceComparer& comparer_;
    ICameraPoseStore& cameraStore_;
    std::unique_ptr<MeshLabMeshRepository> repository_;
    std::unique_ptr<MeshColorService> colorService_;
    QString workspaceUuid_;
    QString suggestedWorkspaceUid_;
    bool workspaceUidManuallyAssigned_ = false;
    bool replacingWorkspace_ = false;
    bool handlingCallback_ = false;
    mutable bool cameraCommandInProgress_ = false;
};
