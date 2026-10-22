// Observe the production editor's periodic refresh using synthetic files only.
// Timing is evidence, not a machine-dependent correctness gate.
#include <QApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTextCursor>
#include <QTextDocument>
#include <algorithm>
#include <iostream>
#include <stdexcept>
#include "Theme.h"
#include "widgets/SourceEditor.h"

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    localcode::gui::Theme::instance().apply();
    try {
        const auto args = app.arguments();
        if (args.size() < 2) throw std::runtime_error("supply a JSON output path");
        const int repeats = std::clamp(args.value(2, "4").toInt(), 1, 20);
        QTemporaryDir directory;
        if (!directory.isValid()) throw std::runtime_error("temporary fixture unavailable");
        QJsonArray cases;
        for (const int lines : {64, 1024}) {
            localcode::gui::SourceEditor editor;
            editor.setAttribute(Qt::WA_DontShowOnScreen);
            editor.resize(1000, 700);
            editor.show();
            QByteArray body;
            for (int line = 0; line < lines; ++line)
                body += "int fixture_" + QByteArray::number(line) +
                        "(int input) { return input + 42; } // Synthetic editor refresh fixture.\n";
            for (int file = 0; file < 6; ++file) {
                const auto path = directory.filePath(QString("source-%1-%2.cpp").arg(lines).arg(file));
                QFile stream(path);
                if (!stream.open(QIODevice::WriteOnly) || stream.write(body) != body.size())
                    throw std::runtime_error("cannot write fixture");
                stream.close();
                editor.openFile(path);
            }
            app.processEvents();
            const auto views = editor.findChildren<localcode::gui::SourceView*>();
            if (views.size() != 6) throw std::runtime_error("fixture tabs missing");
            for (auto* view : views) {
                auto cursor = view->textCursor();
                cursor.setPosition(10);
                cursor.setPosition(25, QTextCursor::KeepAnchor);
                view->setTextCursor(cursor);
            }
            int documentChanges = 0;
            for (auto* view : views)
                QObject::connect(view->document(), &QTextDocument::contentsChanged, &editor,
                                 [&documentChanges] { ++documentChanges; });
            QJsonArray samples;
            for (int repeat = 0; repeat < repeats; ++repeat) {
                QElapsedTimer timer;
                timer.start();
                editor.reloadUnmodified();
                app.processEvents();
                samples.append(static_cast<double>(timer.nsecsElapsed()) / 1e6);
            }
            bool contentExact = true, selectionExact = true;
            for (const auto* view : views) {
                contentExact = contentExact && view->toPlainText().toUtf8() == body;
                selectionExact = selectionExact && view->textCursor().anchor() == 10 &&
                                 view->textCursor().position() == 25;
            }
            if (!contentExact) throw std::runtime_error("fixture text changed");
            cases.append(QJsonObject{{"files", 6}, {"lines_per_file", lines},
                {"bytes_per_file", static_cast<double>(body.size())},
                {"body_sha256", QString::fromLatin1(QCryptographicHash::hash(body, QCryptographicHash::Sha256).toHex())},
                {"refresh_ms", samples}, {"document_changes", documentChanges},
                {"content_exact", contentExact}, {"selection_preserved", selectionExact}});
        }
        const QJsonObject report{{"platform", QGuiApplication::platformName()},
            {"qt", QT_VERSION_STR}, {"cases", cases},
            {"scope", "Production SourceEditor, hidden Qt window, synthetic files, warm file cache; no model or OS input."}};
        QFile output(args[1]);
        const auto bytes = QJsonDocument(report).toJson(QJsonDocument::Indented);
        if (!output.open(QIODevice::WriteOnly) || output.write(bytes) != bytes.size())
            throw std::runtime_error("cannot write report");
        std::cout << bytes.toStdString();
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
