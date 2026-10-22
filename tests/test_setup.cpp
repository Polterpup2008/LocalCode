#include <miniz.h>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTextBrowser>
#include <QTextDocument>
#include <QTimer>
#include <QtEndian>
#include <catch2/catch_test_macros.hpp>
#include <map>
#include <thread>
#include <vector>
#include "dialogs/ResourceSetupDialog.h"
#include "inference/ServerManager.h"
#include "platform/PathEncoding.h"
#include "platform/Paths.h"
#include "plugins/DiscordPlugin.h"
#include "setup/ResourceInstaller.h"
#include "widgets/ModelPanel.h"
#include "widgets/PluginsPanel.h"

using namespace localcode;
using namespace localcode::gui;

namespace {
class SetupHttp final : public HttpClient {
public:
    QByteArray body;
    int status = 200, calls = 0;
    CancellationSource* cancelAfterChunk = nullptr;
    std::string lastUrl;
    std::map<std::string, HttpResponse> replies;
    std::vector<HttpRequest> requests;

    Result<HttpResponse> send(const HttpRequest& request, CancellationToken = {}) override {
        ++calls;
        lastUrl = request.url;
        requests.push_back(request);
        const auto found = replies.find(request.url);
        if (found != replies.end()) return found->second;
        HttpResponse response;
        response.status_code = status;
        response.body = body.toStdString();
        return response;
    }

    Result<HttpResponse> stream(const HttpRequest& request, const HttpStreamCallback& callback,
                                CancellationToken cancel) override {
        ++calls;
        lastUrl = request.url;
        for (qsizetype offset = 0; offset < body.size(); offset += 4096) {
            if (cancel.cancelled()) return make_error(ErrorCode::Cancelled, "fixture cancelled");
            const auto chunk = body.sliced(offset, qMin<qsizetype>(4096, body.size() - offset));
            if (!callback(std::string_view(chunk.constData(), static_cast<size_t>(chunk.size()))))
                break;
            if (cancelAfterChunk) cancelAfterChunk->cancel();
        }
        HttpResponse response;
        response.status_code = status;
        return response;
    }

    Status download(const HttpRequest&, const std::filesystem::path&,
                    const std::function<void(std::uint64_t, std::uint64_t)>&,
                    CancellationToken) override {
        return make_error(ErrorCode::NotSupported, "fixture only streams");
    }

    std::string backend_name() const override { return "setup fixture"; }
};

ResourceAsset assetFor(const QByteArray& bytes, const QString& name = "fixture.gguf") {
    return {
        name,
        "https://huggingface.co/test/repo/resolve/" + QString(40, 'a') + '/' + name,
        QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex()),
        "https://huggingface.co/test/repo",
        QString(40, 'a'),
        "MIT",
        static_cast<quint64>(bytes.size())};
}

QByteArray gguf() {
    auto bytes = QByteArray::fromHex("474755460300000000000000000000000000000000000000");
    return bytes;
}

QByteArray zipEntries(const QList<QPair<QString, QByteArray>>& entries) {
    mz_zip_archive archive{};
    REQUIRE(mz_zip_writer_init_heap(&archive, 0, 0));
    for (const auto& [name, content] : entries) {
        const auto encoded = name.toUtf8();
        REQUIRE(mz_zip_writer_add_mem(&archive, encoded.constData(), content.constData(),
                                     static_cast<size_t>(content.size()), MZ_BEST_SPEED));
    }
    void* bytes = nullptr;
    size_t size = 0;
    REQUIRE(mz_zip_writer_finalize_heap_archive(&archive, &bytes, &size));
    QByteArray result(static_cast<const char*>(bytes), static_cast<qsizetype>(size));
    mz_free(bytes);
    mz_zip_writer_end(&archive);
    return result;
}

QByteArray zip(const QStringList& names) {
    QList<QPair<QString, QByteArray>> entries;
    for (const auto& name : names) entries.emplaceBack(name, QByteArray("test"));
    return zipEntries(entries);
}

QJsonObject remote(const QString& tag, const QString& name) {
    return {{"name", name},
            {"size", 18000},
            {"digest", "sha256:" + QString(64, 'b')},
            {"browser_download_url",
             "https://github.com/ggml-org/llama.cpp/releases/download/" + tag + '/' + name}};
}

void releaseReply(SetupHttp& http, const QString& tag, const QJsonArray& assets) {
    HttpResponse response;
    response.status_code = 200;
    response.body = QJsonDocument(QJsonObject{{"tag_name", tag}, {"assets", assets}})
                        .toJson().toStdString();
    http.replies["https://api.github.com/repos/ggml-org/llama.cpp/releases/tags/" +
                 tag.toStdString()] = response;
}

