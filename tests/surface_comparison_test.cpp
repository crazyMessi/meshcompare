#include <QtTest>

#include <common/ml_document/base_types.h>

#include "services/surface_comparison.h"

#include <array>
#include <limits>
#include <type_traits>

namespace {

SurfaceMeshSnapshot triangleAtZ(double z, bool reverseWinding = false)
{
    SurfaceMeshSnapshot mesh;
    mesh.vertices = {
        SurfacePoint3D{0.0, 0.0, z},
        SurfacePoint3D{1.0, 0.0, z},
        SurfacePoint3D{0.0, 1.0, z},
    };
    mesh.faces.push_back(reverseWinding ? std::array<int, 3>{0, 2, 1}
                                        : std::array<int, 3>{0, 1, 2});
    return mesh;
}

SurfaceMeshSnapshot twoLayerTriangles(bool oppositeNormals)
{
    SurfaceMeshSnapshot mesh;
    mesh.vertices = {
        SurfacePoint3D{0.0, 0.0, 0.0},
        SurfacePoint3D{1.0, 0.0, 0.0},
        SurfacePoint3D{0.0, 1.0, 0.0},
        SurfacePoint3D{0.0, 0.0, 0.001},
        SurfacePoint3D{1.0, 0.0, 0.001},
        SurfacePoint3D{0.0, 1.0, 0.001},
    };
    mesh.faces = {
        std::array<int, 3>{0, 1, 2},
        oppositeNormals ? std::array<int, 3>{3, 5, 4}
                        : std::array<int, 3>{3, 4, 5},
    };
    return mesh;
}

SurfaceComparisonOptions quickOptions()
{
    SurfaceComparisonOptions options;
    options.sampleCount = 2000;
    options.distanceThreshold = 2.0f;
    options.randomSeed = 17;
    return options;
}

void verifyCancelled(const SurfaceComparisonOutcome& outcome)
{
    QVERIFY(!outcome.result.ok);
    QCOMPARE(outcome.result.error, QStringLiteral("Analysis cancelled."));
    QCOMPARE(outcome.comparison.sampleCount, 0);
    QCOMPARE(outcome.comparison.coloredFaceCount, 0);
    QCOMPARE(outcome.comparison.globalScore, 0.0);
    QVERIFY(outcome.comparison.faceScores.isEmpty());
}

} // namespace

class SurfaceComparisonTest : public QObject
{
    Q_OBJECT

private slots:
    void publicSnapshotAndScoresRetainDoublePrecision()
    {
        static_assert(
            std::is_same<decltype(SurfaceComparisonResult{}.faceScores), QVector<double>>::value,
            "Public face scores must retain double precision.");

        SurfaceMeshSnapshot mesh;
        mesh.vertices = {
            SurfacePoint3D{16777216.0, 0.0, 0.0},
            SurfacePoint3D{16777217.0, 0.0, 0.0},
            SurfacePoint3D{16777216.0, 1.0, 0.0},
        };

        QCOMPARE(mesh.vertices[1][0] - mesh.vertices[0][0], 1.0);
    }

    void distanceToReferenceComputesExactPerVertexTriangleDistances()
    {
        SurfaceMeshSnapshot source;
        source.vertices = {
            SurfacePoint3D{0.25, 0.25, 1.0},
            SurfacePoint3D{2.0, 0.0, 0.0},
            SurfacePoint3D{-1.0, -1.0, 0.0},
            SurfacePoint3D{0.5, 0.5, 0.0},
        };
        const SurfaceMeshSnapshot reference = triangleAtZ(0.0);

        const SurfaceComparisonOutcome outcome = compareSampledSurfaces(
            source,
            reference,
            SurfaceComparisonMetric::DistanceToReference,
            SurfaceComparisonOptions{});

        QVERIFY2(outcome.result.ok, qPrintable(outcome.result.error));
        QCOMPARE(outcome.comparison.vertexDistances.size(), 4);
        QVERIFY(qAbs(outcome.comparison.vertexDistances[0] - 1.0) < 1e-12);
        QVERIFY(qAbs(outcome.comparison.vertexDistances[1] - 1.0) < 1e-12);
        QVERIFY(
            qAbs(outcome.comparison.vertexDistances[2] - std::sqrt(2.0))
            < 1e-12);
        QVERIFY(qAbs(outcome.comparison.vertexDistances[3]) < 1e-12);
        QCOMPARE(outcome.comparison.distanceStatistics.vertexCount, 4);
        QCOMPARE(outcome.comparison.distanceStatistics.finiteVertexCount, 4);
        QVERIFY(
            qAbs(outcome.comparison.distanceStatistics.maxDistance - std::sqrt(2.0))
            < 1e-12);
    }

