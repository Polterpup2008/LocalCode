// ---------------------------------------------------------------------------
// Windows that must stay reachable.
//
// Two real cases sit behind these: a main window restored onto a monitor that is
// no longer plugged in, and a Settings dialog 946 px tall placed at y=156 on a
// 1080 px display, which put Save and Cancel below the bottom edge where no
// amount of clicking could reach them.
// ---------------------------------------------------------------------------
#include <catch2/catch_test_macros.hpp>

#include "platform/ScreenFit.h"

using namespace localcode;

namespace {
constexpr ScreenRect kDesktop{0, 0, 1920, 1040};  // 1080 less a taskbar
}

TEST_CASE("a window already inside is left alone", "[screenfit]") {
    const ScreenRect wanted{100, 80, 1200, 800};
    CHECK(fit_inside(wanted, kDesktop) == wanted);
}

TEST_CASE("a dialog whose buttons fall below the screen is lifted", "[screenfit]") {
    // The observed case: 868x946 at y=156 ends at 1102, past the bottom.
    const auto fitted = fit_inside({514, 156, 868, 946}, kDesktop);
    CHECK(fitted.bottom() <= kDesktop.bottom());
    CHECK(fitted.y == 1040 - 946);
    // Lifting it must not have changed its size or moved it sideways.
    CHECK(fitted.height == 946);
    CHECK(fitted.x == 514);
}

TEST_CASE("a window off the right edge is pulled back", "[screenfit]") {
    const auto fitted = fit_inside({1800, 100, 600, 400}, kDesktop);
    CHECK(fitted.right() <= kDesktop.right());
    CHECK(fitted.x == 1920 - 600);
    CHECK(fitted.width == 600);
}

TEST_CASE("a window restored onto a monitor that is gone comes back", "[screenfit]") {
    // Saved at 3200,1400 on a desktop that is now a single 1920x1040 screen.
    const auto fitted = fit_inside({3200, 1400, 1100, 800}, kDesktop);
    CHECK(fitted.x >= kDesktop.x);
    CHECK(fitted.y >= kDesktop.y);
    CHECK(fitted.right() <= kDesktop.right());
    CHECK(fitted.bottom() <= kDesktop.bottom());
}

TEST_CASE("a window larger than the screen is shrunk before it is moved", "[screenfit]") {
    // Position alone cannot fix this, and clamping only the position would keep
    // the overhang while looking like it had done something.
    const auto fitted = fit_inside({-200, -100, 2400, 1300}, kDesktop);
    CHECK(fitted.width == kDesktop.width);
    CHECK(fitted.height == kDesktop.height);
    CHECK(fitted.x == kDesktop.x);
    CHECK(fitted.y == kDesktop.y);
}

TEST_CASE("negative positions are pulled to the origin", "[screenfit]") {
    const auto fitted = fit_inside({-500, -500, 400, 300}, kDesktop);
    CHECK(fitted.x == 0);
    CHECK(fitted.y == 0);
    CHECK(fitted.width == 400);
    CHECK(fitted.height == 300);
}

TEST_CASE("a screen offset from the origin is respected", "[screenfit]") {
    // A second monitor to the right, and a window that belongs on it.
    const ScreenRect second{1920, 0, 1280, 1024};
    const auto fitted = fit_inside({3000, 900, 600, 400}, second);
    CHECK(fitted.x >= second.x);
    CHECK(fitted.bottom() <= second.bottom());
    CHECK(fitted.y == 1024 - 400);
}

TEST_CASE("nothing to fit inside leaves the window untouched", "[screenfit]") {
    // No screen reported: better to leave the window where it is than to
    // collapse it to nothing on the strength of a zero.
    const ScreenRect wanted{100, 100, 800, 600};
    CHECK(fit_inside(wanted, {0, 0, 0, 0}) == wanted);
    CHECK(fit_inside({0, 0, 0, 0}, kDesktop) == ScreenRect{0, 0, 0, 0});
}