void buildReferenceReply(SetupHttp& http, const QByteArray& content) {
    const auto tag = QString("v0.6.0");
    auto asset = remote(tag, "nightly-tag.txt");
    asset["size"] = content.size();
    asset["digest"] = "sha256:" + QString::fromLatin1(
        QCryptographicHash::hash(content, QCryptographicHash::Sha256).toHex());
    releaseReply(http, tag, {asset});
    HttpResponse response;
    response.status_code = 200;
    response.body = content.toStdString();
    http.replies[asset["browser_download_url"].toString().toStdString()] = response;
}
}   // namespace

TEST_CASE("setup resolves official version tags to checksum-verified binary builds",
          "[gui][setup]") {
    auto http = std::make_shared<SetupHttp>();
    ResourceInstaller installer(http);
    buildReferenceReply(*http, "b11429\n");
    releaseReply(*http, "b11429", {remote("b11429", "llama-b11429-bin-win-cpu-x64.zip")});
    const auto packages = installer.runtimes("v0.6.0");
    REQUIRE(packages);
    REQUIRE(packages->size() == 1);
    const auto& package = packages->front();
    CHECK(package.tag == "b11429");
    CHECK(package.flavour == "cpu");
    CHECK(package.assets.front().revision == "b11429");
    CHECK(package.assets.front().source ==
          "https://github.com/ggml-org/llama.cpp/releases/tag/b11429");
    REQUIRE(http->requests.size() == 3);
    CHECK(http->requests[1].url ==
          "https://github.com/ggml-org/llama.cpp/releases/download/v0.6.0/nightly-tag.txt");
    CHECK(http->requests[1].max_response_bytes == 128);
    CHECK(http->requests[2].url ==
          "https://api.github.com/repos/ggml-org/llama.cpp/releases/tags/b11429");
}

TEST_CASE("setup rejects invalid version build references without following another target",
          "[gui][setup]") {
    auto http = std::make_shared<SetupHttp>();
    ResourceInstaller installer(http);
    const auto url = std::string(
        "https://github.com/ggml-org/llama.cpp/releases/download/v0.6.0/nightly-tag.txt");
    buildReferenceReply(*http, "b11429\n");
    SECTION("checksum mismatch") { http->replies[url].body = "b11428\n"; }
    SECTION("truncated response") { http->replies[url].truncated = true; }
    SECTION("incomplete response") { http->replies[url].body = "b11429"; }
    SECTION("unavailable response") { http->replies[url].status_code = 403; }
    SECTION("invalid target") { buildReferenceReply(*http, "../escape\n"); }
    SECTION("extra content") { buildReferenceReply(*http, "b11429\ninvalid\n"); }
    SECTION("another version alias") { buildReferenceReply(*http, "v0.6.0\n"); }
    CHECK_FALSE(installer.runtimes("v0.6.0"));
    CHECK(http->requests.size() == 2);
}

TEST_CASE("setup limits release references and preserves direct binary discovery",
          "[gui][setup]") {
    auto http = std::make_shared<SetupHttp>();
    ResourceInstaller installer(http);
    SECTION("oversized reference is not downloaded") {
        buildReferenceReply(*http, QByteArray(129, 'b'));
        CHECK_FALSE(installer.runtimes("v0.6.0"));
        CHECK(http->calls == 1);
    }
    SECTION("reference outside official release is not downloaded") {
        auto asset = remote("v0.6.0", "nightly-tag.txt");
        asset["size"] = 7;
        asset["browser_download_url"] = "https://example.org/nightly-tag.txt";
        releaseReply(*http, "v0.6.0", {asset});
        CHECK_FALSE(installer.runtimes("v0.6.0"));
        CHECK(http->calls == 1);
    }
    SECTION("direct archives take precedence over a reference") {
        releaseReply(*http, "v0.6.0",
                     {remote("v0.6.0", "nightly-tag.txt"),
                      remote("v0.6.0", "llama-v0.6.0-bin-win-cpu-x64.zip")});
        const auto packages = installer.runtimes("v0.6.0");
        REQUIRE(packages);
        CHECK(packages->front().tag == "v0.6.0");
        CHECK(http->calls == 1);
    }
    SECTION("a second reference is not followed") {
        buildReferenceReply(*http, "b11429\n");
        auto nested = remote("b11429", "nightly-tag.txt");
        nested["size"] = 7;
        releaseReply(*http, "b11429", {nested});
        CHECK_FALSE(installer.runtimes("v0.6.0"));
        CHECK(http->calls == 3);
    }
    SECTION("cancelled discovery sends no request") {
        CancellationSource cancellation;
        cancellation.cancel();
        const auto result = installer.runtimes("v0.6.0", cancellation.token());
        REQUIRE_FALSE(result);
        CHECK(result.error().code == ErrorCode::Cancelled);
        CHECK(http->calls == 0);
    }
}