    void distanceToReferenceBuildsOneIndexForTheWholeVertexBatch()
    {
        SurfaceMeshSnapshot source;
        for (int index = 0; index < 100; ++index) {
            source.vertices.append(
                SurfacePoint3D{double(index) / 100.0, 0.25, 0.5});
        }
        int indexBuildMessages = 0;

        const SurfaceComparisonOutcome outcome = compareSampledSurfaces(
            source,
            triangleAtZ(0.0),
            SurfaceComparisonMetric::DistanceToReference,
            SurfaceComparisonOptions{},
            [&indexBuildMessages](int, const QString& message) {
                if (message == QStringLiteral(
                                   "Building reference triangle index...")) {
                    ++indexBuildMessages;
                }
                return true;
            });

        QVERIFY2(outcome.result.ok, qPrintable(outcome.result.error));
        QCOMPARE(indexBuildMessages, 1);
        QCOMPARE(outcome.comparison.vertexDistances.size(), 100);
    }

    void distanceVertexColorsMatchMatplotlibViridisWithSqrtClamping()
    {
        const QVector<double> distances = {
            0.0,
            0.00015625,
            0.0025,
            0.01,
            0.0225,
            0.04,
            1.0,
            std::numeric_limits<double>::infinity(),
            std::numeric_limits<double>::quiet_NaN(),
        };

        const QVector<QColor> colors = distanceToVertexColors(distances);

        QCOMPARE(colors.size(), distances.size());
        QCOMPARE(colors[0], QColor(68, 1, 84, 255));
        QCOMPARE(colors[1], QColor(72, 24, 106, 255));
        QCOMPARE(colors[2], QColor(59, 82, 139, 255));
        QCOMPARE(colors[3], QColor(33, 145, 140, 255));
        QCOMPARE(colors[4], QColor(94, 201, 98, 255));
        QCOMPARE(colors[5], QColor(253, 231, 37, 255));
        QCOMPARE(colors[6], QColor(253, 231, 37, 255));
        QCOMPARE(colors[7], QColor(0, 0, 0, 255));
        QCOMPARE(colors[8], QColor(0, 0, 0, 255));
        QCOMPARE(
            distanceToVertexColors({0.0, 1.0}, 0.0),
            QVector<QColor>({
                QColor(68, 1, 84, 255),
                QColor(68, 1, 84, 255)}));
    }

    void distanceVertexColorsSupportLinearMapping()
    {
        const QVector<QColor> colors = distanceToVertexColors(
            {0.0, 0.01, 0.02, 0.04, 1.0},
            0.04,
            DistanceColorMapping::Linear);

        QCOMPARE(
            colors,
            QVector<QColor>({
                QColor(68, 1, 84, 255),
                QColor(59, 82, 139, 255),
                QColor(33, 145, 140, 255),
                QColor(253, 231, 37, 255),
                QColor(253, 231, 37, 255),
            }));
    }

    void doubleLayerIsDeterministicAndDetectsOnlyOppositeNearbyNormals()
    {
        SurfaceComparisonOptions options;
        options.sampleCount = 512;
        options.nearestNeighborCount = 20;
        options.oppositeNormalAngleDegrees = 170.0;
        options.doubleLayerRandomSeed = 0;

        const SurfaceComparisonOutcome sameDirection = compareSampledSurfaces(
            twoLayerTriangles(false),
            {},
            SurfaceComparisonMetric::DoubleLayer,
            options);
        const SurfaceComparisonOutcome opposite = compareSampledSurfaces(
            twoLayerTriangles(true),
            {},
            SurfaceComparisonMetric::DoubleLayer,
            options);
        const SurfaceComparisonOutcome repeated = compareSampledSurfaces(
            twoLayerTriangles(true),
            {},
            SurfaceComparisonMetric::DoubleLayer,
            options);

        QVERIFY2(sameDirection.result.ok, qPrintable(sameDirection.result.error));
        QVERIFY2(opposite.result.ok, qPrintable(opposite.result.error));
        QVERIFY2(repeated.result.ok, qPrintable(repeated.result.error));
        QCOMPARE(
            sameDirection.comparison.vertexScores,
            QVector<double>(6, 0.0));
        QVERIFY(opposite.comparison.doubleLayerStatistics.meanSampleScore > 0.0);
        QVERIFY(
            opposite.comparison.doubleLayerStatistics.affectedVertexFraction
            > 0.0);
        QCOMPARE(
            repeated.comparison.vertexScores,
            opposite.comparison.vertexScores);
        QCOMPARE(
            repeated.comparison.doubleLayerStatistics.meanSampleScore,
            opposite.comparison.doubleLayerStatistics.meanSampleScore);
    }

