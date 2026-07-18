#pragma once

#include <QColor>

#include "meshcompare_types.h"

bool isValidDistanceColorMapping(DistanceColorMapping mapping);

double distanceColorPosition(
    double distance,
    double maximumDistance,
    DistanceColorMapping mapping);

QColor viridisColorForPosition(double position);

QColor distanceColorForValue(
    double distance,
    double maximumDistance,
    DistanceColorMapping mapping =
        DistanceColorMapping::SquareRoot);
