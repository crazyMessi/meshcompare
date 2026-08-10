#pragma once

#include <array>

#include <QString>
#include <QVector>

#include "core/mesh_resource_provider.h"

namespace python_hole_filling
{

enum class PatchMethod {
    Planar,
    Hybrid,
};

enum class HoleScope {
    Largest,
    UpToLimit,
    All,
};

enum class OutputMode {
    CurrentMesh,
    NewLayer,
};

struct FillConfig
{
    PatchMethod method = PatchMethod::Hybrid;
    HoleScope scope = HoleScope::UpToLimit;
    int targetVertices = 512;
    int maxHoles = 8;
    int minLoopVertices = 6;
    double minHolePerimeter = 0.0;
    double normalWeight = 40.0;
    double anchorWeight = 0.002;
    double maxTangentRms = 0.15;
};

struct FillRequest
{
    MeshId meshId = 0;
    FillConfig config;
    OutputMode outputMode = OutputMode::NewLayer;
};

struct TriangleMesh
{
    QVector<MeshPoint3D> vertices;
    QVector<std::array<int, 3>> faces;
};

struct HoleResult
{
    int holeIndex = 0;
    bool patched = false;
    int boundaryVertexCount = 0;
    int patchVertexCount = 0;
    int patchFaceCount = 0;
    double perimeter = 0.0;
    double tangentRmsBefore = 0.0;
    double tangentRmsAfter = 0.0;
    QString reason;
};

struct FillResult
{
    OperationResult result;
    TriangleMesh patch;
    TriangleMesh merged;
    QVector<HoleResult> holes;
    int boundaryEdgeCount = 0;
    int nonManifoldEdgeCount = 0;
    int closedBoundaryLoopCount = 0;
    int eligibleBoundaryLoopCount = 0;
    int patchedHoleCount = 0;
};

struct FillSummary
{
    int patchedHoleCount = 0;
    int patchVertexCount = 0;
    int patchFaceCount = 0;
    QString outputLayerName;
};

class Engine
{
public:
    FillResult fill(
        const IMeshGeometryView& geometry,
        const FillConfig& config) const;
};

class ICommands
{
public:
    virtual ~ICommands() = default;
    virtual OperationResult fillPythonHoles(
        const FillRequest& request,
        FillSummary* summary = nullptr) = 0;
};

} // namespace python_hole_filling
