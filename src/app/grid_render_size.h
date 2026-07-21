#pragma once

#include <QSize>
#include <QtGlobal>

constexpr int DefaultGridRenderWidth = 2048;
constexpr int DefaultGridRenderHeight = 1152;
constexpr int MaximumGridRenderDimension = 16384;
constexpr qint64 MaximumGridRenderPixels = 64LL * 1024 * 1024;

inline QSize defaultGridRenderSize()
{
    return QSize(DefaultGridRenderWidth, DefaultGridRenderHeight);
}

inline bool isSupportedGridRenderSize(const QSize& size)
{
    return size.width() > 0 && size.height() > 0 &&
        size.width() <= MaximumGridRenderDimension &&
        size.height() <= MaximumGridRenderDimension &&
        qint64(size.width()) * qint64(size.height()) <= MaximumGridRenderPixels;
}
