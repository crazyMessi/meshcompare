#include "viewport_grid.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <utility>
#include <vector>

#include <QEvent>
#include <QRectF>
#include <QScopedValueRollback>
#include <QSet>
#include <QSize>
#include <QSplitter>
#include <QStringList>
#include <QVBoxLayout>
#include <QWidget>

namespace
{
constexpr int splitterHandleWidth = 2;

struct LayoutNode
{
    explicit LayoutNode(QRectF bounds) : bounds(std::move(bounds)) {}

    bool isLeaf() const { return first == nullptr && second == nullptr; }

    QRectF bounds;
    Qt::Orientation orientation = Qt::Horizontal;
    int viewportIndex = -1;
    std::unique_ptr<LayoutNode> first;
    std::unique_ptr<LayoutNode> second;
};

double area(const LayoutNode& node)
{
    return node.bounds.width() * node.bounds.height();
}

bool comesBefore(const LayoutNode& left, const LayoutNode& right)
{
    if (left.bounds.top() != right.bounds.top())
        return left.bounds.top() < right.bounds.top();
    return left.bounds.left() < right.bounds.left();
}

void split(LayoutNode& node)
{
    node.orientation = node.bounds.width() >= node.bounds.height()
                           ? Qt::Horizontal
                           : Qt::Vertical;
    if (node.orientation == Qt::Horizontal) {
        const qreal availableWidth =
            std::max<qreal>(0.0, node.bounds.width() - splitterHandleWidth);
        const qreal firstWidth = std::floor(availableWidth / 2.0);
        const qreal secondWidth = availableWidth - firstWidth;
        node.first.reset(new LayoutNode(QRectF(
            node.bounds.left(),
            node.bounds.top(),
            firstWidth,
            node.bounds.height())));
        node.second.reset(new LayoutNode(QRectF(
            node.bounds.left() + firstWidth + splitterHandleWidth,
            node.bounds.top(),
            secondWidth,
            node.bounds.height())));
        return;
    }

    const qreal availableHeight =
        std::max<qreal>(0.0, node.bounds.height() - splitterHandleWidth);
    const qreal firstHeight = std::floor(availableHeight / 2.0);
    const qreal secondHeight = availableHeight - firstHeight;
    node.first.reset(new LayoutNode(QRectF(
        node.bounds.left(),
        node.bounds.top(),
        node.bounds.width(),
        firstHeight)));
    node.second.reset(new LayoutNode(QRectF(
        node.bounds.left(),
        node.bounds.top() + firstHeight + splitterHandleWidth,
        node.bounds.width(),
        secondHeight)));
}

std::unique_ptr<LayoutNode> createLayoutPlan(int viewportCount, const QSize& hostSize)
{
    const qreal width = hostSize.width() > 0 ? hostSize.width() : 1600.0;
    const qreal height = hostSize.height() > 0 ? hostSize.height() : 900.0;
    std::unique_ptr<LayoutNode> root(
        new LayoutNode(QRectF(0.0, 0.0, width, height)));
    std::vector<LayoutNode*> leaves{root.get()};

    for (int index = 1; index < viewportCount; ++index) {
        auto selected = leaves.begin();
        for (auto candidate = leaves.begin() + 1; candidate != leaves.end(); ++candidate) {
            const double candidateArea = area(**candidate);
            const double selectedArea = area(**selected);
            if (candidateArea > selectedArea ||
                (candidateArea == selectedArea && comesBefore(**candidate, **selected))) {
                selected = candidate;
            }
        }

        LayoutNode* leaf = *selected;
        split(*leaf);
        *selected = leaf->first.get();
        leaves.push_back(leaf->second.get());
    }

    std::sort(
        leaves.begin(),
        leaves.end(),
        [](const LayoutNode* left, const LayoutNode* right) {
            return comesBefore(*left, *right);
        });
    for (int index = 0; index < static_cast<int>(leaves.size()); ++index)
        leaves[static_cast<std::size_t>(index)]->viewportIndex = index;

    return root;
}

void configureSplitter(QSplitter& splitter, Qt::Orientation orientation)
{
    splitter.setOrientation(orientation);
    splitter.setChildrenCollapsible(false);
    splitter.setHandleWidth(splitterHandleWidth);
    splitter.setOpaqueResize(true);
}

QWidget* buildLayoutBranch(
    const LayoutNode& node,
    std::vector<QWidget*>& viewportHosts);

void populateSplitter(
    QSplitter& splitter,
    const LayoutNode& layout,
    std::vector<QWidget*>& viewportHosts)
{
    configureSplitter(splitter, layout.orientation);
    splitter.addWidget(buildLayoutBranch(*layout.first, viewportHosts));
    splitter.addWidget(buildLayoutBranch(*layout.second, viewportHosts));
    splitter.setStretchFactor(0, 1);
    splitter.setStretchFactor(1, 1);
}

QWidget* buildLayoutBranch(
    const LayoutNode& node,
    std::vector<QWidget*>& viewportHosts)
{
    if (node.isLeaf()) {
        auto* host = new QWidget;
        host->setObjectName(
            QStringLiteral("meshcompareViewportLeaf%1").arg(node.viewportIndex + 1));
        viewportHosts[static_cast<std::size_t>(node.viewportIndex)] = host;
        return host;
    }

    auto* splitter = new QSplitter;
    populateSplitter(*splitter, node, viewportHosts);
    return splitter;
}

void equalizeSplitterTree(QWidget* widget)
{
    auto* splitter = qobject_cast<QSplitter*>(widget);
    if (splitter == nullptr)
        return;

    QList<int> equalSizes;
    equalSizes << 1 << 1;
    splitter->setSizes(equalSizes);
    for (int index = 0; index < splitter->count(); ++index)
        equalizeSplitterTree(splitter->widget(index));
}
} // namespace

