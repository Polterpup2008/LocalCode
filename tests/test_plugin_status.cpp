#include <catch2/catch_test_macros.hpp>
#include "plugins/DiscordPlugin.h"
#include "widgets/PluginsPanel.h"
#include "platform/Paths.h"
#include "platform/PathEncoding.h"
#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QEvent>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QProcess>
#include <QPushButton>
#include <QTemporaryDir>
#include <QTimer>
#include <chrono>
#include <cstdlib>
#include <memory>
#include <thread>
using namespace localcode;
using namespace localcode::gui;
namespace {
template<class Predicate> bool waitFor(Predicate ready,int milliseconds=5000) {
    QElapsedTimer time;time.start();
    while(!ready() && time.elapsed()<milliseconds) {
        QApplication::processEvents();QCoreApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    QApplication::processEvents();QCoreApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete);return ready();
}
struct StatusFixture {
    QTemporaryDir package,storage;
    std::filesystem::path previous=app_paths().data_dir().parent_path();
    std::unique_ptr<DiscordPlugin> plugin;
    QStringList failures;
    int replies=0;
    StatusFixture() {
        REQUIRE(package.isValid());REQUIRE(storage.isValid());
        app_paths().set_root(path_from_utf8(storage.path().toStdString()));
#ifdef Q_OS_WIN
        const QString executable="lcd.exe",fixture="localcode_plugin_status_fixture.exe";
#else
        const QString executable="lcd",fixture="localcode_plugin_status_fixture";
#endif
        REQUIRE(QFile::copy(QDir(QCoreApplication::applicationDirPath()).filePath(fixture),package.filePath(executable)));
        QFile manifest(package.filePath("manifest.json"));REQUIRE(manifest.open(QIODevice::WriteOnly));
        manifest.write(QJsonDocument(QJsonObject{{"id","discord"},{"protocol",1},{"executable",executable}}).toJson());manifest.close();
        plugin=std::make_unique<DiscordPlugin>();REQUIRE(plugin->installPackage(package.filePath("manifest.json")).isEmpty());
        QObject::connect(plugin.get(),&DiscordPlugin::failed,plugin.get(),[this](const QString& error){failures.append(error);});
        QObject::connect(plugin.get(),&DiscordPlugin::completed,plugin.get(),[this]{++replies;});
    }
    ~StatusFixture(){plugin.reset();app_paths().set_root(previous);}
    void mode(const QString& value) {
        REQUIRE(QDir().mkpath(plugin->home()));QFile file(QDir(plugin->home()).filePath("fixture-mode.txt"));
        REQUIRE(file.open(QIODevice::WriteOnly));REQUIRE(file.write(value.toUtf8())==value.toUtf8().size());
    }
    QJsonObject exchange(const QString& command) {
        QJsonObject value;bool done=false;
        const auto signal=QObject::connect(plugin.get(),&DiscordPlugin::completed,plugin.get(),[&](const QString& actual,const QJsonObject& result){CHECK(actual==command);value=result;done=true;});
        const auto before=failures.size();plugin->request(command);
        REQUIRE(waitFor([&]{return !plugin->busy();}));QObject::disconnect(signal);
        REQUIRE(failures.size()==before);REQUIRE(done);return value;
    }
    int starts(const QString& command) {
        QFile file(QDir(plugin->home()).filePath("fixture-events.log"));REQUIRE(file.open(QIODevice::ReadOnly));
        int count=0;for(const auto& line:file.readAll().split('\n'))if(line.startsWith(command.toUtf8()+' '))++count;return count;
    }
    QProcess* client(){return plugin->findChild<QProcess*>("DiscordStatusClient");}
};
}

TEST_CASE("Discord status client reuses one process and keeps legacy requests compatible","[gui][plugin][status-stream]") {
    StatusFixture f;
    REQUIRE(f.exchange("settings").value("statusStream").toBool());
    const auto first=f.exchange("status");const auto second=f.exchange("status");
    CHECK(first.value("clientPid")==second.value("clientPid"));
    CHECK(first.value("index").toInt()==1);CHECK(second.value("index").toInt()==2);
    REQUIRE(f.client());CHECK(f.client()->state()==QProcess::Running);CHECK(f.starts("status-stream")==1);
    f.exchange("settings");CHECK(f.exchange("status").value("index").toInt()==3);
    f.plugin->releaseStatusClient();REQUIRE(waitFor([&]{return !f.client();}));
    CHECK(f.exchange("status").value("index").toInt()==1);CHECK(f.starts("status-stream")==2);
    // Switch while the client is alive: its old home must never be reused.
    REQUIRE(f.plugin->selectInstance("active-client-switch").isEmpty());
    REQUIRE(waitFor([&]{return !f.client();}));
    f.exchange("settings");CHECK(f.exchange("status").value("index").toInt()==1);
    CHECK(f.starts("status-stream")==1);
    REQUIRE(f.plugin->installPackage(f.package.filePath("manifest.json")).isEmpty());
    REQUIRE(waitFor([&]{return !f.client();}));
    f.exchange("settings");CHECK(f.exchange("status").value("index").toInt()==1);
    CHECK(f.starts("status-stream")==2);
    f.mode("legacy");CHECK_FALSE(f.exchange("settings").contains("statusStream"));
    REQUIRE(waitFor([&]{return !f.client();}));
    CHECK(f.exchange("status").value("index").toInt()==1);
    CHECK(f.exchange("status").value("index").toInt()==1);CHECK(f.starts("status")==2);
    CHECK(f.plugin->selectInstance("another-status-home").isEmpty());
    CHECK_FALSE(f.plugin->busy());CHECK_FALSE(f.client());
    CHECK(f.exchange("status").value("index").toInt()==1);CHECK(f.starts("status")==1);
}

