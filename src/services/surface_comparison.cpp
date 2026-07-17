#include "surface_comparison.h"

#include <common/ml_document/cmesh.h>

#include <vcg/space/index/kdtree/kdtree.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <random>
#include <vector>

namespace {

constexpr std::uint32_t kReferenceSeedXor = 0xd36e244du;
constexpr char kFaceColorMessage[] = "Completing source face analysis colors...";

struct WorkingMesh {
    std::vector<Point3m> vertices;
    std::vector<std::array<int, 3>> faces;
    bool hasPositiveArea = false;
};

struct SurfaceSample {
    Point3m position;
    Point3m faceNormal;
    int faceIndex = -1;
};

Point3m normalizedOrZero(Point3m point)
{
    const Scalarm length = point.Norm();
    if (length > Scalarm(0))
        point /= length;
    return point;
}

Scalarm pointDot(const Point3m& left, const Point3m& right)
{
    return left[0] * right[0] + left[1] * right[1] + left[2] * right[2];
}

Point3m triangleCentroid(
    const Point3m& p0,
    const Point3m& p1,
    const Point3m& p2)
{
    Point3m result;
    for (int axis = 0; axis < 3; ++axis) {
        const Scalarm scale = std::max(
            std::abs(p0[axis]),
            std::max(std::abs(p1[axis]), std::abs(p2[axis])));
        result[axis] = scale == Scalarm(0)
            ? Scalarm(0)
            : ((p0[axis] / scale + p1[axis] / scale + p2[axis] / scale)
               / Scalarm(3)) * scale;
    }
    return result;
}

double scoreSurfaceProbe(
    const Point3m& position,
    const Point3m& normal,
    const std::vector<SurfaceSample>& referenceSamples,
    vcg::KdTree<Scalarm>& referenceTree,
    SurfaceComparisonMetric metric,
    const SurfaceComparisonOptions& options)
{
    unsigned int nearestReferenceIndex = 0;
    Scalarm nearestSquaredDistance = Scalarm(0);
    referenceTree.doQueryClosest(
        position, nearestReferenceIndex, nearestSquaredDistance);
    const SurfaceSample& referenceSample = referenceSamples[nearestReferenceIndex];

    if (metric == SurfaceComparisonMetric::PrecisionAtThreshold) {
        const double distance =
            std::sqrt(std::max(0.0, double(nearestSquaredDistance)));
        return distance <= double(options.distanceThreshold) ? 1.0 : 0.0;
    }

    const double dot = std::max(
        -1.0,
        std::min(1.0, double(pointDot(normal, referenceSample.faceNormal))));
    return options.useAbsoluteNormalDot ? std::abs(dot) : std::max(0.0, dot);
}

SurfaceComparisonOutcome failure(const QString& error)
{
    return {OperationResult::failure(error), {}};
}

SurfaceComparisonOutcome cancelled()
{
    return failure(QStringLiteral("Analysis cancelled."));
}

bool reportProgress(const AnalysisProgress& progress, int percent, const char* message)
{
    return !progress || progress(percent, QString::fromLatin1(message));
}

bool isCancellationRequested(const AnalysisCancellation& cancellationRequested)
{
    return cancellationRequested && cancellationRequested();
}

OperationResult makeWorkingMesh(
    const SurfaceMeshSnapshot& snapshot,
    const QString& emptyError,
    const QString& noPositiveAreaError,
    bool requirePositiveArea,
    const AnalysisCancellation& cancellationRequested,
    WorkingMesh* working)
{
    if (snapshot.faces.isEmpty())
        return OperationResult::failure(emptyError);

    const double workingScalarLowest = double(std::numeric_limits<Scalarm>::lowest());
    const double workingScalarMax = double(std::numeric_limits<Scalarm>::max());
    working->vertices.reserve(size_t(snapshot.vertices.size()));
    for (const SurfacePoint3D& vertex : snapshot.vertices) {
        if (isCancellationRequested(cancellationRequested))
            return OperationResult::failure(QStringLiteral("Analysis cancelled."));
        if (!std::isfinite(vertex[0]) || !std::isfinite(vertex[1])
            || !std::isfinite(vertex[2])) {
            return OperationResult::failure(
                QStringLiteral("Surface mesh snapshot vertices must be finite."));
        }
        if (vertex[0] < workingScalarLowest || vertex[0] > workingScalarMax
            || vertex[1] < workingScalarLowest || vertex[1] > workingScalarMax
            || vertex[2] < workingScalarLowest || vertex[2] > workingScalarMax) {
            return OperationResult::failure(QStringLiteral(
                "Surface mesh snapshot vertices exceed the supported scalar range."));
        }
        working->vertices.emplace_back(
            Scalarm(vertex[0]), Scalarm(vertex[1]), Scalarm(vertex[2]));
    }

    working->faces.reserve(size_t(snapshot.faces.size()));
    for (const std::array<int, 3>& face : snapshot.faces) {
        if (isCancellationRequested(cancellationRequested))
            return OperationResult::failure(QStringLiteral("Analysis cancelled."));
        for (int vertexIndex : face) {
            if (vertexIndex < 0 || vertexIndex >= snapshot.vertices.size()) {
                return OperationResult::failure(QStringLiteral(
                    "Surface mesh snapshot contains an out-of-range face vertex index."));
            }
        }

        working->faces.push_back(face);
        const Point3m normal =
            (working->vertices[size_t(face[1])] - working->vertices[size_t(face[0])])
            ^ (working->vertices[size_t(face[2])] - working->vertices[size_t(face[0])]);
        const double area = double(normal.Norm()) * 0.5;
        if (std::isfinite(area) && area > 0.0)
            working->hasPositiveArea = true;
    }

    if (requirePositiveArea && !working->hasPositiveArea)
        return OperationResult::failure(noPositiveAreaError);
    return OperationResult::success();
}

bool sampleMeshSurface(
    const WorkingMesh& mesh,
    int sampleCount,
    std::uint32_t randomSeed,
    int progressBegin,
    int progressEnd,
    const char* progressMessage,
    const AnalysisProgress& progress,
    const AnalysisCancellation& cancellationRequested,
    std::vector<SurfaceSample>* samples)
{
    std::vector<double> cumulativeAreas;
    std::vector<int> faceIndexes;
    cumulativeAreas.reserve(mesh.faces.size());
    faceIndexes.reserve(mesh.faces.size());

    double totalArea = 0.0;
    for (size_t faceIndex = 0; faceIndex < mesh.faces.size(); ++faceIndex) {
        if (isCancellationRequested(cancellationRequested))
            return false;
        const std::array<int, 3>& face = mesh.faces[faceIndex];
        const Point3m& p0 = mesh.vertices[size_t(face[0])];
        const Point3m& p1 = mesh.vertices[size_t(face[1])];
        const Point3m& p2 = mesh.vertices[size_t(face[2])];
        const Point3m normal = (p1 - p0) ^ (p2 - p0);
        const double area = double(normal.Norm()) * 0.5;
        if (!std::isfinite(area) || area <= 0.0)
            continue;

        totalArea += area;
        cumulativeAreas.push_back(totalArea);
        faceIndexes.push_back(int(faceIndex));
    }

    std::mt19937 randomGenerator(randomSeed);
    std::uniform_real_distribution<double> areaDistribution(0.0, totalArea);
    std::uniform_real_distribution<double> unitDistribution(0.0, 1.0);
    samples->reserve(size_t(sampleCount));

    for (int sampleIndex = 0; sampleIndex < sampleCount; ++sampleIndex) {
        if (isCancellationRequested(cancellationRequested))
            return false;
        const double areaSample = areaDistribution(randomGenerator);
        const std::vector<double>::const_iterator selectedArea =
            std::lower_bound(cumulativeAreas.cbegin(), cumulativeAreas.cend(), areaSample);
        const size_t selectedIndex = std::min<size_t>(
            size_t(std::distance(cumulativeAreas.cbegin(), selectedArea)),
            faceIndexes.size() - 1);
        const int faceIndex = faceIndexes[selectedIndex];
        const std::array<int, 3>& face = mesh.faces[size_t(faceIndex)];
        const Point3m& p0 = mesh.vertices[size_t(face[0])];
        const Point3m& p1 = mesh.vertices[size_t(face[1])];
        const Point3m& p2 = mesh.vertices[size_t(face[2])];

        const double root = std::sqrt(unitDistribution(randomGenerator));
        const Scalarm b0 = Scalarm(1.0 - root);
        const Scalarm b1 = Scalarm(root * (1.0 - unitDistribution(randomGenerator)));
        const Scalarm b2 = Scalarm(1.0) - b0 - b1;
        SurfaceSample sample;
        sample.position = p0 * b0 + p1 * b1 + p2 * b2;
        sample.faceNormal = normalizedOrZero((p1 - p0) ^ (p2 - p0));
        sample.faceIndex = faceIndex;
        samples->push_back(sample);

        if ((sampleIndex + 1) % 8192 == 0 || sampleIndex + 1 == sampleCount) {
            const int percent = progressBegin
                + int((static_cast<long long>(sampleIndex + 1)
                       * (progressEnd - progressBegin))
                      / sampleCount);
            if (!reportProgress(progress, percent, progressMessage))
                return false;
        }
    }
    return true;
}

QColor redYellowGreen(double score)
{
    const double clampedScore = std::max(0.0, std::min(1.0, score));
    if (clampedScore <= 0.5) {
        const int green = int(std::lround(clampedScore * 510.0));
        return QColor(255, green, 0, 255);
    }
    const int red = int(std::lround((1.0 - clampedScore) * 510.0));
    return QColor(red, 255, 0, 255);
}

} // namespace

