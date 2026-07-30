#include "glb_mesh_loader.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <exception>
#include <limits>
#include <utility>

#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QtEndian>

#include <common/ml_document/mesh_model.h>
#include <vcg/complex/allocate.h>

#include "meshlab_mesh_repository.h"

namespace
{
constexpr quint32 GlbMagic = 0x46546c67;
constexpr quint32 JsonChunkType = 0x4e4f534a;
constexpr quint32 BinaryChunkType = 0x004e4942;

struct Matrix
{
    std::array<double, 16> values{{
        1, 0, 0, 0,
        0, 1, 0, 0,
        0, 0, 1, 0,
        0, 0, 0, 1}};
};

struct AccessorView
{
    const char* data = nullptr;
    qsizetype offset = 0;
    qsizetype stride = 0;
    int componentType = 0;
    int componentSize = 0;
    int componentCount = 0;
    int count = 0;
    bool normalized = false;
};

struct ParsedLayer
{
    QString name;
    QVector<CMeshO::CoordType> positions;
    QVector<vcg::Color4b> colors;
    QVector<std::array<int, 3>> faces;
    bool hasColors = false;
};

quint32 readUInt32(const QByteArray& bytes, qsizetype offset)
{
    quint32 value = 0;
    std::memcpy(&value, bytes.constData() + offset, sizeof(value));
    return qFromLittleEndian(value);
}

OperationResult integerField(
    const QJsonObject& object,
    const QString& key,
    int* value,
    bool required = true,
    int defaultValue = 0)
{
    if (!object.contains(key)) {
        if (!required) {
            *value = defaultValue;
            return OperationResult::success();
        }
        return OperationResult::failure(
            QStringLiteral("GLB field \"%1\" is missing.").arg(key));
    }
    const QJsonValue jsonValue = object.value(key);
    const double number = jsonValue.toDouble(
        std::numeric_limits<double>::quiet_NaN());
    if (!std::isfinite(number) || std::floor(number) != number ||
        number < std::numeric_limits<int>::min() ||
        number > std::numeric_limits<int>::max()) {
        return OperationResult::failure(
            QStringLiteral("GLB field \"%1\" must be an integer.").arg(key));
    }
    *value = static_cast<int>(number);
    return OperationResult::success();
}

OperationResult arrayIndex(
    const QJsonValue& value,
    const QString& description,
    int* destination)
{
    const double number = value.toDouble(
        std::numeric_limits<double>::quiet_NaN());
    if (!std::isfinite(number) || std::floor(number) != number) {
        return OperationResult::failure(
            QStringLiteral("GLB %1 index must be an integer.").arg(description));
    }
    if (number < 0 || number > std::numeric_limits<int>::max()) {
        return OperationResult::failure(
            QStringLiteral("GLB %1 index is out of range.").arg(description));
    }
    *destination = int(number);
    return OperationResult::success();
}

int componentCountForType(const QString& type)
{
    if (type == QStringLiteral("SCALAR"))
        return 1;
    if (type == QStringLiteral("VEC2"))
        return 2;
    if (type == QStringLiteral("VEC3"))
        return 3;
    if (type == QStringLiteral("VEC4"))
        return 4;
    return 0;
}

int byteSizeForComponent(int componentType)
{
    switch (componentType) {
    case 5120:
    case 5121:
        return 1;
    case 5122:
    case 5123:
        return 2;
    case 5125:
    case 5126:
        return 4;
    default:
        return 0;
    }
}

OperationResult accessorView(
    const QJsonObject& root,
    const QByteArray& binary,
    int accessorIndex,
    AccessorView* destination)
{
    const QJsonArray accessors = root.value(QStringLiteral("accessors")).toArray();
    if (accessorIndex < 0 || accessorIndex >= accessors.size()) {
        return OperationResult::failure(
            QStringLiteral("GLB references invalid accessor %1.").arg(accessorIndex));
    }
    const QJsonObject accessor = accessors.at(accessorIndex).toObject();
    if (accessor.contains(QStringLiteral("sparse"))) {
        return OperationResult::failure(
            QStringLiteral("Sparse GLB accessors are not supported."));
    }

    int bufferViewIndex = 0;
    int accessorOffset = 0;
    OperationResult result = integerField(
        accessor, QStringLiteral("bufferView"), &bufferViewIndex);
    if (!result.ok)
        return result;
    result = integerField(
        accessor,
        QStringLiteral("byteOffset"),
        &accessorOffset,
        false,
        0);
    if (!result.ok || accessorOffset < 0)
        return result.ok
                   ? OperationResult::failure(
                         QStringLiteral("GLB accessor byteOffset must be non-negative."))
                   : result;

    int componentType = 0;
    int count = 0;
    result = integerField(
        accessor, QStringLiteral("componentType"), &componentType);
    if (!result.ok)
        return result;
    result = integerField(accessor, QStringLiteral("count"), &count);
    if (!result.ok || count < 0)
        return result.ok
                   ? OperationResult::failure(
                         QStringLiteral("GLB accessor count must be non-negative."))
                   : result;

    const int componentSize = byteSizeForComponent(componentType);
    const int componentCount = componentCountForType(
        accessor.value(QStringLiteral("type")).toString());
    if (componentSize == 0 || componentCount == 0) {
        return OperationResult::failure(
            QStringLiteral("GLB accessor has an unsupported component type or shape."));
    }

    const QJsonArray bufferViews =
        root.value(QStringLiteral("bufferViews")).toArray();
    if (bufferViewIndex < 0 || bufferViewIndex >= bufferViews.size()) {
        return OperationResult::failure(
            QStringLiteral("GLB references invalid bufferView %1.")
                .arg(bufferViewIndex));
    }
    const QJsonObject bufferView = bufferViews.at(bufferViewIndex).toObject();
    int bufferIndex = 0;
    int viewOffset = 0;
    int viewLength = 0;
    int byteStride = componentSize * componentCount;
    result = integerField(bufferView, QStringLiteral("buffer"), &bufferIndex);
    if (!result.ok)
        return result;
    result = integerField(
        bufferView, QStringLiteral("byteOffset"), &viewOffset, false, 0);
    if (!result.ok)
        return result;
    result = integerField(
        bufferView, QStringLiteral("byteLength"), &viewLength);
    if (!result.ok)
        return result;
    result = integerField(
        bufferView,
        QStringLiteral("byteStride"),
        &byteStride,
        false,
        byteStride);
    if (!result.ok)
        return result;
    if (bufferIndex != 0) {
        return OperationResult::failure(
            QStringLiteral("GLB geometry must use the embedded binary buffer."));
    }
    if (viewOffset < 0 || viewLength < 0 || byteStride <= 0 ||
        byteStride < componentSize * componentCount) {
        return OperationResult::failure(
            QStringLiteral("GLB bufferView has invalid byte bounds or stride."));
    }

    const qint64 offset = qint64(viewOffset) + accessorOffset;
    const qint64 itemSize = qint64(componentSize) * componentCount;
    const qint64 lastByte = count == 0
                                ? offset
                                : offset + qint64(count - 1) * byteStride + itemSize;
    const qint64 viewEnd = qint64(viewOffset) + viewLength;
    if (offset < viewOffset || lastByte > viewEnd ||
        viewEnd > binary.size()) {
        return OperationResult::failure(
            QStringLiteral("GLB accessor exceeds its embedded buffer bounds."));
    }

    destination->data = binary.constData();
    destination->offset = static_cast<qsizetype>(offset);
    destination->stride = byteStride;
    destination->componentType = componentType;
    destination->componentSize = componentSize;
    destination->componentCount = componentCount;
    destination->count = count;
    destination->normalized =
        accessor.value(QStringLiteral("normalized")).toBool(false);
    return OperationResult::success();
}

template<typename T>
T readScalar(const char* data)
{
    T value{};
    std::memcpy(&value, data, sizeof(value));
    return qFromLittleEndian(value);
}

double componentValue(const AccessorView& view, int item, int component)
{
    const char* data = view.data + view.offset +
                       item * view.stride +
                       component * view.componentSize;
    switch (view.componentType) {
    case 5120: {
        const qint8 value = *reinterpret_cast<const qint8*>(data);
        return view.normalized ? std::max(-1.0, double(value) / 127.0)
                               : double(value);
    }
    case 5121: {
        const quint8 value = *reinterpret_cast<const quint8*>(data);
        return view.normalized ? double(value) / 255.0 : double(value);
    }
    case 5122: {
        const qint16 value = readScalar<qint16>(data);
        return view.normalized ? std::max(-1.0, double(value) / 32767.0)
                               : double(value);
    }
    case 5123: {
        const quint16 value = readScalar<quint16>(data);
        return view.normalized ? double(value) / 65535.0 : double(value);
    }
    case 5125: {
        const quint32 value = readScalar<quint32>(data);
        return view.normalized
                   ? double(value) / double(std::numeric_limits<quint32>::max())
                   : double(value);
    }
    case 5126: {
        quint32 bits = readScalar<quint32>(data);
        float value = 0;
        std::memcpy(&value, &bits, sizeof(value));
        return value;
    }
    }
    return 0;
}

OperationResult indexValue(
    const AccessorView& view,
    int item,
    quint32* destination)
{
    if (view.componentCount != 1 ||
        (view.componentType != 5121 &&
         view.componentType != 5123 &&
         view.componentType != 5125)) {
        return OperationResult::failure(
            QStringLiteral("GLB indices must use an unsigned scalar accessor."));
    }
    const double value = componentValue(view, item, 0);
    if (!std::isfinite(value) || value < 0 ||
        value > std::numeric_limits<quint32>::max()) {
        return OperationResult::failure(
            QStringLiteral("GLB contains an invalid mesh index."));
    }
    *destination = static_cast<quint32>(value);
    return OperationResult::success();
}

Matrix multiply(const Matrix& first, const Matrix& second)
{
    Matrix result;
    for (int column = 0; column < 4; ++column) {
        for (int row = 0; row < 4; ++row) {
            double value = 0;
            for (int inner = 0; inner < 4; ++inner) {
                value += first.values[inner * 4 + row] *
                         second.values[column * 4 + inner];
            }
            result.values[column * 4 + row] = value;
        }
    }
    return result;
}

OperationResult finiteArray(
    const QJsonValue& value,
    int size,
    QVector<double>* destination)
{
    const QJsonArray array = value.toArray();
    if (array.size() != size)
        return OperationResult::failure(
            QStringLiteral("GLB transform array has the wrong length."));
    destination->clear();
    destination->reserve(size);
    for (const QJsonValue& item : array) {
        const double number = item.toDouble(
            std::numeric_limits<double>::quiet_NaN());
        if (!std::isfinite(number))
            return OperationResult::failure(
                QStringLiteral("GLB transform contains a non-finite number."));
        destination->append(number);
    }
    return OperationResult::success();
}

OperationResult nodeTransform(const QJsonObject& node, Matrix* destination)
{
    if (node.contains(QStringLiteral("matrix"))) {
        QVector<double> values;
        const OperationResult result = finiteArray(
            node.value(QStringLiteral("matrix")), 16, &values);
        if (!result.ok)
            return result;
        for (int index = 0; index < 16; ++index)
            destination->values[index] = values.at(index);
        return OperationResult::success();
    }

    QVector<double> translation{0, 0, 0};
    QVector<double> rotation{0, 0, 0, 1};
    QVector<double> scale{1, 1, 1};
    OperationResult result = OperationResult::success();
    if (node.contains(QStringLiteral("translation")))
        result = finiteArray(node.value(QStringLiteral("translation")), 3, &translation);
    if (result.ok && node.contains(QStringLiteral("rotation")))
        result = finiteArray(node.value(QStringLiteral("rotation")), 4, &rotation);
    if (result.ok && node.contains(QStringLiteral("scale")))
        result = finiteArray(node.value(QStringLiteral("scale")), 3, &scale);
    if (!result.ok)
        return result;

    const double x = rotation[0];
    const double y = rotation[1];
    const double z = rotation[2];
    const double w = rotation[3];
    const double length = std::sqrt(x * x + y * y + z * z + w * w);
    if (!std::isfinite(length) || length <= 0)
        return OperationResult::failure(
            QStringLiteral("GLB node has an invalid rotation quaternion."));
    const double qx = x / length;
    const double qy = y / length;
    const double qz = z / length;
    const double qw = w / length;

    Matrix matrix;
    matrix.values = {{
        (1 - 2 * (qy * qy + qz * qz)) * scale[0],
        (2 * (qx * qy + qz * qw)) * scale[0],
        (2 * (qx * qz - qy * qw)) * scale[0],
        0,
        (2 * (qx * qy - qz * qw)) * scale[1],
        (1 - 2 * (qx * qx + qz * qz)) * scale[1],
        (2 * (qy * qz + qx * qw)) * scale[1],
        0,
        (2 * (qx * qz + qy * qw)) * scale[2],
        (2 * (qy * qz - qx * qw)) * scale[2],
        (1 - 2 * (qx * qx + qy * qy)) * scale[2],
        0,
        translation[0],
        translation[1],
        translation[2],
        1}};
    *destination = matrix;
    return OperationResult::success();
}

CMeshO::CoordType transformedPoint(
    const Matrix& matrix,
    double x,
    double y,
    double z)
{
    return CMeshO::CoordType(
        Scalarm(matrix.values[0] * x + matrix.values[4] * y +
                matrix.values[8] * z + matrix.values[12]),
        Scalarm(matrix.values[1] * x + matrix.values[5] * y +
                matrix.values[9] * z + matrix.values[13]),
        Scalarm(matrix.values[2] * x + matrix.values[6] * y +
                matrix.values[10] * z + matrix.values[14]));
}

bool reversesOrientation(const Matrix& matrix)
{
    const double determinant =
        matrix.values[0] *
            (matrix.values[5] * matrix.values[10] -
             matrix.values[9] * matrix.values[6]) -
        matrix.values[4] *
            (matrix.values[1] * matrix.values[10] -
             matrix.values[9] * matrix.values[2]) +
        matrix.values[8] *
            (matrix.values[1] * matrix.values[6] -
             matrix.values[5] * matrix.values[2]);
    return std::isfinite(determinant) && determinant < 0;
}

OperationResult appendPrimitive(
    const QJsonObject& root,
    const QByteArray& binary,
    const QJsonObject& primitive,
    const Matrix& transform,
    ParsedLayer* layer)
{
    int mode = 4;
    OperationResult result = integerField(
        primitive, QStringLiteral("mode"), &mode, false, 4);
    if (!result.ok)
        return result;
    if (mode != 4 && mode != 5 && mode != 6) {
        return OperationResult::failure(
            QStringLiteral("GLB primitive mode %1 is not a triangle mesh.")
                .arg(mode));
    }
    if (primitive.value(QStringLiteral("extensions"))
            .toObject()
            .contains(QStringLiteral("KHR_draco_mesh_compression"))) {
        return OperationResult::failure(
            QStringLiteral("Draco-compressed GLB geometry is not supported."));
    }

    const QJsonObject attributes =
        primitive.value(QStringLiteral("attributes")).toObject();
    int positionAccessor = 0;
    result = integerField(
        attributes, QStringLiteral("POSITION"), &positionAccessor);
    if (!result.ok)
        return OperationResult::failure(
            QStringLiteral("GLB triangle primitive has no POSITION attribute."));
    AccessorView positions;
    result = accessorView(root, binary, positionAccessor, &positions);
    if (!result.ok)
        return result;
    if (positions.componentCount != 3) {
        return OperationResult::failure(
            QStringLiteral("GLB POSITION accessor must be VEC3."));
    }
    const bool usesMeshQuantization =
        root.value(QStringLiteral("extensionsUsed"))
            .toArray()
            .contains(QStringLiteral("KHR_mesh_quantization"));
    const bool floatPositions = positions.componentType == 5126;
    const bool quantizedPositions =
        usesMeshQuantization &&
        (positions.componentType == 5120 ||
         positions.componentType == 5121 ||
         positions.componentType == 5122 ||
         positions.componentType == 5123);
    if (!floatPositions && !quantizedPositions) {
        return OperationResult::failure(
            QStringLiteral(
                "GLB POSITION must use float components, or integer "
                "components with KHR_mesh_quantization."));
    }

    AccessorView colors;
    const bool primitiveHasColors =
        attributes.contains(QStringLiteral("COLOR_0"));
    if (primitiveHasColors) {
        int colorAccessor = 0;
        result = integerField(
            attributes, QStringLiteral("COLOR_0"), &colorAccessor);
        if (!result.ok)
            return result;
        result = accessorView(root, binary, colorAccessor, &colors);
        if (!result.ok)
            return result;
        if ((colors.componentCount != 3 && colors.componentCount != 4) ||
            colors.count != positions.count) {
            return OperationResult::failure(
                QStringLiteral("GLB COLOR_0 must match POSITION as VEC3 or VEC4."));
        }
        const bool floatColors = colors.componentType == 5126;
        const bool normalizedIntegerColors =
            colors.normalized &&
            (colors.componentType == 5121 ||
             colors.componentType == 5123);
        if (!floatColors && !normalizedIntegerColors) {
            return OperationResult::failure(
                QStringLiteral(
                    "GLB COLOR_0 must use floats or normalized unsigned byte/short "
                    "components."));
        }
    }

    const int vertexBase = layer->positions.size();
    layer->positions.reserve(vertexBase + positions.count);
    layer->colors.reserve(vertexBase + positions.count);
    for (int vertex = 0; vertex < positions.count; ++vertex) {
        const double x = componentValue(positions, vertex, 0);
        const double y = componentValue(positions, vertex, 1);
        const double z = componentValue(positions, vertex, 2);
        if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) {
            return OperationResult::failure(
                QStringLiteral("GLB contains a non-finite vertex position."));
        }
        layer->positions.append(transformedPoint(transform, x, y, z));
        if (primitiveHasColors) {
            const auto channel = [&colors, vertex](int index, double fallback) {
                const double value = index < colors.componentCount
                                         ? componentValue(colors, vertex, index)
                                         : fallback;
                return quint8(std::round(
                    std::max(0.0, std::min(1.0, value)) * 255.0));
            };
            layer->colors.append(vcg::Color4b(
                channel(0, 1),
                channel(1, 1),
                channel(2, 1),
                channel(3, 1)));
            layer->hasColors = true;
        }
        else {
            layer->colors.append(vcg::Color4b(255, 255, 255, 255));
        }
    }

