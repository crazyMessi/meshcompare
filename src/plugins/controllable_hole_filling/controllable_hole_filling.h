#pragma once

#include <array>

#include <QString>
#include <QVector>

#include "core/mesh_resource_provider.h"

namespace controllable_hole_filling
{

enum class OutputMode {
    CurrentMesh,
    NewLayer,
};

struct FillConfig
{
    int resolution = 512;
    int cclIterations = 3;
    double epsFactor = 2.0;
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

struct FillResult
{
    OperationResult result;
    TriangleMesh mesh;
    int candidateCellCount = 0;
    int activeCellCount = 0;
    qint64 elapsedMilliseconds = 0;
};

struct FillSummary
{
    int vertexCount = 0;
    int faceCount = 0;
    int activeCellCount = 0;
    qint64 elapsedMilliseconds = 0;
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
    virtual OperationResult fillHoles(
        const FillRequest& request,
        FillSummary* summary = nullptr) = 0;
};

} // namespace controllable_hole_filling
