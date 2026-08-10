#include "controllable_hole_filling.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <exception>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

#include <QDebug>
#include <QElapsedTimer>

#include <vcg/complex/algorithms/create/mc_lookup_table.h>

namespace controllable_hole_filling
{
namespace
{

constexpr float kDomainMin = -1.0f;
constexpr float kDomainMax = 1.0f;
constexpr float kTargetHalfExtent = 0.95f;
constexpr int kMaximumResolution = 1536;
constexpr int kMaximumBandRadius = 31;
constexpr int kMaximumCclSweeps = 256;

struct Vec3f
{
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;

    float& operator[](int axis)
    {
        return axis == 0 ? x : (axis == 1 ? y : z);
    }

    float operator[](int axis) const
    {
        return axis == 0 ? x : (axis == 1 ? y : z);
    }
};

Vec3f operator-(const Vec3f& left, const Vec3f& right)
{
    return {left.x - right.x, left.y - right.y, left.z - right.z};
}

Vec3f operator+(const Vec3f& left, const Vec3f& right)
{
    return {left.x + right.x, left.y + right.y, left.z + right.z};
}

Vec3f operator*(const Vec3f& value, float scale)
{
    return {value.x * scale, value.y * scale, value.z * scale};
}

float dot(const Vec3f& left, const Vec3f& right)
{
    return left.x * right.x + left.y * right.y + left.z * right.z;
}

Vec3f cross(const Vec3f& left, const Vec3f& right)
{
    return {
        left.y * right.z - left.z * right.y,
        left.z * right.x - left.x * right.z,
        left.x * right.y - left.y * right.x};
}

float squaredLength(const Vec3f& value)
{
    return dot(value, value);
}

bool finite(const Vec3f& value)
{
    return std::isfinite(value.x) &&
           std::isfinite(value.y) &&
           std::isfinite(value.z);
}

struct SourceMesh
{
    std::vector<Vec3f> vertices;
    std::vector<std::array<int, 3>> faces;
    Vec3f center;
    float scale = 1.0f;
};

QString validateConfig(const FillConfig& config)
{
    if (config.resolution < 16 ||
        config.resolution > kMaximumResolution) {
        return QStringLiteral("Resolution r must be in [16, %1].")
            .arg(kMaximumResolution);
    }
    if (config.cclIterations < 0 ||
        config.cclIterations > kMaximumBandRadius) {
        return QStringLiteral("ccl-iter must be in [0, %1].")
            .arg(kMaximumBandRadius);
    }
    if (!std::isfinite(config.epsFactor) ||
        config.epsFactor <= 0.0 ||
        config.epsFactor > 32.0) {
        return QStringLiteral("eps must be finite and in (0, 32].");
    }
    const int levelRadius = static_cast<int>(
        std::ceil(config.epsFactor / (kDomainMax - kDomainMin)));
    if (std::max(config.cclIterations, levelRadius) >
        kMaximumBandRadius) {
        return QStringLiteral(
            "The effective sparse band radius must not exceed %1.")
            .arg(kMaximumBandRadius);
    }
    return {};
}

SourceMesh prepareSource(const IMeshGeometryView& geometry)
{
    if (geometry.vertexCount() <= 0 || geometry.faceCount() <= 0)
        throw std::runtime_error("The selected mesh has no triangle geometry.");

    SourceMesh source;
    source.vertices.reserve(static_cast<std::size_t>(geometry.vertexCount()));
    Vec3f minimum{
        std::numeric_limits<float>::infinity(),
        std::numeric_limits<float>::infinity(),
        std::numeric_limits<float>::infinity()};
    Vec3f maximum{
        -std::numeric_limits<float>::infinity(),
        -std::numeric_limits<float>::infinity(),
        -std::numeric_limits<float>::infinity()};
    for (int index = 0; index < geometry.vertexCount(); ++index) {
        const MeshPoint3D point = geometry.vertexPosition(index);
        const Vec3f value{
            static_cast<float>(point[0]),
            static_cast<float>(point[1]),
            static_cast<float>(point[2])};
        if (!finite(value))
            throw std::runtime_error("The selected mesh contains non-finite vertices.");
        source.vertices.push_back(value);
        for (int axis = 0; axis < 3; ++axis) {
            minimum[axis] = std::min(minimum[axis], value[axis]);
            maximum[axis] = std::max(maximum[axis], value[axis]);
        }
    }

    source.center = (minimum + maximum) * 0.5f;
    const Vec3f extent = maximum - minimum;
    const float maximumExtent = std::max(extent.x, std::max(extent.y, extent.z));
    if (!std::isfinite(maximumExtent) ||
        maximumExtent <= std::numeric_limits<float>::epsilon()) {
        throw std::runtime_error("The selected mesh has a degenerate bounding box.");
    }
    source.scale = (kTargetHalfExtent * 2.0f) / maximumExtent;
    for (Vec3f& point : source.vertices)
        point = (point - source.center) * source.scale;

    source.faces.reserve(static_cast<std::size_t>(geometry.faceCount()));
    for (int index = 0; index < geometry.faceCount(); ++index) {
        const std::array<int, 3> face = geometry.faceVertexIndices(index);
        for (int corner = 0; corner < 3; ++corner) {
            if (face[corner] < 0 ||
                face[corner] >= geometry.vertexCount()) {
                throw std::runtime_error(
                    "The selected mesh contains an invalid face index.");
            }
        }
        if (face[0] == face[1] ||
            face[1] == face[2] ||
            face[2] == face[0]) {
            continue;
        }
        const Vec3f normal = cross(
            source.vertices[static_cast<std::size_t>(face[1])] -
                source.vertices[static_cast<std::size_t>(face[0])],
            source.vertices[static_cast<std::size_t>(face[2])] -
                source.vertices[static_cast<std::size_t>(face[0])]);
        if (squaredLength(normal) >
            std::numeric_limits<float>::epsilon()) {
            source.faces.push_back(face);
        }
    }
    if (source.faces.empty())
        throw std::runtime_error("The selected mesh has no non-degenerate triangles.");
    return source;
}

template<typename Function>
void parallelFor(
    std::size_t count,
    std::size_t grain,
    const Function& function)
{
    if (count == 0)
        return;
    const unsigned int hardware = std::max(1u, std::thread::hardware_concurrency());
    const std::size_t usefulWorkers = (count + grain - 1) / grain;
    const unsigned int workerCount = static_cast<unsigned int>(
        std::min<std::size_t>(hardware, usefulWorkers));
    if (workerCount <= 1) {
        for (std::size_t index = 0; index < count; ++index)
            function(index);
        return;
    }

    std::atomic<std::size_t> next(0);
    std::atomic<bool> failed(false);
    std::exception_ptr exception;
    std::mutex exceptionMutex;
    std::vector<std::thread> workers;
    workers.reserve(workerCount);
    for (unsigned int worker = 0; worker < workerCount; ++worker) {
        workers.emplace_back([&] {
            try {
                while (!failed.load(std::memory_order_relaxed)) {
                    const std::size_t begin =
                        next.fetch_add(grain, std::memory_order_relaxed);
                    if (begin >= count)
                        break;
                    const std::size_t end = std::min(count, begin + grain);
                    for (std::size_t index = begin; index < end; ++index)
                        function(index);
                }
            }
            catch (...) {
                failed.store(true, std::memory_order_relaxed);
                std::lock_guard<std::mutex> lock(exceptionMutex);
                if (!exception)
                    exception = std::current_exception();
            }
        });
    }
    for (std::thread& worker : workers)
        worker.join();
    if (exception)
        std::rethrow_exception(exception);
}

class PackedGrid
{
public:
    PackedGrid() = default;

    explicit PackedGrid(int dimension)
        : dimension_(dimension),
          rowWords_((dimension + 31) / 32),
          words_(
              static_cast<std::size_t>(dimension) *
                  static_cast<std::size_t>(dimension) *
                  static_cast<std::size_t>(rowWords_),
              0u)
    {
    }

    int dimension() const { return dimension_; }
    int rowWords() const { return rowWords_; }
    std::size_t wordCount() const { return words_.size(); }

    uint32_t* data() { return words_.data(); }
    const uint32_t* data() const { return words_.data(); }

    uint32_t word(int first, int second, int wordIndex) const
    {
        if (first < 0 || first >= dimension_ ||
            second < 0 || second >= dimension_ ||
            wordIndex < 0 || wordIndex >= rowWords_) {
            return 0u;
        }
        return words_[wordOffset(first, second, wordIndex)];
    }

    uint32_t word(std::size_t index) const
    {
        return words_[index];
    }

    uint32_t& word(std::size_t index)
    {
        return words_[index];
    }

    bool get(int first, int second, int third) const
    {
        if (first < 0 || first >= dimension_ ||
            second < 0 || second >= dimension_ ||
            third < 0 || third >= dimension_) {
            return false;
        }
        return (words_[wordOffset(first, second, third >> 5)] &
                (uint32_t(1) << (third & 31))) != 0u;
    }

    bool getLinear(uint64_t index) const
    {
        const uint64_t plane =
            static_cast<uint64_t>(dimension_) * dimension_;
        const int first = static_cast<int>(index / plane);
        const int second = static_cast<int>((index / dimension_) % dimension_);
        const int third = static_cast<int>(index % dimension_);
        return get(first, second, third);
    }

    void setAtomic(int first, int second, int third)
    {
        if (first < 0 || first >= dimension_ ||
            second < 0 || second >= dimension_ ||
            third < 0 || third >= dimension_) {
            return;
        }
        uint32_t* target = &words_[wordOffset(first, second, third >> 5)];
        __atomic_fetch_or(
            target,
            uint32_t(1) << (third & 31),
            __ATOMIC_RELAXED);
    }

    uint32_t validMask(int wordIndex) const
    {
        const int validBits = dimension_ - wordIndex * 32;
        if (validBits >= 32)
            return 0xffffffffu;
        if (validBits <= 0)
            return 0u;
        return (uint32_t(1) << validBits) - 1u;
    }

private:
    std::size_t wordOffset(int first, int second, int wordIndex) const
    {
        return (static_cast<std::size_t>(first) * dimension_ + second) *
                   rowWords_ +
               wordIndex;
    }