TEST_CASE("setup discovers immutable public metadata and complete CUDA dependencies",
          "[gui][setup]") {
    auto http = std::make_shared<SetupHttp>();
    ResourceInstaller installer(http);
    const auto tag = QString("b10655");
    http->body =
        QJsonDocument(
            QJsonObject{
                {"tag_name", tag},
                {"assets", QJsonArray{remote(tag, "llama-b10655-bin-win-cuda-13.3-x64.zip"),
                                      remote(tag, "llama-b10655-bin-win-cpu-x64.zip"),
                                      remote(tag, "cudart-llama-bin-win-cuda-13.3-x64.zip")}}})
            .toJson();
    const auto packages = installer.runtimes(tag);
    REQUIRE(packages);
    REQUIRE(packages->size() == 2);
    CHECK(packages->front().flavour == "cpu");
    CHECK(packages->back().assets.size() == 2);
    CHECK(packages->back().bytes() == 36000);
    const auto calls = http->calls;
    CHECK_FALSE(installer.runtimes("../escape"));
    CHECK(http->calls == calls);
    http->body =
        QJsonDocument(
            QJsonObject{
                {"id", "test/repo"},
                {"sha", QString(40, 'a')},
                {"cardData", QJsonObject{{"license", "apache-2.0"}}},
                {"siblings",
                 QJsonArray{
                     QJsonObject{{"rfilename", "model.gguf"},
                                 {"lfs", QJsonObject{{"sha256", QString(64, 'c')},
                                                     {"size", 2707514688ll}}}},
                     QJsonObject{{"rfilename", "partial-00001-of-00002.gguf"},
                                 {"lfs", QJsonObject{{"sha256", QString(64, 'c')}, {"size", 123}}}},
                     QJsonObject{
                         {"rfilename", "../escape.gguf"},
                         {"lfs", QJsonObject{{"sha256", QString(64, 'c')}, {"size", 123}}}}}}})
            .toJson();
    const auto models = installer.models("test/repo");
    REQUIRE(models);
    REQUIRE(models->files.size() == 1);
    CHECK(models->license == "apache-2.0");
    CHECK(models->files.front().bytes == 2707514688ull);
    CHECK(models->files.front().url.contains("/resolve/" + QString(40, 'a') + "/model.gguf"));
    CHECK_FALSE(installer.models("https://elsewhere/repo"));
}

TEST_CASE("verified GGUF publication supports Unicode and preserves provenance", "[gui][setup]") {
    QTemporaryDir storage;
    REQUIRE(storage.isValid());
    auto http = std::make_shared<SetupHttp>();
    http->body = gguf();
    ResourceInstaller installer(http);
    const auto root = storage.filePath("modèles é");
    quint64 progress = 0;
    const auto installed =
        installer.installModel(assetFor(http->body), root, [&](quint64 now, quint64 total) {
            CHECK(now <= total);
            progress = now;
        });
    REQUIRE(installed);
    CHECK(QFile::exists(*installed));
    CHECK(progress == static_cast<quint64>(http->body.size()));
    QFile file(*installed);
    REQUIRE(file.open(QIODevice::ReadOnly));
    CHECK(file.readAll() == http->body);
    QFile receipt(QFileInfo(*installed).dir().filePath("download.json"));
    REQUIRE(receipt.open(QIODevice::ReadOnly));
    const auto rows = QJsonDocument::fromJson(receipt.readAll()).array();
    REQUIRE(rows.size() == 1);
    CHECK(rows[0].toObject().value("sha256").toString() == assetFor(http->body).sha256);
}

TEST_CASE("failed and cancelled downloads publish nothing and preserve existing files",
          "[gui][setup]") {
    for (int failure = 0; failure < 5; ++failure) {
        QTemporaryDir storage;
        REQUIRE(storage.isValid());
        const auto root = storage.filePath("resources");
        QDir().mkpath(root);
        QFile sentinel(QDir(root).filePath("keep.txt"));
        REQUIRE(sentinel.open(QIODevice::WriteOnly));
        sentinel.write("private");
        sentinel.close();
        auto http = std::make_shared<SetupHttp>();
        http->body = gguf();
        auto asset = assetFor(http->body);
        CancellationSource cancel;
        if (failure == 0) asset.sha256 = QString(64, '0');
        if (failure == 1) asset.bytes -= 1;
        if (failure == 2) http->body.chop(1);
        if (failure == 3) {
            http->body.fill('x', 7000);
            asset = assetFor(http->body);
            http->cancelAfterChunk = &cancel;
        }
        if (failure == 4) {
            http->body = "not gguf";
            asset = assetFor(http->body);
        }
        ResourceInstaller installer(http);
        CHECK_FALSE(installer.installModel(asset, root, {}, cancel.token()));
        CHECK(QDir(root).entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot) ==
              QStringList{"keep.txt"});
        REQUIRE(sentinel.open(QIODevice::ReadOnly));
        CHECK(sentinel.readAll() == "private");
    }
}

