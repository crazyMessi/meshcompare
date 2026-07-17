#pragma once

#include "core/meshcompare_types.h"

#include <QColor>
#include <QString>
#include <QVector>

#include <array>
#include <cstdint>
#include <functional>

enum class SurfaceComparisonMetric {
    PrecisionAtThreshold,
    NormalAgreement,
};

using SurfacePoint3D = std::array<double, 3>;

struct SurfaceMeshSnapshot {
    QVector<SurfacePoint3D> vertices;
    QVector<SurfacePoint3D> vertexNormals;
    QVector<std::array<int, 3>> faces;
};

struct SurfaceComparisonOptions {
    int sampleCount = 500000;
    float distanceThreshold = 0.004f;
    bool useAbsoluteNormalDot = true;
    std::uint32_t randomSeed = 0x4d595df4u;
};

struct SurfaceComparisonResult {
    int sampleCount = 0;
    int coloredFaceCount = 0;
    double globalScore = 0.0;
    QVector<double> faceScores;
};

struct SurfaceComparisonOutcome {
    OperationResult result;
    SurfaceComparisonResult comparison;
};

using AnalysisProgress = std::function<bool(int percent, const QString& message)>;
using AnalysisCancellation = std::function<bool()>;

class ISurfaceComparer
{
public:
    virtual ~ISurfaceComparer() = default;

    virtual SurfaceComparisonOutcome compare(
        const SurfaceMeshSnapshot& source,
        const SurfaceMeshSnapshot& reference,
        SurfaceComparisonMetric metric,
        const SurfaceComparisonOptions& options,
        AnalysisProgress progress = {},
        AnalysisCancellation cancellationRequested = {}) const = 0;
};

class SurfaceComparer final : public ISurfaceComparer
{
public:
    SurfaceComparisonOutcome compare(
        const SurfaceMeshSnapshot& source,
        const SurfaceMeshSnapshot& reference,
        SurfaceComparisonMetric metric,
        const SurfaceComparisonOptions& options,
        AnalysisProgress progress = {},
        AnalysisCancellation cancellationRequested = {}) const override;
};

OperationResult validateSurfaceComparisonOptions(
    SurfaceComparisonMetric metric,
    const SurfaceComparisonOptions& options);

SurfaceComparisonOutcome compareSampledSurfaces(
    const SurfaceMeshSnapshot& source,
    const SurfaceMeshSnapshot& reference,
    SurfaceComparisonMetric metric,
    const SurfaceComparisonOptions& options,
    AnalysisProgress progress = {},
    AnalysisCancellation cancellationRequested = {});

QVector<QColor> surfaceScoreColors(const QVector<double>& faceScores);
