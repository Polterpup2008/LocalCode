#include <QAbstractItemModel>
#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFontDatabase>
#include <QFutureWatcher>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QScreen>
#include <QScrollArea>
#include <QScrollBar>
#include <QSemaphore>
#include <QSettings>
#include <QSpinBox>
#include <QStackedWidget>
#include <QStyleFactory>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTextBrowser>
#include <QThreadPool>
#include <QTimer>
#include <QToolBar>
#include <QtConcurrent/QtConcurrent>
#include <catch2/catch_session.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <thread>

#include "MainWindow.h"
#include "ScreenGuard.h"
#include "Theme.h"
#include "dialogs/InferenceTuningDialog.h"
#include "dialogs/SettingsDialog.h"
#include "platform/FileSystem.h"
#include "platform/PathEncoding.h"
#include "platform/Paths.h"
#include "plugins/DiscordPlugin.h"
#include "support/MockBackend.h"
#include "util/Log.h"
#include "widgets/ActivityTimeline.h"
#include "widgets/ChatView.h"
#include "widgets/ModelPanel.h"
#include "widgets/OutputPanel.h"
#include "widgets/PluginsPanel.h"
#include "widgets/ProjectExplorer.h"
#include "widgets/PromptComposer.h"
#include "widgets/SourceEditor.h"
#include "widgets/StatusStrip.h"
#include "widgets/TaskPanel.h"
#include "widgets/VerificationPanel.h"

using namespace localcode;
using namespace localcode::gui;

TEST_CASE("scope completion updates the streamed reply without duplicating it",
          "[gui][chat][completion]") {
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    const auto original = app_paths().data_dir();

    struct Restore {
        std::filesystem::path path;

        ~Restore() { app_paths().set_data_dir(path); }
    } restore{original};

    app_paths().set_data_dir(path_from_utf8(directory.path().toStdString()));
    Application application;
    REQUIRE(application.initialise());
    MainWindow window(application);
    auto* chat = window.findChild<ChatView*>();
    REQUIRE(chat);
    chat->setShowReasoning(true);
    chat->addMessage(ChatRole::User, "Use file tools only.");
    chat->beginAssistantMessage();
    chat->appendReasoning("Retained native reasoning.");
    chat->appendDelta("The addition line is corrected.");
    SECTION("the stream is still active") {}
    SECTION("the controller already completed the stream") {
        chat->endAssistantMessage();
    }
    UiOutcome outcome;
    outcome.completed = true;
    outcome.finalMessage =
        "The addition line is corrected.\n\n*Not verified: execution checks were outside this "
        "file-only task.*";
    outcome.taskVerification = "not verified";
    REQUIRE(QMetaObject::invokeMethod(&window, "onRunFinished", Qt::DirectConnection,
                                      Q_ARG(UiOutcome, outcome)));
    const auto transcript = chat->toMarkdown();
    CHECK(transcript.count("The addition line is corrected.") == 1);
    REQUIRE(QMetaObject::invokeMethod(chat, "handleAnchorClicked", Qt::DirectConnection,
                                      Q_ARG(QUrl, QUrl("localcode:reasoning/1"))));
    auto* browser = chat->findChild<QTextBrowser*>();
    REQUIRE(browser);
    CHECK(browser->toPlainText().count("Retained native reasoning.") == 1);
    CHECK(transcript.contains("Not verified: execution checks were outside this file-only task."));
    CHECK_FALSE(chat->streaming());
    CHECK(window.findChild<QLabel*>("TaskSummary")->text() == "Answered");
    REQUIRE(QMetaObject::invokeMethod(&window, "onRunFinished", Qt::DirectConnection,
                                      Q_ARG(UiOutcome, outcome)));
    CHECK(chat->toMarkdown() == transcript);
}

TEST_CASE("final reply reconciliation preserves distinct messages and notices",
          "[gui][chat][completion]") {
    ChatView chat;
    chat.addMessage(ChatRole::Assistant, "Earlier reply.");
    SECTION("a different final reply is another message") {
        chat.setFinalMessage("Different final reply.");
        CHECK(chat.toMarkdown().contains("Earlier reply."));
        CHECK(chat.toMarkdown().contains("Different final reply."));
        CHECK(chat.toMarkdown().count("## LocalCode") == 2);
    }
    SECTION("an intervening notice is preserved") {
        chat.addMessage(ChatRole::Notice, "Application notice.");
        chat.setFinalMessage("Earlier reply.\n\nScope note.");
        CHECK(chat.toMarkdown().contains("## Notice\n\nApplication notice."));
        CHECK(chat.toMarkdown().count("## LocalCode") == 2);
    }
    SECTION("whitespace-only differences do not duplicate an answer") {
        chat.setFinalMessage("  Earlier reply.\n");
        CHECK(chat.toMarkdown().count("Earlier reply.") == 1);
    }
}

TEST_CASE("source editor preserves non-ANSI file paths on read and save", "[gui][unicode]") {
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    const auto folder = directory.filePath(QString::fromUtf8("projet é 路径 🦍"));
    REQUIRE(QDir().mkpath(folder));
    const auto file = folder + QString::fromUtf8("/calcul é 路径 🦍.py");
    const auto native = path_from_utf8(file.toStdString());
    REQUIRE(fs::write_text_atomic(native, "def add(a, b):\n    return a - b\n"));
    SourceView view;
    QString error;
    REQUIRE(view.loadFile(file, &error));
    CHECK(view.toPlainText().contains("return a - b"));
    view.setPlainText("def add(a, b):\n    return a + b\n");
    REQUIRE(view.saveFile(&error));
    const auto saved = fs::read_text(native);
    REQUIRE(saved);
    CHECK(saved->content == "def add(a, b):\n    return a + b\n");
    CHECK_FALSE(view.modified());
}

TEST_CASE("source editor rejects a truncated file without replacing the current document",
          "[gui][editor][data-loss]") {
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    const auto small = directory.filePath("current.py");
    const auto large = directory.filePath("oversized.txt");
    REQUIRE(fs::write_text_atomic(path_from_utf8(small.toStdString()), "original = 1\n"));
    constexpr int limit = 16 * 1024 * 1024;
    const QByteArray tail("\nUNREAD_TAIL_MUST_SURVIVE\n");
    QByteArray oversized(limit, 'x');
    oversized += tail;
    {
        QFile file(large);
        REQUIRE(file.open(QIODevice::WriteOnly));
        REQUIRE(file.write(oversized) == oversized.size());
    }

    SourceView view;
    QString error;
    REQUIRE(view.loadFile(small, &error));
    view.moveCursor(QTextCursor::End);
    view.insertPlainText("local_edit = 2\n");
    const auto previous = view.toPlainText();
    const auto cursor = view.textCursor().position();
    REQUIRE(view.modified());
    REQUIRE(view.document()->isUndoAvailable());

    REQUIRE_FALSE(view.loadFile(large, &error));
    CHECK(error.contains("16 MiB"));
    CHECK(error.contains("external editor"));
    CHECK(view.filePath() == small);
    CHECK(view.toPlainText() == previous);
    CHECK(view.textCursor().position() == cursor);
    CHECK(view.modified());
    CHECK(view.document()->isUndoAvailable());

    REQUIRE(view.saveFile(&error));
    const auto saved = fs::read_text(path_from_utf8(small.toStdString()));
    REQUIRE(saved);
    CHECK(saved->content == previous.toStdString());
    QFile preserved(large);
    REQUIRE(preserved.open(QIODevice::ReadOnly));
    CHECK(preserved.size() == oversized.size());
    REQUIRE(preserved.seek(limit));
    CHECK(preserved.readAll() == tail);
}

TEST_CASE("unchanged source refresh preserves selection scroll and saved undo",
          "[gui][editor][refresh]") {
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    const auto path = directory.filePath(QString::fromUtf8("source é 路径.py"));
    const auto native = path_from_utf8(path.toStdString());
    QString original;
    for (int line = 0; line < 120; ++line)
        original += QString("value_%1 = %1\n").arg(line);
    REQUIRE(fs::write_text_atomic(native, original.toStdString()));
    SourceEditor editor;
    editor.resize(700, 400);
    editor.show();
    editor.openFile(path);
    QApplication::processEvents();
    auto* view = editor.findChild<SourceView*>();
    REQUIRE(view);
    view->moveCursor(QTextCursor::End);
    view->insertPlainText("saved = True\n");
    QString error;
    REQUIRE(editor.saveCurrent(&error));
    REQUIRE(view->document()->isUndoAvailable());
    const auto saved = view->toPlainText();
    auto cursor = view->textCursor();
    cursor.setPosition(20);
    cursor.setPosition(35, QTextCursor::KeepAnchor);
    view->setTextCursor(cursor);
    view->verticalScrollBar()->setValue(25);
    const int scroll = view->verticalScrollBar()->value();
    int resets = 0;
    QObject::connect(view->document(), &QTextDocument::contentsChanged, &editor,
                     [&resets] { ++resets; });
    editor.reloadUnmodified();
    CHECK(resets == 0);
    CHECK(view->toPlainText() == saved);
    CHECK(view->textCursor().anchor() == 20);
    CHECK(view->textCursor().position() == 35);
    CHECK(view->verticalScrollBar()->value() == scroll);
    CHECK(view->document()->isUndoAvailable());
    const auto evidence = qEnvironmentVariable("LOCALCODE_GUI_EVIDENCE_DIR");
    if (!evidence.isEmpty()) {
        REQUIRE(QDir().mkpath(evidence));
        REQUIRE(editor.grab().save(evidence + "/source-refresh-selection.png"));
    }
    view->undo();
    CHECK(view->toPlainText() == original);
    REQUIRE(view->modified());
    REQUIRE(fs::write_text_atomic(native, "external edit\n"));
    editor.reloadUnmodified();
    CHECK(view->toPlainText() == original);
    CHECK_FALSE(view->reloadIfChanged(&error));
    CHECK(error.isEmpty());
    CHECK(view->toPlainText() == original);
}

TEST_CASE("source refresh detects equal metadata and encoding-only replacements",
          "[gui][editor][refresh]") {
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    const auto path = directory.filePath("fixture.cpp");
    const auto native = path_from_utf8(path.toStdString());
    const std::string original = "int original = 1;\n";
    const std::string external = "int external = 2;\n";
    REQUIRE(original.size() == external.size());
    REQUIRE(fs::write_text_atomic(native, original));
    const auto stamp = std::filesystem::last_write_time(native);
    SourceView view;
    QString error;
    REQUIRE(view.loadFile(path, &error));
    REQUIRE(fs::write_text_atomic(native, external));
    std::filesystem::last_write_time(native, stamp);
    REQUIRE(std::filesystem::last_write_time(native) == stamp);
    REQUIRE(std::filesystem::file_size(native) == original.size());
    REQUIRE(view.reloadIfChanged(&error));
    CHECK(view.toPlainText().toStdString() == external);
    CHECK_FALSE(view.reloadIfChanged(&error));
    CHECK(error.isEmpty());
    REQUIRE(
        fs::write_text_atomic(native, external, fs::TextEncoding::Utf16Le, fs::LineEnding::CrLf));
    REQUIRE(view.reloadIfChanged(&error));
    CHECK(view.encodingName() == "utf-16le");
    CHECK(view.toPlainText().toStdString() == external);
    REQUIRE(view.saveFile(&error));
    CHECK_FALSE(view.reloadIfChanged(&error));
    const auto saved = fs::read_text(native);
    REQUIRE(saved);
    CHECK(saved->encoding == fs::TextEncoding::Utf16Le);
    CHECK(saved->line_ending == fs::LineEnding::CrLf);
    REQUIRE(fs::write_text_atomic(native, external, fs::TextEncoding::Utf16Le, fs::LineEnding::Lf));
    REQUIRE(view.reloadIfChanged(&error));
    REQUIRE(view.saveFile(&error));
    const auto lf = fs::read_text(native);
    REQUIRE(lf);
    CHECK(lf->line_ending == fs::LineEnding::Lf);
}

TEST_CASE("source refresh retries a missing file without losing the open document",
          "[gui][editor][refresh]") {
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    const auto path = directory.filePath("fixture.py");
    const auto native = path_from_utf8(path.toStdString());
    REQUIRE(fs::write_text_atomic(native, "original = 1\n"));
    SourceView view;
    QString error;
    REQUIRE(view.loadFile(path, &error));
    REQUIRE(QFile::remove(path));
    CHECK_FALSE(view.reloadIfChanged(&error));
    CHECK_FALSE(error.isEmpty());
    CHECK(view.toPlainText() == "original = 1\n");
    CHECK(view.filePath() == path);
    REQUIRE(fs::write_text_atomic(native, "external = 2\n"));
    REQUIRE(view.reloadIfChanged(&error));
    CHECK(error.isEmpty());
    CHECK(view.toPlainText() == "external = 2\n");
}

