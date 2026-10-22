#include "DiscordPlugin.h"
#include "platform/Paths.h"
#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QSaveFile>
#include <QTimer>
#include <QUuid>
#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace localcode::gui {
namespace {
constexpr qint64 maxResponse = 512 * 1024;
QString writeJson(const QString& path, const QJsonObject& object) {
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(QJsonDocument(object).toJson()) < 0
        || !file.commit()) return QObject::tr("Cannot save plugin registration: %1").arg(file.errorString());
    return {};
}
}

DiscordPlugin::DiscordPlugin(QObject* parent) : QObject(parent) {
    timeout_ = new QTimer(this);
    timeout_->setObjectName("PluginRequestTimeout");
    timeout_->setSingleShot(true);
    connect(timeout_, &QTimer::timeout, this, [this] {
        if (statusPending_) { releaseStatusClient(); emit failed(tr("Plugin request timed out.")); }
        else if (request_) { request_->kill(); emit failed(tr("Plugin request timed out.")); }
    });
    loadRegistration();
}

DiscordPlugin::~DiscordPlugin() {
    releaseStatusClient();
    // Only a short-lived configuration/control client is owned here. The bot
    // was detached explicitly and survives closing its management window.
    if (request_) { request_->disconnect(this); request_->kill(); request_->waitForFinished(1000); }
}

QString DiscordPlugin::baseDirectory() const {
#ifdef Q_OS_WIN
    return QDir(QString::fromStdWString(app_paths().data_dir().native())).filePath("plugins/discord");
#else
    return QDir(QString::fromStdString(app_paths().data_dir().string())).filePath("plugins/discord");
#endif
}
QString DiscordPlugin::home() const { return QDir(baseDirectory()).filePath("instances/" + instance_); }
QString DiscordPlugin::instanceName() const { return instance_; }
bool DiscordPlugin::available() const { return !manifestPath_.isEmpty() && validatePackage(manifestPath_).isEmpty(); }
bool DiscordPlugin::busy() const { return request_ != nullptr || statusPending_; }
bool DiscordPlugin::refreshingStatus() const { return busy() && command_ == "status"; }

QString DiscordPlugin::validatePackage(const QString& manifestPath, QString* executable) {
    QFile file(manifestPath);
    if (!file.open(QIODevice::ReadOnly) || file.size() > 16 * 1024)
        return tr("Cannot read the plugin manifest (maximum 16 KiB)." );
    const auto document = QJsonDocument::fromJson(file.readAll());
    if (!document.isObject()) return tr("Invalid plugin manifest.");
    const auto object = document.object();
    if (object.value("id").toString() != "discord" || object.value("protocol").toInt() != 1)
        return tr("This panel requires the Discord plugin with protocol 1.");
    const auto name = object.value("executable").toString();
#ifdef Q_OS_WIN
    if (name != "lcd.exe") return tr("The Discord worker must be lcd.exe.");
#else
    if (name != "lcd") return tr("The Discord worker must be lcd.");
#endif
    const auto binary = QDir(QFileInfo(manifestPath).absolutePath()).filePath(name);
    const QFileInfo info(binary);
    if (!info.isFile() || info.isSymLink() || info.size() > 100 * 1024 * 1024)
        return tr("The plugin worker is missing or exceeds 100 MiB.");
    if (executable) *executable = binary;
    return {};
}

void DiscordPlugin::loadRegistration() {
    QFile file(QDir(baseDirectory()).filePath("installed.json"));
    if (file.open(QIODevice::ReadOnly) && file.size() <= 16 * 1024) {
        const auto object = QJsonDocument::fromJson(file.readAll()).object();
        const auto name = object.value("instance").toString("default");
        if (QRegularExpression("^[A-Za-z0-9_-]{1,48}$").match(name).hasMatch()) instance_ = name;
        const auto manifest = object.value("manifest").toString();
        if (validatePackage(manifest).isEmpty()) manifestPath_ = manifest;
    }
    if (manifestPath_.isEmpty()) {
        const auto exe = QCoreApplication::applicationDirPath();
        for (const auto& candidate : { QDir(exe).filePath("../plugins/discord/manifest.json"),
                                      QDir(exe).filePath("plugins/discord/manifest.json") }) {
            if (validatePackage(candidate).isEmpty()) { manifestPath_ = QFileInfo(candidate).absoluteFilePath(); break; }
        }
    }
}

QString DiscordPlugin::saveRegistration() {
    if (!QDir().mkpath(baseDirectory())) return tr("Cannot create the plugin user folder.");
    return writeJson(QDir(baseDirectory()).filePath("installed.json"),
                     {{"manifest", manifestPath_}, {"instance", instance_}});
}