    void doubleLayerProjectsFaceMaximumsAndUsesTheReferenceOrangeRedColors()
    {
        const QVector<std::array<int, 3>> faces = {
            std::array<int, 3>{0, 1, 2},
            std::array<int, 3>{2, 3, 4},
        };

        const QVector<double> vertexScores =
            projectFaceMaximumScoresToVertices(
                {0.25, 0.75},
                faces,
                5);
        const QVector<QColor> colors = doubleLayerVertexColors(
            {0.0, 0.25, 0.5625, 1.0});

        QCOMPARE(
            vertexScores,
            QVector<double>({0.25, 0.25, 0.75, 0.75, 0.75}));
        QCOMPARE(
            colors,
            QVector<QColor>({
                QColor(180, 180, 180, 255),
                QColor(255, 105, 24, 255),
                QColor(255, 52, 36, 255),
                QColor(255, 0, 48, 255),
            }));
    }

    void newAnalysisDefaultsValidationAndCancellationAreModeSpecific()
    {
        const SurfaceComparisonOptions defaults;
        QCOMPARE(defaults.sampleCount, 500000);
        QCOMPARE(defaults.distanceDisplayThreshold, 0.04);
        QCOMPARE(
            defaults.distanceColorMapping,
            DistanceColorMapping::SquareRoot);
        QCOMPARE(defaults.nearestNeighborCount, 20);
        QCOMPARE(defaults.oppositeNormalAngleDegrees, 170.0);
        QCOMPARE(defaults.doubleLayerRandomSeed, std::uint32_t(0));
        QCOMPARE(defaults.randomSeed, std::uint32_t(0x4d595df4u));

        SurfaceComparisonOptions invalid = defaults;
        invalid.sampleCount = 0;
        QVERIFY(!validateSurfaceComparisonOptions(
                     SurfaceComparisonMetric::DoubleLayer,
                     invalid)
                     .ok);
        invalid = defaults;
        invalid.distanceColorMapping =
            static_cast<DistanceColorMapping>(99);
        QVERIFY(!validateSurfaceComparisonOptions(
                     SurfaceComparisonMetric::DistanceToReference,
                     invalid)
                     .ok);
        invalid = defaults;
        invalid.nearestNeighborCount = 0;
        QVERIFY(!validateSurfaceComparisonOptions(
                     SurfaceComparisonMetric::DoubleLayer,
                     invalid)
                     .ok);
        invalid = defaults;
        invalid.oppositeNormalAngleDegrees =
            std::numeric_limits<double>::quiet_NaN();
        QVERIFY(!validateSurfaceComparisonOptions(
                     SurfaceComparisonMetric::DoubleLayer,
                     invalid)
                     .ok);
        invalid = defaults;
        invalid.oppositeNormalAngleDegrees = 181.0;
        QVERIFY(!validateSurfaceComparisonOptions(
                     SurfaceComparisonMetric::DoubleLayer,
                     invalid)
                     .ok);

        int progressCalls = 0;
        SurfaceComparisonOptions quick = defaults;
        quick.sampleCount = 64;
        const SurfaceComparisonOutcome cancelled = compareSampledSurfaces(
            twoLayerTriangles(true),
            {},
            SurfaceComparisonMetric::DoubleLayer,
            quick,
            [&progressCalls](int, const QString&) {
                ++progressCalls;
                return false;
            });
        QVERIFY(!cancelled.result.ok);
        QCOMPARE(
            cancelled.result.error,
            QStringLiteral("Analysis cancelled."));
        QCOMPARE(progressCalls, 1);
        QVERIFY(cancelled.comparison.vertexScores.isEmpty());
        QCOMPARE(cancelled.comparison.doubleLayerStatistics.sampleCount, 0);
    }

    void identicalTrianglesHavePerfectScores()
    {
        const SurfaceMeshSnapshot source = triangleAtZ(0.0f);
        const SurfaceMeshSnapshot reference = triangleAtZ(0.0f);
        const SurfaceComparisonOptions options = quickOptions();

        const SurfaceComparisonOutcome precision = compareSampledSurfaces(
            source, reference, SurfaceComparisonMetric::PrecisionAtThreshold, options);
        const SurfaceComparisonOutcome normals = compareSampledSurfaces(
            source, reference, SurfaceComparisonMetric::NormalAgreement, options);

        QVERIFY(precision.result.ok);
        QVERIFY(normals.result.ok);
        QCOMPARE(precision.comparison.globalScore, 1.0);
        QCOMPARE(normals.comparison.globalScore, 1.0);
    }