    QVector<quint32> indices;
    if (primitive.contains(QStringLiteral("indices"))) {
        int indexAccessor = 0;
        result = integerField(
            primitive, QStringLiteral("indices"), &indexAccessor);
        if (!result.ok)
            return result;
        AccessorView indexView;
        result = accessorView(root, binary, indexAccessor, &indexView);
        if (!result.ok)
            return result;
        indices.reserve(indexView.count);
        for (int index = 0; index < indexView.count; ++index) {
            quint32 value = 0;
            result = indexValue(indexView, index, &value);
            if (!result.ok)
                return result;
            if (value >= quint32(positions.count)) {
                return OperationResult::failure(
                    QStringLiteral("GLB face references a vertex outside POSITION."));
            }
            indices.append(value);
        }
    }
    else {
        indices.reserve(positions.count);
        for (int index = 0; index < positions.count; ++index)
            indices.append(quint32(index));
    }

    const bool reverseWinding = reversesOrientation(transform);
    auto appendFace = [layer, vertexBase, reverseWinding](
                          quint32 first,
                          quint32 second,
                          quint32 third) {
        if (reverseWinding)
            std::swap(second, third);
        layer->faces.append(std::array<int, 3>{{
            vertexBase + int(first),
            vertexBase + int(second),
            vertexBase + int(third)}});
    };
    if (mode == 4) {
        if (indices.size() % 3 != 0)
            return OperationResult::failure(
                QStringLiteral("GLB triangle index count is not divisible by three."));
        for (int index = 0; index < indices.size(); index += 3)
            appendFace(indices[index], indices[index + 1], indices[index + 2]);
    }
    else if (mode == 5) {
        for (int index = 2; index < indices.size(); ++index) {
            if (index % 2 == 0)
                appendFace(indices[index - 2], indices[index - 1], indices[index]);
            else
                appendFace(indices[index - 1], indices[index - 2], indices[index]);
        }
    }
    else {
        for (int index = 2; index < indices.size(); ++index)
            appendFace(indices[0], indices[index - 1], indices[index]);
    }
    return OperationResult::success();
}