    int dimension_ = 0;
    int rowWords_ = 0;
    std::vector<uint32_t> words_;
};

float gridIndex(float coordinate, int resolution)
{
    return (coordinate - kDomainMin) *
           (static_cast<float>(resolution) / (kDomainMax - kDomainMin));
}

void rasterizeSurface(
    const SourceMesh& source,
    int resolution,
    PackedGrid* barrier)
{
    const std::size_t taskCount = source.faces.size() * 3;
    parallelFor(taskCount, 16, [&](std::size_t task) {
        const int axis = static_cast<int>(task % 3);
        const std::array<int, 3>& face = source.faces[task / 3];
        Vec3f points[3];
        for (int corner = 0; corner < 3; ++corner) {
            const Vec3f& point =
                source.vertices[static_cast<std::size_t>(face[corner])];
            points[corner] = {
                gridIndex(point.x, resolution),
                gridIndex(point.y, resolution),
                gridIndex(point.z, resolution)};
        }

        const Vec3f firstEdge = points[1] - points[0];
        const Vec3f secondEdge = points[2] - points[0];
        const Vec3f normal = cross(firstEdge, secondEdge);
        const float normalLength = std::sqrt(squaredLength(normal));
        const float degenerateTolerance =
            1.0e-12f * std::max(1.0f, float(resolution * resolution));
        const float normalAxis = normal[axis];
        if (normalLength <= degenerateTolerance ||
            std::abs(normalAxis) <=
                std::max(degenerateTolerance, 1.0e-7f * normalLength)) {
            return;
        }

        const int uAxis = axis == 0 ? 1 : 0;
        const int vAxis = axis == 2 ? 1 : 2;
        const float au = points[0][uAxis];
        const float av = points[0][vAxis];
        const float bu = points[1][uAxis];
        const float bv = points[1][vAxis];
        const float cu = points[2][uAxis];
        const float cv = points[2][vAxis];
        const int u0 = std::max(
            0,
            static_cast<int>(std::floor(std::min(au, std::min(bu, cu)) - 1.0e-6f)));
        const int v0 = std::max(
            0,
            static_cast<int>(std::floor(std::min(av, std::min(bv, cv)) - 1.0e-6f)));
        const int u1 = std::min(
            resolution,
            static_cast<int>(std::ceil(std::max(au, std::max(bu, cu)) + 1.0e-6f)));
        const int v1 = std::min(
            resolution,
            static_cast<int>(std::ceil(std::max(av, std::max(bv, cv)) + 1.0e-6f)));
        const float denominator =
            (bu - au) * (cv - av) - (bv - av) * (cu - au);
        if (u0 > u1 || v0 > v1 || std::abs(denominator) <= 1.0e-20f)
            return;

        for (int vInteger = v0; vInteger <= v1; ++vInteger) {
            for (int uInteger = u0; uInteger <= u1; ++uInteger) {
                const float u = static_cast<float>(uInteger);
                const float v = static_cast<float>(vInteger);
                const float bary0 =
                    ((bu - u) * (cv - v) - (bv - v) * (cu - u)) /
                    denominator;
                const float bary1 =
                    ((cu - u) * (av - v) - (cv - v) * (au - u)) /
                    denominator;
                const float bary2 = 1.0f - bary0 - bary1;
                if (bary0 < -1.0e-5f || bary1 < -1.0e-5f ||
                    bary2 < -1.0e-5f || bary0 > 1.00001f ||
                    bary1 > 1.00001f || bary2 > 1.00001f) {
                    continue;
                }
                float coordinate =
                    points[0][axis] -
                    (normal[uAxis] * (u - points[0][uAxis]) +
                     normal[vAxis] * (v - points[0][vAxis])) /
                        normalAxis;
                if (!std::isfinite(coordinate) ||
                    coordinate < -1.0e-4f ||
                    coordinate > resolution + 1.0e-4f) {
                    continue;
                }
                coordinate = std::max(
                    0.0f, std::min(float(resolution), coordinate));
                const int nearest = std::max(
                    0,
                    std::min(
                        resolution,
                        static_cast<int>(std::round(coordinate))));
                int starts[2];
                int startCount = 0;
                if (std::abs(coordinate - nearest) > 1.0e-4f) {
                    starts[startCount++] = std::max(
                        0,
                        std::min(
                            resolution - 1,
                            static_cast<int>(std::floor(coordinate))));
                }
                else {
                    if (nearest > 0)
                        starts[startCount++] = nearest - 1;
                    if (nearest < resolution)
                        starts[startCount++] = nearest;
                }
                for (int which = 0; which < startCount; ++which) {
                    int coordinates[3];
                    coordinates[axis] = starts[which];
                    coordinates[uAxis] = uInteger;
                    coordinates[vAxis] = vInteger;
                    barrier->setAtomic(
                        coordinates[0], coordinates[1], coordinates[2]);
                    ++coordinates[axis];
                    barrier->setAtomic(
                        coordinates[0], coordinates[1], coordinates[2]);
                }
            }
        }
    });
}

uint32_t loadMorphWord(
    const PackedGrid& input,
    int first,
    int second,
    int wordIndex,
    bool dilate)
{
    if (first < 0 || first >= input.dimension() ||
        second < 0 || second >= input.dimension() ||
        wordIndex < 0 || wordIndex >= input.rowWords()) {
        return dilate ? 0u : 0xffffffffu;
    }
    const uint32_t value = input.word(first, second, wordIndex);
    return dilate ? value : ~value;
}

uint32_t loadMorphShift(
    const PackedGrid& input,
    int first,
    int second,
    int outputWord,
    int shift,
    bool dilate)
{
    if (shift == 0) {
        return loadMorphWord(
            input, first, second, outputWord, dilate);
    }
    const int magnitude = std::abs(shift);
    const int wholeWords = magnitude >> 5;
    const int bits = magnitude & 31;
    if (shift > 0) {
        const int base = outputWord + wholeWords;
        const uint32_t low =
            loadMorphWord(input, first, second, base, dilate);
        if (bits == 0)
            return low;
        const uint32_t high =
            loadMorphWord(input, first, second, base + 1, dilate);
        return (low >> bits) | (high << (32 - bits));
    }
    const int base = outputWord - wholeWords;
    const uint32_t high =
        loadMorphWord(input, first, second, base, dilate);
    if (bits == 0)
        return high;
    const uint32_t low =
        loadMorphWord(input, first, second, base - 1, dilate);
    return (high << bits) | (low >> (32 - bits));
}

void morphologyAxis(
    const PackedGrid& input,
    PackedGrid* output,
    int axis,
    int step,
    bool dilate)
{
    const int dimension = input.dimension();
    const int rowWords = input.rowWords();
    parallelFor(input.wordCount(), 2048, [&](std::size_t index) {
        const int wordIndex = static_cast<int>(index % rowWords);
        const int second =
            static_cast<int>((index / rowWords) % dimension);
        const int first = static_cast<int>(
            index / (static_cast<std::size_t>(dimension) * rowWords));
        uint32_t expanded =
            loadMorphWord(input, first, second, wordIndex, dilate);
        if (axis == 0) {
            expanded |= loadMorphWord(
                input, first - step, second, wordIndex, dilate);
            expanded |= loadMorphWord(
                input, first + step, second, wordIndex, dilate);
        }
        else if (axis == 1) {
            expanded |= loadMorphWord(
                input, first, second - step, wordIndex, dilate);
            expanded |= loadMorphWord(
                input, first, second + step, wordIndex, dilate);
        }
        else {
            expanded |= loadMorphShift(
                input, first, second, wordIndex, -step, dilate);
            expanded |= loadMorphShift(
                input, first, second, wordIndex, step, dilate);
        }
        const uint32_t result = dilate ? expanded : ~expanded;
        output->word(index) = result & input.validMask(wordIndex);
    });
}

PackedGrid close26(const PackedGrid& input, int radius)
{
    if (radius == 0)
        return input;
    std::vector<int> steps;
    int reach = 0;
    while (reach < radius) {
        const int step = std::min(2 * reach + 1, radius - reach);
        steps.push_back(step);
        reach += step;
    }

    PackedGrid current = input;
    PackedGrid next(input.dimension());
    for (int axis = 0; axis < 3; ++axis) {
        for (int step : steps) {
            morphologyAxis(current, &next, axis, step, true);
            std::swap(current, next);
        }
    }
    for (int axis = 0; axis < 3; ++axis) {
        for (int step : steps) {
            morphologyAxis(current, &next, axis, step, false);
            std::swap(current, next);
        }
    }
    return current;
}

uint32_t propagateLowToHigh(uint32_t value, uint32_t freeMask)
{
    uint32_t span = freeMask;
    for (int shift = 1; shift < 32; shift <<= 1) {
        value |= (value << shift) & span;
        span &= span << shift;
    }
    return value & freeMask;
}

uint32_t propagateHighToLow(uint32_t value, uint32_t freeMask)
{
    uint32_t span = freeMask;
    for (int shift = 1; shift < 32; shift <<= 1) {
        value |= (value >> shift) & span;
        span &= span >> shift;
    }
    return value & freeMask;
}

PackedGrid exteriorFill(const PackedGrid& barrier)
{
    const int dimension = barrier.dimension();
    const int rowWords = barrier.rowWords();
    PackedGrid outside(dimension);
    parallelFor(barrier.wordCount(), 2048, [&](std::size_t index) {
        const int wordIndex = static_cast<int>(index % rowWords);
        const int second =
            static_cast<int>((index / rowWords) % dimension);
        const int first = static_cast<int>(
            index / (static_cast<std::size_t>(dimension) * rowWords));
        const uint32_t valid = barrier.validMask(wordIndex);
        uint32_t boundary = 0u;
        if (first == 0 || first + 1 == dimension ||
            second == 0 || second + 1 == dimension) {
            boundary = valid;
        }
        else {
            if (wordIndex == 0)
                boundary |= 1u;
            const int last = dimension - 1;
            if (wordIndex == (last >> 5))
                boundary |= uint32_t(1) << (last & 31);
        }
        outside.word(index) = boundary & ~barrier.word(index) & valid;
    });

    const std::size_t transverseLineCount =
        static_cast<std::size_t>(dimension) * rowWords;
    const std::size_t rowCount =
        static_cast<std::size_t>(dimension) * dimension;
    for (int sweep = 0; sweep < kMaximumCclSweeps; ++sweep) {
        std::atomic<bool> changed(false);
        for (int axis = 0; axis < 2; ++axis) {
            parallelFor(
                transverseLineCount,
                64,
                [&](std::size_t line) {
                    const int fixed = static_cast<int>(line / rowWords);
                    const int wordIndex = static_cast<int>(line % rowWords);
                    const uint32_t valid = barrier.validMask(wordIndex);
                    uint32_t reached = 0u;
                    for (int position = 0; position < dimension; ++position) {
                        const int first = axis == 0 ? position : fixed;
                        const int second = axis == 0 ? fixed : position;
                        const std::size_t index =
                            (static_cast<std::size_t>(first) * dimension + second) *
                                rowWords +
                            wordIndex;
                        const uint32_t freeMask =
                            ~barrier.word(index) & valid;
                        const uint32_t old = outside.word(index);
                        reached = (reached | old) & freeMask;
                        if ((reached & ~old) != 0u)
                            changed.store(true, std::memory_order_relaxed);
                        outside.word(index) = old | reached;
                    }
                    reached = 0u;
                    for (int position = dimension - 1; position >= 0; --position) {
                        const int first = axis == 0 ? position : fixed;
                        const int second = axis == 0 ? fixed : position;
                        const std::size_t index =
                            (static_cast<std::size_t>(first) * dimension + second) *
                                rowWords +
                            wordIndex;
                        const uint32_t freeMask =
                            ~barrier.word(index) & valid;
                        const uint32_t old = outside.word(index);
                        reached = (reached | old) & freeMask;
                        if ((reached & ~old) != 0u)
                            changed.store(true, std::memory_order_relaxed);
                        outside.word(index) = old | reached;
                    }
                });
        }
        parallelFor(rowCount, 64, [&](std::size_t row) {
            const std::size_t base = row * rowWords;
            bool carry = false;
            for (int wordIndex = 0; wordIndex < rowWords; ++wordIndex) {
                const std::size_t index = base + wordIndex;
                const uint32_t valid = barrier.validMask(wordIndex);
                const uint32_t freeMask =
                    ~barrier.word(index) & valid;
                const uint32_t old = outside.word(index);
                uint32_t value = old & freeMask;
                if (carry && (freeMask & 1u))
                    value |= 1u;
                value = propagateLowToHigh(value, freeMask);
                if ((value & ~old) != 0u)
                    changed.store(true, std::memory_order_relaxed);
                outside.word(index) = old | value;
                carry = (value & 0x80000000u) != 0u;
            }
            carry = false;
            for (int wordIndex = rowWords - 1; wordIndex >= 0; --wordIndex) {
                const std::size_t index = base + wordIndex;
                const uint32_t valid = barrier.validMask(wordIndex);
                const uint32_t freeMask =
                    ~barrier.word(index) & valid;
                const uint32_t old = outside.word(index);
                uint32_t value = old & freeMask;
                if (carry && (freeMask & 0x80000000u))
                    value |= 0x80000000u;
                value = propagateHighToLow(value, freeMask);
                if ((value & ~old) != 0u)
                    changed.store(true, std::memory_order_relaxed);
                outside.word(index) = old | value;
                carry = (value & 1u) != 0u;
            }
        });
        if (!changed.load(std::memory_order_relaxed))
            return outside;
    }
    throw std::runtime_error(
        "Exterior connected-component fill did not converge.");
}

void initializeBand(
    const PackedGrid& core,
    PackedGrid* closedBarrier)
{
    parallelFor(core.wordCount(), 2048, [&](std::size_t index) {
        const int wordIndex =
            static_cast<int>(index % core.rowWords());
        closedBarrier->word(index) =
            closedBarrier->word(index) &
            ~core.word(index) &
            core.validMask(wordIndex);
    });
}

uint32_t labelWord(
    const PackedGrid& core,
    const PackedGrid& outside,
    const PackedGrid& unresolved,
    int first,
    int second,
    int wordIndex,
    bool inside)
{
    if (first < 0 || first >= core.dimension() ||
        second < 0 || second >= core.dimension() ||
        wordIndex < 0 || wordIndex >= core.rowWords()) {
        return 0u;
    }
    const std::size_t index =
        (static_cast<std::size_t>(first) * core.dimension() + second) *
            core.rowWords() +
        wordIndex;
    if (!inside)
        return outside.word(index);
    return ~(core.word(index) |
             outside.word(index) |
             unresolved.word(index)) &
           core.validMask(wordIndex);
}

void growBand(
    const PackedGrid& core,
    PackedGrid* outside,
    PackedGrid* unresolved,
    int iterations)
{
    PackedGrid growth(core.dimension());
    const int dimension = core.dimension();
    const int rowWords = core.rowWords();
    for (int iteration = 0; iteration < iterations; ++iteration) {
        for (int pass = 0; pass < 2; ++pass) {
            const bool inside = pass == 0;
            parallelFor(core.wordCount(), 2048, [&](std::size_t index) {
                const int wordIndex = static_cast<int>(index % rowWords);
                const int second =
                    static_cast<int>((index / rowWords) % dimension);
                const int first = static_cast<int>(
                    index /
                    (static_cast<std::size_t>(dimension) * rowWords));
                const uint32_t center = labelWord(
                    core,
                    *outside,
                    *unresolved,
                    first,
                    second,
                    wordIndex,
                    inside);
                const uint32_t previous = labelWord(
                    core,
                    *outside,
                    *unresolved,
                    first,
                    second,
                    wordIndex - 1,
                    inside);
                const uint32_t next = labelWord(
                    core,
                    *outside,
                    *unresolved,
                    first,
                    second,
                    wordIndex + 1,
                    inside);
                uint32_t neighbors =
                    (center << 1) | (previous >> 31) |
                    (center >> 1) | (next << 31);
                neighbors |= labelWord(
                    core,
                    *outside,
                    *unresolved,
                    first - 1,
                    second,
                    wordIndex,
                    inside);
                neighbors |= labelWord(
                    core,
                    *outside,
                    *unresolved,
                    first + 1,
                    second,
                    wordIndex,
                    inside);
                neighbors |= labelWord(
                    core,
                    *outside,
                    *unresolved,
                    first,
                    second - 1,
                    wordIndex,
                    inside);
                neighbors |= labelWord(
                    core,
                    *outside,
                    *unresolved,
                    first,
                    second + 1,
                    wordIndex,
                    inside);
                growth.word(index) =
                    unresolved->word(index) & neighbors;
            });
            parallelFor(core.wordCount(), 2048, [&](std::size_t index) {
                const uint32_t added = growth.word(index);
                if (!inside)
                    outside->word(index) |= added;
                unresolved->word(index) &= ~added;
            });
        }
    }
}

struct RunTable
{
    std::vector<uint64_t> rowOffsets;
    std::vector<uint16_t> starts;
    std::vector<uint16_t> ends;
    std::vector<int32_t> parents;
    int dimension = 0;
};

uint32_t runMaskWord(
    const PackedGrid& first,
    const PackedGrid* second,
    std::size_t index,
    bool complement)
{
    const int wordIndex =
        static_cast<int>(index % first.rowWords());
    uint32_t bits = first.word(index);
    if (second != nullptr)
        bits |= second->word(index);
    if (complement)
        bits = ~bits;
    return bits & first.validMask(wordIndex);
}

int32_t findRoot(const std::vector<int32_t>& parents, int32_t node)
{
    while (parents[static_cast<std::size_t>(node)] != node)
        node = parents[static_cast<std::size_t>(node)];
    return node;
}

void uniteRoots(std::vector<int32_t>* parents, int32_t left, int32_t right)
{
    left = findRoot(*parents, left);
    right = findRoot(*parents, right);
    if (left == right)
        return;
    if (left > right)
        std::swap(left, right);
    (*parents)[static_cast<std::size_t>(right)] = left;
}

void mergeRunRows(
    const RunTable& runs,
    std::vector<int32_t>* parents,
    uint64_t leftRow,
    uint64_t rightRow)
{
    uint64_t left = runs.rowOffsets[static_cast<std::size_t>(leftRow)];
    uint64_t right = runs.rowOffsets[static_cast<std::size_t>(rightRow)];
    const uint64_t leftEnd =
        runs.rowOffsets[static_cast<std::size_t>(leftRow + 1)];
    const uint64_t rightEnd =
        runs.rowOffsets[static_cast<std::size_t>(rightRow + 1)];
    while (left < leftEnd && right < rightEnd) {
        if (runs.ends[static_cast<std::size_t>(left)] <
            runs.starts[static_cast<std::size_t>(right)]) {
            ++left;
        }
        else if (runs.ends[static_cast<std::size_t>(right)] <
                 runs.starts[static_cast<std::size_t>(left)]) {
            ++right;
        }
        else {
            uniteRoots(
                parents,
                static_cast<int32_t>(left),
                static_cast<int32_t>(right));
            if (runs.ends[static_cast<std::size_t>(left)] <=
                runs.ends[static_cast<std::size_t>(right)]) {
                ++left;
            }
            else {
                ++right;
            }
        }
    }
}

RunTable buildRuns(
    const PackedGrid& first,
    const PackedGrid* second,
    bool complement)
{
    RunTable runs;
    runs.dimension = first.dimension();
    const int dimension = first.dimension();
    const int rowWords = first.rowWords();
    const std::size_t rowCount =
        static_cast<std::size_t>(dimension) * dimension;
    std::vector<uint32_t> rowCounts(rowCount, 0u);
    parallelFor(rowCount, 256, [&](std::size_t row) {
        const std::size_t base = row * rowWords;
        uint32_t previous = 0u;
        uint32_t count = 0u;
        for (int wordIndex = 0; wordIndex < rowWords; ++wordIndex) {
            const uint32_t bits = runMaskWord(
                first, second, base + wordIndex, complement);
            const uint32_t starts =
                bits & ~((bits << 1) | (previous >> 31));
            count += static_cast<uint32_t>(__builtin_popcount(starts));
            previous = bits;
        }
        rowCounts[row] = count;
    });

    runs.rowOffsets.resize(rowCount + 1, 0u);
    for (std::size_t row = 0; row < rowCount; ++row)
        runs.rowOffsets[row + 1] = runs.rowOffsets[row] + rowCounts[row];
    const uint64_t runCount = runs.rowOffsets.back();
    if (runCount >
        static_cast<uint64_t>(std::numeric_limits<int32_t>::max())) {
        throw std::runtime_error("RLE component count exceeds the int32 limit.");
    }
    runs.starts.resize(static_cast<std::size_t>(runCount));
    runs.ends.resize(static_cast<std::size_t>(runCount));
    runs.parents.resize(static_cast<std::size_t>(runCount));

    parallelFor(rowCount, 256, [&](std::size_t row) {
        uint64_t startOutput = runs.rowOffsets[row];
        uint64_t endOutput = startOutput;
        const std::size_t base = row * rowWords;
        for (int wordIndex = 0; wordIndex < rowWords; ++wordIndex) {
            const uint32_t bits = runMaskWord(
                first, second, base + wordIndex, complement);
            const uint32_t previous = wordIndex > 0
                ? runMaskWord(
                      first, second, base + wordIndex - 1, complement)
                : 0u;
            const uint32_t next = wordIndex + 1 < rowWords
                ? runMaskWord(
                      first, second, base + wordIndex + 1, complement)
                : 0u;
            uint32_t starts =
                bits & ~((bits << 1) | (previous >> 31));
            uint32_t ends =
                bits & ~((bits >> 1) | (next << 31));
            while (starts != 0u) {
                const int bit = __builtin_ctz(starts);
                const std::size_t run =
                    static_cast<std::size_t>(startOutput++);
                runs.starts[run] =
                    static_cast<uint16_t>(wordIndex * 32 + bit);
                runs.parents[run] = static_cast<int32_t>(run);
                starts &= starts - 1u;
            }
            while (ends != 0u) {
                const int bit = __builtin_ctz(ends);
                runs.ends[static_cast<std::size_t>(endOutput++)] =
                    static_cast<uint16_t>(wordIndex * 32 + bit);
                ends &= ends - 1u;
            }
        }
    });

    for (uint64_t row = 0; row < rowCount; ++row) {
        const int secondCoordinate = static_cast<int>(row % dimension);
        if (secondCoordinate > 0)
            mergeRunRows(runs, &runs.parents, row, row - 1);
        if (row >= static_cast<uint64_t>(dimension))
            mergeRunRows(runs, &runs.parents, row, row - dimension);
    }
    for (std::size_t index = 0; index < runs.parents.size(); ++index)
        runs.parents[index] =
            findRoot(runs.parents, static_cast<int32_t>(index));
    return runs;
}

uint32_t intervalMask(int wordIndex, int start, int end)
{
    const int firstBit = std::max(0, start - wordIndex * 32);
    const int lastBit = std::min(31, end - wordIndex * 32);
    if (firstBit > lastBit)
        return 0u;
    const uint32_t lower = 0xffffffffu << firstBit;
    const uint32_t upper = lastBit == 31
        ? 0xffffffffu
        : ((uint32_t(1) << (lastBit + 1)) - 1u);
    return lower & upper;
}

int popcountRange(
    const PackedGrid& grid,
    uint64_t row,
    int start,
    int end)
{
    if (row >=
        static_cast<uint64_t>(grid.dimension()) * grid.dimension()) {
        return 0;
    }
    int count = 0;
    const int firstWord = start >> 5;
    const int lastWord = end >> 5;
    const std::size_t base =
        static_cast<std::size_t>(row) * grid.rowWords();
    for (int wordIndex = firstWord;
         wordIndex <= lastWord;
         ++wordIndex) {
        const uint32_t mask =
            intervalMask(wordIndex, start, end) &
            grid.validMask(wordIndex);
        count += __builtin_popcount(grid.word(base + wordIndex) & mask);
    }
    return count;
}

int popcountImplicitInsideRange(
    const PackedGrid& core,
    const PackedGrid& outside,
    const PackedGrid& unresolved,
    uint64_t row,
    int start,
    int end)
{
    if (row >=
        static_cast<uint64_t>(core.dimension()) * core.dimension()) {
        return 0;
    }
    int count = 0;
    const int firstWord = start >> 5;
    const int lastWord = end >> 5;
    const std::size_t base =
        static_cast<std::size_t>(row) * core.rowWords();
    for (int wordIndex = firstWord;
         wordIndex <= lastWord;
         ++wordIndex) {
        const uint32_t mask =
            intervalMask(wordIndex, start, end) &
            core.validMask(wordIndex);
        const uint32_t inside =
            ~(core.word(base + wordIndex) |
              outside.word(base + wordIndex) |
              unresolved.word(base + wordIndex));
        count += __builtin_popcount(inside & mask);
    }
    return count;
}

void setRange(
    PackedGrid* grid,
    uint64_t row,
    int start,
    int end)
{
    const int firstWord = start >> 5;
    const int lastWord = end >> 5;
    const std::size_t base =
        static_cast<std::size_t>(row) * grid->rowWords();
    for (int wordIndex = firstWord;
         wordIndex <= lastWord;
         ++wordIndex) {
        grid->word(base + wordIndex) |=
            intervalMask(wordIndex, start, end);
    }
}

void clearRange(
    PackedGrid* grid,
    uint64_t row,
    int start,
    int end)
{
    const int firstWord = start >> 5;
    const int lastWord = end >> 5;
    const std::size_t base =
        static_cast<std::size_t>(row) * grid->rowWords();
    for (int wordIndex = firstWord;
         wordIndex <= lastWord;
         ++wordIndex) {
        grid->word(base + wordIndex) &=
            ~intervalMask(wordIndex, start, end);
    }
}

int unresolvedPointScore(
    const PackedGrid& core,
    const PackedGrid& outside,
    const PackedGrid& unresolved,
    int first,
    int second,
    int third)
{
    if (first < 0 || first >= core.dimension() ||
        second < 0 || second >= core.dimension() ||
        third < 0 || third >= core.dimension()) {
        return 0;
    }
    if (outside.get(first, second, third))
        return 1;
    if (!core.get(first, second, third) &&
        !unresolved.get(first, second, third)) {
        return -1;
    }
    return 0;
}

void assignUnresolved(
    const PackedGrid& core,
    PackedGrid* outside,
    PackedGrid* unresolved)
{
    const RunTable runs = buildRuns(*unresolved, nullptr, false);
    if (runs.parents.empty())
        return;
    std::vector<int64_t> scores(runs.parents.size(), 0);
    const int dimension = runs.dimension;
    for (uint64_t row = 0;
         row + 1 < runs.rowOffsets.size();
         ++row) {
        const int first = static_cast<int>(row / dimension);
        const int second = static_cast<int>(row % dimension);
        for (uint64_t run = runs.rowOffsets[static_cast<std::size_t>(row)];
             run < runs.rowOffsets[static_cast<std::size_t>(row + 1)];
             ++run) {
            const int start = runs.starts[static_cast<std::size_t>(run)];
            const int end = runs.ends[static_cast<std::size_t>(run)];
            int64_t score = 0;
            score += unresolvedPointScore(
                core, *outside, *unresolved, first, second, start - 1);
            score += unresolvedPointScore(
                core, *outside, *unresolved, first, second, end + 1);
            const uint64_t neighborRows[4] = {
                second > 0 ? row - 1 : std::numeric_limits<uint64_t>::max(),
                second + 1 < dimension
                    ? row + 1
                    : std::numeric_limits<uint64_t>::max(),
                first > 0
                    ? row - dimension
                    : std::numeric_limits<uint64_t>::max(),
                first + 1 < dimension
                    ? row + dimension
                    : std::numeric_limits<uint64_t>::max(),
            };
            for (uint64_t neighbor : neighborRows) {
                score += popcountRange(
                    *outside, neighbor, start, end);
                score -= popcountImplicitInsideRange(
                    core,
                    *outside,
                    *unresolved,
                    neighbor,
                    start,
                    end);
            }
            scores[static_cast<std::size_t>(
                runs.parents[static_cast<std::size_t>(run)])] += score;
        }
    }

    for (uint64_t row = 0;
         row + 1 < runs.rowOffsets.size();
         ++row) {
        for (uint64_t run = runs.rowOffsets[static_cast<std::size_t>(row)];
             run < runs.rowOffsets[static_cast<std::size_t>(row + 1)];
             ++run) {
            const int start = runs.starts[static_cast<std::size_t>(run)];
            const int end = runs.ends[static_cast<std::size_t>(run)];
            if (scores[static_cast<std::size_t>(
                    runs.parents[static_cast<std::size_t>(run)])] > 0) {
                setRange(outside, row, start, end);
            }
            clearRange(unresolved, row, start, end);
        }
    }
}

int insideFilterPointScore(
    const PackedGrid& core,
    const PackedGrid& outside,
    int first,
    int second,
    int third)
{
    if (first < 0 || first >= core.dimension() ||
        second < 0 || second >= core.dimension() ||
        third < 0 || third >= core.dimension()) {
        return 0;
    }
    if (outside.get(first, second, third))
        return 1;
    return core.get(first, second, third) ? -1 : 0;
}

void filterInside(
    const PackedGrid& core,
    PackedGrid* outside)
{
    const RunTable runs = buildRuns(*outside, &core, true);
    if (runs.parents.empty())
        return;
    std::vector<int64_t> scores(runs.parents.size(), 0);
    const int dimension = runs.dimension;
    for (uint64_t row = 0;
         row + 1 < runs.rowOffsets.size();
         ++row) {
        const int first = static_cast<int>(row / dimension);
        const int second = static_cast<int>(row % dimension);
        for (uint64_t run = runs.rowOffsets[static_cast<std::size_t>(row)];
             run < runs.rowOffsets[static_cast<std::size_t>(row + 1)];
             ++run) {
            const int start = runs.starts[static_cast<std::size_t>(run)];
            const int end = runs.ends[static_cast<std::size_t>(run)];
            int64_t score = 0;
            score += insideFilterPointScore(
                core, *outside, first, second, start - 1);
            score += insideFilterPointScore(
                core, *outside, first, second, end + 1);
            const uint64_t neighborRows[4] = {
                second > 0 ? row - 1 : std::numeric_limits<uint64_t>::max(),
                second + 1 < dimension
                    ? row + 1
                    : std::numeric_limits<uint64_t>::max(),
                first > 0
                    ? row - dimension
                    : std::numeric_limits<uint64_t>::max(),
                first + 1 < dimension
                    ? row + dimension
                    : std::numeric_limits<uint64_t>::max(),
            };
            for (uint64_t neighbor : neighborRows) {
                score += popcountRange(
                    *outside, neighbor, start, end);
                score -= popcountRange(
                    core, neighbor, start, end);
            }
            scores[static_cast<std::size_t>(
                runs.parents[static_cast<std::size_t>(run)])] += score;
        }
    }

    for (uint64_t row = 0;
         row + 1 < runs.rowOffsets.size();
         ++row) {
        for (uint64_t run = runs.rowOffsets[static_cast<std::size_t>(row)];
             run < runs.rowOffsets[static_cast<std::size_t>(row + 1)];
             ++run) {
            if (scores[static_cast<std::size_t>(
                    runs.parents[static_cast<std::size_t>(run)])] > 0) {
                setRange(
                    outside,
                    row,
                    runs.starts[static_cast<std::size_t>(run)],
                    runs.ends[static_cast<std::size_t>(run)]);
            }
        }
    }
}

void classifyCore(
    const PackedGrid& core,
    PackedGrid* outside)
{
    PackedGrid coreOutside(core.dimension());
    const int dimension = core.dimension();
    const int rowWords = core.rowWords();
    parallelFor(core.wordCount(), 1024, [&](std::size_t index) {
        const int wordIndex = static_cast<int>(index % rowWords);
        const int second =
            static_cast<int>((index / rowWords) % dimension);
        const int first = static_cast<int>(
            index / (static_cast<std::size_t>(dimension) * rowWords));
        uint32_t pending = core.word(index) & core.validMask(wordIndex);
        uint32_t result = 0u;
        while (pending != 0u) {
            const int bit = __builtin_ctz(pending);
            const int third = wordIndex * 32 + bit;
            int insideCount = 0;
            int outsideCount = 0;
            const int neighbors[6][3] = {
                {first - 1, second, third},
                {first + 1, second, third},
                {first, second - 1, third},
                {first, second + 1, third},
                {first, second, third - 1},
                {first, second, third + 1},
            };
            for (const auto& neighbor : neighbors) {
                if (neighbor[0] < 0 || neighbor[0] >= dimension ||
                    neighbor[1] < 0 || neighbor[1] >= dimension ||
                    neighbor[2] < 0 || neighbor[2] >= dimension ||
                    core.get(neighbor[0], neighbor[1], neighbor[2])) {
                    continue;
                }
                if (outside->get(
                        neighbor[0], neighbor[1], neighbor[2])) {
                    ++outsideCount;
                }
                else {
                    ++insideCount;
                }
            }
            if (outsideCount > insideCount)
                result |= uint32_t(1) << bit;
            pending &= pending - 1u;
        }
        coreOutside.word(index) = result;
    });
    parallelFor(core.wordCount(), 2048, [&](std::size_t index) {
        outside->word(index) |= coreOutside.word(index);
    });
}

void addVertexSeeds(
    const SourceMesh& source,
    int resolution,
    PackedGrid* surface)
{
    parallelFor(source.vertices.size(), 256, [&](std::size_t index) {
        const Vec3f& point = source.vertices[index];
        surface->setAtomic(
            std::max(
                0,
                std::min(
                    resolution,
                    static_cast<int>(std::round(gridIndex(point.x, resolution))))),
            std::max(
                0,
                std::min(
                    resolution,
                    static_cast<int>(std::round(gridIndex(point.y, resolution))))),
            std::max(
                0,
                std::min(
                    resolution,
                    static_cast<int>(std::round(gridIndex(point.z, resolution))))));
    });
}

void addEdgeSeeds(
    const SourceMesh& source,
    int resolution,
    PackedGrid* surface)
{
    const std::size_t edgeCount = source.faces.size() * 3;
    parallelFor(edgeCount, 64, [&](std::size_t edgeIndex) {
        const int edge = static_cast<int>(edgeIndex % 3);
        const std::array<int, 3>& face = source.faces[edgeIndex / 3];
        const Vec3f& first =
            source.vertices[static_cast<std::size_t>(face[edge])];
        const Vec3f& second =
            source.vertices[static_cast<std::size_t>(face[(edge + 1) % 3])];
        Vec3f start{
            gridIndex(first.x, resolution),
            gridIndex(first.y, resolution),
            gridIndex(first.z, resolution)};
        Vec3f delta{
            gridIndex(second.x, resolution) - start.x,
            gridIndex(second.y, resolution) - start.y,
            gridIndex(second.z, resolution) - start.z};
        const float maximumDelta = std::max(
            std::abs(delta.x),
            std::max(std::abs(delta.y), std::abs(delta.z)));
        const int steps =
            static_cast<int>(std::ceil(2.0f * maximumDelta));
        for (int sample = 0; sample <= steps; ++sample) {
            const float t =
                steps == 0 ? 0.0f : float(sample) / steps;
            surface->setAtomic(
                std::max(
                    0,
                    std::min(
                        resolution,
                        static_cast<int>(std::round(start.x + t * delta.x)))),
                std::max(
                    0,
                    std::min(
                        resolution,
                        static_cast<int>(std::round(start.y + t * delta.y)))),
                std::max(
                    0,
                    std::min(
                        resolution,
                        static_cast<int>(std::round(start.z + t * delta.z)))));
        }
    });
}

void addTriangleSeeds(
    const SourceMesh& source,
    int resolution,
    PackedGrid* surface)
{
    parallelFor(source.faces.size(), 32, [&](std::size_t faceIndex) {
        const std::array<int, 3>& face = source.faces[faceIndex];
        Vec3f points[3];
        for (int corner = 0; corner < 3; ++corner) {
            const Vec3f& point =
                source.vertices[static_cast<std::size_t>(face[corner])];
            points[corner] = {
                gridIndex(point.x, resolution),
                gridIndex(point.y, resolution),
                gridIndex(point.z, resolution)};
        }
        float edgeLengthSquared[3] = {0.0f, 0.0f, 0.0f};
        for (int edge = 0; edge < 3; ++edge) {
            edgeLengthSquared[edge] =
                squaredLength(points[(edge + 1) % 3] - points[edge]);
        }
        int baseStart = 0;
        if (edgeLengthSquared[1] > edgeLengthSquared[baseStart])
            baseStart = 1;
        if (edgeLengthSquared[2] > edgeLengthSquared[baseStart])
            baseStart = 2;
        const int baseEnd = (baseStart + 1) % 3;
        const int apex = (baseStart + 2) % 3;
        const Vec3f base = points[baseEnd] - points[baseStart];
        const Vec3f toApex = points[apex] - points[baseStart];
        const float baseLengthSquared = squaredLength(base);
        if (baseLengthSquared <= 0.0f)
            return;
        const float projection = dot(base, toApex);
        const float heightSquared = std::max(
            0.0f,
            squaredLength(toApex) -
                projection * projection / baseLengthSquared);
        const int rowSteps =
            static_cast<int>(std::ceil(0.5f * std::sqrt(heightSquared)));
        for (int row = 0; row <= rowSteps; ++row) {
            const float t =
                rowSteps == 0 ? 0.0f : float(row) / rowSteps;
            const Vec3f start =
                points[apex] + (points[baseStart] - points[apex]) * t;
            const Vec3f end =
                points[apex] + (points[baseEnd] - points[apex]) * t;
            const Vec3f delta = end - start;
            const float maximumDelta = std::max(
                std::abs(delta.x),
                std::max(std::abs(delta.y), std::abs(delta.z)));
            const int steps =
                static_cast<int>(std::ceil(0.5f * maximumDelta));
            for (int sample = 0; sample <= steps; ++sample) {
                const float u =
                    steps == 0 ? 0.0f : float(sample) / steps;
                surface->setAtomic(
                    std::max(
                        0,
                        std::min(
                            resolution,
                            static_cast<int>(std::round(start.x + u * delta.x)))),
                    std::max(
                        0,
                        std::min(
                            resolution,
                            static_cast<int>(std::round(start.y + u * delta.y)))),
                    std::max(
                        0,
                        std::min(
                            resolution,
                            static_cast<int>(std::round(start.z + u * delta.z)))));
            }
        }
    });
}

uint32_t packedWordOrZero(
    const uint32_t* row,
    int rowWords,
    int wordIndex)
{
    return wordIndex >= 0 && wordIndex < rowWords
        ? row[wordIndex]
        : 0u;
}

uint32_t shiftedPackedWord(
    const uint32_t* row,
    int rowWords,
    int outputWord,
    int sourceOffset)
{
    int sourceBit = outputWord * 32 + sourceOffset;
    int sourceWord = sourceBit / 32;
    int shift = sourceBit % 32;
    if (shift < 0) {
        shift += 32;
        --sourceWord;
    }
    uint32_t value =
        packedWordOrZero(row, rowWords, sourceWord) >> shift;
    if (shift != 0) {
        value |=
            packedWordOrZero(row, rowWords, sourceWord + 1) <<
            (32 - shift);
    }
    return value;
}

PackedGrid candidateCells(
    const PackedGrid& surface,
    int resolution,
    int radius)
{
    const int width = resolution + 1;
    const int sourceWords = surface.rowWords();
    const int cellWords = (resolution + 31) / 32;
    const std::size_t zCount =
        static_cast<std::size_t>(width) * width * cellWords;
    const std::size_t yCount =
        static_cast<std::size_t>(width) * resolution * cellWords;
    const std::size_t outputCount =
        static_cast<std::size_t>(resolution) * resolution * cellWords;
    std::vector<uint32_t> zPass(zCount);
    std::vector<uint32_t> yPass(yCount);
    PackedGrid output(resolution);
    const int validLastBits =
        resolution - (cellWords - 1) * 32;
    const uint32_t lastMask = validLastBits >= 32
        ? 0xffffffffu
        : ((uint32_t(1) << validLastBits) - 1u);

    parallelFor(zCount, 2048, [&](std::size_t index) {
        const int wordIndex = static_cast<int>(index % cellWords);
        const int second =
            static_cast<int>((index / cellWords) % width);
        const int first = static_cast<int>(
            index / (static_cast<std::size_t>(width) * cellWords));
        const uint32_t* row =
            surface.data() +
            (static_cast<std::size_t>(first) * width + second) *
                sourceWords;
        uint32_t bits = 0u;
        for (int offset = -radius; offset <= radius + 1; ++offset)
            bits |= shiftedPackedWord(row, sourceWords, wordIndex, offset);
        if (wordIndex == cellWords - 1)
            bits &= lastMask;
        zPass[index] = bits;
    });

    parallelFor(yCount, 2048, [&](std::size_t index) {
        const int wordIndex = static_cast<int>(index % cellWords);
        const int cellSecond =
            static_cast<int>((index / cellWords) % resolution);
        const int first = static_cast<int>(
            index /
            (static_cast<std::size_t>(resolution) * cellWords));
        uint32_t bits = 0u;
        for (int offset = -radius; offset <= radius + 1; ++offset) {
            const int sourceSecond = cellSecond + offset;
            if (sourceSecond >= 0 && sourceSecond < width) {
                bits |= zPass[
                    (static_cast<std::size_t>(first) * width + sourceSecond) *
                        cellWords +
                    wordIndex];
            }
        }
        yPass[index] = bits;
    });
    zPass.clear();
    zPass.shrink_to_fit();

    parallelFor(outputCount, 2048, [&](std::size_t index) {
        const int wordIndex = static_cast<int>(index % cellWords);
        const int second =
            static_cast<int>((index / cellWords) % resolution);
        const int first = static_cast<int>(
            index /
            (static_cast<std::size_t>(resolution) * cellWords));
        uint32_t bits = 0u;
        for (int offset = -radius; offset <= radius + 1; ++offset) {
            const int sourceFirst = first + offset;
            if (sourceFirst >= 0 && sourceFirst < width) {
                bits |= yPass[
                    (static_cast<std::size_t>(sourceFirst) * resolution +
                     second) *
                        cellWords +
                    wordIndex];
            }
        }
        output.word(index) = bits;
    });
    return output;
}

std::vector<uint64_t> compactBits(const PackedGrid& grid)
{
    uint64_t count = 0;
    for (std::size_t index = 0; index < grid.wordCount(); ++index)
        count += static_cast<uint64_t>(__builtin_popcount(grid.word(index)));
    std::vector<uint64_t> result;
    result.reserve(static_cast<std::size_t>(count));
    const int dimension = grid.dimension();
    const int rowWords = grid.rowWords();
    for (int first = 0; first < dimension; ++first) {
        for (int second = 0; second < dimension; ++second) {
            const std::size_t base =
                (static_cast<std::size_t>(first) * dimension + second) *
                rowWords;
            for (int wordIndex = 0; wordIndex < rowWords; ++wordIndex) {
                uint32_t bits = grid.word(base + wordIndex);
                while (bits != 0u) {
                    const int bit = __builtin_ctz(bits);
                    const int third = wordIndex * 32 + bit;
                    if (third < dimension) {
                        result.push_back(
                            (static_cast<uint64_t>(first) * dimension +
                             second) *
                                dimension +
                            third);
                    }
                    bits &= bits - 1u;
                }
            }
        }
    }
    return result;
}

PackedGrid candidatePointMask(
    const std::vector<uint64_t>& cells,
    int resolution)
{
    PackedGrid points(resolution + 1);
    const uint64_t plane =
        static_cast<uint64_t>(resolution) * resolution;
    parallelFor(cells.size(), 512, [&](std::size_t index) {
        const uint64_t cell = cells[index];
        const int first = static_cast<int>(cell / plane);
        const int second =
            static_cast<int>((cell / resolution) % resolution);
        const int third = static_cast<int>(cell % resolution);
        for (int firstOffset = 0; firstOffset <= 1; ++firstOffset) {
            for (int secondOffset = 0; secondOffset <= 1; ++secondOffset) {
                for (int thirdOffset = 0; thirdOffset <= 1; ++thirdOffset) {
                    points.setAtomic(
                        first + firstOffset,
                        second + secondOffset,
                        third + thirdOffset);
                }
            }
        }
    });
    return points;
}

struct Bounds
{
    Vec3f minimum{
        std::numeric_limits<float>::infinity(),
        std::numeric_limits<float>::infinity(),
        std::numeric_limits<float>::infinity()};
    Vec3f maximum{
        -std::numeric_limits<float>::infinity(),
        -std::numeric_limits<float>::infinity(),
        -std::numeric_limits<float>::infinity()};
};

void include(Bounds* bounds, const Vec3f& point)
{
    bounds->minimum.x = std::min(bounds->minimum.x, point.x);
    bounds->minimum.y = std::min(bounds->minimum.y, point.y);
    bounds->minimum.z = std::min(bounds->minimum.z, point.z);
    bounds->maximum.x = std::max(bounds->maximum.x, point.x);
    bounds->maximum.y = std::max(bounds->maximum.y, point.y);
    bounds->maximum.z = std::max(bounds->maximum.z, point.z);
}

void include(Bounds* bounds, const Bounds& other)
{
    include(bounds, other.minimum);
    include(bounds, other.maximum);
}

float pointBoundsSquaredDistance(
    const Vec3f& point,
    const Bounds& bounds)
{
    float distance = 0.0f;
    for (int axis = 0; axis < 3; ++axis) {
        if (point[axis] < bounds.minimum[axis]) {
            const float delta = bounds.minimum[axis] - point[axis];
            distance += delta * delta;
        }
        else if (point[axis] > bounds.maximum[axis]) {
            const float delta = point[axis] - bounds.maximum[axis];
            distance += delta * delta;
        }
    }
    return distance;
}

float pointTriangleSquaredDistance(
    const Vec3f& point,
    const Vec3f& first,
    const Vec3f& second,
    const Vec3f& third)
{
    const Vec3f ab = second - first;
    const Vec3f ac = third - first;
    const Vec3f ap = point - first;
    const float d1 = dot(ab, ap);
    const float d2 = dot(ac, ap);
    if (d1 <= 0.0f && d2 <= 0.0f)
        return squaredLength(ap);

    const Vec3f bp = point - second;
    const float d3 = dot(ab, bp);
    const float d4 = dot(ac, bp);
    if (d3 >= 0.0f && d4 <= d3)
        return squaredLength(bp);

    const float vc = d1 * d4 - d3 * d2;
    if (vc <= 0.0f && d1 >= 0.0f && d3 <= 0.0f) {
        const float v = d1 / (d1 - d3);
        return squaredLength(point - (first + ab * v));
    }

    const Vec3f cp = point - third;
    const float d5 = dot(ab, cp);
    const float d6 = dot(ac, cp);
    if (d6 >= 0.0f && d5 <= d6)
        return squaredLength(cp);

    const float vb = d5 * d2 - d1 * d6;
    if (vb <= 0.0f && d2 >= 0.0f && d6 <= 0.0f) {
        const float w = d2 / (d2 - d6);
        return squaredLength(point - (first + ac * w));
    }

    const float va = d3 * d6 - d5 * d4;
    if (va <= 0.0f &&
        d4 - d3 >= 0.0f &&
        d5 - d6 >= 0.0f) {
        const Vec3f edge = third - second;
        const float w = (d4 - d3) /
                        ((d4 - d3) + (d5 - d6));
        return squaredLength(point - (second + edge * w));
    }

    const Vec3f normal = cross(ab, ac);
    const float normalLengthSquared = squaredLength(normal);
    if (normalLengthSquared <= std::numeric_limits<float>::epsilon())
        return std::numeric_limits<float>::infinity();
    const float projection = dot(ap, normal);
    return projection * projection / normalLengthSquared;
}

class TriangleBvh
{
public:
    explicit TriangleBvh(const SourceMesh& source)
        : source_(source)
    {
        bounds_.resize(source.faces.size());
        centroids_.resize(source.faces.size());
        indexes_.resize(source.faces.size());
        for (std::size_t index = 0; index < source.faces.size(); ++index) {
            indexes_[index] = static_cast<int>(index);
            const std::array<int, 3>& face = source.faces[index];
            Bounds bounds;
            Vec3f centroid;
            for (int corner = 0; corner < 3; ++corner) {
                const Vec3f& point =
                    source.vertices[static_cast<std::size_t>(face[corner])];
                include(&bounds, point);
                centroid = centroid + point;
            }
            bounds_[index] = bounds;
            centroids_[index] = centroid * (1.0f / 3.0f);
        }
        nodes_.reserve(source.faces.size() * 2);
        root_ = build(0, static_cast<int>(indexes_.size()));
    }