TEST_CASE("runtime extraction rejects escaping and colliding archive paths before writing",
          "[gui][setup]") {
    for (const auto& names : QList<QStringList>{{"../outside.txt"},
                                                {"C:/outside.txt"},
                                                {"a\\outside.txt"},
                                                {"NUL.txt"},
                                                {"folder./file"},
                                                {"one.exe", "ONE.exe"}}) {
        QTemporaryDir storage;
        REQUIRE(storage.isValid());
        const auto directory = storage.filePath("extract");
        QDir().mkpath(directory);
        QFile archive(storage.filePath("runtime.zip"));
        REQUIRE(archive.open(QIODevice::WriteOnly));
        archive.write(zip(names));
        archive.close();
        CHECK_FALSE(ResourceInstaller::extractRuntimeZip(archive.fileName(), directory));
        CHECK(QDir(directory)
                  .entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot)
                  .isEmpty());
        CHECK_FALSE(QFile::exists(storage.filePath("outside.txt")));
    }
}

TEST_CASE("runtime archive publication installs dependencies beside the server", "[gui][setup]") {
    QTemporaryDir storage;
    REQUIRE(storage.isValid());
    auto http = std::make_shared<SetupHttp>();
    http->body = zip({"llama-server.exe", "ggml.dll", "LICENSE.txt"});
    ResourceInstaller installer(http);
    RuntimePackage package{"fixture", "cpu", {assetFor(http->body, "runtime.zip")}};
    const auto installed = installer.installRuntime(package, storage.filePath("moteur é"));
    REQUIRE(installed);
    CHECK(QFileInfo(*installed).fileName() == "llama-server.exe");
    CHECK(QFile::exists(QFileInfo(*installed).dir().filePath("ggml.dll")));
}

#ifdef Q_OS_WIN
TEST_CASE("managed runtime stages matching compiler DLLs without replacing provider files",
          "[gui][setup][compiler-runtime]") {
    QTemporaryDir storage;
    REQUIRE(storage.isValid());
    QFile executable(QCoreApplication::applicationFilePath());
    REQUIRE(executable.open(QIODevice::ReadOnly));
    const auto header = executable.read(4096);
    REQUIRE(header.size() == 4096);
    bool matching = true, provider = false, blocked = false, noSupport = false;
    SECTION("matching PE") {}
    SECTION("provider wins") { provider = true; }
    SECTION("other architecture") { matching = false; }
    SECTION("missing support files") { noSupport = true; }
    SECTION("copy failure publishes nothing") { blocked = true; }
    auto http = std::make_shared<SetupHttp>();
    auto server = header;
    if (!matching) {
        const auto offset = qFromLittleEndian<quint32>(server.constData() + 60);
        REQUIRE(offset + 6 < static_cast<quint32>(server.size()));
        server[static_cast<int>(offset) + 4] ^= 0x01;
    }
    QList<QPair<QString, QByteArray>> entries{{"llama-server.exe", server}};
    if (provider) entries.emplaceBack("msvcp140.dll", QByteArray("provider DLL"));
    if (blocked) entries.emplaceBack("msvcp140.dll/keep.txt", QByteArray("directory blocks DLL copy"));
    http->body = zipEntries(entries);
    const auto support = storage.filePath("support é");
    REQUIRE(QDir().mkpath(support));
    if (!noSupport) {
        QFile dll(QDir(support).filePath("msvcp140.dll"));
        REQUIRE(dll.open(QIODevice::WriteOnly));
        REQUIRE(dll.write(header) == header.size());
    }
    ResourceInstaller installer(http, support);
    const auto root = storage.filePath("managed é");
    const auto installed = installer.installRuntime(
        {"fixture", "cpu", {assetFor(http->body, "runtime.zip")}}, root);
    if (blocked) {
        REQUIRE_FALSE(installed);
        CHECK(QDir(root).entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot).isEmpty());
        return;
    }
    REQUIRE(installed);
    const QDir directory = QFileInfo(*installed).dir();
    QFile dll(directory.filePath("msvcp140.dll"));
    if (!matching || noSupport) {
        CHECK_FALSE(dll.exists());
    } else {
        REQUIRE(dll.open(QIODevice::ReadOnly));
        CHECK(dll.readAll() == (provider ? QByteArray("provider DLL") : header));
    }
    QFile provenance(directory.filePath("localcode-compiler-runtime.json"));
    CHECK(provenance.exists() == (matching && !provider && !noSupport));
    if (provenance.exists()) {
        REQUIRE(provenance.open(QIODevice::ReadOnly));
        const auto rows = QJsonDocument::fromJson(provenance.readAll()).array();
        REQUIRE(rows.size() == 1);
        CHECK(rows[0].toObject().value("sha256").toString() == QString::fromLatin1(
            QCryptographicHash::hash(header, QCryptographicHash::Sha256).toHex()));
    }
}
#endif

