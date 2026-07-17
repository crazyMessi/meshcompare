#pragma once

#include <array>

#include "meshcompare_types.h"

using MeshPoint3D = std::array<double, 3>;

class IMeshGeometryView {
public:
    virtual ~IMeshGeometryView() = default;
    virtual int vertexCount() const = 0;
    virtual MeshPoint3D vertexPosition(int index) const = 0;
    virtual MeshPoint3D vertexNormal(int index) const = 0;
    virtual int faceCount() const = 0;
    virtual std::array<int, 3> faceVertexIndices(int index) const = 0;
};

class IMeshResourceProvider {
public:
    virtual ~IMeshResourceProvider() = default;
    virtual const IMeshGeometryView* geometry(MeshResourceId id) const = 0;
};
