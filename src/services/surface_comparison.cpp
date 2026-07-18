#include "surface_comparison.h"

#include "core/analysis_color_map.h"

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

SurfacePoint3D subtract(
    const SurfacePoint3D& left,
    const SurfacePoint3D& right)
{
    return {
        left[0] - right[0],
        left[1] - right[1],
        left[2] - right[2]};
}

SurfacePoint3D addScaled(
    const SurfacePoint3D& origin,
    const SurfacePoint3D& direction,
    double scale)
{
    return {
        origin[0] + direction[0] * scale,
        origin[1] + direction[1] * scale,
        origin[2] + direction[2] * scale};
}

double vectorDot(const SurfacePoint3D& left, const SurfacePoint3D& right)
{
    return left[0] * right[0] + left[1] * right[1] + left[2] * right[2];
}

double squaredLength(const SurfacePoint3D& value)
{
    return vectorDot(value, value);
}

double pointTriangleSquaredDistance(
    const SurfacePoint3D& point,
    const SurfacePoint3D& first,
    const SurfacePoint3D& second,
    const SurfacePoint3D& third)
{
    const SurfacePoint3D firstToSecond = subtract(second, first);
    const SurfacePoint3D firstToThird = subtract(third, first);
    const SurfacePoint3D firstToPoint = subtract(point, first);
    const double firstSecondProjection =
        vectorDot(firstToSecond, firstToPoint);
    const double firstThirdProjection =
        vectorDot(firstToThird, firstToPoint);
    if (firstSecondProjection <= 0.0 && firstThirdProjection <= 0.0)
        return squaredLength(firstToPoint);

    const SurfacePoint3D secondToPoint = subtract(point, second);
    const double secondProjection = vectorDot(firstToSecond, secondToPoint);
    const double secondThirdProjection =
        vectorDot(firstToThird, secondToPoint);
    if (secondProjection >= 0.0 &&
        secondThirdProjection <= secondProjection) {
        return squaredLength(secondToPoint);
    }

    const double firstEdgeRegion =
        firstSecondProjection * secondThirdProjection
        - secondProjection * firstThirdProjection;
    if (firstEdgeRegion <= 0.0 && firstSecondProjection >= 0.0 &&
        secondProjection <= 0.0) {
        const double edgeFraction =
            firstSecondProjection / (firstSecondProjection - secondProjection);
        return squaredLength(
            subtract(point, addScaled(first, firstToSecond, edgeFraction)));
    }

    const SurfacePoint3D thirdToPoint = subtract(point, third);
    const double thirdSecondProjection =
        vectorDot(firstToSecond, thirdToPoint);
    const double thirdProjection = vectorDot(firstToThird, thirdToPoint);
    if (thirdProjection >= 0.0 &&
        thirdSecondProjection <= thirdProjection) {
        return squaredLength(thirdToPoint);
    }

    const double secondEdgeRegion =
        thirdSecondProjection * firstThirdProjection
        - firstSecondProjection * thirdProjection;
    if (secondEdgeRegion <= 0.0 && firstThirdProjection >= 0.0 &&
        thirdProjection <= 0.0) {
        const double edgeFraction =
            firstThirdProjection / (firstThirdProjection - thirdProjection);
        return squaredLength(
            subtract(point, addScaled(first, firstToThird, edgeFraction)));
    }

    const double oppositeEdgeRegion =
        secondProjection * thirdProjection
        - thirdSecondProjection * secondThirdProjection;
    if (oppositeEdgeRegion <= 0.0 &&
        secondThirdProjection - secondProjection >= 0.0 &&
        thirdSecondProjection - thirdProjection >= 0.0) {
        const SurfacePoint3D secondToThird = subtract(third, second);
        const double edgeFraction =
            (secondThirdProjection - secondProjection)
            / ((secondThirdProjection - secondProjection)
               + (thirdSecondProjection - thirdProjection));
        return squaredLength(
            subtract(point, addScaled(second, secondToThird, edgeFraction)));
    }

    const double inverseDenominator =
        1.0 / (firstEdgeRegion + secondEdgeRegion + oppositeEdgeRegion);
    const double secondWeight = secondEdgeRegion * inverseDenominator;
    const double thirdWeight = firstEdgeRegion * inverseDenominator;
    const SurfacePoint3D closest = addScaled(
        addScaled(first, firstToSecond, secondWeight),
        firstToThird,
        thirdWeight);
    return squaredLength(subtract(point, closest));
}