TEST_CASE("Unicode GUI projects restore files draft and conversation",
          "[gui][unicode][persistence]") {
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    const auto original = app_paths().data_dir();

    struct Restore {
        std::filesystem::path path;

        ~Restore() { app_paths().set_data_dir(path); }
    } restore{original};

    const auto home = directory.filePath(QString::fromUtf8("home é 路径 🦍"));
    const auto project = directory.filePath(QString::fromUtf8("projet é 路径 🦍"));
    REQUIRE(QDir().mkpath(home));
    REQUIRE(QDir().mkpath(project));
    app_paths().set_data_dir(path_from_utf8(home.toStdString()));
    const auto file = project + QString::fromUtf8("/calcul é 路径 🦍.py");
    REQUIRE(fs::write_text_atomic(path_from_utf8(file.toStdString()), "saved Unicode fixture\n"));
    // Dismiss an unexpected warning so the before-fix reproduction cannot hang.
    int warnings = 0;
    QTimer dismiss;
    QObject::connect(&dismiss, &QTimer::timeout, [&] {
        for (auto* widget : QApplication::topLevelWidgets()) {
            if (auto* message = qobject_cast<QMessageBox*>(widget);
                message && message->isVisible()) {
                ++warnings;
                message->accept();
            }
        }
    });
    dismiss.start(100);
    const auto draft = QString::fromUtf8("Brouillon é 路径 🦍");
    {
        Application application;
        REQUIRE(application.initialise());
        MainWindow window(application);
        window.openProjectFolder(project);
        REQUIRE(application.workspace().is_open());
        CHECK(path_to_utf8(application.workspace().root()) == project.toStdString());
        REQUIRE(application.start_conversation("Unicode saved chat"));
        REQUIRE(application.persist_conversation({ChatMessage::user("Saved group context")}));
        auto* composer = window.findChild<PromptComposer*>();
        REQUIRE(composer);
        composer->setPlainText(draft);
        window.openProjectFolder(project);
        CHECK(composer->toPlainText() == draft);
        auto* list = window.findChild<QListWidget*>("ConversationList");
        REQUIRE(list);
        CHECK(list->count() == 1);
        CHECK(list->item(0)->text() == "Unicode saved chat");
        REQUIRE(QMetaObject::invokeMethod(&window, "openFileFromPath", Qt::DirectConnection,
                                          Q_ARG(QString, QFileInfo(file).fileName()),
                                          Q_ARG(int, 1)));
        auto* editor = window.findChild<SourceEditor*>();
        REQUIRE(editor);
        CHECK(editor->openFiles() == QStringList{file});
        CHECK(window.close());
    }
    {
        Application application;
        REQUIRE(application.initialise());
        MainWindow window(application);
        window.restoreSession();
        CHECK(application.workspace().is_open());
        CHECK(path_to_utf8(application.workspace().root()) == project.toStdString());
        auto* editor = window.findChild<SourceEditor*>();
        REQUIRE(editor);
        CHECK(editor->openFiles() == QStringList{file});
        auto* composer = window.findChild<PromptComposer*>();
        REQUIRE(composer);
        CHECK(composer->toPlainText() == draft);
        auto* list = window.findChild<QListWidget*>("ConversationList");
        REQUIRE(list);
        CHECK(list->count() == 1);
        auto* chat = window.findChild<ChatView*>();
        REQUIRE(chat);
        CHECK(chat->toMarkdown().contains("Saved group context"));
    }
    CHECK(warnings == 0);
}

namespace {
QString label_text(QWidget& widget) {
    QString text;
    for (const auto* label : widget.findChildren<QLabel*>())
        text += label->text() + '\n';
    return text;
}

class RecoveryHealthClient : public HttpClient {
public:
    Result<HttpResponse> send(const HttpRequest& request, CancellationToken) override {
        HttpResponse response;
        response.status_code = 200;
        response.body = request.url.ends_with("/props")
                            ? R"({"model_path":"fixture.gguf","n_ctx":32768})"
                            : "{}";
        return response;
    }

    Result<HttpResponse> stream(const HttpRequest&, const HttpStreamCallback&,
                                CancellationToken) override {
        return make_error(ErrorCode::NotSupported, "recovery UI fixture does not generate");
    }

    Status download(const HttpRequest&, const std::filesystem::path&,
                    const std::function<void(std::uint64_t, std::uint64_t)>&,
                    CancellationToken) override {
        return make_error(ErrorCode::NotSupported, "recovery UI fixture does not download");
    }

    std::string backend_name() const override { return "recovery UI fixture"; }
};

class ConfirmationProbe : public ITool {
public:
    std::string name() const override { return "confirmation_probe"; }

    std::string description() const override { return "Test a queued confirmation."; }

    Json parameters_schema() const override { return Json{{"type", "object"}}; }

    ToolResult execute(const Json&, ToolContext& context) override {
        ConfirmationRequest request;
        request.title = "Fixture confirmation";
        const bool approved = context.confirm && context.confirm(request);
        return ToolResult::success("confirmation completed", approved ? "approved" : "refused");
    }
};

void render(QWidget& widget, const QString& name) {
    if (qobject_cast<QMainWindow*>(&widget))
        widget.resize(1280, 800);
    else
        widget.resize(600, 420);
    widget.show();
    QApplication::processEvents();
    const auto directory = qEnvironmentVariable("LOCALCODE_GUI_EVIDENCE_DIR");
    if (!directory.isEmpty()) {
        QDir().mkpath(directory);
        REQUIRE(widget.grab().save(directory + '/' + name + ".png"));
    }
}
}   // namespace

TEST_CASE("rendered task labels distinguish checks from a verified objective", "[gui]") {
    TaskPanel panel;
    panel.setVerification("verified", {}, false, true);
    render(panel, "checks-only");
    CHECK(label_text(panel).contains("CHECKS PASSED"));
    CHECK_FALSE(label_text(panel).contains("VERIFIED —"));
    panel.setVerification("not verified", {}, false, false);
    render(panel, "answered-question");
    CHECK_FALSE(label_text(panel).contains("NOT VERIFIED"));
    panel.setVerification("not verified", {}, false, true);
    render(panel, "unclassified-checks");
    CHECK(label_text(panel).contains("no classified build or test evidence"));
    CHECK_FALSE(label_text(panel).contains("nothing was built or tested"));
    panel.setVerification("verified", {}, true, true);
    render(panel, "verified-objective");
    CHECK(label_text(panel).contains("VERIFIED — every acceptance criterion"));
    panel.setVerification("verified", {"tests passed without a test run"}, true, true);
    render(panel, "unsupported-claim");
    CHECK(label_text(panel).contains("Unsupported claims"));
    panel.clearVerification();
    CHECK_FALSE(label_text(panel).contains("VERIFIED"));
}

TEST_CASE("streaming edits only the chat tail and preserves history selection", "[gui][chat]") {
    ChatView chat;
    chat.resize(700, 450);
    chat.show();
    for (int index = 0; index < 100; ++index) {
        chat.addMessage(
            index % 2 ? ChatRole::Assistant : ChatRole::User,
            QString("Retained source %1\n\nA paragraph with **formatting**.").arg(index));
    }
    chat.beginAssistantMessage();
    auto* browser = chat.findChild<QTextBrowser*>();
    REQUIRE(browser);
    auto selected = browser->document()->find("Retained source 17");
    REQUIRE(!selected.isNull());
    browser->setTextCursor(selected);
    browser->verticalScrollBar()->setValue(0);
    const auto history = browser->toPlainText();
    qint64 removed = 0;
    QObject::connect(browser->document(), &QTextDocument::contentsChange, &chat,
                     [&](int, int gone, int) { removed += gone; });
    chat.appendReasoning("A separate native thought.");
    chat.appendDelta("The actual answer is **bounded**.");
    REQUIRE(QMetaObject::invokeMethod(&chat, "flushPending", Qt::DirectConnection));
    CHECK(removed < history.size() / 10);
    CHECK(browser->textCursor().selectedText() == "Retained source 17");
    CHECK(browser->verticalScrollBar()->value() == 0);
    chat.endAssistantMessage();
    const auto text = browser->toPlainText();
    for (int index = 0; index < 100; ++index)
        CHECK(text.count(QString("Retained source %1\n").arg(index)) == 1);
    CHECK(text.contains("The actual answer is bounded."));
    CHECK_FALSE(text.contains("A separate native thought."));
    CHECK(text.contains("Show reasoning"));
    REQUIRE(QMetaObject::invokeMethod(&chat, "handleAnchorClicked", Qt::DirectConnection,
                                      Q_ARG(QUrl, QUrl("localcode:reasoning/100"))));
    CHECK(browser->toPlainText().contains("A separate native thought."));
    REQUIRE(QMetaObject::invokeMethod(&chat, "handleAnchorClicked", Qt::DirectConnection,
                                      Q_ARG(QUrl, QUrl("localcode:copy-reasoning/100"))));
    CHECK(QApplication::clipboard()->text() == "A separate native thought.");
    chat.setShowReasoning(false);
    CHECK_FALSE(browser->toPlainText().contains("A separate native thought."));
    chat.setShowReasoning(true);
    CHECK(browser->toPlainText().contains("A separate native thought."));
}

TEST_CASE("a notice cannot receive the previous or next assistant stream",
          "[gui][chat][notice-stream]") {
    ChatView chat;
    chat.beginAssistantMessage();
    chat.appendReasoning("first thought");
    chat.appendDelta("first answer");
    chat.addMessage(ChatRole::Notice, "scope reminder");
    CHECK_FALSE(chat.streaming());
    CHECK(chat.toMarkdown() == "## LocalCode\n\nfirst answer\n\n## Notice\n\nscope reminder");
    for (const auto* timer : chat.findChildren<QTimer*>())
        CHECK_FALSE(timer->isActive());
    chat.appendReasoning("next thought");
    chat.appendDelta("next answer");
    chat.endAssistantMessage();
    CHECK(chat.toMarkdown() ==
          "## LocalCode\n\nfirst answer\n\n## Notice\n\nscope reminder\n\n## LocalCode\n\nnext "
          "answer");
}

TEST_CASE("chat restore and activity transitions release all reasoning timers", "[gui][chat]") {
    ChatView chat;
    auto* browser = chat.findChild<QTextBrowser*>();
    REQUIRE(browser);
    chat.setActivityNote("Processing the prompt.");
    CHECK(browser->toPlainText().contains("Processing the prompt."));
    chat.addMessages({{ChatRole::User, "Saved question", {}},
                      {ChatRole::Assistant, "Saved answer", "Saved thought"}});
    CHECK_FALSE(chat.streaming());
    CHECK(browser->toPlainText().count("Saved question") == 1);
    CHECK(browser->toPlainText().count("Saved answer") == 1);
    CHECK_FALSE(browser->toPlainText().contains("Saved thought"));
    REQUIRE(QMetaObject::invokeMethod(&chat, "handleAnchorClicked", Qt::DirectConnection,
                                      Q_ARG(QUrl, QUrl("localcode:reasoning/1"))));
    CHECK(browser->toPlainText().count("Saved thought") == 1);
    chat.clearActivityNote();
    CHECK_FALSE(browser->toPlainText().contains("Processing the prompt."));
    chat.beginAssistantMessage();
    chat.appendReasoning("A task interrupted during thinking.");
    chat.clear();
    for (auto* timer : chat.findChildren<QTimer*>())
        CHECK_FALSE(timer->isActive());
    CHECK_FALSE(chat.streaming());
    chat.addMessage(ChatRole::User, "A fresh task");
    CHECK(browser->toPlainText().contains("A fresh task"));
    CHECK_FALSE(browser->toPlainText().contains("Saved answer"));
    CHECK_FALSE(browser->toPlainText().contains("interrupted during thinking"));
}

TEST_CASE("paged transcript restore retains all messages and expands old reasoning",
          "[gui][chat]") {
    ChatView chat;
    auto* browser = chat.findChild<QTextBrowser*>();
    REQUIRE(browser);
    QList<ChatView::TranscriptMessage> transcript;
    for (int index = 0; index < 500; ++index)
        transcript.push_back({ChatRole::Assistant, QString("Saved message %1.").arg(index),
                              QString("Stored reasoning %1.").arg(index)});
    chat.restoreMessages(transcript);
    CHECK(browser->toPlainText().contains("Saved message 499."));
    CHECK_FALSE(browser->toPlainText().contains("Saved message 0."));
    CHECK(browser->toPlainText().contains("450 earlier message(s)"));
    CHECK(chat.toMarkdown().contains("Saved message 0."));
    chat.beginAssistantMessage();
    chat.appendDelta("A new answer after restoring the full context.");
    chat.endAssistantMessage();
    CHECK(browser->toPlainText().contains("A new answer after restoring the full context."));
    for (int page = 0; page < 9; ++page)
        REQUIRE(QMetaObject::invokeMethod(&chat, "handleAnchorClicked", Qt::DirectConnection,
                                          Q_ARG(QUrl, QUrl("localcode:earlier"))));
    CHECK(browser->toPlainText().contains("Saved message 0."));
    CHECK(browser->toPlainText().contains("Saved message 499."));
    CHECK_FALSE(browser->toPlainText().contains("Load earlier messages"));
    REQUIRE(QMetaObject::invokeMethod(&chat, "handleAnchorClicked", Qt::DirectConnection,
                                      Q_ARG(QUrl, QUrl("localcode:reasoning/0"))));
    CHECK(browser->toPlainText().contains("Stored reasoning 0."));
    chat.clear();
    chat.addMessage(ChatRole::User, "Fresh conversation");
    CHECK_FALSE(browser->toPlainText().contains("earlier message"));
}

