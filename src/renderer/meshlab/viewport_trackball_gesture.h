#pragma once

#include <wrap/gui/trackball.h>

// Keeps one mouse gesture's initial button/modifier state together with the
// trackball calls that consume it.
class ViewportTrackballGesture
{
public:
    void mouseDown(vcg::Trackball& trackball, int x, int y, int buttons)
    {
        activeButtons_ = buttons;
        trackball.MouseDown(x, y, buttons);
    }

    void mouseMove(vcg::Trackball& trackball, int x, int y)
    {
        trackball.MouseMove(x, y);
    }

    void mouseUp(vcg::Trackball& trackball, int x, int y, int)
    {
        // A modifier can be released before the mouse button.  Trackball must
        // clear the state that began the gesture, not the modifier state that
        // happens to be present on the release event.
        trackball.MouseUp(x, y, activeButtons_);
        activeButtons_ = vcg::Trackball::BUTTON_NONE;
    }

private:
    int activeButtons_ = vcg::Trackball::BUTTON_NONE;
};
