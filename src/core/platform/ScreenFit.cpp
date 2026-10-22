#include "ScreenFit.h"

#include <algorithm>

namespace localcode {

ScreenRect fit_inside(ScreenRect wanted, ScreenRect available) {
    if (available.empty() || wanted.empty()) return wanted;

    wanted.width = std::min(wanted.width, available.width);
    wanted.height = std::min(wanted.height, available.height);

    // Right and bottom first, then left and top. Doing it in this order means a
    // window that overhangs both ends - which can only happen when it is exactly
    // as large as the screen - ends up flush with the top-left rather than
    // pushed back off the other side.
    if (wanted.right() > available.right()) wanted.x = available.right() - wanted.width;
    if (wanted.bottom() > available.bottom()) wanted.y = available.bottom() - wanted.height;
    if (wanted.x < available.x) wanted.x = available.x;
    if (wanted.y < available.y) wanted.y = available.y;

    return wanted;
}

}  // namespace localcode
