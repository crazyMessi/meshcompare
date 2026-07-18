#include "analysis_color_map.h"

#include <algorithm>
#include <cmath>

namespace
{
int hexDigit(char value)
{
    if (value >= '0' && value <= '9')
        return value - '0';
    return value - 'a' + 10;
}

QColor viridisColor(int index)
{
    // Exact uint8 form of Matplotlib 3.10's 256-entry _viridis_data after
    // np.round(channel * 255). ListedColormap selects floor(value * 256).
    static const char colors[] =
        "44015444025645045745055946075a46085c460a5d460b5e470d60470e61471063471164471365481467481668481769"
        "48186a481a6c481b6d481c6e481d6f481f70482071482173482374482475482576482677482878482979472a7a472c7a"
        "472d7b472e7c472f7d46307e46327e46337f463480453581453781453882443983443a83443b84433d84433e85423f85424086"
        "4241864142874144874045884046883f47883f48893e49893e4a893e4c8a3d4d8a3d4e8a3c4f8a3c508b3b518b3b528b"
        "3a538b3a548c39558c39568c38588c38598c375a8c375b8d365c8d365d8d355e8d355f8d34608d34618d33628d33638d"
        "32648e32658e31668e31678e31688e30698e306a8e2f6b8e2f6c8e2e6d8e2e6e8e2e6f8e2d708e2d718e2c718e2c728e"
        "2c738e2b748e2b758e2a768e2a778e2a788e29798e297a8e297b8e287c8e287d8e277e8e277f8e27808e26818e26828e"
        "26828e25838e25848e25858e24868e24878e23888e23898e238a8d228b8d228c8d228d8d218e8d218f8d21908d21918c"
        "20928c20928c20938c1f948c1f958b1f968b1f978b1f988b1f998a1f9a8a1e9b8a1e9c891e9d891f9e891f9f881fa088"
        "1fa1881fa1871fa28720a38620a48621a58521a68522a78522a88423a98324aa8325ab8225ac8226ad8127ad8128ae80"
        "29af7f2ab07f2cb17e2db27d2eb37c2fb47c31b57b32b67a34b67935b77937b87838b9773aba763bbb753dbc743fbc73"
        "40bd7242be7144bf7046c06f48c16e4ac16d4cc26c4ec36b50c46a52c56954c56856c66758c7655ac8645cc8635ec962"
        "60ca6063cb5f65cb5e67cc5c69cd5b6ccd5a6ece5870cf5773d05675d05477d1537ad1517cd2507fd34e81d34d84d44b"
        "86d54989d5488bd6468ed64590d74393d74195d84098d83e9bd93c9dd93ba0da39a2da37a5db36a8db34aadc32addc30"
        "b0dd2fb2dd2db5de2bb8de29bade28bddf26c0df25c2df23c5e021c8e020cae11fcde11dd0e11cd2e21bd5e21ad8e219"
        "dae319dde318dfe318e2e418e5e419e7e419eae51aece51befe51cf1e51df4e61ef6e620f8e621fbe723fde725";
    static_assert(
        sizeof(colors) == 256 * 6 + 1,
        "The embedded viridis table must contain exactly 256 RGB entries.");
    const int offset = std::max(0, std::min(255, index)) * 6;
    const auto channel = [offset](int channelOffset) {
        return hexDigit(colors[offset + channelOffset]) * 16 +
               hexDigit(colors[offset + channelOffset + 1]);
    };
    return QColor(channel(0), channel(2), channel(4), 255);
}
} // namespace

bool isValidDistanceColorMapping(DistanceColorMapping mapping)
{
    switch (mapping) {
    case DistanceColorMapping::Linear:
    case DistanceColorMapping::SquareRoot:
        return true;
    default:
        return false;
    }
}

double distanceColorPosition(
    double distance,
    double maximumDistance,
    DistanceColorMapping mapping)
{
    if (!std::isfinite(distance) || !std::isfinite(maximumDistance) ||
        maximumDistance <= 0.0 || !isValidDistanceColorMapping(mapping)) {
        return 0.0;
    }

    const double normalized =
        std::max(0.0, std::min(1.0, distance / maximumDistance));
    return mapping == DistanceColorMapping::SquareRoot
               ? std::sqrt(normalized)
               : normalized;
}

QColor viridisColorForPosition(double position)
{
    if (!std::isfinite(position))
        return QColor(0, 0, 0, 255);
    const double clamped = std::max(0.0, std::min(1.0, position));
    const int colorIndex =
        clamped >= 1.0 ? 255 : int(std::floor(clamped * 256.0));
    return viridisColor(colorIndex);
}

QColor distanceColorForValue(
    double distance,
    double maximumDistance,
    DistanceColorMapping mapping)
{
    if (!std::isfinite(distance))
        return QColor(0, 0, 0, 255);
    return viridisColorForPosition(
        distanceColorPosition(distance, maximumDistance, mapping));
}