TEST_CASE("corrupt, linked and oversized archives cannot publish a runtime", "[gui][setup]") {
    for (int failure = 0; failure < 4; ++failure) {
        auto bytes = zip({"llama-server.exe"});
        const auto central = bytes.indexOf(QByteArray::fromHex("504b0102"));
        REQUIRE(central >= 0);
        auto put32 = [&](int offset, quint32 value) {
            for (int i = 0; i < 4; ++i)
                bytes[central + offset + i] = static_cast<char>(value >> (8 * i));
        };
        if (failure == 0) bytes[central + 16] = static_cast<char>(bytes[central + 16] ^ 0x42);
        if (failure == 1) put32(38, 0120777u << 16);   // Unix symbolic link.
        if (failure == 2) put32(24, 0x80000001u);      // Expanded entry exceeds 2 GiB.
        if (failure == 3) {
            const auto footer = bytes.lastIndexOf(QByteArray::fromHex("504b0506"));
            REQUIRE(footer >= 0);
            const quint32 oversizedDirectory = 4 * 1024 * 1024 + 1;
            for (int i = 0; i < 4; ++i)
                bytes[footer + 12 + i] = static_cast<char>(oversizedDirectory >> (8 * i));
        }
        auto http = std::make_shared<SetupHttp>();
        http->body = bytes;
        ResourceInstaller installer(http);
        QTemporaryDir storage;
        REQUIRE(storage.isValid());
        const auto root = storage.filePath("resources");
        REQUIRE(QDir().mkpath(root));
        const auto installed =
            installer.installRuntime({"fixture", "cpu", {assetFor(bytes, "runtime.zip")}}, root);
        CHECK_FALSE(installed);
        CHECK(
            QDir(root).entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot).isEmpty());
    }
}