TEST_CASE("code copy identifies its own message instead of the first transcript block",
          "[gui][chat]") {
    ChatView chat;
    chat.restoreMessages({{ChatRole::Assistant, "```cpp\nfirst_source();\n```", {}},
                          {ChatRole::Assistant, "```cpp\nsecond_source();\n```", {}}});
    auto* browser = chat.findChild<QTextBrowser*>();
    REQUIRE(browser);
    CHECK(browser->toHtml().contains("localcode:copy/1/0"));
    REQUIRE(QMetaObject::invokeMethod(&chat, "handleAnchorClicked", Qt::DirectConnection,
                                      Q_ARG(QUrl, QUrl("localcode:copy/1/0"))));
    CHECK(QApplication::clipboard()->text() == "second_source();");
    REQUIRE(QMetaObject::invokeMethod(&chat, "handleAnchorClicked", Qt::DirectConnection,
                                      Q_ARG(QUrl, QUrl("localcode:copy/0/0"))));
    CHECK(QApplication::clipboard()->text() == "first_source();");
    REQUIRE(QMetaObject::invokeMethod(&chat, "handleAnchorClicked", Qt::DirectConnection,
                                      Q_ARG(QUrl, QUrl("localcode:copy/1/99"))));
    CHECK(QApplication::clipboard()->text() == "first_source();");
}

TEST_CASE("output coalescing wakes only for pending data", "[gui][output]") {
    OutputPanel output;
    const auto timers = output.findChildren<QTimer*>();
    REQUIRE(timers.size() == 1);
    CHECK_FALSE(timers.front()->isActive());
    output.appendCommandOutput("first\n", false);
    output.appendCommandOutput("second\n", true);
    output.appendServerLog("server fixture");
    CHECK(timers.front()->isActive());
    REQUIRE(QMetaObject::invokeMethod(&output, "flush", Qt::DirectConnection));
    CHECK_FALSE(timers.front()->isActive());
    QString all;
    for (const auto* view : output.findChildren<QPlainTextEdit*>())
        all += view->toPlainText();
    CHECK(all.contains("first\nsecond"));
    CHECK(all.contains("server fixture"));
    output.appendCommandOutput("discarded fixture", false);
    output.appendServerLog("retained server fixture");
    output.clearCommandOutput();
    CHECK(timers.front()->isActive());
    REQUIRE(QMetaObject::invokeMethod(&output, "flush", Qt::DirectConnection));
    CHECK_FALSE(timers.front()->isActive());
    output.appendServerLog("discarded server fixture");
    output.clearServerLog();
    CHECK_FALSE(timers.front()->isActive());
}

TEST_CASE("rendered ledger rejects stale and failed objective evidence", "[gui]") {
    VerificationPanel panel;
    UiVerification ledger;
    ledger.revision = 2;
    for (const QString& status : {QString("pass"), QString("stale"), QString("fail")}) {
        ledger.stages = {{"objective", status, "", "e1", 1}};
        panel.setVerification(ledger);
        render(panel, "objective-" + status);
        const auto labels = label_text(panel);
        if (status == "pass")
            CHECK(labels.contains("VERIFIED —"));
        else
            CHECK_FALSE(labels.contains("VERIFIED —"));
        if (status == "stale") CHECK(labels.contains("OUT OF DATE"));
        if (status == "fail") CHECK(labels.contains("FAILED"));
    }
}

TEST_CASE("a passed objective retains its model-proposed criterion caveat", "[gui]") {
    VerificationPanel panel;
    UiVerification ledger;
    ledger.stages = {{"objective", "pass", "each check was proposed by the model", "e1", 1}};
    panel.setVerification(ledger);
    render(panel, "model-proposed-checks");
    const auto* text = panel.findChild<QTextBrowser*>();
    REQUIRE(text);
    CHECK(text->toPlainText().contains("each check was proposed by the model"));
}

TEST_CASE("the real window displays failed command exit codes", "[gui]") {
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    const auto original = app_paths().data_dir();
    app_paths().set_data_dir(directory.path().toStdString());

    struct Restore {
        std::filesystem::path path;

        ~Restore() { app_paths().set_data_dir(path); }
    } restore{original};

    Application application;
    REQUIRE(application.initialise());
    MainWindow window(application);
    UiToolEvent event;
    event.name = "run_command";
    event.exitCode = 7;
    event.ok = false;
    event.summary = "command failed";
    REQUIRE(QMetaObject::invokeMethod(&window, "onToolFinished", Qt::DirectConnection,
                                      Q_ARG(UiToolEvent, event)));
    auto* output = window.findChild<OutputPanel*>();
    REQUIRE(output);
    QString text;
    for (const auto* edit : output->findChildren<QPlainTextEdit*>())
        text += edit->toPlainText();
    CHECK(text.contains("[exit 7"));
    CHECK_FALSE(text.contains("[exit 0"));

    UiOutcome outcome;
    outcome.objectiveVerified = true;
    outcome.hadPlan = true;
    outcome.taskVerification = "verified";
    outcome.presentationFailed = true;
    outcome.presentation = "reasoning only";
    outcome.headline = "Verified — no summary";
    outcome.finalizationAttempts = 2;
    REQUIRE(QMetaObject::invokeMethod(&window, "onRunFinished", Qt::DirectConnection,
                                      Q_ARG(UiOutcome, outcome)));
    auto* task = window.findChild<TaskPanel*>();
    auto* chat = window.findChild<ChatView*>();
    REQUIRE(task);
    REQUIRE(chat);
    render(window, "window-finalizer-failed");
    CHECK(label_text(*task).contains("VERIFIED — every acceptance criterion"));
    CHECK(chat->toMarkdown().contains("What failed is the"));

    outcome = {};
    outcome.completed = true;
    outcome.finalMessage = "391";
    outcome.taskVerification = "not verified";
    REQUIRE(QMetaObject::invokeMethod(&window, "onRunFinished", Qt::DirectConnection,
                                      Q_ARG(UiOutcome, outcome)));
    render(window, "window-question-answered");
    CHECK_FALSE(label_text(*task).contains("NOT VERIFIED"));
    CHECK_FALSE(label_text(*task).contains("VERIFIED —"));
    CHECK(chat->toMarkdown().endsWith("391"));
    CHECK(window.findChild<QLabel*>("TaskSummary")->text() == "Answered");
}

TEST_CASE("conversation navigation stays inside the open project", "[gui][persistence]") {
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    const auto original = app_paths().data_dir();
    app_paths().set_data_dir(directory.filePath("data").toStdString());

    struct Restore {
        std::filesystem::path path;

        ~Restore() { app_paths().set_data_dir(path); }
    } restore{original};

    const auto first = directory.filePath("project-a");
    const auto second = directory.filePath("project-b");
    REQUIRE(QDir().mkpath(first));
    REQUIRE(QDir().mkpath(second));
    Application application;
    REQUIRE(application.initialise());
    REQUIRE(application.open_workspace(first.toStdString()));
    const auto firstChat = application.start_conversation("First conversation");
    REQUIRE(firstChat);
    application.agent().set_history({ChatMessage::user("First saved question")});
    REQUIRE(application.persist_conversation(application.agent().history()));
    REQUIRE(application.start_conversation("Second conversation"));
    application.agent().set_history({ChatMessage::user("Second saved question")});
    REQUIRE(application.persist_conversation(application.agent().history()));
    MainWindow window(application);
    window.restoreSession();
    auto* list = window.findChild<QListWidget*>("ConversationList");
    auto* chat = window.findChild<ChatView*>();
    REQUIRE(list);
    REQUIRE(chat);
    CHECK(list->count() == 2);
    for (int row = 0; row < list->count(); ++row) {
        if (list->item(row)->data(Qt::UserRole).toLongLong() == *firstChat)
            list->setCurrentRow(row);
    }
    REQUIRE(QMetaObject::invokeMethod(&window, "switchConversation", Qt::DirectConnection));
    CHECK(application.current_conversation() == *firstChat);
    CHECK(chat->toMarkdown().contains("First saved question"));
    CHECK_FALSE(chat->toMarkdown().contains("Second saved question"));
    window.openProjectFolder(second);
    REQUIRE(application.start_conversation("Other project's conversation"));
    const auto foreign = application.current_conversation();
    window.openProjectFolder(first);
    CHECK(list->count() == 2);
    const auto rejected = application.load_conversation(foreign);
    REQUIRE_FALSE(rejected);
    CHECK(rejected.error().code == ErrorCode::PermissionDenied);
    CHECK(application.current_conversation() == 0);
    CHECK(application.agent().history().empty());
}

TEST_CASE("chat deletion confirms the clicked chat and persists active deletion",
          "[gui][persistence][chat-delete]") {
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    const auto original = app_paths().data_dir();

    struct Restore {
        std::filesystem::path path;

        ~Restore() { app_paths().set_data_dir(path); }
    } restore{original};

    app_paths().set_data_dir(path_from_utf8(directory.filePath("data").toStdString()));
    const auto project = directory.filePath("project");
    REQUIRE(QDir().mkpath(project));
    const auto file = path_from_utf8(directory.filePath("project/keep.txt").toStdString());
    REQUIRE(fs::write_text_atomic(file, "keep project files\n"));
    {
        Application application;
        REQUIRE(application.initialise());
        REQUIRE(application.open_workspace(path_from_utf8(project.toStdString())));
        const auto older = application.start_conversation("Older chat");
        REQUIRE(older);
        REQUIRE(application.persist_message(ChatMessage::user("older saved question")));
        const auto active = application.start_conversation("Active chat");
        REQUIRE(active);
        REQUIRE(application.persist_message(ChatMessage::user("active saved question")));
        REQUIRE(application.app_state_repository().set(
            "conversation.terminal." + std::to_string(*older), "older outcome"));
        REQUIRE(application.app_state_repository().set(
            "conversation.terminal." + std::to_string(*active), "active outcome"));
        MainWindow window(application);
        window.restoreSession();
        auto history = application.agent().history();
        history.push_back(ChatMessage::assistant("unsaved active answer"));
        application.agent().set_history(history);
        auto* list = window.findChild<QListWidget*>("ConversationList");
        auto* chat = window.findChild<ChatView*>();
        auto* tabs = window.findChild<QTabWidget*>("ProjectSidebar");
        REQUIRE(list);
        REQUIRE(chat);
        REQUIRE(tabs);
        tabs->setCurrentWidget(list->parentWidget());
        window.show();
        QApplication::processEvents();
        REQUIRE(list->count() == 2);
        const auto requestDeletion = [&](std::int64_t id, QMessageBox::StandardButton answer) {
            QListWidgetItem* item = nullptr;
            for (int row = 0; row < list->count(); ++row)
                if (list->item(row)->data(Qt::UserRole).toLongLong() == id) item = list->item(row);
            REQUIRE(item);
            const auto position = list->visualItemRect(item).center();
            REQUIRE(QMetaObject::invokeMethod(list, "customContextMenuRequested",
                                              Qt::DirectConnection, Q_ARG(QPoint, position)));
            QPointer<QMenu> menu = list->findChild<QMenu*>("ConversationContextMenu");
            REQUIRE(menu);
            auto* remove = menu->findChild<QAction*>("DeleteChatAction");
            REQUIRE(remove);
            CHECK(remove->text() == "Delete chat");
            bool confirmed = false;
            bool defaultNo = false;
            QTimer::singleShot(0, &window, [&] {
                auto* question = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
                if (!question) return;
                defaultNo = question->defaultButton() == question->button(QMessageBox::No);
                if (auto* button = question->button(answer)) {
                    confirmed = true;
                    button->click();
                } else {
                    question->reject();
                }
            });
            remove->trigger();
            QApplication::processEvents();
            CHECK(confirmed);
            CHECK(defaultNo);
            if (menu) menu->close();
            QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        };
        requestDeletion(*older, QMessageBox::No);
        CHECK(list->count() == 2);
        REQUIRE(application.conversation_repository().find(*older)->has_value());
        CHECK(application.current_conversation() == *active);
        REQUIRE(application.agent().history().size() == 2);

        requestDeletion(*older, QMessageBox::Yes);
        CHECK(list->count() == 1);
        CHECK_FALSE(application.conversation_repository().find(*older)->has_value());
        CHECK(application.conversation_repository().message_count(*older).value_or(-1) == 0);
        CHECK(application.current_conversation() == *active);
        REQUIRE(application.agent().history().size() == 2);
        CHECK(application.agent().history().back().content == "unsaved active answer");
        CHECK(chat->toMarkdown().contains("active saved question"));
        CHECK(application.app_state_repository()
                  .get("conversation.terminal." + std::to_string(*older))
                  .value_or("missing")
                  .empty());

        requestDeletion(*active, QMessageBox::Yes);
        CHECK(list->count() == 0);
        CHECK(application.current_conversation() == 0);
        CHECK(application.agent().history().empty());
        CHECK(chat->toMarkdown().isEmpty());
        const auto recovered = application.recover_session();
        REQUIRE(recovered);
        CHECK(recovered->conversation_id == 0);
        CHECK(window.close());
    }
    Application reopened;
    REQUIRE(reopened.initialise());
    const auto recovered = reopened.recover_session();
    REQUIRE(recovered);
    CHECK(recovered->conversation_id == 0);
    REQUIRE(reopened.open_workspace(path_from_utf8(project.toStdString())));
    const auto workspace = reopened.workspace_repository().find_by_path(project.toStdString());
    REQUIRE(workspace);
    REQUIRE(workspace->has_value());
    const auto remaining = reopened.conversation_repository().recent((*workspace)->id);
    REQUIRE(remaining);
    CHECK(remaining->empty());
    const auto contents = fs::read_text(file);
    REQUIRE(contents);
    CHECK(contents->content == "keep project files\n");
}