    void offsetTrianglesFailPrecisionButKeepNormalAgreement()
    {
        const SurfaceMeshSnapshot source = triangleAtZ(0.0f);
        const SurfaceMeshSnapshot reference = triangleAtZ(1.0f);
        SurfaceComparisonOptions options = quickOptions();
        options.distanceThreshold = 0.01f;

        const SurfaceComparisonOutcome precision = compareSampledSurfaces(
            source, reference, SurfaceComparisonMetric::PrecisionAtThreshold, options);
        const SurfaceComparisonOutcome normals = compareSampledSurfaces(
            source, reference, SurfaceComparisonMetric::NormalAgreement, options);

        QVERIFY(precision.result.ok);
        QVERIFY(normals.result.ok);
        QCOMPARE(precision.comparison.globalScore, 0.0);
        QCOMPARE(normals.comparison.globalScore, 1.0);
    }

    void reversedWindingHonorsAbsoluteAndDirectionalModes()
    {
        const SurfaceMeshSnapshot source = triangleAtZ(0.0f, true);
        const SurfaceMeshSnapshot reference = triangleAtZ(0.0f);
        SurfaceComparisonOptions options = quickOptions();

        options.useAbsoluteNormalDot = true;
        const SurfaceComparisonOutcome absolute = compareSampledSurfaces(
            source, reference, SurfaceComparisonMetric::NormalAgreement, options);
        options.useAbsoluteNormalDot = false;
        const SurfaceComparisonOutcome directional = compareSampledSurfaces(
            source, reference, SurfaceComparisonMetric::NormalAgreement, options);

        QVERIFY(absolute.result.ok);
        QVERIFY(directional.result.ok);
        QCOMPARE(absolute.comparison.globalScore, 1.0);
        QCOMPARE(directional.comparison.globalScore, 0.0);
    }

    void repeatabilityAndGoldenDetectSeedOrDrawOrderChanges()
    {
        SurfaceMeshSnapshot source = triangleAtZ(0.0f);
        source.faces.push_back({0, 2, 1});
        const SurfaceMeshSnapshot reference = triangleAtZ(0.0f);
        SurfaceComparisonOptions options;
        options.sampleCount = 20;
        options.useAbsoluteNormalDot = false;
        options.randomSeed = 17;

        const SurfaceComparisonOutcome first = compareSampledSurfaces(
            source, reference, SurfaceComparisonMetric::NormalAgreement, options);
        const SurfaceComparisonOutcome second = compareSampledSurfaces(
            source, reference, SurfaceComparisonMetric::NormalAgreement, options);

        QVERIFY(first.result.ok);
        QVERIFY(second.result.ok);
        QCOMPARE(first.comparison.sampleCount, 20);
        QCOMPARE(first.comparison.coloredFaceCount, 2);
        QCOMPARE(first.comparison.globalScore, 0.55);
        QCOMPARE(first.comparison.faceScores, QVector<double>({1.0, 0.0}));
        QCOMPARE(second.comparison.sampleCount, first.comparison.sampleCount);
        QCOMPARE(second.comparison.coloredFaceCount, first.comparison.coloredFaceCount);
        QCOMPARE(second.comparison.globalScore, first.comparison.globalScore);
        QCOMPARE(second.comparison.faceScores, first.comparison.faceScores);
    }

    void faceScoresPreserveLegacyScalarAccumulation()
    {
        SurfaceMeshSnapshot reference;
        reference.vertices = {
            SurfacePoint3D{0.0, 0.0, 0.0},
            SurfacePoint3D{1.0, 0.0, 0.0},
            SurfacePoint3D{0.0, 1.0, 1.0},
        };
        reference.faces.push_back({0, 1, 2});
        SurfaceComparisonOptions options = quickOptions();
        options.sampleCount = 20;

        const SurfaceComparisonOutcome outcome = compareSampledSurfaces(
            triangleAtZ(0.0),
            reference,
            SurfaceComparisonMetric::NormalAgreement,
            options);

        QVERIFY(outcome.result.ok);
        const Scalarm scalarScore = Scalarm(1) / vcg::math::Sqrt(Scalarm(2));
        Scalarm scalarSum = Scalarm(0);
        for (int sampleIndex = 0; sampleIndex < options.sampleCount; ++sampleIndex)
            scalarSum += scalarScore;
        const double expectedFaceScore =
            double(scalarSum / Scalarm(options.sampleCount));
        QCOMPARE(outcome.comparison.faceScores, QVector<double>({expectedFaceScore}));
    }

