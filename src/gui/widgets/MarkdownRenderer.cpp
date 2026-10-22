#include "MarkdownRenderer.h"

#include <QRegularExpression>
#include <QSet>
#include <QStringView>

#include "../Theme.h"

namespace localcode::gui {
namespace {

QString colour(const QColor& c) { return c.name(QColor::HexRgb); }

// Keyword sets for the languages that actually turn up in a coding session.
const QSet<QString>& keywordsFor(const QString& language) {
    static const QSet<QString> cpp = {
        "alignas", "alignof", "auto", "bool", "break", "case", "catch", "char", "class",
        "concept", "const", "consteval", "constexpr", "constinit", "const_cast", "continue",
        "co_await", "co_return", "co_yield", "decltype", "default", "delete", "do", "double",
        "dynamic_cast", "else", "enum", "explicit", "export", "extern", "false", "float",
        "for", "friend", "goto", "if", "inline", "int", "long", "mutable", "namespace", "new",
        "noexcept", "nullptr", "operator", "private", "protected", "public", "register",
        "reinterpret_cast", "requires", "return", "short", "signed", "sizeof", "static",
        "static_assert", "static_cast", "struct", "switch", "template", "this", "thread_local",
        "throw", "true", "try", "typedef", "typeid", "typename", "union", "unsigned", "using",
        "virtual", "void", "volatile", "wchar_t", "while"};
    static const QSet<QString> python = {
        "and", "as", "assert", "async", "await", "break", "class", "continue", "def", "del",
        "elif", "else", "except", "False", "finally", "for", "from", "global", "if", "import",
        "in", "is", "lambda", "None", "nonlocal", "not", "or", "pass", "raise", "return",
        "True", "try", "while", "with", "yield"};
    static const QSet<QString> rust = {
        "as", "async", "await", "break", "const", "continue", "crate", "dyn", "else", "enum",
        "extern", "false", "fn", "for", "if", "impl", "in", "let", "loop", "match", "mod",
        "move", "mut", "pub", "ref", "return", "self", "Self", "static", "struct", "super",
        "trait", "true", "type", "unsafe", "use", "where", "while"};
    static const QSet<QString> javascript = {
        "async", "await", "break", "case", "catch", "class", "const", "continue", "debugger",
        "default", "delete", "do", "else", "export", "extends", "false", "finally", "for",
        "function", "if", "import", "in", "instanceof", "let", "new", "null", "return",
        "static", "super", "switch", "this", "throw", "true", "try", "typeof", "var", "void",
        "while", "yield"};
    static const QSet<QString> cmake = {
        "if", "elseif", "else", "endif", "foreach", "endforeach", "while", "endwhile",
        "function", "endfunction", "macro", "endmacro", "set", "unset", "add_executable",
        "add_library", "add_subdirectory", "target_link_libraries", "target_include_directories",
        "find_package", "include", "option", "message", "project", "cmake_minimum_required",
        "install", "add_test", "enable_testing"};
    static const QSet<QString> shell = {
        "if", "then", "else", "elif", "fi", "for", "while", "do", "done", "case", "esac",
        "function", "return", "export", "local", "echo", "cd", "set"};
    static const QSet<QString> empty;

    const auto lang = language.toLower();
    if (lang == "cpp" || lang == "c++" || lang == "c" || lang == "cc" || lang == "hpp" ||
        lang == "h" || lang == "cxx" || lang == "cs" || lang == "java" || lang == "glsl" ||
        lang == "hlsl") {
        return cpp;
    }
    if (lang == "python" || lang == "py") return python;
    if (lang == "rust" || lang == "rs") return rust;
    if (lang == "javascript" || lang == "js" || lang == "typescript" || lang == "ts") {
        return javascript;
    }
    if (lang == "cmake") return cmake;
    if (lang == "bash" || lang == "sh" || lang == "shell" || lang == "zsh" ||
        lang == "powershell" || lang == "ps1" || lang == "console" || lang == "batch") {
        return shell;
    }
    return empty;
}

bool isIdentifierChar(QChar c) { return c.isLetterOrNumber() || c == '_'; }

// Line-comment prefix for a language, empty when it has none we recognise.
QString lineCommentFor(const QString& language) {
    const auto lang = language.toLower();
    if (lang == "python" || lang == "py" || lang == "bash" || lang == "sh" || lang == "shell" ||
        lang == "cmake" || lang == "yaml" || lang == "yml" || lang == "toml" || lang == "ps1" ||
        lang == "powershell") {
        return "#";
    }
    if (lang == "sql") return "--";
    if (lang.isEmpty()) return {};
    return "//";
}

}  // namespace

namespace {
void appendEscaped(QString& out, QChar c) {
    switch (c.unicode()) {
        case '&':  out += "&amp;";  break;
        case '<':  out += "&lt;";   break;
        case '>':  out += "&gt;";   break;
        case '"':  out += "&quot;"; break;
        case '\'': out += "&#39;";  break;
        default:   out += c;        break;
    }
}
} // namespace

QString MarkdownRenderer::escape(const QString& text) {
    QString out;
    out.reserve(text.size());
    for (const QChar c : text) appendEscaped(out, c);
    return out;
}

QString MarkdownRenderer::highlight(const QString& code, const QString& language) {
    const auto& p = colours();
    const auto& keywords = keywordsFor(language);
    const QString comment_prefix = lineCommentFor(language);

    QString out;
    out.reserve(code.size() * 2);

    const QStringList lines = code.split('\n');
    bool in_block_comment = false;

    for (int index = 0; index < lines.size(); ++index) {
        const QString& line = lines[index];
        const QStringView lineView(line);
        if (index) out += '\n';

        int i = 0;
        while (i < line.size()) {
            // Block comments span lines, so the state is carried across them.
            if (in_block_comment) {
                const int end = line.indexOf("*/", i);
                const QString chunk = end < 0 ? line.mid(i) : line.mid(i, end - i + 2);
                out += QString("<span style=\"color:%1\">%2</span>")
                           .arg(colour(p.synComment), escape(chunk));
                if (end < 0) break;
                in_block_comment = false;
                i = end + 2;
                continue;
            }

            if (!comment_prefix.isEmpty() && lineView.sliced(i).startsWith(QStringView(comment_prefix))) {
                out += QString("<span style=\"color:%1\">%2</span>")
                           .arg(colour(p.synComment), escape(line.mid(i)));
                break;
            }
            if (lineView.sliced(i).startsWith(QLatin1StringView("/*"))) {
                in_block_comment = true;
                continue;
            }

            const QChar c = line[i];

            // Strings and character literals.
            if (c == '"' || c == '\'' || c == '`') {
                const QChar quote = c;
                int j = i + 1;
                while (j < line.size()) {
                    if (line[j] == '\\') { j += 2; continue; }
                    if (line[j] == quote) { ++j; break; }
                    ++j;
                }
                out += QString("<span style=\"color:%1\">%2</span>")
                           .arg(colour(p.synString), escape(line.mid(i, j - i)));
                i = j;
                continue;
            }

            // Preprocessor directives.
            if (c == '#' && lineView.first(i).trimmed().isEmpty() && comment_prefix != "#") {
                out += QString("<span style=\"color:%1\">%2</span>")
                           .arg(colour(p.synPreprocessor), escape(line.mid(i)));
                break;
            }

            // Numbers.
            if (c.isDigit() && (i == 0 || !isIdentifierChar(line[i - 1]))) {
                int j = i;
                while (j < line.size() &&
                       (line[j].isLetterOrNumber() || line[j] == '.' || line[j] == '\'')) {
                    ++j;
                }
                out += QString("<span style=\"color:%1\">%2</span>")
                           .arg(colour(p.synNumber), escape(line.mid(i, j - i)));
                i = j;
                continue;
            }

            // Identifiers: keyword, function call, or plain.
            if (c.isLetter() || c == '_') {
                int j = i;
                while (j < line.size() && isIdentifierChar(line[j])) ++j;
                const QString word = line.mid(i, j - i);

                if (keywords.contains(word)) {
                    out += QString("<span style=\"color:%1;font-weight:600\">%2</span>")
                               .arg(colour(p.synKeyword), escape(word));
                } else {
                    // A '(' immediately after makes it a call site.
                    int k = j;
                    while (k < line.size() && line[k] == ' ') ++k;
                    const bool is_call = k < line.size() && line[k] == '(';
                    if (is_call) {
                        out += QString("<span style=\"color:%1\">%2</span>")
                                   .arg(colour(p.synFunction), escape(word));
                    } else if (word.size() > 2 && word[0].isUpper()) {
                        out += QString("<span style=\"color:%1\">%2</span>")
                                   .arg(colour(p.synType), escape(word));
                    } else {
                        out += escape(word);
                    }
                }
                i = j;
                continue;
            }

            appendEscaped(out, c);
            ++i;
        }
    }
    return out;
}

QString MarkdownRenderer::renderInline(const QString& text) {
    // Inline code is extracted first and restored last, so its contents are
    // never treated as emphasis or link syntax.
    QStringList codes;
    QString working;
    working.reserve(text.size());

    for (int i = 0; i < text.size(); ++i) {
        if (text[i] == '`') {
            int ticks = 0;
            while (i + ticks < text.size() && text[i + ticks] == '`') ++ticks;
            const QString fence(ticks, '`');
            const int close = text.indexOf(fence, i + ticks);
            if (close > 0) {
                codes << text.mid(i + ticks, close - i - ticks);
                working += QString("\x01%1\x02").arg(codes.size() - 1);
                i = close + ticks - 1;
                continue;
            }
        }
        working += text[i];
    }

    QString html = escape(working);

    // Links: [text](url). Only http(s) and file URLs are made clickable.
    static const QRegularExpression link(R"(\[([^\]]+)\]\(([^)\s]+)\))");
    html.replace(link, QString("<a style=\"color:%1;text-decoration:none\" href=\"\\2\">\\1</a>")
                           .arg(colour(colours().accent)));

    // Bold then italic, longest delimiter first so ** is not eaten by *.
    static const QRegularExpression bold(R"(\*\*([^*]+)\*\*)");
    html.replace(bold, "<b>\\1</b>");
    static const QRegularExpression boldUnderscore(R"(__([^_]+)__)");
    html.replace(boldUnderscore, "<b>\\1</b>");
    static const QRegularExpression italic(R"((?<![\w*])\*([^*\n]+)\*(?![\w*]))");
    html.replace(italic, "<i>\\1</i>");
    static const QRegularExpression strike(R"(~~([^~]+)~~)");
    html.replace(strike, "<s>\\1</s>");

    // Restore inline code.
    const auto& p = colours();
    for (int i = 0; i < codes.size(); ++i) {
        const QString replacement =
            QString("<code style=\"background:%1;color:%2;padding:1px 4px;"
                    "border-radius:3px;font-family:'%3'\">%4</code>")
                .arg(colour(p.surfaceRaised), colour(p.synString),
                     Theme::instance().monoFont().family(), escape(codes[i]));
        html.replace(QString("\x01%1\x02").arg(i), replacement);
    }
    return html;
}

QString MarkdownRenderer::renderBlocks(const QStringList& lines, bool streaming) {
    const auto& p = colours();
    const QString mono = Theme::instance().monoFont().family();

    QString html;
    html.reserve(lines.size() * 80);

    bool in_list = false;
    bool ordered_list = false;
    int  code_index = 0;

    auto closeList = [&] {
        if (in_list) {
            html += ordered_list ? "</ol>" : "</ul>";
            in_list = false;
        }
    };

    for (int i = 0; i < lines.size(); ++i) {
        const QString& raw = lines[i];
        const QString trimmed = raw.trimmed();

        // --- Fenced code ---------------------------------------------------
        if (trimmed.startsWith("```") || trimmed.startsWith("~~~")) {
            closeList();
            const QString fence = trimmed.left(3);
            const QString language = trimmed.mid(3).trimmed();

            QStringList code;
            int j = i + 1;
            bool closed = false;
            for (; j < lines.size(); ++j) {
                if (lines[j].trimmed().startsWith(fence)) {
                    closed = true;
                    break;
                }
                code << lines[j];
            }
            // While streaming, an unterminated fence is rendered as a code block
            // anyway - otherwise the view flickers as the closing fence arrives.
            if (!closed && !streaming) {
                // A stray fence in a finished message: treat it as text.
                html += "<p>" + renderInline(raw) + "</p>";
                continue;
            }
            i = j;

            const QString label = language.isEmpty() ? QStringLiteral("text") : language;
            html += QString(
                        "<div style=\"margin:8px 0\">"
                        "<div style=\"background:%1;border:1px solid %2;border-bottom:none;"
                        "border-top-left-radius:4px;border-top-right-radius:4px;"
                        "padding:3px 8px;color:%3;font-size:10px\">"
                        "%4 &nbsp;&middot;&nbsp; <a href=\"localcode:copy/%5\" "
                        "style=\"color:%6;text-decoration:none\">copy</a>"
                        "</div>"
                        "<pre style=\"background:%7;border:1px solid %2;margin:0;"
                        "border-bottom-left-radius:4px;border-bottom-right-radius:4px;"
                        "padding:8px 10px;font-family:'%8';white-space:pre\">%9</pre>"
                        "</div>")
                        .arg(colour(p.surfaceRaised), colour(p.border), colour(p.textMuted),
                             escape(label), QString::number(code_index++), colour(p.accent),
                             colour(p.surfaceAlt), mono,
                             highlight(code.join('\n'), language));
            continue;
        }

        // --- Blank line ----------------------------------------------------
        if (trimmed.isEmpty()) {
            closeList();
            continue;
        }

        // --- Headings ------------------------------------------------------
        if (trimmed.startsWith('#')) {
            int level = 0;
            while (level < trimmed.size() && trimmed[level] == '#') ++level;
            if (level <= 6 && level < trimmed.size() && trimmed[level] == ' ') {
                closeList();
                const int size = qMax(11, 18 - level * 2);
                html += QString("<div style=\"font-size:%1px;font-weight:600;color:%2;"
                                "margin:12px 0 4px 0\">%3</div>")
                            .arg(size)
                            .arg(colour(p.text), renderInline(trimmed.mid(level + 1)));
                continue;
            }
        }

        // --- Horizontal rule -----------------------------------------------
        if (trimmed == "---" || trimmed == "***" || trimmed == "___") {
            closeList();
            html += QString("<div style=\"border-top:1px solid %1;margin:10px 0\"></div>")
                        .arg(colour(p.border));
            continue;
        }

        // --- Block quote ---------------------------------------------------
        if (trimmed.startsWith("> ")) {
            closeList();
            html += QString("<div style=\"border-left:3px solid %1;padding:2px 0 2px 10px;"
                            "margin:6px 0;color:%2\">%3</div>")
                        .arg(colour(p.borderStrong), colour(p.textMuted),
                             renderInline(trimmed.mid(2)));
            continue;
        }

        // --- Table ---------------------------------------------------------
        if (trimmed.startsWith('|') && i + 1 < lines.size() &&
            lines[i + 1].trimmed().startsWith('|') && lines[i + 1].contains("---")) {
            closeList();
            html += QString("<table cellspacing=\"0\" cellpadding=\"5\" "
                            "style=\"border-collapse:collapse;margin:8px 0\">");

            auto cells = [](const QString& row) {
                QString inner = row.trimmed();
                if (inner.startsWith('|')) inner.remove(0, 1);
                if (inner.endsWith('|')) inner.chop(1);
                return inner.split('|');
            };

            const auto header = cells(trimmed);
            html += "<tr>";
            for (const auto& cell : header) {
                html += QString("<th style=\"border:1px solid %1;background:%2;color:%3;"
                                "text-align:left\">%4</th>")
                            .arg(colour(p.border), colour(p.surfaceRaised), colour(p.textMuted),
                                 renderInline(cell.trimmed()));
            }
            html += "</tr>";

            int j = i + 2;
            for (; j < lines.size(); ++j) {
                const QString row = lines[j].trimmed();
                if (!row.startsWith('|')) break;
                html += "<tr>";
                for (const auto& cell : cells(row)) {
                    html += QString("<td style=\"border:1px solid %1\">%2</td>")
                                .arg(colour(p.border), renderInline(cell.trimmed()));
                }
                html += "</tr>";
            }
            html += "</table>";
            i = j - 1;
            continue;
        }

        // --- Lists ---------------------------------------------------------
        static const QRegularExpression unordered(R"(^\s*[-*+]\s+(.*)$)");
        static const QRegularExpression ordered(R"(^\s*\d+[.)]\s+(.*)$)");

        if (const auto match = unordered.match(raw); match.hasMatch()) {
            if (!in_list || ordered_list) {
                closeList();
                html += QString("<ul style=\"margin:4px 0 4px 18px\">");
                in_list = true;
                ordered_list = false;
            }
            html += "<li>" + renderInline(match.captured(1)) + "</li>";
            continue;
        }
        if (const auto match = ordered.match(raw); match.hasMatch()) {
            if (!in_list || !ordered_list) {
                closeList();
                html += QString("<ol style=\"margin:4px 0 4px 18px\">");
                in_list = true;
                ordered_list = true;
            }
            html += "<li>" + renderInline(match.captured(1)) + "</li>";
            continue;
        }

        // --- Paragraph -------------------------------------------------------
        closeList();
        html += "<p style=\"margin:4px 0\">" + renderInline(raw) + "</p>";
    }
    closeList();
    return html;
}

QString MarkdownRenderer::toHtml(const QString& markdown) {
    return renderBlocks(markdown.split('\n'), /*streaming=*/false);
}

QString MarkdownRenderer::toHtmlStreaming(const QString& markdown) {
    return renderBlocks(markdown.split('\n'), /*streaming=*/true);
}

QList<MarkdownRenderer::CodeBlock> MarkdownRenderer::codeBlocks(const QString& markdown) {
    QList<CodeBlock> blocks;
    const QStringList lines = markdown.split('\n');

    for (int i = 0; i < lines.size(); ++i) {
        const QString trimmed = lines[i].trimmed();
        if (!trimmed.startsWith("```") && !trimmed.startsWith("~~~")) continue;

        CodeBlock block;
        block.language = trimmed.mid(3).trimmed();
        const QString fence = trimmed.left(3);

        QStringList code;
        int j = i + 1;
        for (; j < lines.size(); ++j) {
            if (lines[j].trimmed().startsWith(fence)) break;
            code << lines[j];
        }
        block.code = code.join('\n');
        blocks << block;
        i = j;
    }
    return blocks;
}

}  // namespace localcode::gui