QString DiscordPlugin::installPackage(const QString& manifestPath) {
    if (busy()) return tr("Wait for the current plugin request to finish.");
    QString binary;
    if (const auto error = validatePackage(manifestPath, &binary); !error.isEmpty()) return error;
    const auto destination = QDir(baseDirectory()).filePath("packages/" + QUuid::createUuid().toString(QUuid::Id128));
    if (!QDir().mkpath(destination)) return tr("Cannot create the plugin installation folder.");
    const auto source = QFileInfo(manifestPath).absolutePath();
    // Copy public artifacts only; never recurse through a bot checkout.
    const QStringList names = {
        "manifest.json", QFileInfo(binary).fileName(), "LICENSE",
        "README.md",     "THIRD_PARTY_NOTICES.txt",    "benchmark-prompt-cache.ps1"};
    for (const auto& name : names) {
        const auto input = QDir(source).filePath(name);
        if (name != "manifest.json" && name != QFileInfo(binary).fileName() && !QFileInfo::exists(input)) continue;
        if (!QFile::copy(input, QDir(destination).filePath(name))) return tr("Cannot install plugin file: %1").arg(name);
    }
    const auto licenses = QDir(source).filePath("licenses");
    if (QFileInfo(licenses).isDir() && !QFileInfo(licenses).isSymLink()) {
        QDirIterator files(licenses, QDir::Files, QDirIterator::Subdirectories);
        int count = 0; qint64 bytes = 0;
        const auto canonical = QFileInfo(licenses).canonicalFilePath() + '/';
        while (files.hasNext()) {
            files.next(); const auto info = files.fileInfo();
            bytes += info.size();
            if (++count > 1024 || bytes > 16 * 1024 * 1024 || info.isSymLink()
                || !info.canonicalFilePath().startsWith(canonical)) return tr("Invalid plugin license directory.");
            const auto relative = QDir(source).relativeFilePath(info.absoluteFilePath());
            const auto output = QDir(destination).filePath(relative);
            if (!QDir().mkpath(QFileInfo(output).absolutePath()) || !QFile::copy(info.absoluteFilePath(), output))
                return tr("Cannot copy a plugin license file.");
        }
    }
    const auto previous = manifestPath_;
    manifestPath_ = QDir(destination).filePath("manifest.json");
    if (auto error = saveRegistration(); !error.isEmpty()) { manifestPath_ = previous; return error; }
    releaseStatusClient(); statusStreamSupported_ = false;
    return {};
}

QString DiscordPlugin::selectInstance(const QString& name) {
    if (busy()) return tr("Wait for the current plugin request to finish.");
    if (!QRegularExpression("^[A-Za-z0-9_-]{1,48}$").match(name).hasMatch())
        return tr("Use 1–48 letters, digits, underscores or hyphens for the bot name.");
    const auto previous = instance_;
    instance_ = name;
    if (auto error = saveRegistration(); !error.isEmpty()) { instance_ = previous; return error; }
    releaseStatusClient(); statusStreamSupported_ = false;
    return {};
}

void DiscordPlugin::configureProcess(QProcess* process) const {
    auto environment = QProcessEnvironment::systemEnvironment();
    for (const auto& key : {"DISCORD_BOT_TOKEN", "DISCORD_APPLICATION_ID", "TAVILY_API_KEY", "SEARXNG_BASE_URL", "LCD_CONFIG_FILE"})
        environment.remove(key);
    environment.insert("LCD_HOME", home());
    process->setProcessEnvironment(environment);
    process->setWorkingDirectory(home());
#ifdef Q_OS_WIN
    process->setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments* arguments) {
        arguments->flags |= CREATE_NO_WINDOW;
    });
#endif
}

void DiscordPlugin::releaseStatusClient() {
    auto* process = statusClient_;
    statusClient_ = nullptr;
    const bool pending = statusPending_;
    statusPending_ = false;
    statusOutput_.clear();
    if (process) {
        process->disconnect(this);
        if (process->state() != QProcess::NotRunning) {
            process->kill();
            process->waitForFinished(1000);
        }
        process->deleteLater();
    }
    if (pending) { timeout_->stop(); emit busyChanged(false); }
}