    void precisionGoldenBindsIndependentReferenceSeedAndSamplePositions()
    {
        SurfaceComparisonOptions options;
        options.sampleCount = 20;
        options.distanceThreshold = 0.05f;
        options.randomSeed = 17;

        const SurfaceComparisonOutcome outcome = compareSampledSurfaces(
            triangleAtZ(0.0f),
            triangleAtZ(0.0f),
            SurfaceComparisonMetric::PrecisionAtThreshold,
            options);

        QVERIFY(outcome.result.ok);
        QCOMPARE(outcome.comparison.sampleCount, 20);
        QCOMPARE(outcome.comparison.globalScore, 0.3);
        QCOMPARE(
            outcome.comparison.faceScores,
            QVector<double>({double(Scalarm(6) / Scalarm(20))}));

        SurfaceComparisonOptions legacyDefaults;
        legacyDefaults.sampleCount = 20;
        legacyDefaults.distanceThreshold = 0.025f;
        const SurfaceComparisonOutcome defaultSeedOutcome = compareSampledSurfaces(
            triangleAtZ(0.0f),
            triangleAtZ(0.0f),
            SurfaceComparisonMetric::PrecisionAtThreshold,
            legacyDefaults);
        QVERIFY(defaultSeedOutcome.result.ok);
        QCOMPARE(defaultSeedOutcome.comparison.globalScore, 0.2);
        QCOMPARE(
            defaultSeedOutcome.comparison.faceScores,
            QVector<double>({double(Scalarm(4) / Scalarm(20))}));
    }

    void precisionThresholdIsInclusive()
    {
        const double base = std::is_same<Scalarm, double>::value
            ? 9007199254740992.0
            : 100000000.0;
        const double scalarUlp = std::is_same<Scalarm, double>::value ? 2.0 : 8.0;
        SurfaceMeshSnapshot source;
        source.vertices = {
            SurfacePoint3D{base, base, 0.0},
            SurfacePoint3D{base + scalarUlp, base, 0.0},
            SurfacePoint3D{base, base + scalarUlp, 0.0},
        };
        source.faces.push_back({0, 1, 2});
        SurfaceMeshSnapshot reference = source;
        for (SurfacePoint3D& vertex : reference.vertices)
            vertex[2] = 1.0;

        SurfaceComparisonOptions options;
        options.sampleCount = 1;
        options.distanceThreshold = 1.0f;
        options.randomSeed = 6;

        const SurfaceComparisonOutcome outcome = compareSampledSurfaces(
            source, reference, SurfaceComparisonMetric::PrecisionAtThreshold, options);

        QVERIFY(outcome.result.ok);
        QCOMPARE(outcome.comparison.globalScore, 1.0);
    }

    void zeroAreaFacesReceiveDefinedAnalysisColor()
    {
        SurfaceMeshSnapshot source = triangleAtZ(0.0f);
        source.vertices.push_back(SurfacePoint3D{2.0, 0.0, 0.0});
        source.vertices.push_back(SurfacePoint3D{3.0, 0.0, 0.0});
        source.vertices.push_back(SurfacePoint3D{4.0, 0.0, 0.0});
        source.faces.push_back({3, 5, 4});
        SurfaceComparisonOptions options = quickOptions();
        options.sampleCount = 1;
        options.useAbsoluteNormalDot = false;

        const SurfaceComparisonOutcome outcome = compareSampledSurfaces(
            source,
            triangleAtZ(0.0f),
            SurfaceComparisonMetric::NormalAgreement,
            options);

        QVERIFY(outcome.result.ok);
        QCOMPARE(outcome.comparison.sampleCount, 1);
        QCOMPARE(outcome.comparison.coloredFaceCount, 2);
        QCOMPARE(outcome.comparison.globalScore, 1.0);
        QCOMPARE(outcome.comparison.faceScores, QVector<double>({1.0, 0.0}));
        QCOMPARE(surfaceScoreColors(outcome.comparison.faceScores),
                 QVector<QColor>({QColor(0, 255, 0, 255), QColor(255, 0, 0, 255)}));
    }

    void zeroAreaFacesHaveZeroPrecisionScore()
    {
        SurfaceMeshSnapshot source = triangleAtZ(0.0f);
        source.vertices.push_back(SurfacePoint3D{0.0, 0.0, 0.0});
        source.vertices.push_back(SurfacePoint3D{0.5, 0.0, 0.0});
        source.vertices.push_back(SurfacePoint3D{1.0, 0.0, 0.0});
        source.faces.push_back({3, 4, 5});
        SurfaceComparisonOptions options = quickOptions();
        options.sampleCount = 1;
        options.distanceThreshold = std::numeric_limits<float>::infinity();

        const SurfaceComparisonOutcome outcome = compareSampledSurfaces(
            source,
            triangleAtZ(0.0f),
            SurfaceComparisonMetric::PrecisionAtThreshold,
            options);

        QVERIFY(outcome.result.ok);
        QCOMPARE(outcome.comparison.coloredFaceCount, 2);
        QCOMPARE(outcome.comparison.faceScores.size(), 2);
        QCOMPARE(outcome.comparison.faceScores.at(1), 0.0);
    }

