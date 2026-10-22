#pragma once
// ---------------------------------------------------------------------------
// Markdown to HTML for the chat view.
//
// Qt's built-in setMarkdown() is not usable here: it replaces the whole
// document, which makes streaming repaint everything on every token, and it
// gives no control over code-block presentation. This renderer produces HTML
// styled to the application palette, with syntax-highlighted code blocks that
// carry a language label and a copy affordance.
//
// It handles the subset that actually appears in a coding assistant's replies -
// headings, lists, emphasis, inline code, fenced code, links, tables, block
// quotes and rules - and escapes everything else rather than passing raw HTML
// through, so a model cannot inject markup into the view.
// ---------------------------------------------------------------------------
#include <QString>
#include <QStringList>

namespace localcode::gui {

class MarkdownRenderer {
public:
    // Renders a complete document.
    [[nodiscard]] static QString toHtml(const QString& markdown);

    // Renders a partial document that is still streaming: an unterminated code
    // fence is closed so the view does not flicker between styles as the
    // closing ``` arrives.
    [[nodiscard]] static QString toHtmlStreaming(const QString& markdown);

    // Extracts the fenced code blocks, so the copy buttons have something to
    // copy and the chat view can index them.
    struct CodeBlock {
        QString language;
        QString code;
    };
    [[nodiscard]] static QList<CodeBlock> codeBlocks(const QString& markdown);

    [[nodiscard]] static QString escape(const QString& text);

    // Colourises one code block for display. Falls back to plain text for
    // languages it does not know, which is the honest behaviour.
    [[nodiscard]] static QString highlight(const QString& code, const QString& language);

private:
    static QString renderInline(const QString& text);
    static QString renderBlocks(const QStringList& lines, bool streaming);
};

}  // namespace localcode::gui