SurfaceComparisonOutcome compareSampledSurfaces(
    const SurfaceMeshSnapshot& source,
    const SurfaceMeshSnapshot& reference,
    SurfaceComparisonMetric metric,
    const SurfaceComparisonOptions& options,
    AnalysisProgress progress,
    AnalysisCancellation cancellationRequested)
{
    const OperationResult optionsValidation =
        validateSurfaceComparisonOptions(metric, options);
    if (!optionsValidation.ok)
        return failure(optionsValidation.error);

    WorkingMesh sourceMesh;
    OperationResult validation = makeWorkingMesh(
        source,
        QStringLiteral("The current source layer has no faces."),
        QStringLiteral("The current source layer has no positive-area triangles."),
        false,
        cancellationRequested,
        &sourceMesh);
    if (!validation.ok)
        return failure(validation.error);

    WorkingMesh referenceMesh;
    validation = makeWorkingMesh(
        reference,
        QStringLiteral("The reference layer has no faces."),
        QStringLiteral("The reference layer has no positive-area triangles."),
        true,
        cancellationRequested,
        &referenceMesh);
    if (!validation.ok)
        return failure(validation.error);

    if (!sourceMesh.hasPositiveArea) {
        SurfaceComparisonResult result;
        result.faceScores.fill(0.0, source.faces.size());
        result.coloredFaceCount = result.faceScores.size();
        if (!reportProgress(progress, 0, kFaceColorMessage))
            return cancelled();
        for (int faceIndex = 0; faceIndex < result.faceScores.size(); ++faceIndex) {
            if (isCancellationRequested(cancellationRequested))
                return cancelled();
            if ((faceIndex + 1) % 8192 == 0 ||
                faceIndex + 1 == result.faceScores.size()) {
                const int percent = int(
                    (static_cast<long long>(faceIndex + 1) * 95)
                    / result.faceScores.size());
                if (!reportProgress(progress, percent, kFaceColorMessage))
                    return cancelled();
            }
        }
        return {OperationResult::success(), result};
    }

    if (!reportProgress(progress, 0, "Sampling source surface..."))
        return cancelled();

    std::vector<SurfaceSample> sourceSamples;
    if (!sampleMeshSurface(
            sourceMesh,
            options.sampleCount,
            options.randomSeed,
            0,
            30,
            "Sampling source surface...",
            progress,
            cancellationRequested,
            &sourceSamples)) {
        return cancelled();
    }

    std::vector<SurfaceSample> referenceSamples;
    if (!sampleMeshSurface(
            referenceMesh,
            options.sampleCount,
            options.randomSeed ^ kReferenceSeedXor,
            30,
            60,
            "Sampling reference surface...",
            progress,
            cancellationRequested,
            &referenceSamples)) {
        return cancelled();
    }

    if (!reportProgress(progress, 60, "Building reference sample KD-tree..."))
        return cancelled();

    std::vector<Point3m> referencePositions;
    referencePositions.reserve(referenceSamples.size());
    for (const SurfaceSample& sample : referenceSamples) {
        if (isCancellationRequested(cancellationRequested))
            return cancelled();
        referencePositions.push_back(sample.position);
    }
    if (isCancellationRequested(cancellationRequested))
        return cancelled();
    vcg::VectorConstDataWrapper<std::vector<Point3m>> referenceWrapper(referencePositions);
    vcg::KdTree<Scalarm> referenceTree(referenceWrapper);
    if (isCancellationRequested(cancellationRequested))
        return cancelled();

    SurfaceComparisonResult result;
    result.sampleCount = int(sourceSamples.size());
    result.faceScores.fill(-1.0, source.faces.size());
    std::vector<Scalarm> faceScoreSums(size_t(source.faces.size()), Scalarm(0));
    std::vector<unsigned int> faceSampleCounts(size_t(source.faces.size()), 0);
    double globalScoreSum = 0.0;

    const char* computeMessage = metric == SurfaceComparisonMetric::PrecisionAtThreshold
        ? "Computing source-to-reference precision..."
        : "Computing source-to-reference normal agreement...";
    if (!reportProgress(progress, 65, computeMessage))
        return cancelled();

    for (size_t sampleIndex = 0; sampleIndex < sourceSamples.size(); ++sampleIndex) {
        if (isCancellationRequested(cancellationRequested))
            return cancelled();
        const SurfaceSample& sourceSample = sourceSamples[sampleIndex];
        const double score = scoreSurfaceProbe(
            sourceSample.position,
            sourceSample.faceNormal,
            referenceSamples,
            referenceTree,
            metric,
            options);

        faceScoreSums[size_t(sourceSample.faceIndex)] += Scalarm(score);
        ++faceSampleCounts[size_t(sourceSample.faceIndex)];
        globalScoreSum += score;

        if ((sampleIndex + 1) % 8192 == 0 || sampleIndex + 1 == sourceSamples.size()) {
            const int percent = 65
                + int((static_cast<long long>(sampleIndex + 1) * 20)
                      / sourceSamples.size());
            if (!reportProgress(progress, percent, computeMessage))
                return cancelled();
        }
    }

    for (int faceIndex = 0; faceIndex < result.faceScores.size(); ++faceIndex) {
        if (isCancellationRequested(cancellationRequested))
            return cancelled();
        if (faceSampleCounts[size_t(faceIndex)] > 0) {
            result.faceScores[faceIndex] = double(
                faceScoreSums[size_t(faceIndex)]
                / Scalarm(faceSampleCounts[size_t(faceIndex)]));
        }
        else {
            const std::array<int, 3>& face = sourceMesh.faces[size_t(faceIndex)];
            const Point3m& p0 = sourceMesh.vertices[size_t(face[0])];
            const Point3m& p1 = sourceMesh.vertices[size_t(face[1])];
            const Point3m& p2 = sourceMesh.vertices[size_t(face[2])];
            Point3m faceNormal = (p1 - p0) ^ (p2 - p0);
            const Scalarm normalLength = faceNormal.Norm();
            if (!std::isfinite(double(normalLength)) || normalLength <= Scalarm(0)) {
                result.faceScores[faceIndex] = 0.0;
            }
            else {
                faceNormal /= normalLength;
                result.faceScores[faceIndex] = scoreSurfaceProbe(
                    triangleCentroid(p0, p1, p2),
                    faceNormal,
                    referenceSamples,
                    referenceTree,
                    metric,
                    options);
            }
        }
        ++result.coloredFaceCount;
        if ((faceIndex + 1) % 8192 == 0 ||
            faceIndex + 1 == result.faceScores.size()) {
            const int percent = 85
                + int((static_cast<long long>(faceIndex + 1) * 10)
                      / result.faceScores.size());
            if (!reportProgress(progress, percent, kFaceColorMessage))
                return cancelled();
        }
    }
    result.globalScore = sourceSamples.empty()
        ? 0.0
        : globalScoreSum / double(sourceSamples.size());
    return {OperationResult::success(), result};
}