    void everyPositiveAreaFaceReceivesAnalysisColor()
    {
        SurfaceMeshSnapshot source = triangleAtZ(0.0f);
        source.vertices.push_back(SurfacePoint3D{0.0, 0.0, 5.0});
        source.vertices.push_back(SurfacePoint3D{1.0, 0.0, 5.0});
        source.vertices.push_back(SurfacePoint3D{0.0, 1.0, 5.0});
        source.faces.push_back({3, 5, 4});
        SurfaceComparisonOptions options = quickOptions();
        options.sampleCount = 1;
        options.useAbsoluteNormalDot = false;

        const SurfaceComparisonOutcome outcome = compareSampledSurfaces(
            source,
            triangleAtZ(0.0f),
            SurfaceComparisonMetric::NormalAgreement,
            options);

        QVERIFY(outcome.result.ok);
        QCOMPARE(outcome.comparison.coloredFaceCount, 2);
        QCOMPARE(outcome.comparison.globalScore, 1.0);
        QCOMPARE(outcome.comparison.faceScores.size(), 2);
        QCOMPARE(outcome.comparison.faceScores.at(0), 1.0);
        QCOMPARE(outcome.comparison.faceScores.at(1), 0.0);
    }

    void colorRampHasExactLegacyAnchors()
    {
        const QVector<double> scores = {-1.0, 0.0, 0.25, 0.5, 0.75, 1.0};

        const QVector<QColor> colors = surfaceScoreColors(scores);

        QCOMPARE(colors,
                 QVector<QColor>({
                     QColor(128, 128, 128, 255),
                     QColor(255, 0, 0, 255),
                     QColor(255, 128, 0, 255),
                     QColor(255, 255, 0, 255),
                     QColor(128, 255, 0, 255),
                     QColor(0, 255, 0, 255),
                 }));
    }

    void rejectsInvalidOptionsAndSnapshots()
    {
        const SurfaceMeshSnapshot triangle = triangleAtZ(0.0f);
        SurfaceComparisonOptions options = quickOptions();
        options.sampleCount = 0;
        SurfaceComparisonOutcome outcome = compareSampledSurfaces(
            triangle, triangle, SurfaceComparisonMetric::NormalAgreement, options);
        QVERIFY(!outcome.result.ok);
        QCOMPARE(outcome.result.error, QStringLiteral("Sample count must be greater than zero."));

        options = quickOptions();
        options.distanceThreshold = -0.01f;
        outcome = compareSampledSurfaces(
            triangle, triangle, SurfaceComparisonMetric::PrecisionAtThreshold, options);
        QVERIFY(!outcome.result.ok);
        QCOMPARE(outcome.result.error, QStringLiteral("Distance threshold must be non-negative."));

        options.distanceThreshold = std::numeric_limits<float>::quiet_NaN();
        outcome = compareSampledSurfaces(
            triangle, triangle, SurfaceComparisonMetric::PrecisionAtThreshold, options);
        QVERIFY(!outcome.result.ok);
        QCOMPARE(outcome.result.error, QStringLiteral("Distance threshold must be non-negative."));

        options.distanceThreshold = std::numeric_limits<float>::infinity();
        outcome = compareSampledSurfaces(
            triangle, triangle, SurfaceComparisonMetric::PrecisionAtThreshold, options);
        QVERIFY(outcome.result.ok);
        QCOMPARE(outcome.comparison.globalScore, 1.0);
        QCOMPARE(outcome.comparison.faceScores, QVector<double>({1.0}));

        options = quickOptions();
        outcome = compareSampledSurfaces(
            {}, triangle, SurfaceComparisonMetric::NormalAgreement, options);
        QVERIFY(!outcome.result.ok);
        QCOMPARE(outcome.result.error, QStringLiteral("The current source layer has no faces."));

        outcome = compareSampledSurfaces(
            triangle, {}, SurfaceComparisonMetric::NormalAgreement, options);
        QVERIFY(!outcome.result.ok);
        QCOMPARE(outcome.result.error, QStringLiteral("The reference layer has no faces."));

        SurfaceMeshSnapshot zeroArea;
        zeroArea.vertices = {
            SurfacePoint3D{0.0, 0.0, 0.0},
            SurfacePoint3D{1.0, 0.0, 0.0},
            SurfacePoint3D{2.0, 0.0, 0.0},
        };
        zeroArea.faces.push_back({0, 1, 2});
        outcome = compareSampledSurfaces(
            zeroArea, triangle, SurfaceComparisonMetric::NormalAgreement, options);
        QVERIFY(outcome.result.ok);
        QCOMPARE(outcome.comparison.sampleCount, 0);
        QCOMPARE(outcome.comparison.coloredFaceCount, 1);
        QCOMPARE(outcome.comparison.globalScore, 0.0);
        QCOMPARE(outcome.comparison.faceScores, QVector<double>({0.0}));

        outcome = compareSampledSurfaces(
            zeroArea, triangle, SurfaceComparisonMetric::PrecisionAtThreshold, options);
        QVERIFY(outcome.result.ok);
        QCOMPARE(outcome.comparison.sampleCount, 0);
        QCOMPARE(outcome.comparison.coloredFaceCount, 1);
        QCOMPARE(outcome.comparison.globalScore, 0.0);
        QCOMPARE(outcome.comparison.faceScores, QVector<double>({0.0}));

        outcome = compareSampledSurfaces(
            triangle, zeroArea, SurfaceComparisonMetric::NormalAgreement, options);
        QVERIFY(!outcome.result.ok);
        QCOMPARE(outcome.result.error,
                 QStringLiteral("The reference layer has no positive-area triangles."));

        SurfaceMeshSnapshot invalidIndex = triangle;
        invalidIndex.faces[0][2] = 3;
        outcome = compareSampledSurfaces(
            invalidIndex, triangle, SurfaceComparisonMetric::NormalAgreement, options);
        QVERIFY(!outcome.result.ok);
        QCOMPARE(
            outcome.result.error,
            QStringLiteral(
                "Surface mesh snapshot contains an out-of-range face vertex index."));

        SurfaceMeshSnapshot nonFinite = triangle;
        nonFinite.vertices[0][0] = std::numeric_limits<double>::quiet_NaN();
        outcome = compareSampledSurfaces(
            nonFinite, triangle, SurfaceComparisonMetric::NormalAgreement, options);
        QVERIFY(!outcome.result.ok);
        QCOMPARE(
            outcome.result.error,
            QStringLiteral("Surface mesh snapshot vertices must be finite."));
    }

