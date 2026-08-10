#include "python_patch.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <queue>
#include <set>
#include <stdexcept>
#include <tuple>
#include <utility>
#include <vector>

#include <Eigen/Core>
#include <Eigen/Eigenvalues>
#include <Eigen/OrderingMethods>
#include <Eigen/SparseCholesky>
#include <Eigen/SparseCore>
#include <Eigen/SparseQR>

namespace python_hole_filling
{

namespace
{

using Face = std::array<int, 3>;
using Vec2 = Eigen::Vector2d;
using Vec3 = Eigen::Vector3d;

struct Source
{
    std::vector<Vec3> vertices;
    std::vector<Face> faces;
    std::vector<Vec3> faceNormals;
    std::vector<Vec3> vertexNormals;
};

struct Loop
{
    std::vector<int> vertices;
    double perimeter = 0.0;
};

struct LocalPatch
{
    std::vector<Vec3> vertices;
    std::vector<Face> faces;
    std::vector<int> boundary;
    double tangentBefore = 0.0;
    double tangentAfter = 0.0;
};

Vec3 toVec3(const MeshPoint3D& point)
{
    return Vec3(point[0], point[1], point[2]);
}

MeshPoint3D toPoint(const Vec3& point)
{
    return MeshPoint3D{{point.x(), point.y(), point.z()}};
}

QString validate(const FillConfig& config)
{
    if (config.targetVertices < 3)
        return QStringLiteral("Target patch vertices must be at least 3.");
    if (config.maxHoles < 1)
        return QStringLiteral("The hole limit must be at least 1.");
    if (config.minLoopVertices < 3)
        return QStringLiteral("Minimum loop vertices must be at least 3.");
    if (!std::isfinite(config.minHolePerimeter) ||
        config.minHolePerimeter < 0.0) {
        return QStringLiteral(
            "Minimum hole perimeter must be finite and non-negative.");
    }
    if (!std::isfinite(config.normalWeight) ||
        !std::isfinite(config.anchorWeight) ||
        config.normalWeight < 0.0 ||
        config.anchorWeight < 0.0) {
        return QStringLiteral(
            "Geometry constraint weights must be finite and non-negative.");
    }
    if (!std::isfinite(config.maxTangentRms) ||
        config.maxTangentRms <= 0.0 ||
        config.maxTangentRms > 1.0) {
        return QStringLiteral(
            "Maximum tangent RMS must be in the interval (0, 1].");
    }
    return {};
}

Source prepareSource(const IMeshGeometryView& geometry)
{
    if (geometry.vertexCount() <= 0 || geometry.faceCount() <= 0)
        throw std::runtime_error("The selected mesh has no triangle geometry.");

    Source source;
    std::vector<int> remap(
        static_cast<std::size_t>(geometry.vertexCount()), -1);
    std::map<std::tuple<double, double, double>, int> uniqueVertices;
    for (int index = 0; index < geometry.vertexCount(); ++index) {
        const Vec3 point = toVec3(geometry.vertexPosition(index));
        if (!point.allFinite())
            throw std::runtime_error("The selected mesh contains non-finite vertices.");
        const auto key = std::make_tuple(point.x(), point.y(), point.z());
        const auto found = uniqueVertices.find(key);
        if (found != uniqueVertices.end()) {
            remap[static_cast<std::size_t>(index)] = found->second;
            continue;
        }
        const int mapped = static_cast<int>(source.vertices.size());
        uniqueVertices.emplace(key, mapped);
        remap[static_cast<std::size_t>(index)] = mapped;
        source.vertices.push_back(point);
    }

    std::set<Face> uniqueFaces;
    for (int index = 0; index < geometry.faceCount(); ++index) {
        const Face original = geometry.faceVertexIndices(index);
        Face face;
        for (int corner = 0; corner < 3; ++corner) {
            if (original[corner] < 0 ||
                original[corner] >= geometry.vertexCount()) {
                throw std::runtime_error(
                    "The selected mesh contains an invalid face index.");
            }
            face[corner] =
                remap[static_cast<std::size_t>(original[corner])];
        }
        if (face[0] == face[1] ||
            face[1] == face[2] ||
            face[2] == face[0]) {
            continue;
        }
        Face key = face;
        std::sort(key.begin(), key.end());
        if (uniqueFaces.insert(key).second)
            source.faces.push_back(face);
    }
    if (source.faces.empty())
        throw std::runtime_error("The selected mesh has no non-degenerate triangles.");

    source.vertexNormals.assign(source.vertices.size(), Vec3::Zero());
    for (const Face& face : source.faces) {
        const Vec3 normal =
            (source.vertices[static_cast<std::size_t>(face[1])] -
             source.vertices[static_cast<std::size_t>(face[0])])
                .cross(
                    source.vertices[static_cast<std::size_t>(face[2])] -
                    source.vertices[static_cast<std::size_t>(face[0])]);
        source.faceNormals.push_back(normal);
        for (int vertex : face)
            source.vertexNormals[static_cast<std::size_t>(vertex)] += normal;
    }
    for (Vec3& normal : source.vertexNormals) {
        const double length = normal.norm();
        if (std::isfinite(length) &&
            length > std::numeric_limits<double>::epsilon()) {
            normal /= length;
        }
        else {
            normal.setZero();
        }
    }
    return source;
}

std::vector<std::pair<int, int>> findBoundaryEdges(
    const Source& source,
    int* nonManifold)
{
    std::map<std::pair<int, int>, int> counts;
    for (const Face& face : source.faces) {
        for (int edge = 0; edge < 3; ++edge) {
            int first = face[edge];
            int second = face[(edge + 1) % 3];
            if (first > second)
                std::swap(first, second);
            ++counts[std::make_pair(first, second)];
        }
    }
    std::vector<std::pair<int, int>> result;
    *nonManifold = 0;
    for (const auto& item : counts) {
        if (item.second == 1)
            result.push_back(item.first);
        else if (item.second > 2)
            ++*nonManifold;
    }
    return result;
}

std::vector<Loop> orderBoundaryLoops(
    const Source& source,
    const std::vector<std::pair<int, int>>& edges)
{
    std::map<int, std::vector<int>> adjacency;
    for (const auto& edge : edges) {
        adjacency[edge.first].push_back(edge.second);
        adjacency[edge.second].push_back(edge.first);
    }
    for (auto& item : adjacency)
        std::sort(item.second.begin(), item.second.end());

    std::set<int> unseen;
    for (const auto& item : adjacency)
        unseen.insert(item.first);
    std::vector<Loop> loops;
    while (!unseen.empty()) {
        const int seed = *unseen.begin();
        std::set<int> component;
        std::vector<int> stack(1, seed);
        while (!stack.empty()) {
            const int vertex = stack.back();
            stack.pop_back();
            if (!component.insert(vertex).second)
                continue;
            const std::vector<int>& neighbors = adjacency[vertex];
            stack.insert(stack.end(), neighbors.begin(), neighbors.end());
        }
        for (int vertex : component)
            unseen.erase(vertex);

        bool cycle = true;
        for (int vertex : component)
            cycle = cycle && adjacency[vertex].size() == 2;
        if (!cycle)
            continue;

        std::vector<int> ordered;
        const int start = *component.begin();
        int previous = -1;
        int current = start;
        for (std::size_t step = 0; step <= component.size(); ++step) {
            ordered.push_back(current);
            const std::vector<int>& neighbors = adjacency[current];
            const int next =
                neighbors[0] != previous ? neighbors[0] : neighbors[1];
            previous = current;
            current = next;
            if (current == start)
                break;
        }
        const std::set<int> unique(ordered.begin(), ordered.end());
        if (current != start ||
            ordered.size() != component.size() ||
            unique.size() != ordered.size()) {
            continue;
        }

        double perimeter = 0.0;
        for (std::size_t index = 0; index < ordered.size(); ++index) {
            perimeter +=
                (source.vertices[static_cast<std::size_t>(
                     ordered[(index + 1) % ordered.size()])] -
                 source.vertices[static_cast<std::size_t>(ordered[index])])
                    .norm();
        }
        loops.push_back({ordered, perimeter});
    }
    std::sort(
        loops.begin(),
        loops.end(),
        [](const Loop& left, const Loop& right) {
            if (left.perimeter != right.perimeter)
                return left.perimeter > right.perimeter;
            return left.vertices < right.vertices;
        });
    return loops;
}

bool adjacentNormal(
    const Source& source,
    const std::vector<int>& boundary,
    Vec3* output)
{
    std::vector<bool> selected(source.vertices.size(), false);
    for (int vertex : boundary)
        selected[static_cast<std::size_t>(vertex)] = true;
    Vec3 normal = Vec3::Zero();
    for (std::size_t index = 0; index < source.faces.size(); ++index) {
        const Face& face = source.faces[index];
        if (selected[static_cast<std::size_t>(face[0])] ||
            selected[static_cast<std::size_t>(face[1])] ||
            selected[static_cast<std::size_t>(face[2])]) {
            normal += source.faceNormals[index];
        }
    }
    const double length = normal.norm();
    if (!std::isfinite(length) ||
        length <= std::numeric_limits<double>::epsilon()) {
        return false;
    }
    *output = normal / length;
    return true;
}

double cross2(const Vec2& first, const Vec2& second)
{
    return first.x() * second.y() - first.y() * second.x();
}

double polygonArea(const std::vector<Vec2>& points)
{
    double result = 0.0;
    for (std::size_t index = 0; index < points.size(); ++index) {
        const Vec2& first = points[index];
        const Vec2& second = points[(index + 1) % points.size()];
        result += first.x() * second.y() - first.y() * second.x();
    }
    return result * 0.5;
}

std::vector<Vec2> projectBoundary(
    const std::vector<Vec3>& points,
    const Vec3* preferredNormal)
{
    Vec3 origin = Vec3::Zero();
    for (const Vec3& point : points)
        origin += point;
    origin /= static_cast<double>(points.size());
    Eigen::Matrix3d covariance = Eigen::Matrix3d::Zero();
    for (const Vec3& point : points) {
        const Vec3 centered = point - origin;
        covariance += centered * centered.transpose();
    }
    covariance /= static_cast<double>(points.size());

    Eigen::SelfAdjointEigenSolver<Eigen::Matrix3d> solver(covariance);
    if (solver.info() != Eigen::Success)
        throw std::runtime_error("PCA failed for the boundary loop.");
    Vec3 normal = solver.eigenvectors().col(0);
    const Vec3 axisU = solver.eigenvectors().col(2);
    if (preferredNormal != nullptr && normal.dot(*preferredNormal) < 0.0)
        normal = -normal;
    Vec3 axisV = normal.cross(axisU);
    const double length = axisV.norm();
    if (length <= std::numeric_limits<double>::epsilon())
        throw std::runtime_error("The fitted boundary plane is degenerate.");
    axisV /= length;

    std::vector<Vec2> projected;
    projected.reserve(points.size());
    for (const Vec3& point : points) {
        const Vec3 centered = point - origin;
        projected.push_back(Vec2(centered.dot(axisU), centered.dot(axisV)));
    }
    return projected;
}

bool insideTriangle(
    const Vec2& point,
    const Vec2& first,
    const Vec2& second,
    const Vec2& third,
    double tolerance)
{
    return cross2(second - first, point - first) >= -tolerance &&
           cross2(third - second, point - second) >= -tolerance &&
           cross2(first - third, point - third) >= -tolerance;
}

std::vector<Face> earClip(const std::vector<Vec2>& points)
{
    double minimumX = points.front().x();
    double maximumX = minimumX;
    double minimumY = points.front().y();
    double maximumY = minimumY;
    for (const Vec2& point : points) {
        minimumX = std::min(minimumX, point.x());
        maximumX = std::max(maximumX, point.x());
        minimumY = std::min(minimumY, point.y());
        maximumY = std::max(maximumY, point.y());
    }
    const double extent = std::max(
        1.0, std::max(maximumX - minimumX, maximumY - minimumY));
    const double tolerance =
        std::numeric_limits<double>::epsilon() * extent * extent * 64.0;
    if (polygonArea(points) <= tolerance)
        throw std::runtime_error("The projected boundary has invalid orientation.");

    std::vector<int> remaining(points.size());
    for (std::size_t index = 0; index < points.size(); ++index)
        remaining[index] = static_cast<int>(index);
    std::vector<Face> result;
    while (remaining.size() > 3) {
        bool found = false;
        double best = -std::numeric_limits<double>::infinity();
        std::size_t bestPosition = 0;
        Face bestFace{{0, 0, 0}};
        for (std::size_t position = 0;
             position < remaining.size();
             ++position) {
            const int previous =
                remaining[(position + remaining.size() - 1) % remaining.size()];
            const int current = remaining[position];
            const int next =
                remaining[(position + 1) % remaining.size()];
            const Vec2& first = points[static_cast<std::size_t>(previous)];
            const Vec2& middle = points[static_cast<std::size_t>(current)];
            const Vec2& third = points[static_cast<std::size_t>(next)];
            const double convexity =
                cross2(middle - first, third - middle);
            if (convexity <= tolerance)
                continue;
            bool contains = false;
            for (int candidate : remaining) {
                if (candidate == previous ||
                    candidate == current ||
                    candidate == next) {
                    continue;
                }
                contains = insideTriangle(
                    points[static_cast<std::size_t>(candidate)],
                    first,
                    middle,
                    third,
                    tolerance);
                if (contains)
                    break;
            }
            if (!contains && (!found || convexity > best)) {
                found = true;
                best = convexity;
                bestPosition = position;
                bestFace = Face{{previous, current, next}};
            }
        }
        if (!found) {
            throw std::runtime_error(
                "Ear clipping failed; the projected boundary may self-intersect.");
        }
        result.push_back(bestFace);
        remaining.erase(
            remaining.begin() + static_cast<std::ptrdiff_t>(bestPosition));
    }
    result.push_back(Face{{remaining[0], remaining[1], remaining[2]}});

    double area = 0.0;
    for (Face& face : result) {
        double twiceArea = cross2(
            points[static_cast<std::size_t>(face[1])] -
                points[static_cast<std::size_t>(face[0])],
            points[static_cast<std::size_t>(face[2])] -
                points[static_cast<std::size_t>(face[0])]);
        if (twiceArea < 0.0) {
            std::swap(face[1], face[2]);
            twiceArea = -twiceArea;
        }
        if (twiceArea <= tolerance * 2.0e-5)
            throw std::runtime_error("The triangulation contains a degenerate triangle.");
        area += twiceArea * 0.5;
    }
    const double expected = polygonArea(points);
    if (result.size() != points.size() - 2 ||
        std::abs(area - expected) >
            std::max(std::abs(expected) * 1.0e-7, 1.0e-12)) {
        throw std::runtime_error("The triangulation does not cover the polygon.");
    }
    return result;
}

double faceArea(const std::vector<Vec2>& points, const Face& face)
{
    return std::abs(cross2(
               points[static_cast<std::size_t>(face[1])] -
                   points[static_cast<std::size_t>(face[0])],
               points[static_cast<std::size_t>(face[2])] -
                   points[static_cast<std::size_t>(face[0])])) *
           0.5;
}

struct Priority
{
    double area = 0.0;
    int id = 0;
    bool operator<(const Priority& other) const
    {
        return area != other.area ? area < other.area : id > other.id;
    }
};

void refine(
    const std::vector<Vec2>& boundary2D,
    const std::vector<Vec3>& boundary3D,
    const std::vector<Face>& support,
    int target,
    std::vector<Vec3>* vertices,
    std::vector<Face>* faces)
{
    std::vector<Vec2> points = boundary2D;
    *vertices = boundary3D;
    std::map<int, Face> active;
    std::priority_queue<Priority> queue;
    int nextId = 0;
    for (const Face& face : support) {
        const int center = static_cast<int>(points.size());
        points.push_back(
            (points[static_cast<std::size_t>(face[0])] +
             points[static_cast<std::size_t>(face[1])] +
             points[static_cast<std::size_t>(face[2])]) /
            3.0);
        vertices->push_back(
            ((*vertices)[static_cast<std::size_t>(face[0])] +
             (*vertices)[static_cast<std::size_t>(face[1])] +
             (*vertices)[static_cast<std::size_t>(face[2])]) /
            3.0);
        const Face children[] = {
            {{face[0], face[1], center}},
            {{face[1], face[2], center}},
            {{face[2], face[0], center}},
        };
        for (const Face& child : children) {
            active.emplace(nextId, child);
            queue.push({faceArea(points, child), nextId});
            ++nextId;
        }
    }
    target = std::max(target, static_cast<int>(points.size()));
    while (static_cast<int>(points.size()) < target && !queue.empty()) {
        const Priority top = queue.top();
        queue.pop();
        const auto found = active.find(top.id);
        if (found == active.end())
            continue;
        const Face face = found->second;
        active.erase(found);
        const int center = static_cast<int>(points.size());
        points.push_back(
            (points[static_cast<std::size_t>(face[0])] +
             points[static_cast<std::size_t>(face[1])] +
             points[static_cast<std::size_t>(face[2])]) /
            3.0);
        vertices->push_back(
            ((*vertices)[static_cast<std::size_t>(face[0])] +
             (*vertices)[static_cast<std::size_t>(face[1])] +
             (*vertices)[static_cast<std::size_t>(face[2])]) /
            3.0);
        const Face children[] = {
            {{face[0], face[1], center}},
            {{face[1], face[2], center}},
            {{face[2], face[0], center}},
        };
        for (const Face& child : children) {
            active.emplace(nextId, child);
            queue.push({faceArea(points, child), nextId});
            ++nextId;
        }
    }
    faces->clear();
    for (const auto& item : active)
        faces->push_back(item.second);
}

std::vector<int> thirdVertices(
    const std::vector<Face>& faces,
    int boundaryCount)
{
    std::map<std::pair<int, int>, int> edgeThird;
    for (const Face& face : faces) {
        const int triples[3][3] = {
            {face[0], face[1], face[2]},
            {face[1], face[2], face[0]},
            {face[2], face[0], face[1]},
        };
        for (const auto& triple : triples) {
            if (triple[0] >= boundaryCount ||
                triple[1] >= boundaryCount) {
                continue;
            }
            int first = triple[0];
            int second = triple[1];
            if (first > second)
                std::swap(first, second);
            edgeThird[std::make_pair(first, second)] = triple[2];
        }
    }
    std::vector<int> result;
    for (int index = 0; index < boundaryCount; ++index) {
        int first = index;
        int second = (index + 1) % boundaryCount;
        if (first > second)
            std::swap(first, second);
        const auto found = edgeThird.find(std::make_pair(first, second));
        if (found == edgeThird.end() || found->second < boundaryCount) {
            throw std::runtime_error(
                "The patch topology is missing an interior boundary vertex.");
        }
        result.push_back(found->second);
    }
    return result;
}

void constraints(
    const Source& source,
    const LocalPatch& patch,
    std::vector<int>* thirds,
    std::vector<Vec3>* normals,
    std::vector<Vec3>* midpoints)
{
    const int count = static_cast<int>(patch.boundary.size());
    *thirds = thirdVertices(patch.faces, count);
    for (int index = 0; index < count; ++index) {
        const int next = (index + 1) % count;
        const Vec3& firstNormal =
            source.vertexNormals[static_cast<std::size_t>(
                patch.boundary[static_cast<std::size_t>(index)])];
        const Vec3& secondNormal =
            source.vertexNormals[static_cast<std::size_t>(
                patch.boundary[static_cast<std::size_t>(next)])];
        Vec3 normal = firstNormal + secondNormal;
        double length = normal.norm();
        if (length <= std::numeric_limits<double>::epsilon()) {
            normal = firstNormal;
            length = normal.norm();
        }
        if (length <= std::numeric_limits<double>::epsilon()) {
            throw std::runtime_error(
                "Boundary normals cannot be estimated from the source surface.");
        }
        normals->push_back(normal / length);
        midpoints->push_back(
            0.5 *
            (patch.vertices[static_cast<std::size_t>(index)] +
             patch.vertices[static_cast<std::size_t>(next)]));
    }
}

double tangentRms(
    const std::vector<Vec3>& vertices,
    const std::vector<int>& thirds,
    const std::vector<Vec3>& normals,
    const std::vector<Vec3>& midpoints)
{
    double sum = 0.0;
    for (std::size_t index = 0; index < thirds.size(); ++index) {
        const Vec3 vector =
            vertices[static_cast<std::size_t>(thirds[index])] -
            midpoints[index];
        const double residual =
            vector.dot(normals[index]) /
            std::max(vector.norm(), std::numeric_limits<double>::epsilon());
        sum += residual * residual;
    }
    return std::sqrt(sum / static_cast<double>(thirds.size()));
}

void solve(const Source& source, const FillConfig& config, LocalPatch* patch)
{
    const int boundaryCount = static_cast<int>(patch->boundary.size());
    const int interiorCount =
        static_cast<int>(patch->vertices.size()) - boundaryCount;
    if (interiorCount <= 0)
        throw std::runtime_error("The patch has no interior free vertices.");

    std::vector<int> thirds;
    std::vector<Vec3> normals;
    std::vector<Vec3> midpoints;
    constraints(source, *patch, &thirds, &normals, &midpoints);
    patch->tangentBefore =
        tangentRms(patch->vertices, thirds, normals, midpoints);

    std::vector<std::set<int>> neighbors(patch->vertices.size());
    for (const Face& face : patch->faces) {
        neighbors[static_cast<std::size_t>(face[0])].insert(face[1]);
        neighbors[static_cast<std::size_t>(face[0])].insert(face[2]);
        neighbors[static_cast<std::size_t>(face[1])].insert(face[0]);
        neighbors[static_cast<std::size_t>(face[1])].insert(face[2]);
        neighbors[static_cast<std::size_t>(face[2])].insert(face[0]);
        neighbors[static_cast<std::size_t>(face[2])].insert(face[1]);
    }

    const int laplacianRows = interiorCount * 3;
    const int normalRows =
        config.normalWeight > 0.0 ? boundaryCount : 0;
    const int anchorRows =
        config.anchorWeight > 0.0 ? interiorCount * 3 : 0;
    const int rowCount = laplacianRows + normalRows + anchorRows;
    const int columnCount = interiorCount * 3;
    Eigen::VectorXd rhs = Eigen::VectorXd::Zero(rowCount);
    std::vector<Eigen::Triplet<double>> triplets;

    for (int interior = 0; interior < interiorCount; ++interior) {
        const int vertex = boundaryCount + interior;
        const std::set<int>& adjacent =
            neighbors[static_cast<std::size_t>(vertex)];
        if (adjacent.empty())
            throw std::runtime_error("The patch contains an isolated vertex.");
        const double inverse = 1.0 / static_cast<double>(adjacent.size());
        for (int coordinate = 0; coordinate < 3; ++coordinate) {
            const int row = interior * 3 + coordinate;
            triplets.emplace_back(row, interior * 3 + coordinate, 1.0);
            for (int neighbor : adjacent) {
                if (neighbor < boundaryCount) {
                    rhs[row] +=
                        inverse *
                        patch->vertices[static_cast<std::size_t>(neighbor)]
                            [coordinate];
                }
                else {
                    triplets.emplace_back(
                        row,
                        (neighbor - boundaryCount) * 3 + coordinate,
                        -inverse);
                }
            }
        }
    }

    int row = laplacianRows;
    if (normalRows > 0) {
        const double scale = std::sqrt(config.normalWeight);
        for (int index = 0; index < boundaryCount; ++index, ++row) {
            const int interior =
                thirds[static_cast<std::size_t>(index)] - boundaryCount;
            for (int coordinate = 0; coordinate < 3; ++coordinate) {
                triplets.emplace_back(
                    row,
                    interior * 3 + coordinate,
                    scale *
                        normals[static_cast<std::size_t>(index)][coordinate]);
            }
            rhs[row] =
                scale *
                normals[static_cast<std::size_t>(index)]
                    .dot(midpoints[static_cast<std::size_t>(index)]);
        }
    }
    if (anchorRows > 0) {
        const double scale = std::sqrt(config.anchorWeight);
        for (int interior = 0; interior < interiorCount; ++interior) {
            for (int coordinate = 0; coordinate < 3; ++coordinate, ++row) {
                triplets.emplace_back(
                    row, interior * 3 + coordinate, scale);
                rhs[row] =
                    scale *
                    patch->vertices[static_cast<std::size_t>(
                        boundaryCount + interior)][coordinate];
            }
        }
    }

    Eigen::SparseMatrix<double> matrix(rowCount, columnCount);
    matrix.setFromTriplets(triplets.begin(), triplets.end());
    matrix.makeCompressed();
    const Eigen::SparseMatrix<double> normalMatrix =
        matrix.transpose() * matrix;
    const Eigen::VectorXd normalRhs = matrix.transpose() * rhs;
    Eigen::VectorXd solution;
    Eigen::SimplicialLDLT<Eigen::SparseMatrix<double>> direct;
    direct.compute(normalMatrix);
    if (direct.info() == Eigen::Success)
        solution = direct.solve(normalRhs);
    if (direct.info() != Eigen::Success ||
        solution.size() != columnCount ||
        !solution.allFinite()) {
        Eigen::SparseQR<
            Eigen::SparseMatrix<double>,
            Eigen::COLAMDOrdering<int>>
            fallback;
        fallback.compute(matrix);
        if (fallback.info() != Eigen::Success)
            throw std::runtime_error("The thin-plate system could not be factorized.");
        solution = fallback.solve(rhs);
        if (fallback.info() != Eigen::Success || !solution.allFinite())
            throw std::runtime_error("The thin-plate solution is not finite.");
    }
    for (int interior = 0; interior < interiorCount; ++interior) {
        patch->vertices[static_cast<std::size_t>(
            boundaryCount + interior)] =
            Vec3(
                solution[interior * 3],
                solution[interior * 3 + 1],
                solution[interior * 3 + 2]);
    }
    patch->tangentAfter =
        tangentRms(patch->vertices, thirds, normals, midpoints);
    if (patch->tangentAfter > config.maxTangentRms) {
        throw std::runtime_error(
            QStringLiteral(
                "Boundary tangent RMS %1 exceeds the configured limit %2.")
                .arg(patch->tangentAfter, 0, 'g', 6)
                .arg(config.maxTangentRms, 0, 'g', 6)
                .toStdString());
    }
}

LocalPatch build(
    const Source& source,
    const Loop& loop,
    const FillConfig& config)
{
    LocalPatch patch;
    patch.boundary = loop.vertices;
    std::vector<Vec3> boundary3D;
    for (int vertex : loop.vertices)
        boundary3D.push_back(source.vertices[static_cast<std::size_t>(vertex)]);
    Vec3 preferred;
    const bool hasPreferred =
        adjacentNormal(source, loop.vertices, &preferred);
    std::vector<Vec2> boundary2D =
        projectBoundary(boundary3D, hasPreferred ? &preferred : nullptr);
    if (polygonArea(boundary2D) < 0.0) {
        std::reverse(patch.boundary.begin(), patch.boundary.end());
        std::reverse(boundary3D.begin(), boundary3D.end());
        std::reverse(boundary2D.begin(), boundary2D.end());
    }
    const std::vector<Face> base = earClip(boundary2D);
    if (config.method == PatchMethod::Planar) {
        Vec3 minimum = boundary3D.front();
        Vec3 maximum = boundary3D.front();
        for (const Vec3& point : boundary3D) {
            minimum = minimum.cwiseMin(point);
            maximum = maximum.cwiseMax(point);
        }
        const double scale = std::max((maximum - minimum).norm(), 1.0);
        const double tolerance =
            std::numeric_limits<double>::epsilon() * scale * scale * 64.0;
        for (const Face& face : base) {
            const double twiceArea =
                (boundary3D[static_cast<std::size_t>(face[1])] -
                 boundary3D[static_cast<std::size_t>(face[0])])
                    .cross(
                        boundary3D[static_cast<std::size_t>(face[2])] -
                        boundary3D[static_cast<std::size_t>(face[0])])
                    .norm();
            if (twiceArea <= tolerance) {
                throw std::runtime_error(
                    "The planar patch contains a degenerate 3D triangle.");
            }
        }
        patch.vertices = boundary3D;
        patch.faces = base;
        return patch;
    }
    refine(
        boundary2D,
        boundary3D,
        base,
        config.targetVertices,
        &patch.vertices,
        &patch.faces);
    solve(source, config, &patch);
    return patch;
}

void appendPatch(const LocalPatch& patch, FillResult* result)
{
    const int patchOffset = result->patch.vertices.size();
    for (const Vec3& point : patch.vertices)
        result->patch.vertices.append(toPoint(point));
    for (const Face& face : patch.faces) {
        result->patch.faces.append(Face{{
            patchOffset + face[0],
            patchOffset + face[1],
            patchOffset + face[2],
        }});
    }

    const int boundaryCount = static_cast<int>(patch.boundary.size());
    std::vector<int> remap(patch.vertices.size(), -1);
    for (int index = 0; index < boundaryCount; ++index)
        remap[static_cast<std::size_t>(index)] =
            patch.boundary[static_cast<std::size_t>(index)];
    for (std::size_t index = static_cast<std::size_t>(boundaryCount);
         index < patch.vertices.size();
         ++index) {
        remap[index] = result->merged.vertices.size();
        result->merged.vertices.append(toPoint(patch.vertices[index]));
    }
    for (const Face& face : patch.faces) {
        result->merged.faces.append(Face{{
            remap[static_cast<std::size_t>(face[0])],
            remap[static_cast<std::size_t>(face[1])],
            remap[static_cast<std::size_t>(face[2])],
        }});
    }
}

} // namespace

FillResult Engine::fill(
    const IMeshGeometryView& geometry,
    const FillConfig& config) const
{
    FillResult result;
    const QString configError = validate(config);
    if (!configError.isEmpty()) {
        result.result = OperationResult::failure(configError);
        return result;
    }

    Source source;
    try {
        source = prepareSource(geometry);
    }
    catch (const std::exception& exception) {
        result.result = OperationResult::failure(
            QString::fromLocal8Bit(exception.what()));
        return result;
    }
    for (const Vec3& point : source.vertices)
        result.merged.vertices.append(toPoint(point));
    for (const Face& face : source.faces)
        result.merged.faces.append(face);

    const std::vector<std::pair<int, int>> edges =
        findBoundaryEdges(source, &result.nonManifoldEdgeCount);
    result.boundaryEdgeCount = static_cast<int>(edges.size());
    const std::vector<Loop> loops = orderBoundaryLoops(source, edges);
    result.closedBoundaryLoopCount = static_cast<int>(loops.size());
    std::vector<Loop> eligible;
    for (const Loop& loop : loops) {
        if (static_cast<int>(loop.vertices.size()) >=
                config.minLoopVertices &&
            loop.perimeter >= config.minHolePerimeter) {
            eligible.push_back(loop);
        }
    }
    result.eligibleBoundaryLoopCount = static_cast<int>(eligible.size());
    if (eligible.empty()) {
        result.result = OperationResult::failure(QStringLiteral(
            "No eligible manifold boundary loops were found."));
        return result;
    }

    std::size_t count = eligible.size();
    if (config.scope == HoleScope::Largest)
        count = 1;
    else if (config.scope == HoleScope::UpToLimit)
        count = std::min(count, static_cast<std::size_t>(config.maxHoles));
    for (std::size_t index = 0; index < count; ++index) {
        HoleResult hole;
        hole.holeIndex = static_cast<int>(index);
        hole.boundaryVertexCount =
            static_cast<int>(eligible[index].vertices.size());
        hole.perimeter = eligible[index].perimeter;
        try {
            const LocalPatch patch = build(source, eligible[index], config);
            appendPatch(patch, &result);
            hole.patched = true;
            hole.patchVertexCount = static_cast<int>(patch.vertices.size());
            hole.patchFaceCount = static_cast<int>(patch.faces.size());
            hole.tangentRmsBefore = patch.tangentBefore;
            hole.tangentRmsAfter = patch.tangentAfter;
            ++result.patchedHoleCount;
        }
        catch (const std::exception& exception) {
            hole.reason = QString::fromLocal8Bit(exception.what());
        }
        result.holes.append(hole);
    }
    if (result.patchedHoleCount == 0) {
        const QString reason =
            result.holes.isEmpty() ? QString() : result.holes.front().reason;
        result.result = OperationResult::failure(
            reason.isEmpty()
                ? QStringLiteral("No selected hole could be patched.")
                : QStringLiteral("No selected hole could be patched: %1")
                      .arg(reason));
        return result;
    }
    result.result = OperationResult::success();
    return result;
}

} // namespace python_hole_filling