TEST_CASE("chat deletion rolls back metadata failures and rejects another project's chat",
          "[gui][persistence][chat-delete]") {
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    const auto original = app_paths().data_dir();

    struct Restore {
        std::filesystem::path path;

        ~Restore() { app_paths().set_data_dir(path); }
    } restore{original};

    app_paths().set_data_dir(path_from_utf8(directory.filePath("data").toStdString()));
    Application application;
    REQUIRE(application.initialise());
    const auto active = application.start_conversation("Keep on failure");
    REQUIRE(active);
    application.agent().set_history({ChatMessage::user("preserve active history")});
    REQUIRE(application.persist_conversation(application.agent().history()));
    const auto other = application.workspace_repository().touch("other-project", "Other project");
    REQUIRE(other);
    const auto foreign = application.conversation_repository().create(other->id, "Foreign", "");
    REQUIRE(foreign);
    const auto denied = application.delete_conversation(*foreign);
    REQUIRE_FALSE(denied);
    CHECK(denied.error().code == ErrorCode::PermissionDenied);
    CHECK(application.conversation_repository().find(*foreign)->has_value());
    REQUIRE(application.database().execute(
        "CREATE TRIGGER reject_deleted_session BEFORE DELETE ON app_state "
        "WHEN OLD.key='session.conversation' BEGIN "
        "SELECT RAISE(ABORT,'fixture metadata failure'); END"));
    CHECK_FALSE(application.delete_conversation(*active));
    CHECK(application.current_conversation() == *active);
    REQUIRE(application.agent().history().size() == 1);
    CHECK(application.agent().history()[0].content == "preserve active history");
    CHECK(application.conversation_repository().find(*active)->has_value());
    CHECK(application.conversation_repository().message_count(*active).value_or(-1) == 1);
    const auto recovered = application.recover_session();
    REQUIRE(recovered);
    CHECK(recovered->conversation_id == *active);
    REQUIRE(application.database().execute("DROP TRIGGER reject_deleted_session"));
    REQUIRE(application.delete_conversation(*active));
    CHECK(application.current_conversation() == 0);
    CHECK(application.agent().history().empty());
}

TEST_CASE("restored internal feedback is labelled as a notice, not the user", "[gui][feedback]") {
    QTemporaryDir storage;
    REQUIRE(storage.isValid());
    const auto original = app_paths().data_dir();

    struct Restore {
        std::filesystem::path path;

        ~Restore() { app_paths().set_data_dir(path); }
    } restore{original};

    app_paths().set_data_dir(storage.filePath("data").toStdString());
    const auto project = storage.filePath("project");
    REQUIRE(QDir().mkpath(project));
    Application application;
    REQUIRE(application.initialise());
    REQUIRE(application.open_workspace(project.toStdString()));
    REQUIRE(application.start_conversation("File-only task"));
    REQUIRE(application.persist_conversation(
        {ChatMessage::user("No commands."), ChatMessage::assistant("The file was edited."),
         ChatMessage::application_feedback("Some verification is missing.")}));
    MainWindow window(application);
    window.restoreSession();
    auto* chat = window.findChild<ChatView*>();
    REQUIRE(chat);
    const auto transcript = chat->toMarkdown();
    CHECK(transcript.contains("## You\n\nNo commands."));
    CHECK(transcript.contains("## Notice\n\n## LocalCode internal feedback"));
    CHECK_FALSE(transcript.contains("## You\n\n## LocalCode internal feedback"));
}

TEST_CASE("panels remember visibility and activity details remain accessible", "[gui][layout]") {
    QSettings settings;
    const auto previous = settings.allKeys();
    QMap<QString, QVariant> values;
    for (const auto& key : previous)
        values.insert(key, settings.value(key));

    struct RestoreSettings {
        QMap<QString, QVariant> values;

        ~RestoreSettings() {
            QSettings saved;
            saved.clear();
            for (auto it = values.begin(); it != values.end(); ++it)
                saved.setValue(it.key(), it.value());
        }
    } restoreSettings{values};

    settings.clear();
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    const auto original = app_paths().data_dir();
    app_paths().set_data_dir(directory.path().toStdString());

    struct RestorePaths {
        std::filesystem::path path;

        ~RestorePaths() { app_paths().set_data_dir(path); }
    } restorePaths{original};

    Application application;
    REQUIRE(application.initialise());
    {
        MainWindow window(application);
        render(window, "panels-default");
        auto* inspector = window.findChild<QWidget*>("InspectorSidebar");
        auto* output = window.findChild<OutputPanel*>();
        auto* chat = window.findChild<ChatView*>();
        REQUIRE(inspector);
        REQUIRE(output);
        REQUIRE(chat);
        CHECK_FALSE(inspector->isVisible());
        CHECK_FALSE(output->isVisible());
        CHECK(chat->width() > window.width() / 2);
        for (auto* action : window.findChildren<QAction*>())
            if (action->text() == "Project sidebar") action->setChecked(false);
        QApplication::processEvents();
        CHECK(chat->width() > window.width() * 4 / 5);
        UiToolEvent tool;
        tool.name = "run_command";
        tool.arguments = "fixture command";
        REQUIRE(QMetaObject::invokeMethod(&window, "onToolStarted", Qt::DirectConnection,
                                          Q_ARG(UiToolEvent, tool)));
        CHECK(output->isVisible());
        const QString detail = "Copyable fixture details\n" + QString(2000, 'x');
        auto* timeline = window.findChild<ActivityTimeline*>();
        REQUIRE(timeline);
        REQUIRE(QMetaObject::invokeMethod(timeline, "entrySelected", Qt::DirectConnection,
                                          Q_ARG(QString, QString("Fixture tool")),
                                          Q_ARG(QString, detail), Q_ARG(QString, QString())));
        auto* dialog = window.findChild<QDialog*>("ToolDetails");
        REQUIRE(dialog);
        CHECK(dialog->isVisible());
        CHECK(dialog->findChild<QPlainTextEdit*>()->toPlainText() == detail);
        for (auto* button : dialog->findChildren<QPushButton*>())
            if (button->text() == "Copy details") button->click();
        CHECK(QApplication::clipboard()->text() == detail);
        dialog->reject();
        CHECK(window.close());
    }
    {
        MainWindow restored(application);
        render(restored, "panels-restored");
        CHECK_FALSE(restored.findChild<QWidget*>("ProjectSidebar")->isVisible());
        CHECK(restored.findChild<OutputPanel*>()->isVisible());
        CHECK_FALSE(restored.findChild<QWidget*>("InspectorSidebar")->isVisible());
    }
}

TEST_CASE("saving before close preserves every modified editor tab", "[gui]") {
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    SourceEditor editor;
    for (const auto* name : {"one.txt", "two.txt"}) {
        QFile file(directory.filePath(name));
        REQUIRE(file.open(QIODevice::WriteOnly));
        file.write("original");
        file.close();
        editor.openFile(file.fileName());
    }
    const auto views = editor.findChildren<SourceView*>();
    REQUIRE(views.size() == 2);
    for (auto* view : views) {
        view->appendPlainText("changed");
        REQUIRE(view->modified());
    }
    QString error;
    REQUIRE(editor.saveAll(&error));
    CHECK_FALSE(editor.hasUnsavedChanges());
    for (const auto* name : {"one.txt", "two.txt"}) {
        QFile file(directory.filePath(name));
        REQUIRE(file.open(QIODevice::ReadOnly));
        CHECK(file.readAll().contains("changed"));
    }
}

TEST_CASE("clamping a decorated Qt window keeps its whole frame on screen", "[gui][geometry]") {
    QWidget window;
    window.resize(600, 420);
    window.move(-100, -100);
    window.show();
    QApplication::processEvents();
    REQUIRE(window.screen());
    REQUIRE(clampToScreen(window));
    QApplication::processEvents();
    INFO("frame=" << window.frameGeometry().x() << ',' << window.frameGeometry().y());
    CHECK(window.screen()->availableGeometry().contains(window.frameGeometry()));
    CHECK_FALSE(clampToScreen(window));
}

TEST_CASE("a failed editor save preserves the tab instead of discarding its buffer",
          "[gui][persistence]") {
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    const auto path = directory.filePath("blocked.txt");
    QFile original(path);
    REQUIRE(original.open(QIODevice::WriteOnly));
    original.write("original");
    original.close();
    SourceEditor editor;
    editor.openFile(path);
    const auto views = editor.findChildren<SourceView*>();
    REQUIRE(views.size() == 1);
    views.front()->appendPlainText("preserve unsaved content");
    REQUIRE(QFile::remove(path));
    REQUIRE(QDir().mkdir(path));
    QTimer::singleShot(0, [] {
        if (auto* message = qobject_cast<QMessageBox*>(QApplication::activeModalWidget()))
            message->accept();
    });
    CHECK_FALSE(editor.closeAll());
    CHECK(editor.openFiles().size() == 1);
    CHECK(views.front()->modified());
    CHECK(views.front()->toPlainText().contains("preserve unsaved content"));
}

TEST_CASE("Settings settles entirely inside the available screen", "[gui][geometry]") {
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    const auto original = app_paths().data_dir();
    app_paths().set_data_dir(directory.path().toStdString());

    struct Restore {
        std::filesystem::path path;

        ~Restore() { app_paths().set_data_dir(path); }
    } restore{original};

    Application application;
    REQUIRE(application.initialise());
    QWidget parent;
    parent.resize(1280, 800);
    parent.show();
    SettingsDialog dialog(application, &parent);
    dialog.show();
    QEventLoop settled;
    QTimer::singleShot(100, &settled, &QEventLoop::quit);
    settled.exec();
    REQUIRE(dialog.screen());
    const auto frame = dialog.frameGeometry();
    const auto available = dialog.screen()->availableGeometry();
    INFO("frame=" << frame.x() << ',' << frame.y() << ' ' << frame.width() << 'x'
                  << frame.height());
    INFO("available=" << available.x() << ',' << available.y() << ' ' << available.width() << 'x'
                      << available.height());
    CHECK(available.contains(frame));
    auto* navigation = dialog.findChild<QListWidget*>("SettingsNavigation");
    auto* pages = dialog.findChild<QStackedWidget*>();
    auto* buttons = dialog.findChild<QDialogButtonBox*>();
    REQUIRE(navigation);
    REQUIRE(pages);
    REQUIRE(buttons);
    CHECK(navigation->count() == 8);
    for (int index = 0; index < navigation->count(); ++index) {
        navigation->setCurrentRow(index);
        QApplication::processEvents();
        CHECK(pages->currentIndex() == index);
        CHECK(qobject_cast<QScrollArea*>(pages->currentWidget()) != nullptr);
        CHECK(dialog.rect().contains(QRect(buttons->mapTo(&dialog, QPoint()), buttons->size())));
    }
}

TEST_CASE("explicit project beneath ignored ancestors stays rooted", "[gui][project]") {
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    const auto root = directory.filePath("build/project");
    REQUIRE(QDir().mkpath(root + "/build"));
    QFile file(root + "/owner.cpp");
    REQUIRE(file.open(QIODevice::WriteOnly));
    file.write("int owner = 1;\n");
    file.close();
    ProjectExplorer explorer;
    explorer.show();
    explorer.setRoot(root);
    auto* tree = explorer.findChild<QTreeView*>();
    auto* model = explorer.findChild<QFileSystemModel*>();
    auto* proxy = explorer.findChild<ExplorerFilterModel*>();
    REQUIRE(tree);
    REQUIRE(model);
    REQUIRE(proxy);
    QEventLoop loaded;
    QObject::connect(model, &QFileSystemModel::directoryLoaded, &loaded, [&](const QString& path) {
        if (QDir(path).absolutePath() == QDir(root).absolutePath()) loaded.quit();
    });
    QTimer::singleShot(1000, &loaded, &QEventLoop::quit);
    loaded.exec();
    REQUIRE(tree->rootIndex().isValid());
    CHECK(model->filePath(proxy->mapToSource(tree->rootIndex())) == root);
    CHECK(proxy->mapFromSource(model->index(root + "/owner.cpp")).isValid());
    CHECK_FALSE(proxy->mapFromSource(model->index(root + "/build")).isValid());
    explorer.setRoot({});
    CHECK_FALSE(tree->isVisible());
}

TEST_CASE("opening a workspace does not mark session recovery twice", "[gui][persistence]") {
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    const auto original = app_paths().data_dir();
    app_paths().set_data_dir(directory.path().toStdString());

    struct Restore {
        std::filesystem::path path;

        ~Restore() { app_paths().set_data_dir(path); }
    } restore{original};

    Application application;
    REQUIRE(application.initialise());
    REQUIRE(application.mark_clean_shutdown());
    MainWindow window(application);
    window.openProjectFolder(directory.path());
    const auto clean = application.app_state_repository().get("session.clean_shutdown");
    REQUIRE(clean);
    CHECK(*clean == "1");
    const auto recovered = application.recover_session();
    REQUIRE(recovered);
    CHECK_FALSE(recovered->had_unclean_shutdown);
}