struct TriangleBounds {
    SurfacePoint3D minimum{{
        std::numeric_limits<double>::infinity(),
        std::numeric_limits<double>::infinity(),
        std::numeric_limits<double>::infinity()}};
    SurfacePoint3D maximum{{
        -std::numeric_limits<double>::infinity(),
        -std::numeric_limits<double>::infinity(),
        -std::numeric_limits<double>::infinity()}};
};

void includePoint(TriangleBounds* bounds, const SurfacePoint3D& point)
{
    for (int axis = 0; axis < 3; ++axis) {
        bounds->minimum[axis] =
            std::min(bounds->minimum[axis], point[axis]);
        bounds->maximum[axis] =
            std::max(bounds->maximum[axis], point[axis]);
    }
}

double pointBoundsSquaredDistance(
    const SurfacePoint3D& point,
    const TriangleBounds& bounds)
{
    double squaredDistance = 0.0;
    for (int axis = 0; axis < 3; ++axis) {
        if (point[axis] < bounds.minimum[axis]) {
            const double delta = bounds.minimum[axis] - point[axis];
            squaredDistance += delta * delta;
        }
        else if (point[axis] > bounds.maximum[axis]) {
            const double delta = point[axis] - bounds.maximum[axis];
            squaredDistance += delta * delta;
        }
    }
    return squaredDistance;
}

class ReferenceTriangleIndex
{
public:
    explicit ReferenceTriangleIndex(const SurfaceMeshSnapshot& reference)
        : reference_(reference)
    {
        triangleIndexes_.reserve(
            static_cast<std::size_t>(reference.faces.size()));
        for (int faceIndex = 0; faceIndex < reference.faces.size(); ++faceIndex) {
            const std::array<int, 3>& face = reference.faces[faceIndex];
            const SurfacePoint3D firstToSecond =
                subtract(reference.vertices[face[1]], reference.vertices[face[0]]);
            const SurfacePoint3D firstToThird =
                subtract(reference.vertices[face[2]], reference.vertices[face[0]]);
            const SurfacePoint3D cross{{
                firstToSecond[1] * firstToThird[2]
                    - firstToSecond[2] * firstToThird[1],
                firstToSecond[2] * firstToThird[0]
                    - firstToSecond[0] * firstToThird[2],
                firstToSecond[0] * firstToThird[1]
                    - firstToSecond[1] * firstToThird[0]}};
            if (squaredLength(cross) > 0.0)
                triangleIndexes_.push_back(faceIndex);
        }
        nodes_.reserve(triangleIndexes_.size() * 2);
        if (!triangleIndexes_.empty())
            root_ = build(0, int(triangleIndexes_.size()));
    }

    bool empty() const
    {
        return root_ < 0;
    }

    double squaredDistance(const SurfacePoint3D& point) const
    {
        double best = std::numeric_limits<double>::infinity();
        query(root_, point, &best);
        return best;
    }

private:
    struct Node {
        TriangleBounds bounds;
        int begin = 0;
        int end = 0;
        int left = -1;
        int right = -1;
    };

    TriangleBounds faceBounds(int faceIndex) const
    {
        TriangleBounds result;
        const std::array<int, 3>& face = reference_.faces[faceIndex];
        includePoint(&result, reference_.vertices[face[0]]);
        includePoint(&result, reference_.vertices[face[1]]);
        includePoint(&result, reference_.vertices[face[2]]);
        return result;
    }

    double faceCentroidAxis(int faceIndex, int axis) const
    {
        const std::array<int, 3>& face = reference_.faces[faceIndex];
        return (
            reference_.vertices[face[0]][axis]
            + reference_.vertices[face[1]][axis]
            + reference_.vertices[face[2]][axis])
            / 3.0;
    }