    void rejectsFiniteCoordinatesOutsideWorkingScalarRange()
    {
        if (!std::is_same<Scalarm, float>::value)
            QSKIP("Requires a single-scalar build with a wider public snapshot type.");

        SurfaceMeshSnapshot outOfRange = triangleAtZ(0.0);
        outOfRange.vertices[0][0] = std::numeric_limits<double>::max();

        const SurfaceComparisonOutcome outcome = compareSampledSurfaces(
            outOfRange,
            triangleAtZ(0.0),
            SurfaceComparisonMetric::NormalAgreement,
            quickOptions());

        QVERIFY(!outcome.result.ok);
        QCOMPARE(
            outcome.result.error,
            QStringLiteral(
                "Surface mesh snapshot vertices exceed the supported scalar range."));
    }

    void cancellationDuringSourceSamplingReturnsEmptyFailure()
    {
        SurfaceComparisonOptions options = quickOptions();
        options.sampleCount = 9000;
        const SurfaceComparisonOutcome outcome = compareSampledSurfaces(
            triangleAtZ(0.0f),
            triangleAtZ(0.0f),
            SurfaceComparisonMetric::NormalAgreement,
            options,
            [](int percent, const QString& message) {
                return !(percent == 27 && message == QStringLiteral("Sampling source surface..."));
            });

        verifyCancelled(outcome);
    }

    void cancellationDuringReferenceSamplingReturnsEmptyFailure()
    {
        SurfaceComparisonOptions options = quickOptions();
        options.sampleCount = 9000;
        const SurfaceComparisonOutcome outcome = compareSampledSurfaces(
            triangleAtZ(0.0f),
            triangleAtZ(0.0f),
            SurfaceComparisonMetric::NormalAgreement,
            options,
            [](int percent, const QString& message) {
                return !(percent == 57
                         && message == QStringLiteral("Sampling reference surface..."));
            });

        verifyCancelled(outcome);
    }

    void cancellationDuringScoringReturnsEmptyFailure()
    {
        SurfaceComparisonOptions options = quickOptions();
        options.sampleCount = 9000;
        const SurfaceComparisonOutcome outcome = compareSampledSurfaces(
            triangleAtZ(0.0f),
            triangleAtZ(0.0f),
            SurfaceComparisonMetric::NormalAgreement,
            options,
            [](int percent, const QString& message) {
                return !(percent == 83 && message.contains(QStringLiteral("normal agreement")));
            });

        verifyCancelled(outcome);
    }

    void cancellationDuringPerFaceCompletionReturnsEmptyFailure()
    {
        SurfaceMeshSnapshot source = triangleAtZ(0.0f);
        source.faces.fill(std::array<int, 3>{0, 1, 2}, 9000);
        SurfaceComparisonOptions options = quickOptions();
        options.sampleCount = 1;

        const SurfaceComparisonOutcome outcome = compareSampledSurfaces(
            source,
            triangleAtZ(0.0f),
            SurfaceComparisonMetric::NormalAgreement,
            options,
            [](int percent, const QString& message) {
                return !(percent == 94 &&
                         message == QStringLiteral(
                             "Completing source face analysis colors..."));
            });

        verifyCancelled(outcome);
    }