    float closestSquared(const Vec3f& point, float maximumSquared) const
    {
        float best = maximumSquared;
        bool found = query(point, &best);
        if (!found && std::isfinite(maximumSquared)) {
            best = std::numeric_limits<float>::infinity();
            query(point, &best);
        }
        return best;
    }

private:
    struct Node
    {
        Bounds bounds;
        int begin = 0;
        int end = 0;
        int left = -1;
        int right = -1;
    };

    int build(int begin, int end)
    {
        Node node;
        node.begin = begin;
        node.end = end;
        Bounds centroidBounds;
        for (int index = begin; index < end; ++index) {
            const int face = indexes_[static_cast<std::size_t>(index)];
            include(&node.bounds, bounds_[static_cast<std::size_t>(face)]);
            include(&centroidBounds, centroids_[static_cast<std::size_t>(face)]);
        }
        const int nodeIndex = static_cast<int>(nodes_.size());
        nodes_.push_back(node);
        if (end - begin <= 8)
            return nodeIndex;

        int axis = 0;
        float extent =
            centroidBounds.maximum.x - centroidBounds.minimum.x;
        for (int candidate = 1; candidate < 3; ++candidate) {
            const float candidateExtent =
                centroidBounds.maximum[candidate] -
                centroidBounds.minimum[candidate];
            if (candidateExtent > extent) {
                axis = candidate;
                extent = candidateExtent;
            }
        }
        const int middle = begin + (end - begin) / 2;
        std::nth_element(
            indexes_.begin() + begin,
            indexes_.begin() + middle,
            indexes_.begin() + end,
            [&](int left, int right) {
                return centroids_[static_cast<std::size_t>(left)][axis] <
                       centroids_[static_cast<std::size_t>(right)][axis];
            });
        nodes_[static_cast<std::size_t>(nodeIndex)].left =
            build(begin, middle);
        nodes_[static_cast<std::size_t>(nodeIndex)].right =
            build(middle, end);
        return nodeIndex;
    }