void DiscordPlugin::requestStatusStream(const QString& binary) {
    if (statusClient_ && statusClient_->state() != QProcess::Running) releaseStatusClient();
    statusPending_ = true;
    statusOutput_.clear();
    emit busyChanged(true);
    timeout_->start(12000);
    if (statusClient_) { statusClient_->write("status\n"); return; }
    auto* process = new QProcess(this);
    process->setObjectName("DiscordStatusClient");
    statusClient_ = process;
    configureProcess(process);
    process->setProcessChannelMode(QProcess::SeparateChannels);
    connect(process, &QProcess::started, this, [this,process] {
        if (statusClient_ == process && statusPending_) process->write("status\n");
    });
    connect(process, &QProcess::readyReadStandardError, process, [process] { process->readAllStandardError(); });
    connect(process, &QProcess::readyReadStandardOutput, this, [this,process] {
        if (statusClient_ != process) return;
        statusOutput_ += process->readAllStandardOutput();
        if (!statusPending_ || statusOutput_.size() > 16 * 1024) {
            releaseStatusClient(); emit failed(tr("Invalid plugin status response.")); return;
        }
        const auto newline = statusOutput_.indexOf('\n');
        if (newline < 0) return;
        const auto document = QJsonDocument::fromJson(statusOutput_.left(newline));
        const auto envelope = document.object();
        if (newline != statusOutput_.size() - 1 || !document.isObject() ||
            envelope.value("protocol").toInt() != 1 || !envelope.value("ok").toBool() ||
            !envelope.value("result").isObject()) {
            releaseStatusClient(); emit failed(tr("Invalid plugin status response.")); return;
        }
        statusPending_ = false;
        statusOutput_.clear();
        timeout_->stop();
        emit busyChanged(false);
        emit completed("status",envelope.value("result").toObject());
    });
    connect(process, &QProcess::errorOccurred, this, [this,process](QProcess::ProcessError error) {
        if (statusClient_ == process && error == QProcess::FailedToStart) {
            const auto detail = process->errorString();
            releaseStatusClient(); emit failed(tr("Cannot start plugin status client: %1").arg(detail));
        }
    });
    connect(process, &QProcess::finished, this, [this,process] {
        if (statusClient_ != process) return;
        const bool pending = statusPending_;
        releaseStatusClient();
        if (pending) emit failed(tr("The plugin status client exited before replying."));
    });
    process->start(binary,{"plugin","status-stream"});
}

void DiscordPlugin::request(const QString& command, const QJsonObject& input) {
    if (busy()) return;
    QString binary;
    if (const auto error = validatePackage(manifestPath_, &binary); !error.isEmpty()) { emit failed(error); return; }
    if (!QDir().mkpath(home())) { emit failed(tr("Cannot create this bot's user folder.")); return; }
    command_ = command; output_.clear(); oversized_ = false;
    if (command == "status" && statusStreamSupported_) { requestStatusStream(binary); return; }
    auto* process = new QProcess(this);
    request_ = process;
    configureProcess(process);
    process->setProcessChannelMode(QProcess::SeparateChannels);
    connect(process, &QProcess::readyReadStandardOutput, this, [this, process] {
        output_ += process->readAllStandardOutput();
        if (output_.size() > maxResponse) { oversized_ = true; output_.clear(); process->kill(); }
    });
    connect(process, &QProcess::readyReadStandardError, process, [process] { process->readAllStandardError(); });
    connect(process, &QProcess::errorOccurred, this, [this, process](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            timeout_->stop(); request_ = nullptr;
            emit busyChanged(false); emit failed(tr("Cannot start plugin worker: %1").arg(process->errorString()));
            process->deleteLater();
        }
    });
    connect(process, &QProcess::finished, this, [this, process](int code, QProcess::ExitStatus status) {
        timeout_->stop(); output_ += process->readAllStandardOutput();
        const auto command = command_;
        request_ = nullptr;
        const auto document = QJsonDocument::fromJson(output_);
        const auto envelope = document.object();
        const auto result = envelope.value("result").toObject();
        const bool valid = !oversized_ && output_.size() <= maxResponse && status == QProcess::NormalExit;
        if (command == "settings" && valid && code == 0 && envelope.value("protocol").toInt() == 1 && envelope.value("ok").toBool()) {
            statusStreamSupported_ = result.value("statusStream").isBool() && result.value("statusStream").toBool();
            if (!statusStreamSupported_) releaseStatusClient();
        }
        emit busyChanged(false);
        if (command == "stop" && valid && code == 0) emit completed(command, {});
        else if (valid && envelope.value("protocol").toInt() == 1 && envelope.value("ok").toBool() && code == 0)
            emit completed(command, result);
        else emit failed(result.value("error").toString(tr("The plugin request failed. Check the bot log.")));
        process->deleteLater();
    });
    connect(process, &QProcess::started, this, [process, command, input] {
        if (command == "save") process->write(QJsonDocument(input).toJson(QJsonDocument::Compact));
        process->closeWriteChannel();
    });
    emit busyChanged(true);
    timeout_->start(command == "stop" ? 95000 : 12000);
    process->start(binary, command == "stop" ? QStringList{"stop"} : QStringList{"plugin", command});
}

void DiscordPlugin::start() {
    if (busy()) return;
    QString binary;
    if (const auto error = validatePackage(manifestPath_, &binary); !error.isEmpty()) { emit failed(error); return; }
    if (!QDir().mkpath(QDir(home()).filePath("data"))) { emit failed(tr("Cannot create the bot data folder.")); return; }
    QProcess process;
    configureProcess(&process);
    process.setProgram(binary); process.setArguments({"run"});
    process.setProcessChannelMode(QProcess::MergedChannels);
    process.setStandardOutputFile(QDir(home()).filePath("data/plugin.log"), QIODevice::Append);
    qint64 pid = 0;
    if (!process.startDetached(&pid)) { emit failed(tr("Cannot start the bot: %1").arg(process.errorString())); return; }
    emit started(pid);
}
}
