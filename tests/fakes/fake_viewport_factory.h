#pragma once

#include <memory>
#include <vector>

#include <QObject>
#include <QHash>
#include <QPointer>
#include <QSet>
#include <QStringList>
#include <QWidget>

#include <common/ml_shared_data_context/ml_scene_gl_shared_data_context.h>

#include "renderer/meshlab/viewport_factory.h"

class FakeViewport final : public IViewport
{
public:
    FakeViewport(
        QWidget* parent,
        IViewportCallbacks& callbacks,
        int viewportId,
        QStringList& lifecycle,
        QString initializationError,
        int& initializationCount)
        : widget_(new QWidget(parent)),
          callbacks_(callbacks),
          viewportId_(viewportId),
          lifecycle_(lifecycle),
          initializationError_(initializationError),
          initializationCount_(initializationCount)
    {
    }

    ~FakeViewport() override
    {
        delete widget_.data();
        lifecycle_.append(QStringLiteral("viewports:destroy"));
    }

    QWidget* widget() const override { return widget_.data(); }
    CameraPose captureCamera() const override { return camera_; }
    OperationResult restoreCamera(const CameraPose& pose) override
    {
        camera_ = pose;
        return OperationResult::success();
    }
    void resetCamera() override { camera_ = {}; }
    void setLabel(QString label) override { label_ = std::move(label); }
    void setSelected(bool selected) override { selected_ = selected; }
    void setReference(bool reference) override { reference_ = reference; }
    void setMeshVisible(int meshModelId, bool visible) override
    {
        meshVisibility_[meshModelId] = visible;
    }
    void setScoreLabel(QString label) override { scoreLabel_ = std::move(label); }
    void setDiagnostic(DiagnosticFlag flag, bool enabled) override
    {
        diagnostics_[static_cast<int>(flag)] = enabled;
    }
    void requestRepaint() override { ++repaintCount_; }

    OperationResult initializeForScenePreparation() override
    {
        ++initializationCount_;
        if (!initializationError_.isEmpty())
            return OperationResult::failure(initializationError_);
        return OperationResult::success();
    }

    void emitActivated() { callbacks_.viewportActivated(viewportId_); }
    bool selected() const { return selected_; }
    bool reference() const { return reference_; }
    const QString& scoreLabel() const { return scoreLabel_; }
    const QString& label() const { return label_; }
    bool meshVisible(int meshModelId) const
    {
        return meshVisibility_.value(meshModelId, true);
    }
    int hiddenMeshCount() const
    {
        int count = 0;
        for (bool visible : meshVisibility_)
            count += visible ? 0 : 1;
        return count;
    }
    const CameraPose& camera() const { return camera_; }
    int repaintCount() const { return repaintCount_; }
    bool diagnosticEnabled(DiagnosticFlag flag) const
    {
        return diagnostics_[static_cast<int>(flag)];
    }

private:
    QPointer<QWidget> widget_;
    IViewportCallbacks& callbacks_;
    int viewportId_ = 0;
    QStringList& lifecycle_;
    QString initializationError_;
    int& initializationCount_;
    CameraPose camera_;
    QString label_;
    QHash<int, bool> meshVisibility_;
    bool selected_ = false;
    bool reference_ = false;
    QString scoreLabel_;
    bool diagnostics_[3] = {false, false, false};
    int repaintCount_ = 0;
};

class FakeViewportFactory final : public IViewportFactory
{
public:
    void failNextCreation(const QString& error)
    {
        nextCreationError_ = error;
    }

    void failNextInitialization(const QString& error)
    {
        nextInitializationError_ = error;
        initializationFailureCountdown_ = 0;
    }

    void failInitializationAfter(int successfulInitializations, const QString& error)
    {
        nextInitializationError_ = error;
        initializationFailureCountdown_ = successfulInitializations;
    }

    OperationResult createViewport(
        QWidget* parent,
        const ViewportDependencies& dependencies,
        std::unique_ptr<IViewport>& viewport) override
    {
        QObject* context = &dependencies.sharedContext;
        if (!observedContexts_.contains(context)) {
            observedContexts_.insert(context);
            QObject::connect(
                context,
                &QObject::destroyed,
                [this, context] {
                    lifecycle_.append(QStringLiteral("context:destroy"));
                    observedContexts_.remove(context);
                });
        }

        viewportCount_ = dependencies.viewportCount;
        lastParent_ = parent;
        ++creationCount_;
        if (!nextCreationError_.isEmpty()) {
            const QString error = nextCreationError_;
            nextCreationError_.clear();
            return OperationResult::failure(error);
        }

        QString initializationError;
        if (initializationFailureCountdown_ == 0) {
            initializationError = nextInitializationError_;
            nextInitializationError_.clear();
            initializationFailureCountdown_ = -1;
        }
        else if (initializationFailureCountdown_ > 0) {
            --initializationFailureCountdown_;
        }
        viewport.reset(new FakeViewport(
            parent,
            dependencies.callbacks,
            dependencies.viewportId,
            lifecycle_,
            initializationError,
            initializationCount_));
        viewports_.push_back(static_cast<FakeViewport*>(viewport.get()));
        return OperationResult::success();
    }

    void clearLifecycle() { lifecycle_.clear(); }
    const QStringList& lifecycle() const { return lifecycle_; }
    int creationCount() const { return creationCount_; }
    int initializationCount() const { return initializationCount_; }
    int viewportCount() const { return viewportCount_; }
    QWidget* lastParent() const { return lastParent_; }
    FakeViewport& viewport(int index) { return *viewports_.at(index); }

private:
    QString nextCreationError_;
    QString nextInitializationError_;
    int initializationFailureCountdown_ = -1;
    QStringList lifecycle_;
    QSet<QObject*> observedContexts_;
    int creationCount_ = 0;
    int initializationCount_ = 0;
    int viewportCount_ = 0;
    QWidget* lastParent_ = nullptr;
    std::vector<FakeViewport*> viewports_;
};