ViewportGrid::ViewportGrid(
    IViewportFactory& viewportFactory,
    IViewportCallbacks& eventSink)
    : viewportFactory_(viewportFactory), eventSink_(eventSink)
{
}

ViewportGrid::~ViewportGrid()
{
    clear();
}

OperationResult ViewportGrid::create(
    QWidget* host,
    const SceneDescriptor& scene,
    const ViewportGridSceneDependencies& dependencies,
    MeshId selectedMeshId)
{
    clear();
    if (host == nullptr)
        return OperationResult::failure(QStringLiteral("A viewport host is required."));
    if (scene.meshes.size() < 2 || scene.meshes.size() > 8) {
        return OperationResult::failure(
            QStringLiteral("A viewport layout requires between 2 and 8 meshes."));
    }
    if (!dependencies.meshModelIdFor) {
        return OperationResult::failure(
            QStringLiteral("The viewport grid has no mesh model resolver."));
    }

    overlayMode_ = scene.layoutMode == SceneLayoutMode::Overlay;
    sceneMeshIds_.reserve(scene.meshes.size());
    for (const SceneMesh& mesh : scene.meshes) {
        sceneMeshIds_.append(mesh.id);
        meshLabels_.insert(mesh.id, mesh.label);
        analysisLabels_.insert(mesh.id, mesh.analysisLabel);
        meshVisibility_.insert(mesh.id, true);
    }

    const int viewportCount = overlayMode_ ? 1 : scene.meshes.size();
    QVector<int> modelIds;
    modelIds.reserve(scene.meshes.size());
    for (const SceneMesh& sceneMesh : scene.meshes) {
        const int modelId = dependencies.meshModelIdFor(sceneMesh.id);
        if (modelId < 0) {
            clear();
            return OperationResult::failure(
                QStringLiteral("%1 was not prepared for rendering.").arg(sceneMesh.label));
        }
        modelIds.append(modelId);
        meshModelIds_.insert(sceneMesh.id, modelId);
    }

    host_ = host;
    std::unique_ptr<LayoutNode> layoutPlan =
        createLayoutPlan(viewportCount, host->size());
    auto* rootSplitter = new QSplitter(host);
    container_ = rootSplitter;
    container_->setObjectName(QStringLiteral("meshcompareViewportGrid"));
    container_->hide();
    fillHost();
    host_->installEventFilter(this);

    std::vector<QWidget*> viewportHosts(
        static_cast<std::size_t>(viewportCount), nullptr);
    if (viewportCount == 1) {
        auto* singleHost = new QWidget;
        singleHost->setObjectName(QStringLiteral("meshcompareViewportLeaf1"));
        rootSplitter->addWidget(singleHost);
        viewportHosts.front() = singleHost;
    }
    else {
        populateSplitter(*rootSplitter, *layoutPlan, viewportHosts);
    }

    for (int index = 0; index < viewportCount; ++index) {
        const SceneMesh& sceneMesh = scene.meshes.at(index);
        const int meshModelId = modelIds.at(index);
        QVector<int> additionalMeshModelIds;
        QString viewportLabel = sceneMesh.label;
        bool selected = sceneMesh.id == selectedMeshId;
        bool reference = sceneMesh.isReference;
        MeshId viewportMeshId = sceneMesh.id;
        if (overlayMode_) {
            additionalMeshModelIds = modelIds.mid(1);
            viewportLabel = QStringLiteral("Overlay · %1 layers")
                                .arg(scene.meshes.size());
            selected = false;
            reference = false;
            viewportMeshId = 0;
        }

        ViewportDependencies viewportDependencies{
            dependencies.document,
            dependencies.sharedContext,
            dependencies.settings,
            *this,
            index + 1,
            meshModelId,
            index + 1,
            viewportCount,
            viewportLabel,
            selected,
            sceneMesh.analysisLabel,
            reference,
            additionalMeshModelIds};
        std::unique_ptr<IViewport> viewport;
        QWidget* viewportHost = viewportHosts[static_cast<std::size_t>(index)];
        auto* viewportLayout = new QVBoxLayout(viewportHost);
        viewportLayout->setContentsMargins(0, 0, 0, 0);
        viewportLayout->setSpacing(0);
        OperationResult result = viewportFactory_.createViewport(
            viewportHost, viewportDependencies, viewport);
        if (!result.ok) {
            clear();
            return result;
        }
        if (!viewport) {
            clear();
            return OperationResult::failure(
                QStringLiteral("Viewport creation returned no viewport."));
        }

        QWidget* widget = viewport->widget();
        if (widget == nullptr) {
            clear();
            return OperationResult::failure(
                QStringLiteral("Viewport creation returned no widget."));
        }
        if (widget->parentWidget() != viewportHost) {
            clear();
            return OperationResult::failure(
                QStringLiteral("A prepared viewport must keep its final layout parent."));
        }
        viewportLayout->addWidget(widget);
        // Mark children visible while their final parent container remains
        // hidden.  QGLWidget preflight can otherwise leave a child in an
        // explicit-hidden state that excludes it from the committed layout.
        viewportHost->show();
        widget->show();

        viewportMeshIds_.push_back(viewportMeshId);
        viewports_.push_back(std::move(viewport));
        result = viewports_.back()->initializeForScenePreparation();
        if (!result.ok) {
            clear();
            return result;
        }
    }

    setSelectedMesh(selectedMeshId);
    if (overlayMode_)
        refreshOverlayLabels();
    if (viewportCount > 1)
        equalizeSplitterTree(rootSplitter);

    return OperationResult::success();
}

