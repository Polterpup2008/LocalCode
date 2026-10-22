#include "SyntaxHighlighter.h"

#include <QFileInfo>

#include "../Theme.h"

namespace localcode::gui {
namespace {

QStringList keywordsFor(Language language) {
    switch (language) {
        case Language::Cpp:
            return {"alignas", "alignof", "auto", "bool", "break", "case", "catch", "char",
                    "class", "concept", "const", "consteval", "constexpr", "constinit",
                    "const_cast", "continue", "co_await", "co_return", "co_yield", "decltype",
                    "default", "delete", "do", "double", "dynamic_cast", "else", "enum",
                    "explicit", "export", "extern", "false", "float", "for", "friend", "goto",
                    "if", "inline", "int", "long", "mutable", "namespace", "new", "noexcept",
                    "nullptr", "operator", "private", "protected", "public", "register",
                    "reinterpret_cast", "requires", "return", "short", "signed", "sizeof",
                    "static", "static_assert", "static_cast", "struct", "switch", "template",
                    "this", "thread_local", "throw", "true", "try", "typedef", "typeid",
                    "typename", "union", "unsigned", "using", "virtual", "void", "volatile",
                    "wchar_t", "while"};
        case Language::Python:
            return {"and", "as", "assert", "async", "await", "break", "class", "continue", "def",
                    "del", "elif", "else", "except", "False", "finally", "for", "from", "global",
                    "if", "import", "in", "is", "lambda", "None", "nonlocal", "not", "or", "pass",
                    "raise", "return", "True", "try", "while", "with", "yield"};
        case Language::Rust:
            return {"as", "async", "await", "break", "const", "continue", "crate", "dyn", "else",
                    "enum", "extern", "false", "fn", "for", "if", "impl", "in", "let", "loop",
                    "match", "mod", "move", "mut", "pub", "ref", "return", "self", "Self",
                    "static", "struct", "super", "trait", "true", "type", "unsafe", "use",
                    "where", "while"};
        case Language::JavaScript:
            return {"async", "await", "break", "case", "catch", "class", "const", "continue",
                    "default", "delete", "do", "else", "export", "extends", "false", "finally",
                    "for", "function", "if", "import", "in", "instanceof", "let", "new", "null",
                    "return", "static", "super", "switch", "this", "throw", "true", "try",
                    "typeof", "var", "void", "while", "yield"};
        case Language::CMake:
            return {"if", "elseif", "else", "endif", "foreach", "endforeach", "while",
                    "endwhile", "function", "endfunction", "macro", "endmacro", "set", "unset",
                    "add_executable", "add_library", "add_subdirectory", "target_link_libraries",
                    "target_include_directories", "target_compile_definitions", "find_package",
                    "include", "option", "message", "project", "cmake_minimum_required",
                    "install", "add_test", "enable_testing", "add_custom_command"};
        case Language::Shell:
            return {"if", "then", "else", "elif", "fi", "for", "while", "do", "done", "case",
                    "esac", "function", "return", "export", "local", "echo", "cd", "set",
                    "param", "process", "begin", "end"};
        case Language::Json:
            return {"true", "false", "null"};
        default:
            return {};
    }
}

QString lineComment(Language language) {
    switch (language) {
        case Language::Python:
        case Language::CMake:
        case Language::Shell:  return "#";
        case Language::Cpp:
        case Language::Rust:
        case Language::JavaScript: return "//";
        default: return {};
    }
}

}  // namespace

Language languageForFile(const QString& fileName) {
    const QString name = QFileInfo(fileName).fileName().toLower();
    const QString suffix = QFileInfo(name).suffix();

    if (name == "cmakelists.txt" || suffix == "cmake") return Language::CMake;
    if (name.startsWith("makefile") || name == "gnumakefile") return Language::Shell;

    static const QHash<QString, Language> kBySuffix = {
        {"c", Language::Cpp},      {"h", Language::Cpp},     {"cc", Language::Cpp},
        {"cpp", Language::Cpp},    {"cxx", Language::Cpp},   {"hpp", Language::Cpp},
        {"hxx", Language::Cpp},    {"inl", Language::Cpp},   {"ipp", Language::Cpp},
        {"cu", Language::Cpp},     {"glsl", Language::Cpp},  {"hlsl", Language::Cpp},
        {"frag", Language::Cpp},   {"vert", Language::Cpp},  {"comp", Language::Cpp},
        {"cs", Language::Cpp},     {"java", Language::Cpp},
        {"py", Language::Python},  {"pyi", Language::Python},
        {"rs", Language::Rust},
        {"js", Language::JavaScript}, {"mjs", Language::JavaScript},
        {"ts", Language::JavaScript}, {"tsx", Language::JavaScript},
        {"jsx", Language::JavaScript},
        {"sh", Language::Shell},   {"bash", Language::Shell}, {"zsh", Language::Shell},
        {"ps1", Language::Shell},  {"psm1", Language::Shell}, {"bat", Language::Shell},
        {"cmd", Language::Shell},
        {"json", Language::Json},  {"jsonc", Language::Json},
        {"xml", Language::Xml},    {"html", Language::Xml},   {"svg", Language::Xml},
        {"ui", Language::Xml},     {"vcxproj", Language::Xml},
        {"md", Language::Markdown}, {"markdown", Language::Markdown},
    };
    return kBySuffix.value(suffix, Language::PlainText);
}

QString languageName(Language language) {
    switch (language) {
        case Language::Cpp:        return "C/C++";
        case Language::Python:     return "Python";
        case Language::Rust:       return "Rust";
        case Language::JavaScript: return "JavaScript";
        case Language::CMake:      return "CMake";
        case Language::Shell:      return "Shell";
        case Language::Json:       return "JSON";
        case Language::Xml:        return "XML";
        case Language::Markdown:   return "Markdown";
        case Language::PlainText:  return "Text";
    }
    return "Text";
}

SyntaxHighlighter::SyntaxHighlighter(QTextDocument* document) : QSyntaxHighlighter(document) {
    rebuildRules();
}

void SyntaxHighlighter::setLanguage(Language language) {
    if (language_ == language) return;
    language_ = language;
    rebuildRules();
    rehighlight();
}

void SyntaxHighlighter::rebuildRules() {
    rules_.clear();
    const auto& p = colours();

    commentFormat_.setForeground(p.synComment);
    commentFormat_.setFontItalic(true);

    if (language_ == Language::PlainText) {
        hasBlockComments_ = false;
        return;
    }

    QTextCharFormat keyword;
    keyword.setForeground(p.synKeyword);
    keyword.setFontWeight(QFont::DemiBold);
    for (const auto& word : keywordsFor(language_)) {
        rules_.push_back({QRegularExpression(QString(R"(\b%1\b)").arg(word)), keyword});
    }

    QTextCharFormat number;
    number.setForeground(p.synNumber);
    rules_.push_back(
        {QRegularExpression(R"(\b(0[xX][0-9a-fA-F']+|\d[\d'.]*([eE][+-]?\d+)?[fFuUlL]*)\b)"),
         number});

    QTextCharFormat function;
    function.setForeground(p.synFunction);
    rules_.push_back({QRegularExpression(R"(\b[A-Za-z_][A-Za-z0-9_]*(?=\s*\())"), function});

    QTextCharFormat type;
    type.setForeground(p.synType);
    if (language_ == Language::Cpp || language_ == Language::Rust) {
        rules_.push_back({QRegularExpression(R"(\b[A-Z][A-Za-z0-9_]{2,}\b)"), type});
    }

    // Strings come after identifiers so they win on overlap.
    QTextCharFormat string;
    string.setForeground(p.synString);
    rules_.push_back({QRegularExpression(R"("([^"\\]|\\.)*")"), string});
    rules_.push_back({QRegularExpression(R"('([^'\\]|\\.)*')"), string});

    if (language_ == Language::Cpp) {
        QTextCharFormat preprocessor;
        preprocessor.setForeground(p.synPreprocessor);
        rules_.push_back({QRegularExpression(R"(^\s*#\s*\w+)"), preprocessor});
        // An include path is a string even though it uses angle brackets.
        rules_.push_back({QRegularExpression(R"(<[A-Za-z0-9_./\\+-]+>)"), string});
    }

    const QString comment = lineComment(language_);
    if (!comment.isEmpty()) {
        rules_.push_back(
            {QRegularExpression(QRegularExpression::escape(comment) + R"([^\n]*)"),
             commentFormat_});
    }

    hasBlockComments_ = language_ == Language::Cpp || language_ == Language::JavaScript ||
                        language_ == Language::Rust;
    if (hasBlockComments_) {
        blockCommentStart_ = QRegularExpression(R"(/\*)");
        blockCommentEnd_ = QRegularExpression(R"(\*/)");
    }
}

void SyntaxHighlighter::highlightBlock(const QString& text) {
    for (const auto& rule : rules_) {
        auto matches = rule.pattern.globalMatch(text);
        while (matches.hasNext()) {
            const auto match = matches.next();
            setFormat(match.capturedStart(), match.capturedLength(), rule.format);
        }
    }

    if (!hasBlockComments_) {
        setCurrentBlockState(0);
        return;
    }

    // Block state carries "inside a comment" across lines, which is the only way
    // a line-at-a-time highlighter can get /* ... */ right.
    setCurrentBlockState(0);
    int start = 0;
    if (previousBlockState() != 1) {
        start = text.indexOf(blockCommentStart_);
    }

    while (start >= 0) {
        const auto end_match = blockCommentEnd_.match(text, start);
        int length = 0;
        if (!end_match.hasMatch()) {
            setCurrentBlockState(1);
            length = text.length() - start;
        } else {
            length = end_match.capturedEnd() - start;
        }
        setFormat(start, length, commentFormat_);
        start = text.indexOf(blockCommentStart_, start + length);
    }
}

}  // namespace localcode::gui
