#pragma once

#include <cstddef>
#include <memory>

#include <QHash>
#include <QPointer>

#include "../../core/mesh_resource_provider.h"

class MeshDocument;
class MeshModel;
class MLRenderingData;
class MLSceneGLSharedDataContext;

namespace vcg
{
class QtThreadSafeMemoryInfo;
}

namespace MeshCompareRenderDetail
{
// Restores the renderer-private invariant for a model whose committed
// presentation has no optional face-color channel.  This also repairs the
// half-enabled OCF state left if EnableColor() throws before MeshModel can set
// its MM_FACECOLOR bit.
void ensureFaceColorDisabled(MeshModel& model) noexcept;
}

struct RenderSceneSettings
{
    std::ptrdiff_t gpuMemoryBytes = std::ptrdiff_t(350) * 1024 * 1024;
    bool highPrecisionRendering = false;
    std::size_t primitivesPerBatch = 100000;
    std::size_t minFacesForSmoothRendering = 2000000;
};

struct RenderPresentationUpload
{
    bool faceColorsChanged = false;
};

// Isolates the MeshLab buffer/publication boundary from the CPU transaction.
// A backend exception is reported and triggers best-effort publication rollback.
class IRenderPresentationPublisher
{
public:
    virtual ~IRenderPresentationPublisher() = default;
    virtual void publish(
        MLSceneGLSharedDataContext& sharedContext,
        int modelId,
        const MLRenderingData& renderingData,
        RenderPresentationUpload upload) = 0;
};

// Owns the MeshLab objects backing one staged scene.  Its explicit destructor
// releases the shared GL context before the document and GPU-memory tracker.
// SceneBundle owns viewports after this object, making viewport destruction
// happen first.
class RenderSceneContext
{
public:
    explicit RenderSceneContext(const RenderSceneSettings& settings = {});
    RenderSceneContext(
        const RenderSceneSettings& settings,
        IRenderPresentationPublisher& publisher);
    ~RenderSceneContext();

    RenderSceneContext(const RenderSceneContext&) = delete;
    RenderSceneContext& operator=(const RenderSceneContext&) = delete;

    MeshDocument& document();
    MLSceneGLSharedDataContext& sharedContext();
    OperationResult addResources(
        const SceneDescriptor& scene,
        const IMeshResourceProvider& resources);
    OperationResult setColorPresentations(
        const QVector<MeshColorPresentationUpdate>& updates);
    const ColorPresentation* colorPresentation(MeshId meshId) const;
    const MLRenderingData* currentRenderingData(MeshId meshId) const;
    int modelIdFor(MeshId meshId) const;

private:
    RenderSceneContext(
        const RenderSceneSettings& settings,
        IRenderPresentationPublisher* publisher);

    RenderSceneSettings settings_;
    std::unique_ptr<vcg::QtThreadSafeMemoryInfo> memoryInfo_;
    std::unique_ptr<MeshDocument> document_;
    QPointer<MLSceneGLSharedDataContext> sharedContext_;
    QHash<MeshId, int> modelIds_;
    QHash<MeshId, ColorPresentation> presentations_;
    QHash<MeshId, std::shared_ptr<const MLRenderingData>> renderingData_;
    std::unique_ptr<IRenderPresentationPublisher> ownedPublisher_;
    IRenderPresentationPublisher* publisher_ = nullptr;
};