    void cancellationIsPolledBetweenScoringQueries()
    {
        SurfaceComparisonOptions options = quickOptions();
        options.sampleCount = 16;
        bool scoringStarted = false;
        bool scoringFinished = false;
        int scoringCancellationChecks = 0;

        const SurfaceComparisonOutcome outcome = compareSampledSurfaces(
            triangleAtZ(0.0f),
            triangleAtZ(1.0f),
            SurfaceComparisonMetric::PrecisionAtThreshold,
            options,
            [&scoringStarted, &scoringFinished](int percent, const QString& message) {
                if (percent == 65 &&
                    message.contains(QStringLiteral(
                        "source-to-reference precision"))) {
                    scoringStarted = true;
                }
                if (percent == 85 &&
                    message.contains(QStringLiteral(
                        "source-to-reference precision"))) {
                    scoringFinished = true;
                }
                return true;
            },
            [&scoringStarted, &scoringFinished, &scoringCancellationChecks] {
                if (!scoringStarted || scoringFinished)
                    return false;
                ++scoringCancellationChecks;
                return false;
            });

        QVERIFY2(outcome.result.ok, qPrintable(outcome.result.error));
        QCOMPARE(outcome.comparison.sampleCount, options.sampleCount);
        QVERIFY2(
            scoringCancellationChecks >= options.sampleCount,
            qPrintable(QStringLiteral(
                "Expected at least one cancellation poll per scoring query; got %1 "
                "polls for %2 samples.")
                           .arg(scoringCancellationChecks)
                           .arg(options.sampleCount)));
    }

    void successfulProgressMatchesLegacyAnalysisSequence()
    {
        SurfaceComparisonOptions options = quickOptions();
        options.sampleCount = 1;
        QVector<QPair<int, QString>> progress;

        const SurfaceComparisonOutcome outcome = compareSampledSurfaces(
            triangleAtZ(0.0f),
            triangleAtZ(0.0f),
            SurfaceComparisonMetric::NormalAgreement,
            options,
            [&progress](int percent, const QString& message) {
                progress.push_back(qMakePair(percent, message));
                return true;
            });

        QVERIFY(outcome.result.ok);
        const QVector<QPair<int, QString>> expected = {
            {0, QStringLiteral("Sampling source surface...")},
            {30, QStringLiteral("Sampling source surface...")},
            {60, QStringLiteral("Sampling reference surface...")},
            {60, QStringLiteral("Building reference sample KD-tree...")},
            {65, QStringLiteral("Computing source-to-reference normal agreement...")},
            {85, QStringLiteral("Computing source-to-reference normal agreement...")},
            {95, QStringLiteral("Completing source face analysis colors...")},
        };
        QCOMPARE(progress, expected);
    }

    void successfulPrecisionProgressMatchesLegacyAnalysisSequence()
    {
        SurfaceComparisonOptions options = quickOptions();
        options.sampleCount = 1;
        QVector<QPair<int, QString>> progress;

        const SurfaceComparisonOutcome outcome = compareSampledSurfaces(
            triangleAtZ(0.0),
            triangleAtZ(0.0),
            SurfaceComparisonMetric::PrecisionAtThreshold,
            options,
            [&progress](int percent, const QString& message) {
                progress.push_back(qMakePair(percent, message));
                return true;
            });

        QVERIFY(outcome.result.ok);
        const QVector<QPair<int, QString>> expected = {
            {0, QStringLiteral("Sampling source surface...")},
            {30, QStringLiteral("Sampling source surface...")},
            {60, QStringLiteral("Sampling reference surface...")},
            {60, QStringLiteral("Building reference sample KD-tree...")},
            {65, QStringLiteral("Computing source-to-reference precision...")},
            {85, QStringLiteral("Computing source-to-reference precision...")},
            {95, QStringLiteral("Completing source face analysis colors...")},
        };
        QCOMPARE(progress, expected);
    }

    void surfaceComparerDelegatesToProductionFunction()
    {
        SurfaceComparer comparer;
        const SurfaceComparisonOutcome outcome = comparer.compare(
            triangleAtZ(0.0f),
            triangleAtZ(0.0f),
            SurfaceComparisonMetric::NormalAgreement,
            quickOptions());

        QVERIFY(outcome.result.ok);
        QCOMPARE(outcome.comparison.globalScore, 1.0);
    }

};

QTEST_APPLESS_MAIN(SurfaceComparisonTest)
#include "surface_comparison_test.moc"
