#pragma once

#include "../core/meshcompare_types.h"
#include "../services/surface_comparison.h"

class IColoringCommands
{
public:
    virtual ~IColoringCommands() = default;

    virtual OperationResult selectMesh(MeshId meshId) = 0;
    virtual OperationResult setReference(MeshId meshId) = 0;
    virtual OperationResult setUniformColor(
        MeshId meshId,
        const QColor& color) = 0;
    virtual OperationResult startAnalysis(
        SurfaceComparisonMetric metric,
        const SurfaceComparisonOptions& options) = 0;
    virtual void cancelAnalysis() = 0;
    virtual OperationResult clearColoring() = 0;
};