OperationResult parseMesh(
    const QJsonObject& root,
    const QByteArray& binary,
    int meshIndex,
    const QString& name,
    const Matrix& transform,
    ParsedLayer* destination)
{
    const QJsonArray meshes = root.value(QStringLiteral("meshes")).toArray();
    if (meshIndex < 0 || meshIndex >= meshes.size()) {
        return OperationResult::failure(
            QStringLiteral("GLB node references invalid mesh %1.").arg(meshIndex));
    }
    const QJsonObject mesh = meshes.at(meshIndex).toObject();
    destination->name = name.isEmpty()
                            ? mesh.value(QStringLiteral("name")).toString()
                            : name;
    const QJsonArray primitives =
        mesh.value(QStringLiteral("primitives")).toArray();
    if (primitives.isEmpty())
        return OperationResult::failure(
            QStringLiteral("GLB mesh contains no primitives."));
    for (const QJsonValue& primitive : primitives) {
        const OperationResult result = appendPrimitive(
            root, binary, primitive.toObject(), transform, destination);
        if (!result.ok)
            return result;
    }
    return OperationResult::success();
}

OperationResult visitNode(
    const QJsonObject& root,
    const QByteArray& binary,
    int nodeIndex,
    const Matrix& parentTransform,
    QVector<bool>* active,
    QVector<ParsedLayer>* layers)
{
    const QJsonArray nodes = root.value(QStringLiteral("nodes")).toArray();
    if (nodeIndex < 0 || nodeIndex >= nodes.size()) {
        return OperationResult::failure(
            QStringLiteral("GLB scene references invalid node %1.").arg(nodeIndex));
    }
    if (active->at(nodeIndex))
        return OperationResult::failure(
            QStringLiteral("GLB node hierarchy contains a cycle."));
    (*active)[nodeIndex] = true;

    const QJsonObject node = nodes.at(nodeIndex).toObject();
    Matrix localTransform;
    OperationResult result = nodeTransform(node, &localTransform);
    if (!result.ok) {
        (*active)[nodeIndex] = false;
        return result;
    }
    const Matrix worldTransform = multiply(parentTransform, localTransform);
    if (node.contains(QStringLiteral("mesh"))) {
        int meshIndex = 0;
        result = integerField(node, QStringLiteral("mesh"), &meshIndex);
        if (result.ok) {
            ParsedLayer layer;
            result = parseMesh(
                root,
                binary,
                meshIndex,
                node.value(QStringLiteral("name")).toString(),
                worldTransform,
                &layer);
            if (result.ok)
                layers->append(std::move(layer));
        }
    }
    if (result.ok) {
        const QJsonArray children = node.value(QStringLiteral("children")).toArray();
        for (const QJsonValue& child : children) {
            int childIndex = 0;
            result = arrayIndex(
                child, QStringLiteral("node child"), &childIndex);
            if (!result.ok)
                break;
            result = visitNode(
                root,
                binary,
                childIndex,
                worldTransform,
                active,
                layers);
            if (!result.ok)
                break;
        }
    }
    (*active)[nodeIndex] = false;
    return result;
}