TEST_CASE("download metadata remains readable in a small setup window", "[gui][setup][visual]") {
    auto http = std::make_shared<SetupHttp>();
    buildReferenceReply(*http, "b11429\n");
    auto archive = remote("b11429", "llama-b11429-bin-win-cpu-x64.zip");
    archive["size"] = 19398918;
    releaseReply(*http, "b11429", {archive});
    ResourceSetupDialog dialog({}, "unused-runtime-root", "unused-model-root", nullptr,
                               std::make_shared<ResourceInstaller>(http));
    dialog.resize(500, 540);
    dialog.show();
    auto ready = [](QPushButton* button) {
        QElapsedTimer wait;
        wait.start();
        while (!button->isEnabled() && wait.elapsed() < 5000) {
            QApplication::processEvents();
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        REQUIRE(button->isEnabled());
        QApplication::processEvents();
    };
    auto* tag = dialog.findChild<QLineEdit*>("SetupReleaseTag");
    REQUIRE(tag);
    CHECK(tag->text() == "b10655");
    tag->setText("v0.6.0");
    dialog.findChild<QPushButton*>("SetupFindRuntimes")->click();
    ready(dialog.findChild<QPushButton*>("SetupDownloadRuntime"));
    auto* build = dialog.findChild<QComboBox*>("SetupRuntimeBuild");
    REQUIRE(build);
    CHECK(build->currentText().contains("b11429"));
    CHECK(dialog.findChild<QLineEdit*>("SetupRuntime")->text().isEmpty());
    CHECK(http->calls == 3);
    auto* pages = dialog.findChild<QTabWidget*>();
    auto* runtimePage = qobject_cast<QScrollArea*>(pages->widget(0));
    REQUIRE(runtimePage);
    CHECK(runtimePage->horizontalScrollBar()->maximum() == 0);
    auto* details = dialog.findChild<QTextBrowser*>("SetupRuntimeDetails");
    REQUIRE(details);
    CHECK(details->toPlainText().contains(QString(64, 'b')));
    CHECK(details->document()->size().width() <= details->viewport()->width());
    CHECK(details->document()->size().height() <= details->viewport()->height());
    const auto image = qEnvironmentVariable("LOCALCODE_SETUP_METADATA_SCREENSHOT");
    if (!image.isEmpty()) CHECK(dialog.grab().save(image));
    dialog.resize(740, 760);
    QApplication::processEvents();
    CHECK(runtimePage->horizontalScrollBar()->maximum() == 0);
    if (!image.isEmpty()) CHECK(dialog.grab().save(image + ".normal.png"));
    dialog.resize(500, 540);
    http->body =
        QJsonDocument(QJsonObject{{"id", "test/repo"},
                                  {"sha", QString(40, 'b')},
                                  {"cardData", QJsonObject{{"license", "apache-2.0"}}},
                                  {"siblings", QJsonArray{QJsonObject{
                                                   {"rfilename", "model.gguf"},
                                                   {"lfs", QJsonObject{{"sha256", QString(64, 'c')},
                                                                       {"size", 2707514688ll}}}}}}})
            .toJson();
    pages->setCurrentIndex(1);
    dialog.findChild<QLineEdit*>("SetupRepository")->setText("test/repo");
    dialog.findChild<QPushButton*>("SetupFindModels")->click();
    ready(dialog.findChild<QPushButton*>("SetupDownloadModel"));
    auto* modelPage = qobject_cast<QScrollArea*>(pages->widget(1));
    REQUIRE(modelPage);
    CHECK(modelPage->horizontalScrollBar()->maximum() == 0);
    if (!image.isEmpty()) CHECK(dialog.grab().save(image + ".model.png"));
}

TEST_CASE("official CPU archive can be installed and queried without a model",
          "[gui][setup][integration]") {
    const auto archive = qEnvironmentVariable("LOCALCODE_TEST_RUNTIME_ARCHIVE");
    if (archive.isEmpty())
        SKIP("Set LOCALCODE_TEST_RUNTIME_ARCHIVE to the recorded official b10655 CPU archive.");
    QFile file(archive);
    REQUIRE(file.open(QIODevice::ReadOnly));
    auto http = std::make_shared<SetupHttp>();
    http->body = file.readAll();
    auto asset = assetFor(http->body, "llama-b10655-bin-win-cpu-x64.zip");
    REQUIRE(asset.sha256 == "1a93e3e6370f2106ba4a2d21da18141d2c466435071769916084e74d6831a387");
    QTemporaryDir storage;
    REQUIRE(storage.isValid());
    ResourceInstaller installer(http);
    const auto installed = installer.installRuntime({"b10655", "cpu", {asset}},
                                                    storage.filePath("Official runtime é"));
    REQUIRE(installed);
#ifdef Q_OS_WIN
    const auto features =
        ModelServer::probe_binary(std::filesystem::path(installed->toStdWString()));
#else
    const auto features =
        ModelServer::probe_binary(std::filesystem::path(installed->toStdString()));
#endif
    REQUIRE(features);
    CHECK(features->jinja);
    CHECK(features->threads);
    CHECK(features->cache_type_k);
}

TEST_CASE("coding setup imports chosen files without loading a model",
          "[gui][setup][integration]") {
#ifdef Q_OS_WIN
    const auto fixture =
        QDir(QCoreApplication::applicationDirPath()).filePath("localcode_server_fixture.exe");
#else
    const auto fixture =
        QDir(QCoreApplication::applicationDirPath()).filePath("localcode_server_fixture");
#endif
    REQUIRE(QFile::exists(fixture));
    QTemporaryDir storage;
    REQUIRE(storage.isValid());
    const auto previous = app_paths().data_dir().parent_path();

    struct Restore {
        std::filesystem::path root;

        ~Restore() { app_paths().set_root(root); }
    } restore{previous};

    app_paths().set_root(storage.path().toStdString());
    Application application;
    REQUIRE(application.initialise());
    const auto files = storage.filePath("files é");
    QDir().mkpath(files);
    const auto runtime = QDir(files).filePath("llama-server.exe");
    REQUIRE(QFile::copy(fixture, runtime));
    const auto model = QDir(files).filePath("fixture.gguf");
    QFile file(model);
    REQUIRE(file.open(QIODevice::WriteOnly));
    file.write(gguf());
    file.close();
    ModelPanel panel(application);
    panel.resize(780, 740);
    panel.show();
    auto* setup = panel.findChild<QPushButton*>("SetupCodingModel");
    REQUIRE(setup);
    bool selected = false;
    QTimer::singleShot(0, &panel, [&] {
        auto* dialog = qobject_cast<ResourceSetupDialog*>(QApplication::activeModalWidget());
        if (!dialog) return;
        dialog->findChild<QLineEdit*>("SetupRuntime")->setText(runtime);
        dialog->findChild<QLineEdit*>("SetupModel")->setText(model);
        auto* cpu = dialog->findChild<QCheckBox*>("SetupCpuProfile");
        cpu->setChecked(true);
        QTimer::singleShot(15000, dialog, &QDialog::reject);
        selected = true;
        dialog->findChild<QPushButton*>("SetupUseFiles")->click();
    });
    setup->click();
    REQUIRE(selected);
    const auto profiles = application.model_profiles();
    REQUIRE(profiles);
    REQUIRE(profiles->size() == 1);
    CHECK(profiles->front().context_size == 4096);
    CHECK(profiles->front().gpu_layers == 0);
    CHECK(profiles->front().ubatch_size == 128);
    CHECK(profiles->front().backend == BackendPreference::LlamaCpp);
    CHECK(application.settings().llama_server_path == runtime.toStdString());
    CHECK(path_to_utf8(application.backends().configured_binary(BackendPreference::LlamaCpp)) ==
          runtime.toStdString());
    CHECK(path_to_utf8(profiles->front().model_path) == model.toStdString());
    auto badSettings = application.settings();
    badSettings.llama_server_path = "bad-\xE9";
    CHECK_FALSE(application.apply_settings(badSettings));
    CHECK(application.settings().llama_server_path == runtime.toStdString());
    CHECK(application.active_model_id().empty());
    const auto screenshot = qEnvironmentVariable("LOCALCODE_SETUP_SCREENSHOT");
    if (!screenshot.isEmpty()) CHECK(panel.grab().save(screenshot));
}

TEST_CASE("public setup metadata and streamed CPU download use the real transport",
          "[gui][setup][network]") {
    if (qEnvironmentVariable("LOCALCODE_TEST_SETUP_NETWORK") != "1")
        SKIP("Set LOCALCODE_TEST_SETUP_NETWORK=1 for explicit public download acceptance.");
    ResourceInstaller installer;
    const auto packages = installer.runtimes("b10655");
    INFO((packages ? "" : packages.error().to_string()));
    REQUIRE(packages);
    REQUIRE_FALSE(packages->isEmpty());
    const auto& cpu = packages->front();
    REQUIRE(cpu.flavour == "cpu");
    REQUIRE(cpu.assets.size() == 1);
    CHECK(cpu.assets.front().sha256 ==
          "1a93e3e6370f2106ba4a2d21da18141d2c466435071769916084e74d6831a387");
    const auto listing = installer.models("mradermacher/Huihui-Qwen3.5-4B-abliterated-GGUF");
    INFO((listing ? "" : listing.error().to_string()));
    REQUIRE(listing);
    CHECK_FALSE(listing->files.isEmpty());
    CHECK(listing->revision.size() == 40);
    QTemporaryDir storage;
    REQUIRE(storage.isValid());
    quint64 received = 0;
    const auto installed = installer.installRuntime(cpu, storage.filePath("Downloaded runtime é"),
                                                    [&](quint64 now, quint64) { received = now; });
    INFO((installed ? "" : installed.error().to_string()));
    REQUIRE(installed);
    CHECK(received == cpu.bytes());
    CHECK(ModelServer::probe_binary(path_from_utf8(installed->toStdString())));
}

TEST_CASE("official version resolves and installs its binary build without a model",
          "[.][gui][setup][version][network]") {
    ResourceInstaller installer;
    const auto packages = installer.runtimes("v0.6.0");
    INFO((packages ? "" : packages.error().to_string()));
    REQUIRE(packages);
    REQUIRE_FALSE(packages->isEmpty());
    const auto& cpu = packages->front();
    REQUIRE(cpu.tag == "b11429");
    REQUIRE(cpu.flavour == "cpu");
    REQUIRE(cpu.assets.size() == 1);
    CHECK(cpu.assets.front().revision == "b11429");
    CHECK(cpu.assets.front().sha256 ==
          "1283323272b04cd07905816a597a0da810918102de958f4ff6f7bbaa70ed2efe");
    QTemporaryDir storage;
    REQUIRE(storage.isValid());
    quint64 received = 0;
    const auto installed = installer.installRuntime(
        cpu, storage.filePath("Version stable é"),
        [&](quint64 now, quint64) { received = now; });
    INFO((installed ? "" : installed.error().to_string()));
    REQUIRE(installed);
    CHECK(received == cpu.bytes());
    CHECK(ModelServer::probe_binary(path_from_utf8(installed->toStdString())));
    const auto directory = QFileInfo(*installed).dir();
    for (const auto* name : {"msvcp140.dll", "vcruntime140.dll", "vcruntime140_1.dll"})
        CHECK(QFileInfo(directory.filePath(name)).isFile());
    QFile receipt(directory.filePath("localcode-compiler-runtime.json"));
    REQUIRE(receipt.open(QIODevice::ReadOnly));
    const auto libraries = QJsonDocument::fromJson(receipt.readAll()).array();
    CHECK_FALSE(libraries.isEmpty());
    const auto report = qEnvironmentVariable("LOCALCODE_SETUP_VERSION_REPORT");
    if (!report.isEmpty()) {
        QFile file(report);
        REQUIRE(file.open(QIODevice::WriteOnly));
        REQUIRE(file.write(QJsonDocument(QJsonObject{{"requestedTag", "v0.6.0"},
                                                    {"resolvedTag", cpu.tag},
                                                    {"server", *installed},
                                                    {"archiveSha256", cpu.assets.front().sha256},
                                                    {"archiveBytes", static_cast<qint64>(received)},
                                                    {"compilerLibraries", libraries}}).toJson()) > 0);
        storage.setAutoRemove(false);
    }
}

TEST_CASE("local CPU archive installs with app-local compiler dependencies",
          "[.][gui][setup][compiler-runtime][native]") {
    const auto archive = qEnvironmentVariable("LOCALCODE_TEST_SETUP_ARCHIVE");
    if (archive.isEmpty()) SKIP("Set the explicit public CPU archive path.");
    QFile input(archive);
    REQUIRE(input.open(QIODevice::ReadOnly));
    REQUIRE(input.size() < 256ll * 1024 * 1024);
    auto http = std::make_shared<SetupHttp>();
    http->body = input.readAll();
    ResourceInstaller installer(http);
    QTemporaryDir storage;
    REQUIRE(storage.isValid());
    const auto installed = installer.installRuntime(
        {"local", "cpu", {assetFor(http->body, "runtime.zip")}}, storage.filePath("moteur é"));
    INFO((installed ? "" : installed.error().to_string()));
    REQUIRE(installed);
    QFile provenance(QFileInfo(*installed).dir().filePath("localcode-compiler-runtime.json"));
    REQUIRE(provenance.open(QIODevice::ReadOnly));
    const auto rows = QJsonDocument::fromJson(provenance.readAll()).array();
    CHECK(rows.size() == 8);
    CHECK(ModelServer::probe_binary(path_from_utf8(installed->toStdString())));
    const auto report = qEnvironmentVariable("LOCALCODE_SETUP_NATIVE_REPORT");
    if (!report.isEmpty()) {
        QFile file(report);
        REQUIRE(file.open(QIODevice::WriteOnly));
        REQUIRE(file.write(QJsonDocument(QJsonObject{{"server", *installed},
                                                    {"compilerLibraries", rows}}).toJson()) > 0);
        storage.setAutoRemove(false); // Preserve only this explicitly requested diagnostic.
    }
}

TEST_CASE("Discord setup stages and saves selected Unicode files through the real worker",
          "[gui][setup][plugin][integration]") {
    const auto manifest = qEnvironmentVariable("LOCALCODE_TEST_DISCORD_PACKAGE");
    if (manifest.isEmpty()) SKIP("Set LOCALCODE_TEST_DISCORD_PACKAGE for actual worker IPC.");
    QTemporaryDir storage;
    REQUIRE(storage.isValid());
    const auto previous = app_paths().data_dir().parent_path();

    struct RestorePaths {
        std::filesystem::path root;

        ~RestorePaths() { app_paths().set_root(root); }
    } restore{previous};

    app_paths().set_root(path_from_utf8(storage.path().toStdString()));
    DiscordPlugin plugin;
    REQUIRE(plugin.installPackage(manifest).isEmpty());
    const auto directory = storage.filePath("fichiers é");
    REQUIRE(QDir().mkpath(directory));
    const auto runtime = QDir(directory).filePath("llama-server.exe");
#ifdef Q_OS_WIN
    const auto fixtureName = "localcode_server_fixture.exe";
#else
    const auto fixtureName = "localcode_server_fixture";
#endif
    REQUIRE(
        QFile::copy(QDir(QCoreApplication::applicationDirPath()).filePath(fixtureName), runtime));
    const auto model = QDir(directory).filePath("model.gguf");
    QFile file(model);
    REQUIRE(file.open(QIODevice::WriteOnly));
    file.write(gguf());
    file.close();
    PluginsPanel panel;
    panel.resize(900, 720);
    panel.show();
    auto* save = panel.findChild<QPushButton*>("SaveDiscordSettings");
    REQUIRE(save);
    QElapsedTimer wait;
    wait.start();
    while (!save->isEnabled() && wait.elapsed() < 15000) {
        QApplication::processEvents();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    REQUIRE(save->isEnabled());
    auto* setup = panel.findChild<QPushButton*>("SetupDiscordModel");
    REQUIRE(setup);
    REQUIRE(setup->isEnabled());
    bool selected = false;
    QTimer::singleShot(0, &panel, [&] {
        auto* dialog = qobject_cast<ResourceSetupDialog*>(QApplication::activeModalWidget());
        if (!dialog) return;
        dialog->findChild<QLineEdit*>("SetupRuntime")->setText(runtime);
        dialog->findChild<QLineEdit*>("SetupModel")->setText(model);
        dialog->findChild<QCheckBox*>("SetupCpuProfile")->setChecked(true);
        const auto image = qEnvironmentVariable("LOCALCODE_SETUP_DIALOG_SCREENSHOT");
        if (!image.isEmpty()) CHECK(dialog->grab().save(image));
        QTimer::singleShot(15000, dialog, &QDialog::reject);
        selected = true;
        dialog->findChild<QPushButton*>("SetupUseFiles")->click();
    });
    setup->click();
    REQUIRE(selected);
    save->click();
    wait.restart();
    while (!save->isEnabled() && wait.elapsed() < 15000) {
        QApplication::processEvents();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    REQUIRE(save->isEnabled());
    QEventLoop loop;
    QJsonObject settings;
    QString error;
    QObject::connect(&plugin, &DiscordPlugin::completed, &loop,
                     [&](const QString&, const QJsonObject& value) {
                         settings = value;
                         loop.quit();
                     });
    QObject::connect(&plugin, &DiscordPlugin::failed, &loop, [&](const QString& value) {
        error = value;
        loop.quit();
    });
    QTimer::singleShot(15000, &loop, [&] {
        error = "timeout";
        loop.quit();
    });
    plugin.request("settings");
    if (plugin.busy()) loop.exec();
    INFO(error.toStdString());
    REQUIRE(error.isEmpty());
    CHECK(settings.value("serverBinary").toString() == runtime);
    CHECK(settings.value("modelPath").toString() == model);
    CHECK(settings.value("contextSize").toInt() == 4096);
    CHECK(settings.value("gpuLayers").toInt() == 0);
    CHECK(settings.value("tuning").toObject().value("prompt_cache_mib").toInt() == 128);
    CHECK_FALSE(settings.value("tokenConfigured").toBool());
}
