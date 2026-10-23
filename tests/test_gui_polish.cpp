#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QDir>
#include <QEnterEvent>
#include <QFile>
#include <QFontInfo>
#include <QFontMetrics>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QTemporaryDir>
#include <QTextBlock>
#include <QTextBrowser>
#include <QTextLayout>
#include <QToolButton>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>

#include "MainWindow.h"
#include "Theme.h"
#include "dialogs/SettingsDialog.h"
#include "platform/PathEncoding.h"
#include "platform/Paths.h"
#include "widgets/ChatView.h"
#include "widgets/ModelPanel.h"
#include "widgets/ProjectExplorer.h"
#include "widgets/PromptComposer.h"
#include "widgets/TaskPanel.h"

using namespace localcode;
using namespace localcode::gui;

namespace {
void capture(QWidget& widget, const QString& name) {
    QApplication::processEvents();
    const auto directory = qEnvironmentVariable("LOCALCODE_GUI_EVIDENCE_DIR");
    if (directory.isEmpty()) return;
    REQUIRE(QDir().mkpath(directory));
    REQUIRE(widget.grab().save(directory + '/' + name + ".png"));
    if (auto* chat = widget.findChild<ChatView*>()) {
        auto* browser = chat->findChild<QTextBrowser*>();
        QJsonArray fonts;
        for (const auto& text : {QString("A focused update"), QString("The layout stays familiar"),
                                 QString("Check"), QString("Result")}) {
            const auto cursor = browser->document()->find(text);
            if (cursor.isNull()) continue;
            const auto font =
                cursor.charFormat().font().resolve(browser->document()->defaultFont());
            const QFontInfo info(font);
            fonts.append(QJsonObject{{"text", text},
                                     {"requested", font.family()},
                                     {"resolved", info.family()},
                                     {"points", info.pointSizeF()}});
        }
        QFile details(directory + '/' + name + "-fonts.json");
        REQUIRE(details.open(QIODevice::WriteOnly));
        details.write(QJsonDocument(fonts).toJson());
    }
}

void checkButtons(QWidget& surface) {
    const auto buttons = surface.findChildren<QAbstractButton*>();
    for (auto* button : buttons) {
        if (button->visibleRegion().isEmpty()) continue;
        const auto before = button->geometry();
        QEnterEvent enter(QPointF(2, 2), QPointF(2, 2), QPointF(button->mapToGlobal(QPoint(2, 2))));
        QApplication::sendEvent(button, &enter);
        QApplication::processEvents();
        INFO(button->text().toStdString());
        CHECK(button->geometry() == before);
        const QRect bounds(button->mapTo(&surface, QPoint()), button->size());
        for (auto* other : buttons) {
            if (other == button || other->visibleRegion().isEmpty()) continue;
            const QRect otherBounds(other->mapTo(&surface, QPoint()), other->size());
            CHECK_FALSE(bounds.intersects(otherBounds));
        }
        QEvent leave(QEvent::Leave);
        QApplication::sendEvent(button, &leave);
    }
}
}   // namespace

TEST_CASE("composer grows for wrapped paragraphs and shrinks after editing", "[gui][ui-polish]") {
    PromptComposer composer;
    composer.resize(360, composer.minimumHeight());
    composer.show();
    QApplication::processEvents();
    const int emptyHeight = composer.height();
    composer.setPlainText(
        QString("Please explain the changes and check the result. ").repeated(12));
    QApplication::processEvents();
    const int wrappedLines = composer.document()->firstBlock().layout()->lineCount();
    INFO("wrapped lines: " << wrappedLines << ", empty height: " << emptyHeight
                           << ", current height: " << composer.height());
    capture(composer, "composer-wrapped");
    REQUIRE(wrappedLines > 3);
    CHECK(composer.height() > emptyHeight);
    const int narrowHeight = composer.height();
    composer.resize(720, composer.height());
    QApplication::processEvents();
    CHECK(composer.height() < narrowHeight);
    composer.clearPrompt();
    QApplication::processEvents();
    CHECK(composer.height() == emptyHeight);
}