OperationResult parseLayers(
    const QJsonObject& root,
    const QByteArray& binary,
    QVector<ParsedLayer>* layers)
{
    const QJsonArray meshes = root.value(QStringLiteral("meshes")).toArray();
    if (meshes.isEmpty())
        return OperationResult::failure(
            QStringLiteral("The GLB file contains no meshes."));
    const QJsonArray nodes = root.value(QStringLiteral("nodes")).toArray();
    const QJsonArray scenes = root.value(QStringLiteral("scenes")).toArray();
    if (nodes.isEmpty() || scenes.isEmpty()) {
        Matrix identity;
        for (int meshIndex = 0; meshIndex < meshes.size(); ++meshIndex) {
            ParsedLayer layer;
            const OperationResult result = parseMesh(
                root,
                binary,
                meshIndex,
                meshes.at(meshIndex).toObject().value(QStringLiteral("name")).toString(),
                identity,
                &layer);
            if (!result.ok)
                return result;
            layers->append(std::move(layer));
        }
        return OperationResult::success();
    }

    int sceneIndex = 0;
    OperationResult result = integerField(
        root, QStringLiteral("scene"), &sceneIndex, false, 0);
    if (!result.ok)
        return result;
    if (sceneIndex < 0 || sceneIndex >= scenes.size())
        return OperationResult::failure(
            QStringLiteral("GLB references an invalid default scene."));
    const QJsonArray roots =
        scenes.at(sceneIndex).toObject().value(QStringLiteral("nodes")).toArray();
    QVector<bool> active(nodes.size(), false);
    Matrix identity;
    for (const QJsonValue& rootNode : roots) {
        int nodeIndex = 0;
        result = arrayIndex(
            rootNode, QStringLiteral("scene node"), &nodeIndex);
        if (!result.ok)
            return result;
        result = visitNode(
            root, binary, nodeIndex, identity, &active, layers);
        if (!result.ok)
            return result;
    }
    if (layers->isEmpty())
        return OperationResult::failure(
            QStringLiteral("The GLB default scene contains no mesh nodes."));
    return OperationResult::success();
}

