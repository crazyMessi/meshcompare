#pragma once

#include <array>

#include <QImage>

#include "meshcompare_types.h"

using MeshPoint3D = std::array<double, 3>;

struct MeshTexCoord {
    double u = 0.0;
    double v = 0.0;
    int textureIndex = -1;
};

class IMeshGeometryView {
public:
    virtual ~IMeshGeometryView() = default;
    virtual int vertexCount() const = 0;
    virtual MeshPoint3D vertexPosition(int index) const = 0;
    virtual MeshPoint3D vertexNormal(int index) const = 0;
    virtual bool hasVertexColors() const { return false; }
    virtual QColor vertexColor(int index) const
    {
        Q_UNUSED(index);
        return {};
    }
    virtual int faceCount() const = 0;
    virtual std::array<int, 3> faceVertexIndices(int index) const = 0;
    virtual bool hasFaceColors() const { return false; }
    virtual QColor faceColor(int index) const
    {
        Q_UNUSED(index);
        return {};
    }
    virtual int textureCount() const { return 0; }
    virtual QImage textureImage(int index) const
    {
        Q_UNUSED(index);
        return {};
    }
    virtual bool hasFaceTexCoords() const { return false; }
    virtual std::array<MeshTexCoord, 3> faceTexCoords(int index) const
    {
        Q_UNUSED(index);
        return {};
    }
    virtual bool isUnlit() const { return false; }
    virtual int textureWrapS(int index) const
    {
        Q_UNUSED(index);
        return 10497; // GL_REPEAT
    }
    virtual int textureWrapT(int index) const
    {
        Q_UNUSED(index);
        return 10497; // GL_REPEAT
    }
};

class IMeshResourceProvider {
public:
    virtual ~IMeshResourceProvider() = default;
    virtual const IMeshGeometryView* geometry(MeshResourceId id) const = 0;
};