TEST_CASE("chat keeps a reader's position when a new assistant segment starts",
          "[gui][ui-polish][chat]") {
    ChatView chat;
    chat.resize(600, 360);
    chat.show();
    for (int i = 0; i < 30; ++i)
        chat.addMessage(
            i % 2 ? ChatRole::Assistant : ChatRole::User,
            QString("Message %1\n\nKeep this paragraph readable while the answer streams.").arg(i));
    QApplication::processEvents();
    auto* browser = chat.findChild<QTextBrowser*>();
    REQUIRE(browser);
    auto* scroll = browser->verticalScrollBar();
    REQUIRE(scroll->maximum() > 0);
    scroll->setValue(0);
    chat.beginAssistantMessage();
    chat.appendDelta("A new segment of the answer.");
    REQUIRE(QMetaObject::invokeMethod(&chat, "flushPending", Qt::DirectConnection));
    QApplication::processEvents();
    CHECK(scroll->value() == 0);
    auto* latest = chat.findChild<QPushButton*>("JumpToLatest");
    REQUIRE(latest);
    CHECK(latest->isVisible());
    chat.refreshTheme();
    CHECK(scroll->value() == 0);
    latest->click();
    QApplication::processEvents();
    CHECK(scroll->value() == scroll->maximum());
    CHECK_FALSE(latest->isVisible());
    chat.appendDelta(" More output follows without losing the bottom position.");
    REQUIRE(QMetaObject::invokeMethod(&chat, "flushPending", Qt::DirectConnection));
    CHECK(scroll->value() == scroll->maximum());
    chat.endAssistantMessage();
}