TEST_CASE("restart restores activity and cancelled outcome with historical checks stale",
          "[gui][persistence]") {
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    const auto original = app_paths().data_dir();
    app_paths().set_data_dir(directory.filePath("data").toStdString());

    struct Restore {
        std::filesystem::path path;

        ~Restore() { app_paths().set_data_dir(path); }
    } restore{original};

    Application application;
    REQUIRE(application.initialise());
    REQUIRE(application.open_workspace(directory.path().toStdString()));
    auto conversation = application.start_conversation("saved activity");
    REQUIRE(conversation);
    TaskState task;
    task.begin("saved activity");
    Evidence check;
    check.kind = EvidenceKind::BuildAttempted;
    check.positive = true;
    task.record_evidence(check);
    check.kind = EvidenceKind::TestsRun;
    task.record_evidence(check);
    application.agent().set_task_state(task);
    VerificationLedger ledger;
    ledger.record(Stage::Build, StageStatus::Pass, "", "previous build passed");
    application.agent().set_verification_ledger(ledger);
    auto assistant = ChatMessage::assistant("saved answer");
    assistant.reasoning_content = "saved separate thought";
    REQUIRE(application.persist_conversation({ChatMessage::user("saved activity"), assistant}));
    ToolHistoryRecord record;
    record.conversation_id = *conversation;
    record.tool_name = "read_file";
    record.summary = "Read the actual source";
    record.detail = "stored tool output";
    record.succeeded = true;
    REQUIRE(application.tool_history_repository().append(record));
    REQUIRE(application.app_state_repository().set(
        "conversation.terminal." + std::to_string(*conversation), "Cancelled."));
    REQUIRE(application.mark_clean_shutdown());
    MainWindow window(application);
    window.restoreSession();
    const auto* timeline = window.findChild<ActivityTimeline*>();
    const auto* chat = window.findChild<ChatView*>();
    REQUIRE(timeline);
    REQUIRE(chat);
    CHECK(timeline->entryCount() == 2);
    CHECK(chat->toMarkdown().contains("saved answer"));
    CHECK(chat->toMarkdown().contains("Previous run: Cancelled."));
    CHECK(application.agent().task_state().verification() == VerificationState::NotAttempted);
    CHECK(application.agent().verification_ledger().status(Stage::Build) == StageStatus::Stale);
    render(window, "restored-cancelled");
}

TEST_CASE("a failed output expectation stays failed in GUI events and saved history",
          "[gui][verification]") {
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    const auto original = app_paths().data_dir();
    app_paths().set_data_dir(directory.path().toStdString());

    struct Restore {
        std::filesystem::path path;

        ~Restore() { app_paths().set_data_dir(path); }
    } restore{original};

    Application application;
    REQUIRE(application.initialise());
    REQUIRE(application.open_workspace(directory.path().toStdString()));
    auto backend = std::make_shared<test::MockBackend>();
    backend
        ->then_call("run_command",
                    Json{{"command", Json::array({"cmd", "/d", "/s", "/c", "echo actual"})},
                         {"expect", Json{{"description", "output contains missing"},
                                         {"stdout_contains", Json::array({"missing"})}}}})
        .then_say("The process exited successfully, but its output check failed.");
    application.agent().set_backend(backend);
    ModelProfile profile;
    profile.id = "mock";
    profile.context_size = 32768;
    profile.capabilities.supports_tools = true;
    profile.tool_protocol = ToolProtocol::Native;
    application.agent().set_profile(profile);
    AgentController controller(application);
    UiToolEvent observed;
    bool received = false;
    QEventLoop loop;
    QObject::connect(&controller, &AgentController::toolFinished, &loop,
                     [&](const UiToolEvent& event) {
                         observed = event;
                         received = true;
                     });
    QObject::connect(&controller, &AgentController::runFinished, &loop, &QEventLoop::quit);
    QTimer::singleShot(60000, &loop, &QEventLoop::quit);
    controller.send("compare output against missing");
    loop.exec();
    INFO(observed.summary.toStdString());
    INFO(observed.detail.toStdString());
    REQUIRE(received);
    CHECK(observed.exitCode == 0);
    CHECK_FALSE(observed.ok);
    CHECK(observed.detail.contains("actual"));
    const auto saved = application.tool_history_repository().recent();
    REQUIRE(saved);
    REQUIRE_FALSE(saved->empty());
    CHECK_FALSE(saved->front().succeeded);
    CHECK(saved->front().detail.find("actual") != std::string::npos);
    ActivityTimeline timeline;
    const int row = timeline.beginEntry(observed.name);
    timeline.completeEntry(row, observed.ok, observed.summary, observed.detail,
                           observed.durationMs);
    const auto* list = timeline.findChild<QListWidget*>();
    REQUIRE(list);
    CHECK(list->item(row)->text().startsWith(QStringLiteral("✗")));
    render(timeline, "failed-expectation");
}

TEST_CASE("queued Git refreshes cannot restore changes from a previous project",
          "[gui][lifecycle][workspace]") {
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    REQUIRE(QDir().mkpath(directory.filePath("first")));
    REQUIRE(QDir().mkpath(directory.filePath("second")));
    const auto original = app_paths().data_dir();
    app_paths().set_data_dir(directory.filePath("data").toStdString());

    struct Restore {
        std::filesystem::path path;

        ~Restore() { app_paths().set_data_dir(path); }
    } restore{original};

    Application application;
    REQUIRE(application.initialise());
    MainWindow window(application);
    auto* pool = QThreadPool::globalInstance();
    const int threads = pool->maxThreadCount();
    pool->waitForDone();
    pool->setMaxThreadCount(1);
    QSemaphore release;
    QSemaphore entered;

    struct Unblock {
        QThreadPool* pool;
        QSemaphore& release;
        int threads;

        ~Unblock() {
            release.release();
            pool->waitForDone();
            pool->setMaxThreadCount(threads);
        }
    } unblock{pool, release, threads};

    auto blocked = QtConcurrent::run([&] {
        entered.release();
        release.acquire();
    });
    REQUIRE(entered.tryAcquire(1, 5000));
    window.openProjectFolder(directory.filePath("first"));
    application.workspace().record_agent_change(
        std::filesystem::path(directory.filePath("first/old-project.cpp").toStdString()),
        AgentChangeKind::Modified);
    REQUIRE(QMetaObject::invokeMethod(&window, "refreshGitState", Qt::DirectConnection));
    window.openProjectFolder(directory.filePath("second"));
    release.release();
    pool->waitForDone();
    QApplication::processEvents();
    const auto* task = window.findChild<TaskPanel*>();
    REQUIRE(task);
    QString text;
    for (const auto* browser : task->findChildren<QTextBrowser*>())
        text += browser->toPlainText();
    CHECK_FALSE(text.contains("old-project.cpp"));
    CHECK(application.workspace().root() ==
          std::filesystem::path(directory.filePath("second").toStdString()));
}

TEST_CASE("Git refresh bursts keep one background scan and retain the latest changes",
          "[gui][audit][git-coalescing]") {
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    REQUIRE(QDir().mkpath(directory.filePath("project")));
    const auto original = app_paths().data_dir();
    app_paths().set_data_dir(directory.filePath("data").toStdString());

    struct Restore {
        std::filesystem::path path;

        ~Restore() { app_paths().set_data_dir(path); }
    } restore{original};

    Application application;
    REQUIRE(application.initialise());
    MainWindow window(application);
    auto* pool = QThreadPool::globalInstance();
    pool->waitForDone();
    const int threads = pool->maxThreadCount();
    pool->setMaxThreadCount(1);
    QSemaphore release;
    QSemaphore entered;

    struct Unblock {
        QThreadPool* pool;
        QSemaphore& release;
        int threads;

        ~Unblock() {
            release.release();
            pool->waitForDone();
            pool->setMaxThreadCount(threads);
        }
    } unblock{pool, release, threads};

    auto blocked = QtConcurrent::run([&] {
        entered.release();
        release.acquire();
    });
    REQUIRE(entered.tryAcquire(1, 5000));
    window.openProjectFolder(directory.filePath("project"));
    for (int index = 0; index < 20; ++index) {
        application.workspace().record_agent_change(
            std::filesystem::path(directory.filePath("project/latest.cpp").toStdString()),
            AgentChangeKind::Modified);
        REQUIRE(QMetaObject::invokeMethod(&window, "refreshGitState", Qt::DirectConnection));
    }
    CHECK(window.findChildren<QFutureWatcherBase*>().size() == 1);
    release.release();
    // A coalesced follow-up is posted when the first watcher completes.
    for (int pass = 0; pass < 3; ++pass) {
        pool->waitForDone();
        QApplication::processEvents();
    }
    const auto* task = window.findChild<TaskPanel*>();
    REQUIRE(task);
    QString text;
    for (const auto* browser : task->findChildren<QTextBrowser*>())
        text += browser->toPlainText();
    CHECK(text.contains("latest.cpp"));
}

TEST_CASE("timer ticks do not invalidate an in-flight Git snapshot", "[gui][audit][git-timer]") {
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    const auto original = app_paths().data_dir();
    app_paths().set_data_dir(directory.filePath("data").toStdString());

    struct Restore {
        std::filesystem::path path;

        ~Restore() { app_paths().set_data_dir(path); }
    } restore{original};

    Application application;
    REQUIRE(application.initialise());
    REQUIRE(application.open_workspace(directory.path().toStdString()));
    application.workspace().record_agent_change(
        path_from_utf8(directory.filePath("timer-first.cpp").toStdString()),
        AgentChangeKind::Modified);
    MainWindow window(application);
    auto* pool = QThreadPool::globalInstance();
    pool->waitForDone();
    const int threads = pool->maxThreadCount();
    pool->setMaxThreadCount(1);
    QSemaphore releaseFirst, enteredFirst, releaseSecond, enteredSecond;

    struct Unblock {
        QThreadPool* pool;
        QSemaphore& first;
        QSemaphore& second;
        int threads;

        ~Unblock() {
            first.release();
            second.release();
            pool->waitForDone();
            pool->setMaxThreadCount(threads);
        }
    } unblock{pool, releaseFirst, releaseSecond, threads};

    auto first = QtConcurrent::run([&] {
        enteredFirst.release();
        releaseFirst.acquire();
    });
    REQUIRE(enteredFirst.tryAcquire(1, 5000));
    REQUIRE(QMetaObject::invokeMethod(&window, "refreshGitState", Qt::DirectConnection));
    // This gate runs after the first scan. Any needless follow-up remains
    // blocked behind it while the first completion is delivered to the GUI.
    auto second = QtConcurrent::run([&] {
        enteredSecond.release();
        releaseSecond.acquire();
    });
    QTimer* gitTimer = nullptr;
    for (auto* timer : window.findChildren<QTimer*>())
        if (timer->interval() == 4000) gitTimer = timer;
    REQUIRE(gitTimer);
    for (int tick = 0; tick < 5; ++tick)
        REQUIRE(QMetaObject::invokeMethod(gitTimer, "timeout", Qt::DirectConnection));
    releaseFirst.release();
    REQUIRE(enteredSecond.tryAcquire(1, 5000));
    QApplication::processEvents();
    const auto* task = window.findChild<TaskPanel*>();
    REQUIRE(task);
    QString text;
    for (const auto* browser : task->findChildren<QTextBrowser*>())
        text += browser->toPlainText();
    CHECK(text.contains("timer-first.cpp"));
}

TEST_CASE("file-only tasks update changed files without queuing a Git refresh",
          "[gui][file-only]") {
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    const auto original = app_paths().data_dir();
    app_paths().set_data_dir(directory.filePath("data").toStdString());

    struct Restore {
        std::filesystem::path path;

        ~Restore() { app_paths().set_data_dir(path); }
    } restore{original};

    Application application;
    REQUIRE(application.initialise());
    REQUIRE(application.open_workspace(directory.path().toStdString()));
    MainWindow window(application);
    auto* pool = QThreadPool::globalInstance();
    pool->waitForDone();
    const int threads = pool->maxThreadCount();
    pool->setMaxThreadCount(1);
    QSemaphore release;
    QSemaphore entered;

    struct Unblock {
        QThreadPool* pool;
        QSemaphore& release;
        int threads;

        ~Unblock() {
            release.release();
            pool->waitForDone();
            pool->setMaxThreadCount(threads);
        }
    } unblock{pool, release, threads};

    auto blocked = QtConcurrent::run([&] {
        entered.release();
        release.acquire();
    });
    REQUIRE(entered.tryAcquire(1, 5000));
    REQUIRE(QMetaObject::invokeMethod(&window, "onRunStarted", Qt::DirectConnection,
                                      Q_ARG(QString, "Edit using only file tools.")));
    application.workspace().record_agent_change(
        std::filesystem::path(directory.filePath("changed.txt").toStdString()),
        AgentChangeKind::Modified);
    REQUIRE(QMetaObject::invokeMethod(&window, "refreshGitState", Qt::DirectConnection));
    const auto* task = window.findChild<TaskPanel*>();
    REQUIRE(task);
    QString text;
    for (const auto* browser : task->findChildren<QTextBrowser*>())
        text += browser->toPlainText();
    CHECK(text.contains("changed.txt"));
    CHECK_FALSE(text.contains("git:"));
    REQUIRE(QMetaObject::invokeMethod(&window, "onRunStarted", Qt::DirectConnection,
                                      Q_ARG(QString, "Inspect the project normally.")));
    application.workspace().record_agent_change(
        std::filesystem::path(directory.filePath("next-task.txt").toStdString()),
        AgentChangeKind::Modified);
    REQUIRE(QMetaObject::invokeMethod(&window, "refreshGitState", Qt::DirectConnection));
    text.clear();
    for (const auto* browser : task->findChildren<QTextBrowser*>())
        text += browser->toPlainText();
    CHECK_FALSE(text.contains("next-task.txt"));
    release.release();
    pool->waitForDone();
    QApplication::processEvents();
    text.clear();
    for (const auto* browser : task->findChildren<QTextBrowser*>())
        text += browser->toPlainText();
    CHECK(text.contains("next-task.txt"));
}