    bool query(const Vec3f& point, float* best) const
    {
        bool found = false;
        int stack[64];
        int stackSize = 0;
        stack[stackSize++] = root_;
        while (stackSize > 0) {
            const int nodeIndex = stack[--stackSize];
            const Node& node = nodes_[static_cast<std::size_t>(nodeIndex)];
            if (pointBoundsSquaredDistance(point, node.bounds) > *best)
                continue;
            if (node.left < 0) {
                for (int index = node.begin; index < node.end; ++index) {
                    const int faceIndex =
                        indexes_[static_cast<std::size_t>(index)];
                    const std::array<int, 3>& face =
                        source_.faces[static_cast<std::size_t>(faceIndex)];
                    const float distance = pointTriangleSquaredDistance(
                        point,
                        source_.vertices[static_cast<std::size_t>(face[0])],
                        source_.vertices[static_cast<std::size_t>(face[1])],
                        source_.vertices[static_cast<std::size_t>(face[2])]);
                    if (distance < *best) {
                        *best = distance;
                        found = true;
                    }
                }
                continue;
            }
            const float leftDistance = pointBoundsSquaredDistance(
                point,
                nodes_[static_cast<std::size_t>(node.left)].bounds);
            const float rightDistance = pointBoundsSquaredDistance(
                point,
                nodes_[static_cast<std::size_t>(node.right)].bounds);
            if (leftDistance <= rightDistance) {
                if (rightDistance <= *best)
                    stack[stackSize++] = node.right;
                if (leftDistance <= *best)
                    stack[stackSize++] = node.left;
            }
            else {
                if (leftDistance <= *best)
                    stack[stackSize++] = node.left;
                if (rightDistance <= *best)
                    stack[stackSize++] = node.right;
            }
        }
        return found;
    }