OperationResult validateSurfaceComparisonOptions(
    SurfaceComparisonMetric metric,
    const SurfaceComparisonOptions& options)
{
    switch (metric) {
    case SurfaceComparisonMetric::PrecisionAtThreshold:
    case SurfaceComparisonMetric::NormalAgreement:
        break;
    default:
        return OperationResult::failure(
            QStringLiteral("Surface comparison metric is invalid."));
    }
    if (options.sampleCount <= 0) {
        return OperationResult::failure(
            QStringLiteral("Sample count must be greater than zero."));
    }
    if (std::isnan(double(options.distanceThreshold))
        || options.distanceThreshold < 0.0f) {
        return OperationResult::failure(
            QStringLiteral("Distance threshold must be non-negative."));
    }
    return OperationResult::success();
}

QVector<QColor> surfaceScoreColors(const QVector<double>& faceScores)
{
    QVector<QColor> colors;
    colors.reserve(faceScores.size());
    for (double score : faceScores) {
        colors.push_back(score < 0.0 ? QColor(128, 128, 128, 255)
                                     : redYellowGreen(score));
    }
    return colors;
}

SurfaceComparisonOutcome SurfaceComparer::compare(
    const SurfaceMeshSnapshot& source,
    const SurfaceMeshSnapshot& reference,
    SurfaceComparisonMetric metric,
    const SurfaceComparisonOptions& options,
    AnalysisProgress progress,
    AnalysisCancellation cancellationRequested) const
{
    return compareSampledSurfaces(
        source,
        reference,
        metric,
        options,
        std::move(progress),
        std::move(cancellationRequested));
}