TEST_CASE("long status paths do not force panels outside the window", "[gui][geometry]") {
    StatusStrip strip;
    // A native CI desktop can be narrower than 1280 pixels. Exercise the
    // available width without asking the window manager for an oversized
    // top-level window that it is free to shrink on show().
    const int width = std::min(1280, QApplication::primaryScreen()->availableGeometry().width() - 64);
    REQUIRE(width > 0);
    strip.resize(width, 30);
    strip.setModelName("Qwen3.5-9B", "llama.cpp");
    strip.setActivity("Running: " + QString(3000, 'x'));
    strip.setWorkspace("L:/" + QString(3000, 'x'));
    strip.setContext(12000, 32768, true);
    CHECK(strip.minimumSizeHint().width() < width);
    strip.show();
    QApplication::processEvents();
    CHECK(strip.width() == width);
}

TEST_CASE("model unload leaves the GUI responsive while owned shutdown is queued",
          "[gui][audit][async-unload]") {
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    const auto original = app_paths().data_dir();
    app_paths().set_data_dir(directory.filePath("data").toStdString());

    struct Restore {
        std::filesystem::path path;

        ~Restore() { app_paths().set_data_dir(path); }
    } restore{original};

    Application application;
    REQUIRE(application.initialise());
    application.backends().set_http_client(std::make_shared<RecoveryHealthClient>());
    ModelProfile profile;
    profile.id = "queued-unload-fixture";
    profile.display_name = "Owned shutdown fixture";
    profile.model_path = path_from_utf8(directory.filePath("fixture.gguf").toStdString());
    profile.context_size = 32768;
    profile.gpu_layers = 0;
#if defined(_WIN32)
    profile.server_binary = path_from_utf8(QDir(QApplication::applicationDirPath())
                                               .filePath("localcode_server_fixture.exe")
                                               .toStdString());
#else
    profile.server_binary = path_from_utf8(QDir(QApplication::applicationDirPath())
                                               .filePath("localcode_server_fixture")
                                               .toStdString());
#endif
    REQUIRE(std::filesystem::is_regular_file(profile.server_binary));
    REQUIRE(application.save_model_profile(profile));
    REQUIRE(application.activate_model(profile.id));
    REQUIRE(application.backends().server_status().state == ServerState::Ready);
    REQUIRE(application.backends().server_status().process_id != 0);
    MainWindow window(application);
    auto* pool = QThreadPool::globalInstance();
    pool->waitForDone();
    const int threads = pool->maxThreadCount();
    pool->setMaxThreadCount(1);
    QSemaphore release, entered;

    struct Unblock {
        QThreadPool* pool;
        QSemaphore& release;
        int threads;

        ~Unblock() {
            release.release();
            pool->waitForDone();
            pool->setMaxThreadCount(threads);
        }
    } unblock{pool, release, threads};

    auto blocked = QtConcurrent::run([&] {
        entered.release();
        release.acquire();
    });
    REQUIRE(entered.tryAcquire(1, 5000));
    bool heartbeat = false;
    QTimer::singleShot(0, &window, [&] { heartbeat = true; });
    REQUIRE(QMetaObject::invokeMethod(&window, "unloadModel", Qt::DirectConnection));
    // The pool gate prevents shutdown from beginning. Synchronous unload
    // would already have dropped the backend and joined the owned process.
    CHECK(application.backends().current_backend() != nullptr);
    CHECK(application.backends().server_status().state == ServerState::Ready);
    QApplication::processEvents();
    CHECK(heartbeat);
    auto* strip = window.findChild<StatusStrip*>();
    REQUIRE(strip);
    CHECK(label_text(*strip).contains("Unloading"));
    auto* chat = window.findChild<ChatView*>();
    REQUIRE(chat);
    const auto transcript = chat->toMarkdown();
    auto queuedState = [&](ServerState value) {
        return QMetaObject::invokeMethod(
            &window, "onServerStateChanged", Qt::DirectConnection, Q_ARG(ServerState, value),
            Q_ARG(QString, QStringLiteral("outgoing server callback")), Q_ARG(int, 74));
    };
    // These events may have been queued just before the user requested unload.
    // They describe the outgoing server, not the current user operation.
    for (const auto state : {ServerState::Ready, ServerState::Starting, ServerState::Restarting,
                             ServerState::Failed}) {
        REQUIRE(queuedState(state));
        CHECK(label_text(*strip).contains("Unloading"));
        CHECK(chat->toMarkdown() == transcript);
    }
    release.release();
    pool->waitForDone();
    QApplication::processEvents();
    CHECK(application.backends().current_backend() == nullptr);
    CHECK(application.backends().server_status().process_id == 0);
    CHECK(label_text(*strip).contains("No model"));
    CHECK_FALSE(label_text(*strip).contains("Unloading"));
    // A very late Ready event must not resurrect readiness after the backend
    // has been removed and the unload operation has completed.
    REQUIRE(queuedState(ServerState::Ready));
    CHECK(label_text(*strip).contains("No model"));
    CHECK_FALSE(label_text(*strip).contains("Ready"));
}

TEST_CASE("managed recovery replaces Ready without flooding chat with server logs",
          "[gui][recovery]") {
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    const auto original = app_paths().data_dir();
    app_paths().set_data_dir(directory.path().toStdString());

    struct Restore {
        std::filesystem::path path;

        ~Restore() { app_paths().set_data_dir(path); }
    } restore{original};

    Application application;
    REQUIRE(application.initialise());
    application.backends().set_http_client(std::make_shared<RecoveryHealthClient>());
    ExternalEndpoint endpoint;
    endpoint.enabled = true;
    endpoint.base_url = "http://127.0.0.1:1";
    application.backends().set_external_endpoint(endpoint);
    ModelProfile profile;
    profile.id = "recovery-fixture";
    profile.model_path = "fixture.gguf";
    profile.display_name = "Recovered model";
    profile.context_size = 32768;
    REQUIRE(application.save_model_profile(profile));
    REQUIRE(application.activate_model(profile.id));
    MainWindow window(application);
    auto* strip = window.findChild<StatusStrip*>();
    auto* chat = window.findChild<ChatView*>();
    REQUIRE(strip);
    REQUIRE(chat);
    auto state = [&](ServerState value, const QString& message) {
        return QMetaObject::invokeMethod(&window, "onServerStateChanged", Qt::DirectConnection,
                                         Q_ARG(ServerState, value), Q_ARG(QString, message),
                                         Q_ARG(int, 74));
    };
    REQUIRE(state(ServerState::Ready, "model loaded"));
    CHECK(label_text(*strip).contains("Ready"));
    CHECK(label_text(*strip).contains("Recovered model"));
    CHECK(label_text(*strip).contains("32.8K"));
    const auto transcript = chat->toMarkdown();
    REQUIRE(state(ServerState::Restarting,
                  "Qwen server stopped unexpectedly. Restarting (1/1).\n"
                  "technical server tail"));
    CHECK(label_text(*strip).contains("Restarting"));
    CHECK_FALSE(label_text(*strip).contains("Ready"));
    CHECK_FALSE(label_text(*strip).contains("Recovered model"));
    CHECK_FALSE(label_text(*strip).contains("32.8K"));
    CHECK(chat->toMarkdown() == transcript);
    const auto* recoveryTimeline = window.findChild<ActivityTimeline*>();
    REQUIRE(recoveryTimeline);
    const auto* recoveryNotes = recoveryTimeline->findChild<QListWidget*>();
    REQUIRE(recoveryNotes);
    REQUIRE(recoveryNotes->count() == 1);
    CHECK(recoveryNotes->item(0)->text().startsWith(QStringLiteral("·")));
    REQUIRE(state(ServerState::Ready, "automatic recovery succeeded"));
    CHECK(label_text(*strip).contains("Ready"));
    CHECK(label_text(*strip).contains("Recovered model"));
    CHECK(label_text(*strip).contains("32.8K"));
    CHECK(chat->toMarkdown() == transcript);
    CHECK_FALSE(recoveryNotes->item(0)->text().startsWith(QStringLiteral("→")));
    for (const auto* timer : strip->findChildren<QTimer*>())
        CHECK_FALSE(timer->isActive());
    REQUIRE(state(ServerState::Restarting, "Restarting (1/1)."));
    REQUIRE(state(ServerState::Failed,
                  "Automatic recovery failed or its restart budget is exhausted.\n"
                  "technical server tail"));
    CHECK(label_text(*strip).contains("Failed"));
    CHECK(chat->toMarkdown() != transcript);
    CHECK(chat->toMarkdown().contains("restart budget is exhausted"));
    CHECK(chat->toMarkdown().contains("Ctrl+J"));
    CHECK_FALSE(chat->toMarkdown().contains("technical server tail"));
    for (const auto* timer : strip->findChildren<QTimer*>())
        CHECK_FALSE(timer->isActive());
}