void ViewportGrid::showCommitted()
{
    if (container_.isNull())
        return;
    fillHost();
    container_->show();
    if (container_->layout() != nullptr) {
        container_->layout()->invalidate();
        container_->layout()->activate();
    }
    container_->raise();
}

void ViewportGrid::setSelectedMesh(MeshId meshId)
{
    if (overlayMode_)
        return;
    for (std::size_t index = 0; index < viewports_.size(); ++index)
        viewports_[index]->setSelected(viewportMeshIds_[index] == meshId);
}

void ViewportGrid::setReferenceMesh(MeshId meshId)
{
    if (overlayMode_)
        return;
    for (std::size_t index = 0; index < viewports_.size(); ++index)
        viewports_[index]->setReference(viewportMeshIds_[index] == meshId);
}

OperationResult ViewportGrid::setMeshVisible(MeshId meshId, bool visible)
{
    if (!overlayMode_ || viewports_.empty()) {
        return OperationResult::failure(
            QStringLiteral("Layer visibility requires an Overlay viewport."));
    }
    const int modelId = meshModelIds_.value(meshId, -1);
    if (modelId < 0) {
        return OperationResult::failure(
            QStringLiteral("Visibility target does not exist in the committed scene."));
    }

    meshVisibility_[meshId] = visible;
    viewports_.front()->setMeshVisible(modelId, visible);
    refreshOverlayLabels();
    return OperationResult::success();
}

