#pragma once
// ---------------------------------------------------------------------------
// Colours, fonts and the application stylesheet.
//
// The look is a developer tool: flat surfaces, restrained accents, dense but
// readable spacing. Colours are named by role rather than by value so a light
// theme is the same layout with a different palette, not a second stylesheet.
// ---------------------------------------------------------------------------
#include <QColor>
#include <QFont>
#include <QString>

namespace localcode::gui {

enum class ThemeMode { Dark, Light };

struct Palette {
    QColor window;          // outermost background
    QColor surface;         // panels
    QColor surfaceAlt;      // input fields, list rows
    QColor surfaceRaised;   // hovered rows, popups
    QColor border;
    QColor borderStrong;

    QColor text;
    QColor textMuted;
    QColor textFaint;
    QColor textInverse;

    QColor accent;          // selection, focus, the send button
    QColor accentHover;
    QColor accentMuted;

    QColor success;
    QColor warning;
    QColor danger;
    QColor info;

    // Diff and git colouring.
    QColor addedBackground;
    QColor addedText;
    QColor removedBackground;
    QColor removedText;

    // Syntax highlighting.
    QColor synKeyword;
    QColor synType;
    QColor synString;
    QColor synNumber;
    QColor synComment;
    QColor synPreprocessor;
    QColor synFunction;
};

class Theme {
public:
    static Theme& instance();

    void setMode(ThemeMode mode);
    [[nodiscard]] ThemeMode mode() const { return mode_; }

    [[nodiscard]] const Palette& palette() const { return palette_; }

    // Applies the palette and stylesheet to the whole application.
    void apply();

    [[nodiscard]] QString styleSheet() const;

    // Interface font, and the monospaced font used for code and output.
    [[nodiscard]] QFont uiFont() const;
    [[nodiscard]] QString richTextStyle() const;
    [[nodiscard]] QFont monoFont(int pointSize = 0) const;
    void setMonoPointSize(int pointSize);
    [[nodiscard]] int monoPointSize() const { return monoPointSize_; }

    // Picks the first installed font from a preference list.
    [[nodiscard]] static QString firstAvailableFamily(const QStringList& candidates,
                                                      const QString& fallback);

private:
    Theme();
    void buildPalette();

    ThemeMode mode_ = ThemeMode::Dark;
    Palette   palette_;
    int       monoPointSize_ = 10;
};

// Named `colours` rather than `palette` because QWidget::palette() would
// otherwise shadow it inside every widget subclass.
inline const Palette& colours() { return Theme::instance().palette(); }

}  // namespace localcode::gui
