#pragma once

#include <QColor>
#include <QString>
#include <QVector>

using MeshId = quint64;
using MeshResourceId = quint64;

enum class WorkspacePhase { Empty, Loading, Ready, Analyzing, FatalError };
enum class ColorMode { Default, UniformColor, PrecisionResult, NormalAgreementResult };
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

struct ColorPresentation {
    ColorMode mode = ColorMode::Default;
    QColor uniformColor;
    QVector<QColor> faceColors;
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
};

struct SceneDescriptor {
    quint64 generation = 0;
    QVector<SceneMesh> meshes;
    MeshId referenceId = 0;
    SceneLayoutMode layoutMode = SceneLayoutMode::ComparisonGrid;
};