OperationResult ViewportGrid::setAnalysisOverlays(
    const QVector<MeshAnalysisOverlayUpdate>& updates)
{
    if (viewports_.empty()) {
        return OperationResult::failure(
            QStringLiteral("No viewport grid is available for analysis overlays."));
    }
    if (updates.isEmpty()) {
        return OperationResult::failure(
            QStringLiteral("An analysis overlay batch cannot be empty."));
    }

    QSet<MeshId> updateIds;
    for (const MeshAnalysisOverlayUpdate& update : updates) {
        if (updateIds.contains(update.meshId)) {
            return OperationResult::failure(
                QStringLiteral("Analysis overlay mesh IDs must be unique."));
        }
        updateIds.insert(update.meshId);

        if (!sceneMeshIds_.contains(update.meshId)) {
            return OperationResult::failure(QStringLiteral(
                "Analysis overlay target does not exist in the committed scene."));
        }
    }

    for (const MeshAnalysisOverlayUpdate& update : updates)
        analysisLabels_[update.meshId] = update.label;

    if (overlayMode_) {
        refreshOverlayLabels();
        return OperationResult::success();
    }

    for (const MeshAnalysisOverlayUpdate& update : updates) {
        for (std::size_t index = 0; index < viewportMeshIds_.size(); ++index) {
            if (viewportMeshIds_[index] == update.meshId) {
                viewports_[index]->setScoreLabel(update.label);
                break;
            }
        }
    }
    return OperationResult::success();
}

void ViewportGrid::refreshOverlayLabels()
{
    if (!overlayMode_ || viewports_.empty())
        return;

    int visibleCount = 0;
    QStringList summary;
    for (MeshId meshId : sceneMeshIds_) {
        if (!meshVisibility_.value(meshId, true))
            continue;
        ++visibleCount;
        const QString label = analysisLabels_.value(meshId);
        if (!label.isEmpty()) {
            summary.append(
                QStringLiteral("%1: %2").arg(meshLabels_.value(meshId), label));
        }
    }
    viewports_.front()->setLabel(
        QStringLiteral("Overlay · %1/%2 visible")
            .arg(visibleCount)
            .arg(sceneMeshIds_.size()));
    viewports_.front()->setScoreLabel(summary.join(QStringLiteral(" · ")));
}

CameraPose ViewportGrid::captureCamera() const
{
    return viewports_.empty() ? CameraPose{} : viewports_.front()->captureCamera();
}

