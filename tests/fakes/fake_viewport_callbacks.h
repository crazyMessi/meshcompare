#pragma once

#include "renderer/meshlab/viewport_dependencies.h"

class FakeViewportCallbacks final : public IViewportCallbacks
{
public:
    void viewportActivated(int viewportId) override
    {
        lastActivatedViewportId_ = viewportId;
        ++viewportActivationCount_;
    }

    void cameraChanged(int viewportId, const CameraPose& pose) override
    {
        lastCameraViewportId_ = viewportId;
        lastCameraPose_ = pose;
        ++cameraChangeCount_;
    }

    void rendererError(int viewportId, const QString& message) override
    {
        lastErrorViewportId_ = viewportId;
        lastErrorMessage_ = message;
        ++rendererErrorCount_;
    }

    int viewportActivationCount() const { return viewportActivationCount_; }
    int cameraChangeCount() const { return cameraChangeCount_; }
    int rendererErrorCount() const { return rendererErrorCount_; }
    int lastActivatedViewportId() const { return lastActivatedViewportId_; }
    int lastCameraViewportId() const { return lastCameraViewportId_; }
    int lastErrorViewportId() const { return lastErrorViewportId_; }
    const CameraPose& lastCameraPose() const { return lastCameraPose_; }
    const QString& lastErrorMessage() const { return lastErrorMessage_; }

private:
    int viewportActivationCount_ = 0;
    int cameraChangeCount_ = 0;
    int rendererErrorCount_ = 0;
    int lastActivatedViewportId_ = -1;
    int lastCameraViewportId_ = -1;
    int lastErrorViewportId_ = -1;
    CameraPose lastCameraPose_;
    QString lastErrorMessage_;
};