TEST_CASE("composer retains keyboard send newline and cancellation behavior", "[gui][ui-polish]") {
    PromptComposer composer;
    int sent = 0, stopped = 0;
    QObject::connect(&composer, &PromptComposer::submitted, &composer,
                     [&](const QString&) { ++sent; });
    QObject::connect(&composer, &PromptComposer::stopRequested, &composer, [&] { ++stopped; });
    composer.setPlainText("First line");
    composer.moveCursor(QTextCursor::End);
    QKeyEvent newline(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier, "\r");
    QApplication::sendEvent(&composer, &newline);
    CHECK(composer.toPlainText() == "First line\n");
    CHECK(sent == 0);
    QKeyEvent send(QEvent::KeyPress, Qt::Key_Return, Qt::ControlModifier);
    QApplication::sendEvent(&composer, &send);
    CHECK(sent == 1);
    composer.setBusy(true);
    QApplication::sendEvent(&composer, &send);
    CHECK(sent == 1);
    QKeyEvent stop(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QApplication::sendEvent(&composer, &stop);
    CHECK(stopped == 1);
}

TEST_CASE("empty project sidebar keeps its heading at the top", "[gui][ui-polish]") {
    ProjectExplorer explorer;
    explorer.resize(220, 650);
    explorer.show();
    QApplication::processEvents();
    auto* heading = explorer.findChild<QLabel*>("PanelTitle");
    auto* empty = explorer.findChild<QLabel*>("EmptyProjectState");
    auto* filter = explorer.findChild<QLineEdit*>();
    REQUIRE(heading);
    REQUIRE(empty);
    REQUIRE(filter);
    CHECK(heading->y() == 0);
    CHECK(heading->height() < 80);
    CHECK(empty->isVisible());
    CHECK_FALSE(filter->isVisible());
}

TEST_CASE("UI review renders a representative window in both themes and sizes",
          "[gui][ui-review]") {
    QTemporaryDir storage;
    REQUIRE(storage.isValid());
    const auto original = app_paths().data_dir();

    struct Restore {
        std::filesystem::path path;
        ThemeMode mode;
        QMap<QString, QVariant> settings;

        ~Restore() {
            app_paths().set_data_dir(path);
            QSettings saved;
            saved.clear();
            for (auto it = settings.cbegin(); it != settings.cend(); ++it)
                saved.setValue(it.key(), it.value());
            Theme::instance().setMode(mode);
            Theme::instance().apply();
        }
    } restore{original, Theme::instance().mode(), {}};

    QSettings saved;
    for (const auto& key : saved.allKeys())
        restore.settings.insert(key, saved.value(key));
    saved.clear();
    app_paths().set_data_dir(path_from_utf8(storage.path().toStdString()));
    Application application;
    REQUIRE(application.initialise());
    for (const auto mode : {ThemeMode::Dark, ThemeMode::Light}) {
        Theme::instance().setMode(mode);
        Theme::instance().apply();
        MainWindow window(application);
        auto* chat = window.findChild<ChatView*>();
        auto* composer = window.findChild<PromptComposer*>();
        auto* task = window.findChild<TaskPanel*>();
        REQUIRE(chat);
        REQUIRE(composer);
        REQUIRE(task);
        auto* openProject = window.findChild<QToolButton*>("OpenProjectMenu");
        REQUIRE(openProject);
        REQUIRE(openProject->menu());
        const auto projectActions = openProject->menu()->actions();
        CHECK(std::any_of(projectActions.cbegin(), projectActions.cend(), [](const auto* action) {
            return action->objectName() == "CreateOutputWorkspace";
        }));
        chat->addMessage(ChatRole::User,
                         "Make the output easier to read and keep the existing layout.");
        chat->beginAssistantMessage();
        chat->appendReasoning(
            "Check the current layout, preserve the user's draft, and run the relevant checks.");
        chat->appendDelta(
            "## A focused update\n\nThe layout stays familiar. The answer is easier to scan, with "
            "clearer spacing between messages.\n\n"
            "- Keep the project and chat panels.\n- Make long prompts comfortable to edit.\n- "
            "Preserve your position while reading.\n\n"
            "```cpp\nint add(int a, int b) {\n    return a + b;\n}\n```\n\n"
            "| Check | Result |\n| --- | --- |\n| Readability | Passed |\n| Draft preserved | "
            "Passed |\n\n"
            "The checks passed. You can review the changed file under **Changes**.");
        chat->endAssistantMessage();
        ChatView::TurnStatsView stats;
        stats.usageFromServer = true;
        stats.completionTokens = 186;
        stats.timingsFromServer = true;
        stats.decodeTokensPerSecond = 25.4;
        stats.reasoningCharacters = 95;
        stats.estimatedReasoningTokens = 24;
        chat->setTurnStats(stats);
        task->setTaskBlock(
            "Current task\n\nImprove readability while keeping the familiar layout.");
        task->setChangedFiles({"src/gui/widgets/ChatView.cpp"}, {});
        composer->setPlainText(
            "Keep the orange and grey appearance. Make the text easier to read and long prompts "
            "easier to edit.");
        window.show();
        for (const auto size : {QSize(1280, 800), QSize(900, 620)}) {
            window.resize(size);
            QApplication::processEvents();
            CHECK(window.width() <= size.width());
            CHECK(window.height() <= size.height());
            checkButtons(window);
            capture(window, QString("window-%1-%2x%3")
                                .arg(mode == ThemeMode::Dark ? "dark" : "light")
                                .arg(size.width())
                                .arg(size.height()));
        }

        ModelPanel modelPanel(application);
        modelPanel.resize(280, 850);
        modelPanel.show();
        QApplication::processEvents();
        CHECK(modelPanel.width() <= 280);
        checkButtons(modelPanel);
        capture(modelPanel, mode == ThemeMode::Dark ? "models-dark-narrow" : "models-light-narrow");

        SettingsDialog settings(application);
        settings.resize(640, 560);
        settings.show();
        auto* navigation = settings.findChild<QListWidget*>("SettingsNavigation");
        REQUIRE(navigation);
        for (int page = 0; page < navigation->count(); ++page) {
            navigation->setCurrentRow(page);
            QApplication::processEvents();
            checkButtons(settings);
            CHECK(settings.width() <= 640);
            CHECK(settings.height() <= 560);
            capture(settings, QString("settings-%1-%2")
                                  .arg(mode == ThemeMode::Dark ? "dark" : "light")
                                  .arg(page));
        }
    }
}