OperationResult ViewportGrid::restoreCamera(const CameraPose& pose)
{
    if (viewports_.empty())
        return OperationResult::failure(QStringLiteral("No committed viewport is available."));

    std::vector<CameraPose> previousPoses;
    previousPoses.reserve(viewports_.size());
    for (const std::unique_ptr<IViewport>& viewport : viewports_)
        previousPoses.push_back(viewport->captureCamera());

    QScopedValueRollback<bool> guard(propagatingCamera_, true);
    for (int index = 0; index < viewportCount(); ++index) {
        const OperationResult result =
            viewports_[static_cast<std::size_t>(index)]->restoreCamera(pose);
        if (result.ok)
            continue;

        QString rollbackError;
        for (int rollbackIndex = index; rollbackIndex >= 0; --rollbackIndex) {
            const OperationResult rolledBack =
                viewports_[static_cast<std::size_t>(rollbackIndex)]->restoreCamera(
                    previousPoses[static_cast<std::size_t>(rollbackIndex)]);
            if (!rolledBack.ok && rollbackError.isEmpty())
                rollbackError = rolledBack.error;
        }
        if (rollbackError.isEmpty())
            return result;
        return OperationResult::failure(
            QStringLiteral("%1 Camera rollback also failed: %2")
                .arg(result.error, rollbackError));
    }
    return OperationResult::success();
}

void ViewportGrid::resetCamera()
{
    QScopedValueRollback<bool> guard(propagatingCamera_, true);
    for (const std::unique_ptr<IViewport>& viewport : viewports_)
        viewport->resetCamera();
}

void ViewportGrid::setDiagnostic(DiagnosticFlag flag, bool enabled)
{
    for (const std::unique_ptr<IViewport>& viewport : viewports_)
        viewport->setDiagnostic(flag, enabled);
}

void ViewportGrid::requestRepaint()
{
    for (const std::unique_ptr<IViewport>& viewport : viewports_)
        viewport->requestRepaint();
}

int ViewportGrid::viewportCount() const
{
    return static_cast<int>(viewports_.size());
}

MeshId ViewportGrid::meshIdForViewport(int viewportId) const
{
    const int index = viewportId - 1;
    if (index < 0 || static_cast<std::size_t>(index) >= viewportMeshIds_.size())
        return 0;
    return viewportMeshIds_[static_cast<std::size_t>(index)];
}

IViewport* ViewportGrid::viewportAt(int index) const
{
    if (index < 0 || static_cast<std::size_t>(index) >= viewports_.size())
        return nullptr;
    return viewports_[static_cast<std::size_t>(index)].get();
}

bool ViewportGrid::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == host_ &&
        (event->type() == QEvent::Resize || event->type() == QEvent::Show)) {
        fillHost();
    }
    return QObject::eventFilter(watched, event);
}

void ViewportGrid::viewportActivated(int viewportId)
{
    const MeshId meshId = meshIdForViewport(viewportId);
    if (meshId == 0)
        return;
    setSelectedMesh(meshId);
    eventSink_.viewportActivated(viewportId);
}

void ViewportGrid::cameraChanged(int viewportId, const CameraPose& pose)
{
    if (propagatingCamera_)
        return;

    QScopedValueRollback<bool> guard(propagatingCamera_, true);
    for (int index = 0; index < viewportCount(); ++index) {
        if (index + 1 == viewportId)
            continue;
        const OperationResult result = viewports_[static_cast<std::size_t>(index)]->restoreCamera(pose);
        if (!result.ok)
            eventSink_.rendererError(index + 1, result.error);
    }
    eventSink_.cameraChanged(viewportId, pose);
}

void ViewportGrid::rendererError(int viewportId, const QString& message)
{
    eventSink_.rendererError(viewportId, message);
}

void ViewportGrid::clear()
{
    if (!host_.isNull())
        host_->removeEventFilter(this);
    viewports_.clear();
    viewportMeshIds_.clear();
    sceneMeshIds_.clear();
    meshLabels_.clear();
    analysisLabels_.clear();
    meshModelIds_.clear();
    meshVisibility_.clear();
    overlayMode_ = false;
    delete container_.data();
    container_.clear();
    host_.clear();
}

void ViewportGrid::fillHost()
{
    if (!host_.isNull() && !container_.isNull())
        container_->setGeometry(host_->rect());
}