    const SourceMesh& source_;
    std::vector<Bounds> bounds_;
    std::vector<Vec3f> centroids_;
    std::vector<int> indexes_;
    std::vector<Node> nodes_;
    int root_ = -1;
};

Vec3f pointForGridIndex(
    uint64_t index,
    int resolution)
{
    const int width = resolution + 1;
    const uint64_t plane = static_cast<uint64_t>(width) * width;
    const int first = static_cast<int>(index / plane);
    const int second = static_cast<int>((index / width) % width);
    const int third = static_cast<int>(index % width);
    const float spacing =
        (kDomainMax - kDomainMin) / resolution;
    return {
        kDomainMin + first * spacing,
        kDomainMin + second * spacing,
        kDomainMin + third * spacing};
}

class EdgeMap
{
public:
    explicit EdgeMap(std::size_t expected)
    {
        std::size_t capacity = 16;
        const std::size_t requested =
            expected > std::numeric_limits<std::size_t>::max() / 2
            ? expected
            : expected * 2;
        while (capacity < requested)
            capacity <<= 1;
        keys_.assign(capacity, emptyKey());
        values_.resize(capacity);
    }

    int findOrInsert(uint64_t key, int newValue, bool* inserted)
    {
        if ((size_ + 1) * 10 > keys_.size() * 7)
            grow();
        std::size_t slot =
            static_cast<std::size_t>(mix(key)) & (keys_.size() - 1);
        while (true) {
            if (keys_[slot] == key) {
                *inserted = false;
                return values_[slot];
            }
            if (keys_[slot] == emptyKey()) {
                keys_[slot] = key;
                values_[slot] = newValue;
                ++size_;
                *inserted = true;
                return newValue;
            }
            slot = (slot + 1) & (keys_.size() - 1);
        }
    }

private:
    static uint64_t emptyKey()
    {
        return std::numeric_limits<uint64_t>::max();
    }

