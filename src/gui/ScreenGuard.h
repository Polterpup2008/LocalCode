#pragma once
// ---------------------------------------------------------------------------
// Keeping a top-level window reachable.
//
// Qt restores and centres the numbers it is given; it does not check that they
// still describe somewhere a person can click. The main window needed this for
// a geometry saved on a monitor that is no longer there, and the Settings dialog
// needed it because a tall dialog centred on a tall parent can put its own
// buttons below the bottom of the screen.
//
// The arithmetic is in localcode::fit_inside, in core, where it is tested.
// ---------------------------------------------------------------------------
class QWidget;

namespace localcode::gui {

// Moves - and shrinks first if it must - `widget` so its frame lies wholly
// within the available area of the screen it is on. Maximised and full-screen
// windows are left alone. Returns true if anything moved.
bool clampToScreen(QWidget& widget);

}  // namespace localcode::gui