    int build(int begin, int end)
    {
        Node node;
        node.begin = begin;
        node.end = end;
        for (int index = begin; index < end; ++index) {
            const TriangleBounds triangle =
                faceBounds(triangleIndexes_[std::size_t(index)]);
            includePoint(&node.bounds, triangle.minimum);
            includePoint(&node.bounds, triangle.maximum);
        }

        const int nodeIndex = int(nodes_.size());
        nodes_.push_back(node);
        if (end - begin <= 8)
            return nodeIndex;

        int splitAxis = 0;
        double largestExtent =
            node.bounds.maximum[0] - node.bounds.minimum[0];
        for (int axis = 1; axis < 3; ++axis) {
            const double extent =
                node.bounds.maximum[axis] - node.bounds.minimum[axis];
            if (extent > largestExtent) {
                splitAxis = axis;
                largestExtent = extent;
            }
        }
        const int middle = begin + (end - begin) / 2;
        std::nth_element(
            triangleIndexes_.begin() + begin,
            triangleIndexes_.begin() + middle,
            triangleIndexes_.begin() + end,
            [this, splitAxis](int left, int right) {
                return faceCentroidAxis(left, splitAxis)
                    < faceCentroidAxis(right, splitAxis);
            });
        const int left = build(begin, middle);
        const int right = build(middle, end);
        nodes_[std::size_t(nodeIndex)].left = left;
        nodes_[std::size_t(nodeIndex)].right = right;
        return nodeIndex;
    }

    void query(
        int nodeIndex,
        const SurfacePoint3D& point,
        double* bestSquaredDistance) const
    {
        if (nodeIndex < 0)
            return;
        const Node& node = nodes_[std::size_t(nodeIndex)];
        if (pointBoundsSquaredDistance(point, node.bounds)
            > *bestSquaredDistance) {
            return;
        }

        if (node.left < 0) {
            for (int index = node.begin; index < node.end; ++index) {
                const std::array<int, 3>& face =
                    reference_.faces[triangleIndexes_[std::size_t(index)]];
                *bestSquaredDistance = std::min(
                    *bestSquaredDistance,
                    pointTriangleSquaredDistance(
                        point,
                        reference_.vertices[face[0]],
                        reference_.vertices[face[1]],
                        reference_.vertices[face[2]]));
            }
            return;
        }

        const double leftDistance = pointBoundsSquaredDistance(
            point, nodes_[std::size_t(node.left)].bounds);
        const double rightDistance = pointBoundsSquaredDistance(
            point, nodes_[std::size_t(node.right)].bounds);
        if (leftDistance <= rightDistance) {
            query(node.left, point, bestSquaredDistance);
            query(node.right, point, bestSquaredDistance);
        }
        else {
            query(node.right, point, bestSquaredDistance);
            query(node.left, point, bestSquaredDistance);
        }
    }