    static uint64_t mix(uint64_t value)
    {
        value ^= value >> 30;
        value *= UINT64_C(0xbf58476d1ce4e5b9);
        value ^= value >> 27;
        value *= UINT64_C(0x94d049bb133111eb);
        value ^= value >> 31;
        return value;
    }

    void grow()
    {
        std::vector<uint64_t> oldKeys;
        std::vector<int> oldValues;
        oldKeys.swap(keys_);
        oldValues.swap(values_);
        keys_.assign(oldKeys.size() * 2, emptyKey());
        values_.resize(keys_.size());
        size_ = 0;
        for (std::size_t index = 0; index < oldKeys.size(); ++index) {
            if (oldKeys[index] == emptyKey())
                continue;
            bool inserted = false;
            findOrInsert(oldKeys[index], oldValues[index], &inserted);
        }
    }

    std::vector<uint64_t> keys_;
    std::vector<int> values_;
    std::size_t size_ = 0;
};

const int kCornerOffsets[8][3] = {
    {0, 0, 0},
    {1, 0, 0},
    {1, 1, 0},
    {0, 1, 0},
    {0, 0, 1},
    {1, 0, 1},
    {1, 1, 1},
    {0, 1, 1},
};

const int kEdgeCorners[12][2] = {
    {0, 1},
    {1, 2},
    {2, 3},
    {3, 0},
    {4, 5},
    {5, 6},
    {6, 7},
    {7, 4},
    {0, 4},
    {1, 5},
    {2, 6},
    {3, 7},
};

uint64_t pointIndex(
    int first,
    int second,
    int third,
    int width)
{
    return (static_cast<uint64_t>(first) * width + second) *
               width +
           third;
}

uint64_t edgeKey(
    int first,
    int second,
    int third,
    int edge,
    int width)
{
    const int firstCorner = kEdgeCorners[edge][0];
    const int secondCorner = kEdgeCorners[edge][1];
    int low[3] = {
        first + std::min(
                    kCornerOffsets[firstCorner][0],
                    kCornerOffsets[secondCorner][0]),
        second + std::min(
                     kCornerOffsets[firstCorner][1],
                     kCornerOffsets[secondCorner][1]),
        third + std::min(
                    kCornerOffsets[firstCorner][2],
                    kCornerOffsets[secondCorner][2]),
    };
    int axis = 0;
    for (int candidate = 0; candidate < 3; ++candidate) {
        if (kCornerOffsets[firstCorner][candidate] !=
            kCornerOffsets[secondCorner][candidate]) {
            axis = candidate;
            break;
        }
    }
    return pointIndex(low[0], low[1], low[2], width) * 3 +
           static_cast<uint64_t>(axis);
}

int addEdgeVertex(
    int first,
    int second,
    int third,
    int edge,
    int resolution,
    float level,
    const float* field,
    const SourceMesh& source,
    EdgeMap* map,
    TriangleMesh* mesh)
{
    const int width = resolution + 1;
    const uint64_t key =
        edgeKey(first, second, third, edge, width);
    bool inserted = false;
    const int newIndex = mesh->vertices.size();
    const int index = map->findOrInsert(key, newIndex, &inserted);
    if (!inserted)
        return index;
    if (newIndex == std::numeric_limits<int>::max())
        throw std::runtime_error("Marching Cubes output exceeds int32 indices.");

    const int firstCorner = kEdgeCorners[edge][0];
    const int secondCorner = kEdgeCorners[edge][1];
    const int firstCoordinates[3] = {
        first + kCornerOffsets[firstCorner][0],
        second + kCornerOffsets[firstCorner][1],
        third + kCornerOffsets[firstCorner][2],
    };
    const int secondCoordinates[3] = {
        first + kCornerOffsets[secondCorner][0],
        second + kCornerOffsets[secondCorner][1],
        third + kCornerOffsets[secondCorner][2],
    };
    const uint64_t firstIndex = pointIndex(
        firstCoordinates[0],
        firstCoordinates[1],
        firstCoordinates[2],
        width);
    const uint64_t secondIndex = pointIndex(
        secondCoordinates[0],
        secondCoordinates[1],
        secondCoordinates[2],
        width);
    const float firstValue = field[firstIndex];
    const float secondValue = field[secondIndex];
    float t = 0.5f;
    const float denominator = secondValue - firstValue;
    if (std::abs(denominator) > 1.0e-20f)
        t = (level - firstValue) / denominator;
    t = std::max(0.0f, std::min(1.0f, t));
    const float spacing =
        (kDomainMax - kDomainMin) / resolution;
    const Vec3f normalized{
        kDomainMin +
            (firstCoordinates[0] +
             t * (secondCoordinates[0] - firstCoordinates[0])) *
                spacing,
        kDomainMin +
            (firstCoordinates[1] +
             t * (secondCoordinates[1] - firstCoordinates[1])) *
                spacing,
        kDomainMin +
            (firstCoordinates[2] +
             t * (secondCoordinates[2] - firstCoordinates[2])) *
                spacing,
    };
    const float inverseScale = 1.0f / source.scale;
    mesh->vertices.append(MeshPoint3D{{
        double(normalized.x * inverseScale + source.center.x),
        double(normalized.y * inverseScale + source.center.y),
        double(normalized.z * inverseScale + source.center.z)}});
    return index;
}

TriangleMesh extractSurface(
    const std::vector<uint64_t>& activeCells,
    int resolution,
    float level,
    const float* field,
    const SourceMesh& source)
{
    TriangleMesh result;
    const std::size_t reserveVertices = std::min<std::size_t>(
        activeCells.size() + activeCells.size() / 8 + 16,
        static_cast<std::size_t>(std::numeric_limits<int>::max()));
    const std::size_t reserveFaces = std::min<std::size_t>(
        activeCells.size() * 2 + 16,
        static_cast<std::size_t>(std::numeric_limits<int>::max()));
    result.vertices.reserve(static_cast<int>(reserveVertices));
    result.faces.reserve(static_cast<int>(reserveFaces));
    EdgeMap edges(std::max<std::size_t>(activeCells.size(), 16));
    const uint64_t cellPlane =
        static_cast<uint64_t>(resolution) * resolution;
    const int width = resolution + 1;

    for (uint64_t cell : activeCells) {
        const int first = static_cast<int>(cell / cellPlane);
        const int second =
            static_cast<int>((cell / resolution) % resolution);
        const int third = static_cast<int>(cell % resolution);
        unsigned char cubeType = 0;
        for (int corner = 0; corner < 8; ++corner) {
            const uint64_t index = pointIndex(
                first + kCornerOffsets[corner][0],
                second + kCornerOffsets[corner][1],
                third + kCornerOffsets[corner][2],
                width);
            if (field[index] > level)
                cubeType |= static_cast<unsigned char>(1u << corner);
        }
        for (int entry = 0; entry < 16; entry += 3) {
            const int firstEdge =
                vcg::tri::MCLookUpTable::CasesClassic(cubeType, entry);
            if (firstEdge < 0)
                break;
            const int secondEdge =
                vcg::tri::MCLookUpTable::CasesClassic(cubeType, entry + 1);
            const int thirdEdge =
                vcg::tri::MCLookUpTable::CasesClassic(cubeType, entry + 2);
            const int firstVertex = addEdgeVertex(
                first,
                second,
                third,
                firstEdge,
                resolution,
                level,
                field,
                source,
                &edges,
                &result);
            const int secondVertex = addEdgeVertex(
                first,
                second,
                third,
                secondEdge,
                resolution,
                level,
                field,
                source,
                &edges,
                &result);
            const int thirdVertex = addEdgeVertex(
                first,
                second,
                third,
                thirdEdge,
                resolution,
                level,
                field,
                source,
                &edges,
                &result);
            if (firstVertex != secondVertex &&
                secondVertex != thirdVertex &&
                thirdVertex != firstVertex) {
                result.faces.append(
                    std::array<int, 3>{{
                        firstVertex, secondVertex, thirdVertex}});
            }
        }
    }
    return result;
}

} // namespace

FillResult fillSparseUdf(
    const IMeshGeometryView& geometry,
    const FillConfig& config)
{
    FillResult result;
    const QString configError = validateConfig(config);
    if (!configError.isEmpty()) {
        result.result = OperationResult::failure(configError);
        return result;
    }

    QElapsedTimer totalTimer;
    QElapsedTimer stageTimer;
    totalTimer.start();
    stageTimer.start();
    qint64 prepareMilliseconds = 0;
    qint64 classifyMilliseconds = 0;
    qint64 candidateMilliseconds = 0;
    qint64 distanceMilliseconds = 0;
    qint64 extractMilliseconds = 0;
    try {
        SourceMesh source = prepareSource(geometry);
        prepareMilliseconds = stageTimer.restart();

        const int resolution = config.resolution;
        const int width = resolution + 1;
        PackedGrid surface(width);
        rasterizeSurface(source, resolution, &surface);
        PackedGrid closed = close26(surface, config.cclIterations);
        PackedGrid outside = exteriorFill(closed);
        initializeBand(surface, &closed);
        growBand(
            surface,
            &outside,
            &closed,
            config.cclIterations);
        assignUnresolved(surface, &outside, &closed);
        filterInside(surface, &outside);
        classifyCore(surface, &outside);
        classifyMilliseconds = stageTimer.restart();

        addVertexSeeds(source, resolution, &surface);
        addEdgeSeeds(source, resolution, &surface);
        addTriangleSeeds(source, resolution, &surface);
        const int levelRadius = static_cast<int>(
            std::ceil(
                config.epsFactor /
                double(kDomainMax - kDomainMin)));
        const int bandRadius =
            std::max(config.cclIterations, levelRadius);
        PackedGrid candidates =
            candidateCells(surface, resolution, bandRadius);
        std::vector<uint64_t> candidateIds = compactBits(candidates);
        result.candidateCellCount = candidateIds.size() >
                static_cast<std::size_t>(std::numeric_limits<int>::max())
            ? std::numeric_limits<int>::max()
            : static_cast<int>(candidateIds.size());
        PackedGrid pointMask =
            candidatePointMask(candidateIds, resolution);
        std::vector<uint64_t> pointIds = compactBits(pointMask);
        candidates = PackedGrid();
        pointMask = PackedGrid();
        surface = PackedGrid();
        closed = PackedGrid();
        candidateMilliseconds = stageTimer.restart();

        const uint64_t pointCount =
            static_cast<uint64_t>(width) * width * width;
        std::unique_ptr<float[]> field(new float[
            static_cast<std::size_t>(pointCount)]);
        const TriangleBvh bvh(source);
        const float spacing =
            (kDomainMax - kDomainMin) / resolution;
        const float initialMaximum =
            std::sqrt(3.0f) * (bandRadius + 4) * spacing;
        const float initialMaximumSquared =
            initialMaximum * initialMaximum;
        parallelFor(pointIds.size(), 256, [&](std::size_t index) {
            const uint64_t pointId = pointIds[index];
            const float unsignedDistance = std::sqrt(
                std::max(
                    0.0f,
                    bvh.closestSquared(
                        pointForGridIndex(pointId, resolution),
                        initialMaximumSquared)));
            field[static_cast<std::size_t>(pointId)] =
                outside.getLinear(pointId)
                    ? unsignedDistance
                    : -unsignedDistance;
        });
        pointIds.clear();
        pointIds.shrink_to_fit();
        outside = PackedGrid();

        const float level = static_cast<float>(
            config.epsFactor / resolution);
        std::vector<uint8_t> activeFlags(candidateIds.size(), 0u);
        const uint64_t cellPlane =
            static_cast<uint64_t>(resolution) * resolution;
        parallelFor(candidateIds.size(), 512, [&](std::size_t index) {
            const uint64_t cell = candidateIds[index];
            const int first = static_cast<int>(cell / cellPlane);
            const int second =
                static_cast<int>((cell / resolution) % resolution);
            const int third = static_cast<int>(cell % resolution);
            unsigned char cubeType = 0;
            for (int corner = 0; corner < 8; ++corner) {
                const uint64_t point = pointIndex(
                    first + kCornerOffsets[corner][0],
                    second + kCornerOffsets[corner][1],
                    third + kCornerOffsets[corner][2],
                    width);
                if (field[static_cast<std::size_t>(point)] > level)
                    cubeType |= static_cast<unsigned char>(1u << corner);
            }
            activeFlags[index] =
                cubeType != 0u && cubeType != 255u ? 1u : 0u;
        });
        std::size_t activeCount = 0;
        for (std::size_t index = 0; index < candidateIds.size(); ++index) {
            if (activeFlags[index] != 0u)
                candidateIds[activeCount++] = candidateIds[index];
        }
        candidateIds.resize(activeCount);
        result.activeCellCount = activeCount >
                static_cast<std::size_t>(std::numeric_limits<int>::max())
            ? std::numeric_limits<int>::max()
            : static_cast<int>(activeCount);
        activeFlags.clear();
        activeFlags.shrink_to_fit();
        distanceMilliseconds = stageTimer.restart();

        if (candidateIds.empty())
            throw std::runtime_error("The selected isovalue produced no surface.");
        result.mesh = extractSurface(
            candidateIds,
            resolution,
            level,
            field.get(),
            source);
        extractMilliseconds = stageTimer.restart();
        if (result.mesh.vertices.isEmpty() || result.mesh.faces.isEmpty())
            throw std::runtime_error("Marching Cubes produced an empty mesh.");
        result.result = OperationResult::success();
    }
    catch (const std::bad_alloc&) {
        result.result = OperationResult::failure(QStringLiteral(
            "Hole filling ran out of memory at the selected resolution."));
    }
    catch (const std::exception& exception) {
        result.result = OperationResult::failure(
            QString::fromLocal8Bit(exception.what()));
    }
    catch (...) {
        result.result = OperationResult::failure(
            QStringLiteral("Hole filling failed with an unknown error."));
    }
    result.elapsedMilliseconds = totalTimer.elapsed();
    qInfo().nospace()
        << "release-1536 fill r=" << config.resolution
        << " ccl=" << config.cclIterations
        << " eps=" << config.epsFactor
        << " total=" << result.elapsedMilliseconds << "ms"
        << " prepare=" << prepareMilliseconds << "ms"
        << " classify=" << classifyMilliseconds << "ms"
        << " candidates=" << candidateMilliseconds << "ms"
        << " distance=" << distanceMilliseconds << "ms"
        << " extract=" << extractMilliseconds << "ms"
        << " cells=" << result.candidateCellCount
        << "/" << result.activeCellCount
        << " output=" << result.mesh.vertices.size()
        << "v/" << result.mesh.faces.size() << "f";
    return result;
}

FillResult Engine::fill(
    const IMeshGeometryView& geometry,
    const FillConfig& config) const
{
    return fillSparseUdf(geometry, config);
}

} // namespace controllable_hole_filling
