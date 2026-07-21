#pragma once

#include <algorithm>
#include <cmath>

struct GridRenderVector3
{
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
};

struct GridRenderCamera
{
    bool enabled = false;
    GridRenderVector3 position;
    GridRenderVector3 target;
    GridRenderVector3 up{0.0, 1.0, 0.0};
    double fieldOfViewDegrees = 60.0;
};

struct GridRenderCameraClipPlanes
{
    double nearPlane = 0.1;
    double farPlane = 500.0;
};

struct GridRenderMeshLabVector3
{
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

inline GridRenderMeshLabVector3 gridRenderVectorForMeshLab(
    const GridRenderVector3& vector)
{
    // MeshLab uses Scalarm (float in this build) for camera transforms.
    // Validate the same values the renderer will receive, rather than only
    // their higher-precision command-line representation.
    return {
        static_cast<float>(vector.x),
        static_cast<float>(vector.y),
        static_cast<float>(vector.z)};
}

inline bool isValidGridRenderCamera(const GridRenderCamera& camera)
{
    if (!camera.enabled)
        return true;

    const auto isFinite = [](const GridRenderVector3& vector) {
        return std::isfinite(vector.x) && std::isfinite(vector.y) &&
            std::isfinite(vector.z);
    };
    // MeshLab ultimately applies this view using single-precision geometry
    // transforms. Keep user coordinates comfortably inside the range where the
    // vector products below, and the later conversion to Scalarm, are stable.
    const auto isWithinRenderableRange = [](const GridRenderVector3& vector) {
        constexpr double MaximumRenderableCoordinate = 1e12;
        return std::abs(vector.x) <= MaximumRenderableCoordinate &&
            std::abs(vector.y) <= MaximumRenderableCoordinate &&
            std::abs(vector.z) <= MaximumRenderableCoordinate;
    };
    if (!isFinite(camera.position) || !isFinite(camera.target) ||
        !isFinite(camera.up) || !isWithinRenderableRange(camera.position) ||
        !isWithinRenderableRange(camera.target) ||
        !isWithinRenderableRange(camera.up) ||
        !std::isfinite(camera.fieldOfViewDegrees) ||
        camera.fieldOfViewDegrees <= 1.0 || camera.fieldOfViewDegrees >= 179.0) {
        return false;
    }

    const GridRenderMeshLabVector3 renderPosition =
        gridRenderVectorForMeshLab(camera.position);
    const GridRenderMeshLabVector3 renderTarget =
        gridRenderVectorForMeshLab(camera.target);
    const GridRenderMeshLabVector3 renderUp =
        gridRenderVectorForMeshLab(camera.up);
    const GridRenderMeshLabVector3 direction{
        renderTarget.x - renderPosition.x,
        renderTarget.y - renderPosition.y,
        renderTarget.z - renderPosition.z};
    const auto negative = [](const GridRenderMeshLabVector3& vector) {
        return GridRenderMeshLabVector3{-vector.x, -vector.y, -vector.z};
    };
    const auto crossProduct = [](const GridRenderMeshLabVector3& left,
                                 const GridRenderMeshLabVector3& right) {
        return GridRenderMeshLabVector3{
            left.y * right.z - left.z * right.y,
            left.z * right.x - left.x * right.z,
            left.x * right.y - left.y * right.x};
    };
    const auto squaredNorm = [](const GridRenderMeshLabVector3& vector) {
        return vector.x * vector.x + vector.y * vector.y + vector.z * vector.z;
    };
    const auto isFiniteMeshLabVector = [](const GridRenderMeshLabVector3& vector) {
        return std::isfinite(vector.x) && std::isfinite(vector.y) &&
            std::isfinite(vector.z);
    };
    const float directionLengthSquared =
        squaredNorm(direction);
    const float upLengthSquared = squaredNorm(renderUp);
    // This follows Shot::LookTowards: its cross products and normalizations
    // are all single-precision operations.
    const GridRenderMeshLabVector3 xDirection =
        crossProduct(renderUp, negative(direction));
    const GridRenderMeshLabVector3 yDirection =
        crossProduct(negative(direction), xDirection);
    const float xDirectionLengthSquared = squaredNorm(xDirection);
    const float yDirectionLengthSquared = squaredNorm(yDirection);
    constexpr float MinimumLengthSquared = 1e-12f;
    return isFiniteMeshLabVector(renderPosition) &&
        isFiniteMeshLabVector(renderTarget) &&
        isFiniteMeshLabVector(renderUp) && isFiniteMeshLabVector(direction) &&
        isFiniteMeshLabVector(xDirection) && isFiniteMeshLabVector(yDirection) &&
        std::isfinite(directionLengthSquared) && std::isfinite(upLengthSquared) &&
        std::isfinite(xDirectionLengthSquared) &&
        std::isfinite(yDirectionLengthSquared) &&
        directionLengthSquared > MinimumLengthSquared &&
        upLengthSquared > MinimumLengthSquared &&
        xDirectionLengthSquared > MinimumLengthSquared &&
        yDirectionLengthSquared > MinimumLengthSquared;
}

inline GridRenderCameraClipPlanes gridRenderCameraClipPlanes(
    const GridRenderCamera& camera)
{
    if (!isValidGridRenderCamera(camera) || !camera.enabled)
        return {};

    const double dx = camera.target.x - camera.position.x;
    const double dy = camera.target.y - camera.position.y;
    const double dz = camera.target.z - camera.position.z;
    const double targetDistance = std::sqrt(dx * dx + dy * dy + dz * dz);
    return {
        std::min(0.1, targetDistance * 0.001),
        std::max(500.0, targetDistance * 100.0)};
}