OperationResult readGlb(
    const QString& path,
    QJsonObject* root,
    QByteArray* binary)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return OperationResult::failure(QStringLiteral("File is not readable."));
    const QByteArray bytes = file.readAll();
    if (bytes.size() < 20 || readUInt32(bytes, 0) != GlbMagic)
        return OperationResult::failure(QStringLiteral("Invalid GLB header."));
    if (readUInt32(bytes, 4) != 2)
        return OperationResult::failure(
            QStringLiteral("Only GLB version 2 is supported."));
    if (readUInt32(bytes, 8) != quint32(bytes.size()))
        return OperationResult::failure(
            QStringLiteral("GLB declared length does not match the file size."));

    QByteArray json;
    qsizetype offset = 12;
    while (offset + 8 <= bytes.size()) {
        const quint32 chunkLength = readUInt32(bytes, offset);
        const quint32 chunkType = readUInt32(bytes, offset + 4);
        offset += 8;
        if (chunkLength > quint32(bytes.size() - offset))
            return OperationResult::failure(
                QStringLiteral("GLB chunk exceeds the file bounds."));
        const QByteArray chunk = bytes.mid(offset, chunkLength);
        if (chunkType == JsonChunkType && json.isEmpty())
            json = chunk;
        else if (chunkType == BinaryChunkType && binary->isEmpty())
            *binary = chunk;
        offset += chunkLength;
    }
    if (offset != bytes.size() || json.isEmpty())
        return OperationResult::failure(
            QStringLiteral("GLB is missing a valid JSON chunk."));

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(json, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        return OperationResult::failure(
            QStringLiteral("Invalid GLB JSON: %1").arg(parseError.errorString()));
    }
    *root = document.object();
    const QJsonArray requiredExtensions =
        root->value(QStringLiteral("extensionsRequired")).toArray();
    for (const QJsonValue& extensionValue : requiredExtensions) {
        const QString extension = extensionValue.toString();
        if (extension != QStringLiteral("KHR_mesh_quantization")) {
            return OperationResult::failure(
                QStringLiteral("GLB requires unsupported extension \"%1\".")
                    .arg(extension.isEmpty()
                             ? QStringLiteral("<invalid>")
                             : extension));
        }
    }
    const QJsonArray buffers = root->value(QStringLiteral("buffers")).toArray();
    if (buffers.isEmpty() || binary->isEmpty())
        return OperationResult::failure(
            QStringLiteral("GLB has no embedded binary geometry buffer."));
    int declaredLength = 0;
    const OperationResult lengthResult = integerField(
        buffers.at(0).toObject(),
        QStringLiteral("byteLength"),
        &declaredLength);
    if (!lengthResult.ok)
        return lengthResult;
    if (declaredLength < 0 || declaredLength > binary->size())
        return OperationResult::failure(
            QStringLiteral("GLB binary chunk is shorter than its declared buffer."));
    return OperationResult::success();
}
} // namespace

