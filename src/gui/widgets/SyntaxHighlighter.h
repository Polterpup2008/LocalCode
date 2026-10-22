#pragma once
// ---------------------------------------------------------------------------
// Syntax highlighting for the source editor.
//
// Rule-driven rather than per-language classes: a language is a keyword set
// plus a comment style, so adding one is data, not code. Multi-line comments
// are handled with block state, which is what QSyntaxHighlighter provides it for.
// ---------------------------------------------------------------------------
#include <QRegularExpression>
#include <QString>
#include <QSyntaxHighlighter>
#include <QTextCharFormat>
#include <QVector>

namespace localcode::gui {

enum class Language { PlainText, Cpp, Python, Rust, JavaScript, CMake, Shell, Json, Xml, Markdown };

Language languageForFile(const QString& fileName);
QString  languageName(Language language);

class SyntaxHighlighter : public QSyntaxHighlighter {
    Q_OBJECT

public:
    explicit SyntaxHighlighter(QTextDocument* document);

    void setLanguage(Language language);
    [[nodiscard]] Language language() const { return language_; }

protected:
    void highlightBlock(const QString& text) override;

private:
    struct Rule {
        QRegularExpression pattern;
        QTextCharFormat    format;
    };

    void rebuildRules();

    Language        language_ = Language::PlainText;
    QVector<Rule>   rules_;
    QRegularExpression blockCommentStart_;
    QRegularExpression blockCommentEnd_;
    QTextCharFormat    commentFormat_;
    bool               hasBlockComments_ = false;
};

}  // namespace localcode::gui
