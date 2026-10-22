#include "ScreenGuard.h"

#include <QGuiApplication>
#include <QRect>
#include <QScreen>
#include <QWidget>

#include "platform/ScreenFit.h"
#include "util/Log.h"

namespace localcode::gui {
namespace {

ScreenRect toRect(const QRect& r) { return {r.x(), r.y(), r.width(), r.height()}; }

}  // namespace

bool clampToScreen(QWidget& widget) {
    if (widget.isMaximized() || widget.isFullScreen()) return false;

    // The screen the window mostly lands on, falling back to the primary one
    // when it lands on none of them - which is exactly the unplugged-monitor
    // case this exists for.
    const QScreen* screen = QGuiApplication::screenAt(widget.frameGeometry().center());
    if (screen == nullptr) screen = QGuiApplication::primaryScreen();
    if (screen == nullptr) return false;

    const QRect frame = widget.frameGeometry();
    const auto fitted = fit_inside(toRect(frame), toRect(screen->availableGeometry()));
    const QRect wanted{fitted.x, fitted.y, fitted.width, fitted.height};
    if (wanted == frame) return false;

    // resize() excludes window decorations, while move() positions the frame.
    // setGeometry() positions the client area and would apply the frame offset
    // twice when given the desired frame position.
    const QSize decoration = frame.size() - widget.size();
    widget.resize(wanted.size() - decoration);
    widget.move(wanted.topLeft());

    log_info(LogChannel::App, "window was off-screen and was clamped",
             {{"widget", widget.objectName().isEmpty()
                             ? std::string{widget.metaObject()->className()}
                             : widget.objectName().toStdString()},
              {"screen", screen->name().toStdString()},
              {"from", std::to_string(frame.x()) + "," + std::to_string(frame.y()) + " " +
                           std::to_string(frame.width()) + "x" + std::to_string(frame.height())},
              {"to", std::to_string(wanted.x()) + "," + std::to_string(wanted.y()) + " " +
                         std::to_string(wanted.width()) + "x" + std::to_string(wanted.height())}});
    return true;
}

}  // namespace localcode::gui
