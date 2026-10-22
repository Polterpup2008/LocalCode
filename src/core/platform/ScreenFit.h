#pragma once
// ---------------------------------------------------------------------------
// Pulling a window back inside the screen.
//
// A saved geometry is a promise about a display that may not be there any more,
// and a dialog centred on a tall parent can be placed with its buttons below the
// bottom edge. Both leave a window the user cannot finish using.
//
// The arithmetic lives here, in plain integers, because the GUI is not linked
// into the test binary and a rule this easy to get subtly wrong should be
// testable. The widget layer converts to and from QRect.
// ---------------------------------------------------------------------------
namespace localcode {

struct ScreenRect {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;

    [[nodiscard]] constexpr int right() const { return x + width; }
    [[nodiscard]] constexpr int bottom() const { return y + height; }
    [[nodiscard]] constexpr bool empty() const { return width <= 0 || height <= 0; }

    friend constexpr bool operator==(const ScreenRect& a, const ScreenRect& b) {
        return a.x == b.x && a.y == b.y && a.width == b.width && a.height == b.height;
    }
};

// Returns `wanted` moved - and shrunk first, if it has to be - so that it lies
// wholly inside `available`.
//
// Shrinking comes first on purpose: a window wider than the desktop cannot be
// placed wholly inside it, and clamping only the position would keep the
// overhang while appearing to have done something.
//
// An empty or absent `available` means there is nothing to fit inside, and
// `wanted` is returned untouched rather than collapsed to nothing.
[[nodiscard]] ScreenRect fit_inside(ScreenRect wanted, ScreenRect available);

}  // namespace localcode