TEST_CASE("closing during a tool saves its terminal result before marking a clean shutdown",
          "[gui][lifecycle][persistence]") {
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    const auto original = app_paths().data_dir();

    struct Restore {
        std::filesystem::path path;

        ~Restore() { app_paths().set_data_dir(path); }
    } restore{original};

    app_paths().set_data_dir(path_from_utf8(directory.filePath("data").toStdString()));
    const auto project = directory.filePath("project");
    REQUIRE(QDir().mkpath(project));
    Application application;
    REQUIRE(application.initialise());
    REQUIRE(application.open_workspace(path_from_utf8(project.toStdString())));
    const auto conversation = application.start_conversation("Close during a tool");
    REQUIRE(conversation);
    REQUIRE(application.app_state_repository().set("session.clean_shutdown", "0"));
    application.agent().set_environment(nullptr);
    application.agent().set_skills(nullptr);

    QSemaphore entered, release;

    class DelayedTool final : public ITool {
    public:
        DelayedTool(QSemaphore& entered, QSemaphore& release)
            : entered_(entered), release_(release) {}

        std::string name() const override { return "delayed_close_probe"; }

        std::string description() const override { return "A bounded delayed tool result."; }

        Json parameters_schema() const override { return Json{{"type", "object"}}; }

        ToolResult execute(const Json&, ToolContext&) override {
            entered_.release();
            if (!release_.tryAcquire(1, 5000))
                return ToolResult::failure("fixture timed out", "tool was not released");
            return ToolResult::success("Tool stopped", "TERMINAL_TOOL_RESULT_AFTER_STOP");
        }

    private:
        QSemaphore& entered_;
        QSemaphore& release_;
    };

    application.tools().add(std::make_shared<DelayedTool>(entered, release));
    auto backend = std::make_shared<test::MockBackend>();
    backend->then_call("delayed_close_probe", Json::object());
    application.agent().set_backend(backend);
    ModelProfile profile;
    profile.id = "mock";
    profile.context_size = 32768;
    profile.capabilities.supports_tools = true;
    profile.tool_protocol = ToolProtocol::Native;
    application.agent().set_profile(profile);

    MainWindow window(application);

    struct Unblock {
        QSemaphore& release;

        ~Unblock() { release.release(); }
    } unblock{release};

    window.show();
    auto* controller = window.findChild<AgentController*>();
    REQUIRE(controller);
    const auto pumpUntil = [](auto ready) {
        QElapsedTimer elapsed;
        elapsed.start();
        while (!ready() && elapsed.elapsed() < 2000) {
            QApplication::processEvents(QEventLoop::AllEvents, 20);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return ready();
    };
    controller->send("Run the delayed tool.");
    REQUIRE(pumpUntil([&] { return entered.available() > 0; }));
    bool acceptedClose = false;
    QTimer::singleShot(0, [&] {
        if (auto* question = qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) {
            if (auto* yes = question->button(QMessageBox::Yes)) {
                acceptedClose = true;
                yes->click();
            } else {
                question->reject();
            }
        }
    });
    CHECK_FALSE(window.close());
    REQUIRE(acceptedClose);
    auto* pendingClose = window.findChild<QTimer*>("PendingCloseTimer");
    REQUIRE(pendingClose);
    REQUIRE(pendingClose->isActive());
    CHECK(window.isVisible());
    CHECK(controller->busy());
    auto clean = application.app_state_repository().get("session.clean_shutdown");
    REQUIRE(clean);
    CHECK(*clean == "0");
    bool responsive = false;
    QTimer::singleShot(0, &window, [&] { responsive = true; });
    REQUIRE(pumpUntil([&] { return responsive; }));

    release.release();
    REQUIRE(pumpUntil([&] { return !window.isVisible(); }));
    CHECK_FALSE(controller->busy());
    const auto saved = application.conversation_repository().messages(*conversation);
    REQUIRE(saved);
    bool terminal_saved = false;
    for (const auto& message : *saved)
        terminal_saved |=
            message.message.role == Role::Tool &&
            message.message.content.find("TERMINAL_TOOL_RESULT_AFTER_STOP") != std::string::npos;
    CHECK(terminal_saved);
    clean = application.app_state_repository().get("session.clean_shutdown");
    REQUIRE(clean);
    CHECK(*clean == "1");
}

TEST_CASE("shutdown cancels a confirmation queued to a blocked GUI thread", "[gui][lifecycle]") {
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    const auto original = app_paths().data_dir();
    app_paths().set_data_dir(directory.path().toStdString());

    struct Restore {
        std::filesystem::path path;

        ~Restore() { app_paths().set_data_dir(path); }
    } restore{original};

    Application application;
    REQUIRE(application.initialise());
    REQUIRE(application.open_workspace(directory.path().toStdString()));
    application.tools().add(std::make_shared<ConfirmationProbe>());
    auto backend = std::make_shared<test::MockBackend>();
    backend->then_call("confirmation_probe", Json::object());
    application.agent().set_backend(backend);
    ModelProfile profile;
    profile.id = "mock";
    profile.context_size = 32768;
    profile.capabilities.supports_tools = true;
    profile.tool_protocol = ToolProtocol::Native;
    application.agent().set_profile(profile);
    std::atomic<bool> requested = false;
    auto controller = std::make_unique<AgentController>(application);
    QObject::connect(
        controller.get(), &AgentController::confirmationRequested, controller.get(),
        [&] { requested.store(true); }, Qt::DirectConnection);
    controller->send("run the requested command");
    // Initial environment discovery can run several local --version probes.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    // Deliberately do not dispatch GUI events: destruction used to join a
    // worker blocked in BlockingQueuedConnection and never return.
    while (!requested.load() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    REQUIRE(requested.load());
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    const auto start = std::chrono::steady_clock::now();
    controller.reset();
    CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds(2));
    QApplication::processEvents();
}

TEST_CASE("Discord plugin installation excludes private files and rejects path escapes",
          "[gui][plugin]") {
    QTemporaryDir package, storage;
    REQUIRE(package.isValid());
    REQUIRE(storage.isValid());
    const auto previous = app_paths().data_dir().parent_path();

    struct RestorePaths {
        std::filesystem::path root;

        ~RestorePaths() { app_paths().set_root(root); }
    } restore{previous};

    app_paths().set_root(storage.path().toStdString());
    auto write = [&](const QString& name, const QByteArray& body) {
        QFile file(package.filePath(name));
        REQUIRE(file.open(QIODevice::WriteOnly));
        REQUIRE(file.write(body) == body.size());
    };
    write("lcd.exe", "public worker fixture");
    write(".env", "DISCORD_BOT_TOKEN=never-copy-this");
    write("persona.private.txt", "private group notes");
    write("manifest.json", R"({"id":"discord","protocol":1,"executable":"../lcd.exe"})");
    REQUIRE_FALSE(DiscordPlugin::validatePackage(package.filePath("manifest.json")).isEmpty());
    write("manifest.json", R"({"id":"discord","protocol":9,"executable":"lcd.exe"})");
    REQUIRE_FALSE(DiscordPlugin::validatePackage(package.filePath("manifest.json")).isEmpty());
    write("manifest.json", R"({"id":"discord","protocol":1,"executable":"lcd.exe"})");
    DiscordPlugin plugin;
    REQUIRE(plugin.installPackage(package.filePath("manifest.json")).isEmpty());
    REQUIRE(plugin.available());
    REQUIRE_FALSE(plugin.selectInstance("../another-bot").isEmpty());
    REQUIRE(plugin.selectInstance("second-bot").isEmpty());
    REQUIRE(plugin.home().endsWith("/instances/second-bot"));
    QDirIterator files(storage.path(), QDir::Files | QDir::Hidden, QDirIterator::Subdirectories);
    while (files.hasNext()) {
        files.next();
        REQUIRE(files.fileName() != ".env");
        REQUIRE(files.fileName() != "persona.private.txt");
    }
}

TEST_CASE("native Discord panel configures the real Rust worker in isolated storage",
          "[gui][plugin][integration]") {
    const auto manifest = qEnvironmentVariable("LOCALCODE_TEST_DISCORD_PACKAGE");
    if (manifest.isEmpty())
        SKIP("Set LOCALCODE_TEST_DISCORD_PACKAGE to run the real process contract.");
    QTemporaryDir storage;
    REQUIRE(storage.isValid());
    const auto previous = app_paths().data_dir().parent_path();

    struct RestorePaths {
        std::filesystem::path root;

        ~RestorePaths() { app_paths().set_root(root); }
    } restore{previous};

    app_paths().set_root(storage.path().toStdString());
    DiscordPlugin plugin;
    REQUIRE(plugin.installPackage(manifest).isEmpty());
    auto request = [&](const QString& command, const QJsonObject& input = QJsonObject{}) {
        QEventLoop loop;
        QJsonObject output;
        QString error;
        const auto success = QObject::connect(&plugin, &DiscordPlugin::completed, &loop,
                                              [&](const QString&, const QJsonObject& result) {
                                                  output = result;
                                                  loop.quit();
                                              });
        const auto failure =
            QObject::connect(&plugin, &DiscordPlugin::failed, &loop, [&](const QString& message) {
                error = message;
                loop.quit();
            });
        QTimer::singleShot(15000, &loop, [&] {
            error = "test timeout";
            loop.quit();
        });
        plugin.request(command, input);
        if (plugin.busy()) loop.exec();
        QObject::disconnect(success);
        QObject::disconnect(failure);
        INFO(error.toStdString());
        REQUIRE(error.isEmpty());
        return output;
    };
    request("init");
    auto settings = request("settings");
    REQUIRE(settings.value("channelIds").toString().isEmpty());
    REQUIRE(settings.value("contextSize").toInt() == 8192);
    REQUIRE_FALSE(settings.contains("token"));
    settings.insert("guildIds", "943486363070787594");
    settings.insert("ownerIds", "1247154641658052608");
    settings.insert("channelIds", "1549853020811038881");
    settings.insert("token", "test.isolated.token");
    settings.insert("persona", "# Persona\nA private test group.");
    request("save", settings);
    const auto saved = request("settings");
    REQUIRE(saved.value("ownerIds").toString() == "1247154641658052608");
    REQUIRE(saved.value("tokenConfigured").toBool());
    REQUIRE_FALSE(QJsonDocument(saved).toJson().contains("test.isolated.token"));
    REQUIRE_FALSE(request("status").value("running").toBool());
    PluginsPanel panel;
    panel.resize(900, 720);
    panel.show();
    auto* save = panel.findChild<QPushButton*>("SaveDiscordSettings");
    REQUIRE(save != nullptr);
    QElapsedTimer wait;
    wait.start();
    while (!save->isEnabled() && wait.elapsed() < 15000) {
        QApplication::processEvents();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    REQUIRE(save->isEnabled());
    REQUIRE(panel.findChild<QLineEdit*>("DiscordOwners")->text() == "1247154641658052608");
    REQUIRE(panel.findChild<QLineEdit*>("DiscordToken")->echoMode() == QLineEdit::Password);
    auto* tune = panel.findChild<QPushButton*>("TuneDiscordModel");
    REQUIRE(tune != nullptr);
    REQUIRE(tune->isEnabled());
    bool edited = false;
    QTimer::singleShot(0, &panel, [&] {
        auto* dialog = qobject_cast<InferenceTuningDialog*>(QApplication::activeModalWidget());
        if (!dialog) return;
        dialog->findChild<QPushButton*>("TuningCpuPreset")->click();
        dialog->findChild<QSpinBox*>("TuningThreads")->setValue(8);
        auto* placement = dialog->findChild<QComboBox*>("TuningCpuPlacement");
        REQUIRE(placement);
        REQUIRE(placement->isEnabled());
        REQUIRE(placement->findData("physical_pool") >= 0);
        placement->setCurrentIndex(placement->findData("physical_pool"));
        auto* hardware = dialog->findChild<QLabel*>("TuningHardware");
        QElapsedTimer detection;
        detection.start();
        while (hardware->text().contains("Detecting") && detection.elapsed() < 5000) {
            QApplication::processEvents();
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        CHECK(hardware->text().contains("logical CPUs"));
        QApplication::processEvents();   // Settle the layout after the asynchronous label update.
        const auto image = qEnvironmentVariable("LOCALCODE_TUNING_SCREENSHOT");
        if (!image.isEmpty()) CHECK(dialog->grab().save(image));
        edited = true;
        dialog->accept();
    });
    tune->click();
    REQUIRE(edited);
    save->click();
    wait.restart();
    while (!save->isEnabled() && wait.elapsed() < 15000) {
        QApplication::processEvents();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    REQUIRE(save->isEnabled());
    const auto tuned = request("settings");
    CHECK(tuned.value("contextSize").toInt() == 4096);
    CHECK(tuned.value("gpuLayers").toInt() == 0);
    CHECK(tuned.value("tuning").toObject().value("threads").toInt() == 8);
    CHECK(tuned.value("tuning").toObject().value("cpu_placement").toString() == "physical_pool");
    CHECK(tuned.value("tuning").toObject().value("ubatch_size").toInt() == 128);
    CHECK(tuned.value("tuning").toObject().value("prompt_cache_mib").toInt() == 128);
    auto* history = panel.findChild<QSpinBox*>("DiscordHistoryMessages");
    REQUIRE(history);
    REQUIRE(history->isEnabled());
    history->setValue(37);
    panel.findChild<QSpinBox*>("DiscordReplyDepth")->setValue(11);
    panel.findChild<QSpinBox*>("DiscordHistoryAge")->setValue(7200);
    panel.findChild<QSpinBox*>("DiscordHistoryBudget")->setValue(2048);
    panel.findChild<QSpinBox*>("DiscordContext")->setValue(24576);
    panel.findChild<QPlainTextEdit*>("DiscordConversationNotes")
        ->setPlainText("User context notes: fishing plans stay with their authors.");
    save->click();
    wait.restart();
    while (!save->isEnabled() && wait.elapsed() < 15000) {
        QApplication::processEvents();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    REQUIRE(save->isEnabled());
    const auto conversation = request("settings");
    CHECK(conversation.value("tuning").toObject().value("cpu_placement").toString() ==
          "physical_pool");
    CHECK(conversation.value("contextSize").toInt() == 24576);
    CHECK(conversation.value("conversationNotes").toString().contains("User context notes"));
    const auto retainedHistory = conversation.value("conversation").toObject();
    CHECK(retainedHistory.value("recentMessages").toInt() == 37);
    CHECK(retainedHistory.value("replyChainDepth").toInt() == 11);
    CHECK(retainedHistory.value("historyMaxAgeS").toInt() == 7200);
    CHECK(retainedHistory.value("historyBudgetTokens").toInt() == 2048);
    const auto conversationImage = qEnvironmentVariable("LOCALCODE_CONVERSATION_SCREENSHOT");
    if (!conversationImage.isEmpty()) {
        auto* tabs = panel.findChild<QTabWidget*>();
        REQUIRE(tabs);
        for (int i = 0; i < tabs->count(); ++i) {
            if (tabs->tabText(i) == "Conversation") tabs->setCurrentIndex(i);
        }
        QApplication::processEvents();
        CHECK(panel.grab().save(conversationImage));
    }
    auto* panelPlugin = panel.findChild<DiscordPlugin*>();
    auto* persona = panel.findChild<QPlainTextEdit*>("DiscordPersona");
    REQUIRE(panelPlugin);
    REQUIRE(persona);
    panelPlugin->request("status");
    REQUIRE(panelPlugin->busy());
    REQUIRE(panelPlugin->refreshingStatus());
    CHECK(persona->isEnabled());
    CHECK_FALSE(save->isEnabled());
    bool tuningOpenedDuringStatus = false;
    QTimer::singleShot(0, &panel, [&] {
        if (auto* dialog =
                qobject_cast<InferenceTuningDialog*>(QApplication::activeModalWidget())) {
            tuningOpenedDuringStatus = true;
            dialog->reject();
        }
    });
    tune->click();
    CHECK(tuningOpenedDuringStatus);
    persona->insertPlainText("\nDraft retained during status refresh.");
    wait.restart();
    while (panelPlugin->busy() && wait.elapsed() < 15000) {
        QApplication::processEvents();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    REQUIRE_FALSE(panelPlugin->busy());
    CHECK(persona->toPlainText().contains("Draft retained during status refresh."));
    CHECK(save->isEnabled());
    panelPlugin->request("settings");
    REQUIRE(panelPlugin->busy());
    CHECK_FALSE(panelPlugin->refreshingStatus());
    CHECK_FALSE(persona->isEnabled());
    wait.restart();
    while (!save->isEnabled() && wait.elapsed() < 15000) {
        QApplication::processEvents();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    REQUIRE(save->isEnabled());
    panel.findChild<QTabWidget*>()->setCurrentIndex(1);
    const auto screenshot = qEnvironmentVariable("LOCALCODE_PLUGIN_SCREENSHOT");
    if (!screenshot.isEmpty()) REQUIRE(panel.grab().save(screenshot));
}

TEST_CASE("performance dialogs validate prompt microbatch limits", "[gui][tuning]") {
    for (const bool plugin : {false, true}) {
        CAPTURE(plugin);
        InferenceTuningDialog dialog({{"batch_size", 64}, {"ubatch_size", 128}}, plugin);
        auto* buttons = dialog.findChild<QDialogButtonBox*>();
        auto* microbatch = dialog.findChild<QSpinBox*>("TuningUbatch");
        REQUIRE(buttons);
        REQUIRE(microbatch);
        bool accepted = false;
        QObject::connect(&dialog, &QDialog::accepted, &dialog, [&] { accepted = true; });
        buttons->button(QDialogButtonBox::Ok)->click();
        CHECK_FALSE(accepted);
        CHECK(dialog.findChild<QLabel*>("TuningHardware")->text().contains("Microbatch"));
        microbatch->setValue(32);
        buttons->button(QDialogButtonBox::Ok)->click();
        CHECK(accepted);
        CHECK(dialog.settings().value("ubatch_size").toInt() == 32);
    }
}

TEST_CASE("unavailable acceleration retains a legacy choice until the owner disables it",
          "[gui][tuning]") {
    InferenceTuningDialog dialog({{"ngram_speculation", true}}, false);
    auto* choices = dialog.findChild<QComboBox*>("TuningSpeculation");
    auto* buttons = dialog.findChild<QDialogButtonBox*>();
    REQUIRE(choices);
    REQUIRE(buttons);
    CHECK(choices->currentData().toBool());
    CHECK_FALSE(choices->model()->flags(choices->model()->index(1, 0)) & Qt::ItemIsEnabled);
    bool accepted = false;
    QObject::connect(&dialog, &QDialog::accepted, &dialog, [&] { accepted = true; });
    buttons->button(QDialogButtonBox::Ok)->click();
    CHECK_FALSE(accepted);
    CHECK(dialog.findChild<QLabel*>("TuningHardware")->text().contains("Choose Disabled"));
    const auto screenshot = qEnvironmentVariable("LOCALCODE_NGRAM_WITHDRAWAL_SCREENSHOT");
    if (!screenshot.isEmpty()) {
        dialog.resize(680, 720);
        dialog.show();
        QApplication::processEvents();
        REQUIRE(dialog.grab().save(screenshot));
    }
    choices->setCurrentIndex(0);
    buttons->button(QDialogButtonBox::Ok)->click();
    CHECK(accepted);
    CHECK_FALSE(dialog.settings().value("ngram_speculation").toBool());
}

TEST_CASE("main model edits preserve default and explicit prompt cache budgets",
          "[gui][tuning][cache]") {
    QTemporaryDir storage;
    REQUIRE(storage.isValid());
    const auto original = app_paths().data_dir();

    struct Restore {
        std::filesystem::path path;

        ~Restore() { app_paths().set_data_dir(path); }
    } restore{original};

    app_paths().set_data_dir(path_from_utf8(storage.path().toStdString()));
    Application application;
    REQUIRE(application.initialise());
    ModelProfile profile;
    profile.id = "main-panel-cache-fixture";
    profile.display_name = "Main panel cache fixture";
    profile.model_path = "fixture.gguf";
    SECTION("runtime default remains absent") {}
    SECTION("explicit disabled cache remains zero") {
        profile.prompt_cache_mib = 0;
    }
    SECTION("explicit cache budget is preserved") {
        profile.prompt_cache_mib = 256;
    }
    SECTION("invalid persisted values remain visible to validation") {
        profile.prompt_cache_mib = -1;
    }
    REQUIRE(application.save_model_profile(profile));
    // Exercise the main Apply path, without opening the tuning dialog (which
    // already strips its own display-only -1 sentinel).
    for (int reopen = 0; reopen < 2; ++reopen) {
        ModelPanel panel(application);
        QSpinBox* context = nullptr;
        for (auto* spin : panel.findChildren<QSpinBox*>())
            if (spin->minimum() == 1024) context = spin;
        REQUIRE(context);
        context->setValue(4096 + reopen * 1024);
        QPushButton* apply = nullptr;
        for (auto* button : panel.findChildren<QPushButton*>())
            if (button->text() == "Apply") apply = button;
        REQUIRE(apply);
        apply->click();
        const auto saved = application.model_profiles();
        REQUIRE(saved);
        REQUIRE(saved->size() == 1);
        CHECK(saved->front().context_size == static_cast<std::uint32_t>(context->value()));
        CHECK(saved->front().prompt_cache_mib == profile.prompt_cache_mib);
    }
}

TEST_CASE("model performance dialog saves through the real profile repository", "[gui][tuning]") {
    QTemporaryDir storage;
    REQUIRE(storage.isValid());
    const auto original = app_paths().data_dir();

    struct Restore {
        std::filesystem::path path;

        ~Restore() { app_paths().set_data_dir(path); }
    } restore{original};

    app_paths().set_data_dir(storage.path().toStdString());
    Application application;
    REQUIRE(application.initialise());
    ModelProfile profile;
    profile.id = "tuning-fixture";
    profile.display_name = "Tuning fixture";
    profile.model_path = "fixture.gguf";
    profile.context_size = 8192;
    profile.cache_type_k = "q8_0";
    profile.prompt_cache_mib = 256;
    profile.weight_repacking = WeightRepacking::Disabled;
    profile.ngram_speculation = true;   // Previously saved selection must be recoverable.
    REQUIRE(application.save_model_profile(profile));
    ModelPanel panel(application);
    panel.resize(800, 720);
    panel.show();
    auto* tune = panel.findChild<QPushButton*>("TuneModelProfile");
    bool captured = true;
    bool microbatch_preset = false;
    REQUIRE(tune != nullptr);
    REQUIRE(tune->isEnabled());
    int requested_context = 4096;
    SECTION("CPU preset context") {}
    SECTION("owner context above the former main-panel limit") {
        requested_context = 2097152;
    }
    bool edited = false;
    QTimer::singleShot(0, &panel, [&] {
        auto* dialog = qobject_cast<InferenceTuningDialog*>(QApplication::activeModalWidget());
        if (!dialog) return;
        auto* speculation = dialog->findChild<QComboBox*>("TuningSpeculation");
        if (!speculation || !speculation->currentData().toBool()) return;
        if (speculation->model()->flags(speculation->model()->index(1, 0)) & Qt::ItemIsEnabled)
            return;
        auto* repacking = dialog->findChild<QComboBox*>("TuningWeightRepacking");
        if (!repacking || repacking->currentIndex() != 2) return;
        dialog->findChild<QPushButton*>("TuningCpuPreset")->click();
        if (repacking->currentIndex() != 2) return;
        repacking->setCurrentIndex(requested_context == 4096 ? 1 : 0);
        auto* promptCache = dialog->findChild<QSpinBox*>("TuningCacheRam");
        if (!promptCache || promptCache->value() != 128) return;
        promptCache->setValue(requested_context == 4096 ? 64 : -1);
        dialog->findChild<QSpinBox*>("TuningContext")->setValue(requested_context);
        dialog->findChild<QSpinBox*>("TuningThreads")->setValue(4);
        dialog->findChild<QSpinBox*>("TuningThreadsBatch")->setValue(6);
        auto* microbatch = dialog->findChild<QSpinBox*>("TuningUbatch");
        if (!microbatch) return;
        microbatch_preset = microbatch->value() == 128;
        microbatch->setValue(64);
        dialog->findChild<QSpinBox*>("TuningBatch")->setValue(0);
        if (!speculation || speculation->currentData().toBool()) return;
        const auto screenshot = qEnvironmentVariable("LOCALCODE_ROOT_TUNING_SCREENSHOT");
        if (!screenshot.isEmpty()) {
            QApplication::processEvents();
            captured = dialog->grab().save(screenshot);
        }
        edited = true;
        dialog->accept();
    });
    tune->click();
    REQUIRE(edited);
    CHECK(captured);
    CHECK(microbatch_preset);
    const auto saved = application.model_profiles();
    REQUIRE(saved);
    REQUIRE(saved->size() == 1);
    CHECK(saved->front().context_size == static_cast<std::uint32_t>(requested_context));
    CHECK(saved->front().gpu_layers == 0);
    CHECK(saved->front().threads == 4);
    CHECK(saved->front().threads_batch == 6);
    CHECK(saved->front().batch_size == 0);
    CHECK(saved->front().ubatch_size == 64);
    if (requested_context == 4096)
        CHECK(saved->front().prompt_cache_mib == 64);
    else
        CHECK_FALSE(saved->front().prompt_cache_mib.has_value());
    CHECK(saved->front().weight_repacking ==
          (requested_context == 4096 ? WeightRepacking::Enabled : WeightRepacking::RuntimeDefault));
    CHECK_FALSE(saved->front().ngram_speculation);
    CHECK(saved->front().cache_type_k.empty());
}

TEST_CASE("plugin view restores the coding panels when returning to chat",
          "[gui][plugin][layout]") {
    QTemporaryDir storage;
    REQUIRE(storage.isValid());
    const auto original = app_paths().data_dir();

    struct Restore {
        std::filesystem::path path;

        ~Restore() { app_paths().set_data_dir(path); }
    } restore{original};

    app_paths().set_data_dir(storage.path().toStdString());
    Application application;
    REQUIRE(application.initialise());
    MainWindow window(application);
    auto* sidebar = window.findChild<QWidget*>("ProjectSidebar");
    REQUIRE(sidebar);
    auto* toolbar = window.findChild<QToolBar*>("CodingToolbar");
    REQUIRE(toolbar);
    auto* tabs = window.findChild<ChatView*>()->parentWidget();
    while (tabs && !qobject_cast<QTabWidget*>(tabs))
        tabs = tabs->parentWidget();
    REQUIRE(tabs);
    qobject_cast<QTabWidget*>(tabs)->setCurrentIndex(0);
    window.show();
    QApplication::processEvents();
    const bool sidebarBefore = !sidebar->isHidden();
    window.showPlugins();
    QApplication::processEvents();
    CHECK(sidebar->isHidden());
    CHECK(toolbar->isHidden());
    qobject_cast<QTabWidget*>(tabs)->setCurrentIndex(0);
    QApplication::processEvents();
    CHECK(sidebar->isHidden() == !sidebarBefore);
    CHECK_FALSE(toolbar->isHidden());
}

TEST_CASE("short windows retain reachable coding and settings controls",
          "[gui][geometry][short-window]") {
    QTemporaryDir storage;
    REQUIRE(storage.isValid());
    const auto original = app_paths().data_dir();

    struct Restore {
        std::filesystem::path path;
        QMap<QString, QVariant> settings;

        ~Restore() {
            app_paths().set_data_dir(path);
            QSettings saved;
            saved.clear();
            for (auto it = settings.cbegin(); it != settings.cend(); ++it)
                saved.setValue(it.key(), it.value());
        }
    } restore{original, {}};

    QSettings saved;
    for (const auto& key : saved.allKeys())
        restore.settings.insert(key, saved.value(key));
    saved.clear();
    app_paths().set_data_dir(path_from_utf8(storage.path().toStdString()));
    Application application;
    REQUIRE(application.initialise());
    MainWindow window(application);
    auto* chat = window.findChild<ChatView*>();
    REQUIRE(chat);
    auto* ancestor = chat->parentWidget();
    while (ancestor && !qobject_cast<QTabWidget*>(ancestor))
        ancestor = ancestor->parentWidget();
    auto* centre = qobject_cast<QTabWidget*>(ancestor);
    REQUIRE(centre);
    centre->setCurrentIndex(0);
    window.show();
    QApplication::processEvents();
    int width = 800;
    QPushButton* important = nullptr;
    SECTION("coding without the inactive plugin form imposing its height") {
        for (auto* button : window.findChildren<QPushButton*>())
            if (button->text() == "Send") important = button;
    }
    SECTION("plugin settings can scroll to their save action") {
        window.showPlugins();
        important = window.findChild<QPushButton*>("SaveDiscordSettings");
    }
    SECTION("model inspector can scroll while coding stays visible") {
        width = 1024;
        for (auto* button : window.findChildren<QPushButton*>())
            if (button->text() == "Details") button->click();
        auto* models = window.findChild<ModelPanel*>();
        REQUIRE(models);
        auto* parent = models->parentWidget();
        while (parent && !qobject_cast<QTabWidget*>(parent))
            parent = parent->parentWidget();
        auto* tabs = qobject_cast<QTabWidget*>(parent);
        REQUIRE(tabs);
        tabs->setCurrentIndex(2);
        important = models->findChild<QPushButton*>("Primary");
    }
    REQUIRE(important);
    window.resize(width, 600);
    QApplication::processEvents();
    CHECK(window.width() <= width);
    CHECK(window.height() <= 600);
    // A button in a scrolling form must remain reachable at this client size.
    auto* parent = important->parentWidget();
    while (parent && !qobject_cast<QScrollArea*>(parent))
        parent = parent->parentWidget();
    if (auto* scroll = qobject_cast<QScrollArea*>(parent)) {
        scroll->ensureWidgetVisible(important);
        QApplication::processEvents();
        CHECK(scroll->viewport()->rect().contains(
            QRect(important->mapTo(scroll->viewport(), QPoint()), important->size())));
    }
    CHECK_FALSE(important->visibleRegion().isEmpty());
}

int main(int argc, char** argv) {
#ifdef Q_OS_WIN
    // This suite uses native Windows widgets. An inherited headless QPA choice
    // (for example from a Python tool) must not select an undeployed plugin.
    // Set it before QApplication: changing it afterwards cannot recover startup.
    qputenv("QT_QPA_PLATFORM", "windows");
#endif
    QApplication app(argc, argv);
    if (auto* fusion = QStyleFactory::create("Fusion")) QApplication::setStyle(fusion);
#ifdef Q_OS_WIN
    // Keep the font fixtures explicit for consistent layout assertions.
    const auto fonts = qEnvironmentVariable("WINDIR") + "/Fonts/";
    QFontDatabase::addApplicationFont(fonts + "segoeui.ttf");
    QFontDatabase::addApplicationFont(fonts + "consola.ttf");
#endif
    app.setOrganizationName("LocalCodeTests");
    app.setApplicationName("LocalCodeTests");
    QTemporaryDir runtime;
    if (!runtime.isValid()) return 1;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, runtime.path());
    app_paths().set_root(runtime.path().toStdString());
    Theme::instance().apply();
    const int result = Catch::Session().run(argc, argv);
    Logger::instance().configure({}, LogLevel::Error);
    return result;
}