TEST_CASE("Discord status client clears busy state and releases failed or cancelled children","[gui][plugin][status-stream]") {
    for(const QString mode:{"bad-json","oversized","double","exit","partial","delay"}) {
        DYNAMIC_SECTION(mode.toStdString()) {
            StatusFixture f;f.exchange("settings");f.mode(mode);const int before=f.replies;
            f.plugin->request("status");REQUIRE(f.plugin->busy());REQUIRE(f.plugin->refreshingStatus());
            REQUIRE(waitFor([&]{return (f.client() && f.client()->state()==QProcess::Running) || !f.failures.isEmpty();}));
            if(mode=="partial" || mode=="delay") {
                auto* timer=f.plugin->findChild<QTimer*>("PluginRequestTimeout");REQUIRE(timer);timer->setInterval(10);
            }
            REQUIRE(waitFor([&]{return !f.plugin->busy() && !f.failures.isEmpty();}));
            REQUIRE(waitFor([&]{return !f.client();}));CHECK(f.failures.size()==1);
            CHECK(f.replies-before<=(mode=="double"?1:0));
            f.mode("good");CHECK(f.exchange("status").value("index").toInt()==1);
        }
    }
    SECTION("suspend cancels a pending read without reporting a bot failure") {
        StatusFixture f;f.exchange("settings");f.mode("delay");const int before=f.replies;
        f.plugin->request("status");REQUIRE(f.plugin->busy());
        f.plugin->releaseStatusClient();REQUIRE(waitFor([&]{return !f.client();}));
        CHECK_FALSE(f.plugin->busy());CHECK(f.failures.isEmpty());CHECK(f.replies==before);
    }
}

TEST_CASE("hidden Discord panel releases its idle status client","[gui][plugin][status-stream]") {
    StatusFixture f;
    PluginsPanel panel;panel.resize(900,700);panel.show();
    auto* plugin=panel.findChild<DiscordPlugin*>();REQUIRE(plugin);
    REQUIRE(waitFor([&]{auto* client=plugin->findChild<QProcess*>("DiscordStatusClient");return client && client->state()==QProcess::Running && !plugin->busy();}));
    panel.hide();CHECK_FALSE(plugin->busy());
    REQUIRE(waitFor([&]{return !plugin->findChild<QProcess*>("DiscordStatusClient");}));
}

TEST_CASE("native status client exits when its Qt manager dies","[.][gui][plugin][status-stream-native]") {
    const auto manifest=qEnvironmentVariable("LOCALCODE_STATUS_NATIVE_PACKAGE");
    const auto report=qEnvironmentVariable("LOCALCODE_STATUS_NATIVE_REPORT");
    REQUIRE(!manifest.isEmpty());REQUIRE(!report.isEmpty());
    const auto home=QDir(QFileInfo(report).absolutePath()).filePath("native-private-home");
    REQUIRE_FALSE(QFileInfo::exists(home));
    app_paths().set_root(path_from_utf8(home.toStdString()));
    DiscordPlugin plugin;REQUIRE(plugin.installPackage(manifest).isEmpty());
    const auto exchange=[&](const QString& command) {
        QJsonObject value;bool done=false;QString error;
        const auto success=QObject::connect(&plugin,&DiscordPlugin::completed,&plugin,[&](const QString& actual,const QJsonObject& result){CHECK(actual==command);value=result;done=true;});
        const auto failure=QObject::connect(&plugin,&DiscordPlugin::failed,&plugin,[&](const QString& message){error=message;});
        plugin.request(command);REQUIRE(waitFor([&]{return !plugin.busy();},15000));
        QObject::disconnect(success);QObject::disconnect(failure);INFO(error.toStdString());REQUIRE(error.isEmpty());REQUIRE(done);return value;
    };
    exchange("init");REQUIRE(exchange("settings").value("statusStream").toBool());
    REQUIRE_FALSE(exchange("status").value("running").toBool());
    auto* client=plugin.findChild<QProcess*>("DiscordStatusClient");REQUIRE(client);REQUIRE(client->state()==QProcess::Running);
    QFile file(report);REQUIRE(file.open(QIODevice::WriteOnly|QIODevice::NewOnly));
    const auto bytes=QJsonDocument(QJsonObject{{"clientPid",static_cast<double>(client->processId())},{"privateHome",plugin.home()},
        {"statusReplies",1},{"exit","hard manager exit; destructors intentionally skipped"}}).toJson();
    REQUIRE(file.write(bytes)==bytes.size());file.close();
    std::_Exit(0);
}
