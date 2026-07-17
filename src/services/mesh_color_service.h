#pragma once

#include <atomic>
#include <memory>

#include <QObject>
#include <QVector>

#include "core/mesh_resource_provider.h"
#include "surface_comparison.h"

class IRendererAdapter;
class MeshColorAnalysisThread;
class WorkspaceState;

struct AnalysisRequest
{
    quint64 generation = 0;
    MeshId referenceId = 0;
    QVector<MeshId> targetIds;
    SurfaceComparisonMetric metric = SurfaceComparisonMetric::PrecisionAtThreshold;
    SurfaceComparisonOptions options;
};

struct MeshAnalysisResult
{
    OperationResult result;
    MeshId meshId = 0;
    double globalScore = 0.0;
    QVector<QColor> faceColors;
};

struct AnalysisBatchResult
{
    OperationResult result;
    quint64 generation = 0;
    quint64 batchSerial = 0;
    SurfaceComparisonMetric metric = SurfaceComparisonMetric::PrecisionAtThreshold;
    QVector<MeshAnalysisResult> meshes;
};

Q_DECLARE_METATYPE(AnalysisBatchResult)

class IMeshColorService
{
public:
    virtual ~IMeshColorService() = default;
    virtual OperationResult startAnalysis(const AnalysisRequest& request) = 0;
    virtual void cancelAnalysis() = 0;
    virtual OperationResult setUniformColor(MeshId meshId, const QColor& color) = 0;
    virtual OperationResult clearColoring() = 0;
};

class MeshColorService final : public QObject, public IMeshColorService
{
    Q_OBJECT

public:
    MeshColorService(
        const IMeshResourceProvider& resources,
        ISurfaceComparer& comparer,
        WorkspaceState& state,
        IRendererAdapter& renderer,
        QObject* parent = nullptr);
    ~MeshColorService() override;

    OperationResult startAnalysis(const AnalysisRequest& request) override;
    void cancelAnalysis() override;
    OperationResult setUniformColor(MeshId meshId, const QColor& color) override;
    OperationResult clearColoring() override;

signals:
    void analysisProgress(
        quint64 generation,
        quint64 batchSerial,
        MeshId meshId,
        int percent,
        const QString& message);
    void analysisFinished(AnalysisBatchResult result);

private:
    friend class MeshColorAnalysisThread;

    void queueProgress(
        quint64 generation,
        quint64 batchSerial,
        MeshId meshId,
        int percent,
        const QString& message);
    void queueCompletion(AnalysisBatchResult result);
    void publishProgress(
        quint64 generation,
        quint64 batchSerial,
        MeshId meshId,
        int percent,
        const QString& message);
    void completeAnalysis(AnalysisBatchResult result);
    void joinWorker();
    void stopAnalysis(bool notify);

    const IMeshResourceProvider& resources_;
    ISurfaceComparer& comparer_;
    WorkspaceState& state_;
    IRendererAdapter& renderer_;
    std::unique_ptr<MeshColorAnalysisThread> worker_;
    std::shared_ptr<std::atomic_bool> cancellation_;
    quint64 nextBatchSerial_ = 0;
    quint64 activeBatchSerial_ = 0;
    quint64 activeGeneration_ = 0;
    SurfaceComparisonMetric activeMetric_ = SurfaceComparisonMetric::PrecisionAtThreshold;
    bool active_ = false;
    bool shuttingDown_ = false;
};
