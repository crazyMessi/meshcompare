#pragma once

#include <QColor>
#include <QString>
#include <QVector>

using MeshId = quint64;
using MeshResourceId = quint64;

enum class WorkspacePhase { Empty, Loading, Ready, Analyzing, FatalError };
enum class ColorMode {
    Default,
    UniformColor,
    PrecisionResult,
    NormalAgreementResult,
    VertexColor,
};
enum class AnalysisKind { None, DistanceToReference, DoubleLayer };
enum class DistanceColorMapping { Linear, SquareRoot };
enum class ColorLegendKind { None, Distance };
enum class DiagnosticFlag { Orthographic, Wireframe, Normals };
enum class SceneLayoutMode { ComparisonGrid, Overlay };

struct OperationResult {
    bool ok = true;
    QString error;
    static OperationResult success() { return {}; }
    static OperationResult failure(const QString& value) {
        OperationResult result;
        result.ok = false;
        result.error = value;
        return result;
    }
};

struct CameraPose { QString viewStateXml; };

struct ColorLegendSpec {
    ColorLegendKind kind = ColorLegendKind::None;
    DistanceColorMapping distanceMapping =
        DistanceColorMapping::SquareRoot;
    double minimum = 0.0;
    double maximum = 0.0;
};

inline bool operator==(
    const ColorLegendSpec& left,
    const ColorLegendSpec& right)
{
    return left.kind == right.kind &&
           left.distanceMapping == right.distanceMapping &&
           left.minimum == right.minimum &&
           left.maximum == right.maximum;
}

inline bool operator!=(
    const ColorLegendSpec& left,
    const ColorLegendSpec& right)
{
    return !(left == right);
}

struct ColorPresentation {
    ColorMode mode = ColorMode::Default;
    QColor uniformColor;
    QVector<QColor> faceColors;
    QVector<QColor> vertexColors;
    bool referenceDependent = false;
    ColorLegendSpec colorLegend;
};

inline bool isReferenceDependent(
    const ColorPresentation& presentation)
{
    return presentation.referenceDependent ||
           presentation.mode == ColorMode::PrecisionResult ||
           presentation.mode == ColorMode::NormalAgreementResult;
}

struct DistanceAnalysisSummary {
    int vertexCount = 0;
    int finiteVertexCount = 0;
    double meanDistance = 0.0;
    double percentile99Distance = 0.0;
    double maxDistance = 0.0;
    double aboveThresholdVertexFraction = 0.0;
};

struct DoubleLayerAnalysisSummary {
    int sampleCount = 0;
    double meanSampleScore = 0.0;
    double affectedSampleFraction = 0.0;
    double affectedFaceFraction = 0.0;
    double affectedVertexFraction = 0.0;
};

struct AnalysisSummary {
    AnalysisKind kind = AnalysisKind::None;
    DistanceAnalysisSummary distance;
    DoubleLayerAnalysisSummary doubleLayer;
};

struct MeshColorPresentationUpdate {
    MeshId meshId = 0;
    ColorPresentation presentation;
};

struct MeshAnalysisOverlayUpdate {
    MeshId meshId = 0;
    QString label;
};

struct RendererDiagnostics {
    QString backendName;
    QString openGlVendor;
    QString openGlRenderer;
    QString openGlVersion;
};

struct SceneMesh {
    MeshId id = 0;
    MeshResourceId resourceId = 0;
    QString label;
    bool isReference = false;
    ColorPresentation presentation;
    QString analysisLabel;
    bool visible = true;
};

struct SceneDescriptor {
    quint64 generation = 0;
    QVector<SceneMesh> meshes;
    MeshId referenceId = 0;
    SceneLayoutMode layoutMode = SceneLayoutMode::ComparisonGrid;
    CameraPose initialCamera;
    bool normalizeGridMeshes = false;
};