    const SurfaceMeshSnapshot& reference_;
    std::vector<int> triangleIndexes_;
    std::vector<Node> nodes_;
    int root_ = -1;
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

SurfaceComparisonOutcome compareVertexDistancesToReference(
    const SurfaceMeshSnapshot& source,
    const SurfaceMeshSnapshot& reference,
    const AnalysisProgress& progress,
    const AnalysisCancellation& cancellationRequested)
{
    if (source.vertices.isEmpty()) {
        return failure(
            QStringLiteral("The current source layer has no vertices."));
    }
    for (const SurfacePoint3D& vertex : source.vertices) {
        if (isCancellationRequested(cancellationRequested))
            return cancelled();
        if (!std::isfinite(vertex[0]) || !std::isfinite(vertex[1])
            || !std::isfinite(vertex[2])) {
            return failure(QStringLiteral(
                "Surface mesh snapshot vertices must be finite."));
        }
    }

    WorkingMesh validatedReference;
    const OperationResult referenceValidation = makeWorkingMesh(
        reference,
        QStringLiteral("The reference layer has no faces."),
        QStringLiteral("The reference layer has no positive-area triangles."),
        true,
        cancellationRequested,
        &validatedReference);
    if (!referenceValidation.ok)
        return failure(referenceValidation.error);

    if (!reportProgress(
            progress, 0, "Building reference triangle index...")) {
        return cancelled();
    }
    ReferenceTriangleIndex referenceIndex(reference);
    if (referenceIndex.empty()) {
        return failure(
            QStringLiteral("The reference layer has no positive-area triangles."));
    }
    if (isCancellationRequested(cancellationRequested))
        return cancelled();

    SurfaceComparisonResult result;
    result.vertexDistances.reserve(source.vertices.size());
    std::vector<double> sortedDistances;
    sortedDistances.reserve(std::size_t(source.vertices.size()));
    double distanceSum = 0.0;
    double maximumDistance = 0.0;
    for (int vertexIndex = 0;
         vertexIndex < source.vertices.size();
         ++vertexIndex) {
        if (isCancellationRequested(cancellationRequested))
            return cancelled();
        const double squaredDistance =
            referenceIndex.squaredDistance(source.vertices[vertexIndex]);
        const double distance =
            std::sqrt(std::max(0.0, squaredDistance));
        if (!std::isfinite(distance)) {
            return failure(QStringLiteral(
                "Distance analysis could not find a finite reference point."));
        }
        result.vertexDistances.append(distance);
        sortedDistances.push_back(distance);
        distanceSum += distance;
        maximumDistance = std::max(maximumDistance, distance);

        if ((vertexIndex + 1) % 8192 == 0 ||
            vertexIndex + 1 == source.vertices.size()) {
            const int percent = 5
                + int((static_cast<long long>(vertexIndex + 1) * 95)
                      / source.vertices.size());
            if (!reportProgress(
                    progress,
                    percent,
                    "Computing exact vertex-to-reference distances...")) {
                return cancelled();
            }
        }
    }

    std::sort(sortedDistances.begin(), sortedDistances.end());
    const double percentilePosition =
        0.99 * double(sortedDistances.size() - 1);
    const std::size_t percentileLower =
        std::size_t(std::floor(percentilePosition));
    const std::size_t percentileUpper =
        std::size_t(std::ceil(percentilePosition));
    const double percentileFraction =
        percentilePosition - double(percentileLower);
    const double percentile99 =
        sortedDistances[percentileLower] * (1.0 - percentileFraction)
        + sortedDistances[percentileUpper] * percentileFraction;

    result.distanceStatistics.vertexCount = source.vertices.size();
    result.distanceStatistics.finiteVertexCount = source.vertices.size();
    result.distanceStatistics.meanDistance =
        distanceSum / double(source.vertices.size());
    result.distanceStatistics.percentile99Distance = percentile99;
    result.distanceStatistics.maxDistance = maximumDistance;
    return {OperationResult::success(), std::move(result)};
}

SurfaceComparisonOutcome detectDoubleLayer(
    const SurfaceMeshSnapshot& source,
    const SurfaceComparisonOptions& options,
    const AnalysisProgress& progress,
    const AnalysisCancellation& cancellationRequested)
{
    WorkingMesh sourceMesh;
    const OperationResult sourceValidation = makeWorkingMesh(
        source,
        QStringLiteral("The current source layer has no faces."),
        QStringLiteral("The current source layer has no positive-area triangles."),
        true,
        cancellationRequested,
        &sourceMesh);
    if (!sourceValidation.ok)
        return failure(sourceValidation.error);

    SurfaceComparisonResult result;
    result.faceScores.fill(0.0, source.faces.size());
    result.vertexScores.fill(0.0, source.vertices.size());
    result.coloredFaceCount = source.faces.size();
    if (!reportProgress(
            progress, 0, "Sampling source surface for double layers...")) {
        return cancelled();
    }
    std::vector<SurfaceSample> samples;
    if (!sampleMeshSurface(
            sourceMesh,
            options.sampleCount,
            options.doubleLayerRandomSeed,
            0,
            35,
            "Sampling source surface for double layers...",
            progress,
            cancellationRequested,
            &samples)) {
        return cancelled();
    }
    result.sampleCount = int(samples.size());
    result.doubleLayerStatistics.sampleCount = int(samples.size());
    if (samples.size() < 2) {
        if (!reportProgress(
                progress, 100, "Double-layer analysis complete.")) {
            return cancelled();
        }
        return {OperationResult::success(), std::move(result)};
    }

    if (!reportProgress(
            progress, 40, "Building double-layer sample KD-tree...")) {
        return cancelled();
    }
    std::vector<Point3m> samplePositions;
    samplePositions.reserve(samples.size());
    for (const SurfaceSample& sample : samples) {
        if (isCancellationRequested(cancellationRequested))
            return cancelled();
        samplePositions.push_back(sample.position);
    }
    vcg::VectorConstDataWrapper<std::vector<Point3m>> sampleWrapper(
        samplePositions);
    vcg::KdTree<Scalarm> sampleTree(sampleWrapper);
    if (isCancellationRequested(cancellationRequested))
        return cancelled();

    const int effectiveNeighborCount = std::min(
        options.nearestNeighborCount,
        int(samples.size()) - 1);
    const double oppositeDotThreshold = std::cos(
        options.oppositeNormalAngleDegrees
        * 3.14159265358979323846
        / 180.0);
    vcg::KdTree<Scalarm>::PriorityQueue neighborQueue;
    double sampleScoreSum = 0.0;
    int affectedSampleCount = 0;
    for (int sampleIndex = 0; sampleIndex < int(samples.size()); ++sampleIndex) {
        if (isCancellationRequested(cancellationRequested))
            return cancelled();

        sampleTree.doQueryK(
            samples[std::size_t(sampleIndex)].position,
            effectiveNeighborCount + 1,
            neighborQueue);
        int consideredNeighbors = 0;
        int oppositeNeighborCount = 0;
        for (int neighborOffset = 0;
             neighborOffset < neighborQueue.getNofElements()
             && consideredNeighbors < effectiveNeighborCount;
             ++neighborOffset) {
            const int neighborIndex = neighborQueue.getIndex(neighborOffset);
            if (neighborIndex == sampleIndex)
                continue;
            ++consideredNeighbors;
            const double normalDot = double(pointDot(
                samples[std::size_t(sampleIndex)].faceNormal,
                samples[std::size_t(neighborIndex)].faceNormal));
            if (normalDot < oppositeDotThreshold)
                ++oppositeNeighborCount;
        }
        const double sampleScore = consideredNeighbors == 0
            ? 0.0
            : double(oppositeNeighborCount) / double(consideredNeighbors);
        sampleScoreSum += sampleScore;
        if (oppositeNeighborCount > 0)
            ++affectedSampleCount;
        const int faceIndex = samples[std::size_t(sampleIndex)].faceIndex;
        result.faceScores[faceIndex] =
            std::max(result.faceScores[faceIndex], sampleScore);

        if ((sampleIndex + 1) % 8192 == 0 ||
            sampleIndex + 1 == int(samples.size())) {
            const int percent = 45
                + int((static_cast<long long>(sampleIndex + 1) * 45)
                      / int(samples.size()));
            if (!reportProgress(
                    progress,
                    percent,
                    "Detecting opposite-normal sample neighbors...")) {
                return cancelled();
            }
        }
    }

    result.vertexScores = projectFaceMaximumScoresToVertices(
        result.faceScores,
        source.faces,
        source.vertices.size());
    int affectedFaceCount = 0;
    for (double faceScore : result.faceScores)
        affectedFaceCount += faceScore > 0.0 ? 1 : 0;
    int affectedVertexCount = 0;
    for (double vertexScore : result.vertexScores)
        affectedVertexCount += vertexScore > 0.0 ? 1 : 0;

    result.globalScore =
        sampleScoreSum / double(samples.size());
    result.doubleLayerStatistics.meanSampleScore = result.globalScore;
    result.doubleLayerStatistics.affectedSampleFraction =
        double(affectedSampleCount) / double(samples.size());
    result.doubleLayerStatistics.affectedFaceFraction =
        source.faces.isEmpty()
            ? 0.0
            : double(affectedFaceCount) / double(source.faces.size());
    result.doubleLayerStatistics.affectedVertexFraction =
        source.vertices.isEmpty()
            ? 0.0
            : double(affectedVertexCount) / double(source.vertices.size());
    if (!reportProgress(
            progress, 100, "Double-layer analysis complete.")) {
        return cancelled();
    }
    return {OperationResult::success(), std::move(result)};
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
    if (metric == SurfaceComparisonMetric::DistanceToReference) {
        return compareVertexDistancesToReference(
            source,
            reference,
            progress,
            cancellationRequested);
    }
    if (metric == SurfaceComparisonMetric::DoubleLayer) {
        return detectDoubleLayer(
            source,
            options,
            progress,
            cancellationRequested);
    }

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
    case SurfaceComparisonMetric::DistanceToReference:
        if (!std::isfinite(options.distanceColorMax)
            || options.distanceColorMax < 0.0) {
            return OperationResult::failure(
                QStringLiteral("Distance color maximum must be non-negative and finite."));
        }
        if (!isValidDistanceColorMapping(
                options.distanceColorMapping)) {
            return OperationResult::failure(
                QStringLiteral("Distance color mapping is invalid."));
        }
        return OperationResult::success();
    case SurfaceComparisonMetric::DoubleLayer:
        if (options.sampleCount <= 0) {
            return OperationResult::failure(
                QStringLiteral("Sample count must be greater than zero."));
        }
        if (options.nearestNeighborCount <= 0) {
            return OperationResult::failure(QStringLiteral(
                "Nearest-neighbor count must be greater than zero."));
        }
        if (!std::isfinite(options.oppositeNormalAngleDegrees)
            || options.oppositeNormalAngleDegrees < 0.0
            || options.oppositeNormalAngleDegrees > 180.0) {
            return OperationResult::failure(QStringLiteral(
                "Opposite-normal angle must be finite and between 0 and 180 degrees."));
        }
        return OperationResult::success();
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

QVector<double> projectFaceMaximumScoresToVertices(
    const QVector<double>& faceScores,
    const QVector<std::array<int, 3>>& faces,
    int vertexCount)
{
    if (vertexCount < 0 || faceScores.size() != faces.size())
        return {};
    QVector<double> vertexScores(vertexCount, 0.0);
    for (int faceIndex = 0; faceIndex < faces.size(); ++faceIndex) {
        const std::array<int, 3>& face = faces[faceIndex];
        for (int vertexIndex : face) {
            if (vertexIndex < 0 || vertexIndex >= vertexCount)
                return {};
            vertexScores[vertexIndex] =
                std::max(vertexScores[vertexIndex], faceScores[faceIndex]);
        }
    }
    return vertexScores;
}

QVector<QColor> distanceToVertexColors(
    const QVector<double>& vertexDistances,
    double maxDistance,
    DistanceColorMapping mapping)
{
    QVector<QColor> colors;
    colors.reserve(vertexDistances.size());
    for (double distance : vertexDistances) {
        colors.append(
            distanceColorForValue(distance, maxDistance, mapping));
    }
    return colors;
}

QVector<QColor> doubleLayerVertexColors(
    const QVector<double>& vertexScores)
{
    QVector<QColor> colors;
    colors.reserve(vertexScores.size());
    for (double score : vertexScores) {
        if (!std::isfinite(score) || score <= 0.0) {
            colors.append(QColor(180, 180, 180, 255));
            continue;
        }
        const double strength =
            std::sqrt(std::max(0.0, std::min(1.0, score)));
        const int green =
            int(std::nearbyint(210.0 * (1.0 - strength)));
        const int blue =
            int(std::nearbyint(48.0 * strength));
        colors.append(QColor(255, green, blue, 255));
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