OperationResult loadGlbMeshModels(
    const QString& path,
    MeshLabMeshRepository& destination,
    QVector<MeshModel*>* loadedModels)
{
    if (loadedModels == nullptr)
        return OperationResult::failure(
            QStringLiteral("No loaded GLB model output was provided."));
    loadedModels->clear();

    QJsonObject root;
    QByteArray binary;
    OperationResult result = readGlb(path, &root, &binary);
    if (!result.ok)
        return result;

    QVector<ParsedLayer> layers;
    result = parseLayers(root, binary, &layers);
    if (!result.ok)
        return result;

    const QString fallbackName = QFileInfo(path).completeBaseName();
    try {
        for (int layerIndex = 0; layerIndex < layers.size(); ++layerIndex) {
            ParsedLayer& layer = layers[layerIndex];
            const QString label = layer.name.isEmpty()
                                      ? (layers.size() == 1
                                             ? fallbackName
                                             : QStringLiteral("%1 %2")
                                                   .arg(fallbackName)
                                                   .arg(layerIndex + 1))
                                      : layer.name;
            MeshModel* model = destination.allocateMesh(path, label);
            loadedModels->append(model);
            if (layers.size() != 1)
                model->setIdInFile(layerIndex);

            vcg::tri::Allocator<CMeshO>::AddVertices(
                model->cm, layer.positions.size());
            for (int index = 0; index < layer.positions.size(); ++index) {
                model->cm.vert[index].P() = layer.positions[index];
                if (layer.hasColors)
                    model->cm.vert[index].C() = layer.colors[index];
            }
            vcg::tri::Allocator<CMeshO>::AddFaces(
                model->cm, layer.faces.size());
            for (int index = 0; index < layer.faces.size(); ++index) {
                const std::array<int, 3>& face = layer.faces[index];
                model->cm.face[index].V(0) = &model->cm.vert[face[0]];
                model->cm.face[index].V(1) = &model->cm.vert[face[1]];
                model->cm.face[index].V(2) = &model->cm.vert[face[2]];
            }
            model->updateBoxAndNormals();
            if (layer.hasColors)
                model->updateDataMask(MeshModel::MM_VERTCOLOR);
        }
    }
    catch (const std::exception& exception) {
        for (MeshModel* model : *loadedModels)
            destination.removeMesh(destination.resourceIdFor(*model));
        loadedModels->clear();
        return OperationResult::failure(
            QString::fromLocal8Bit(exception.what()));
    }
    catch (...) {
        for (MeshModel* model : *loadedModels)
            destination.removeMesh(destination.resourceIdFor(*model));
        loadedModels->clear();
        return OperationResult::failure(
            QStringLiteral("The GLB mesh could not be created."));
    }
    return OperationResult::success();
}
