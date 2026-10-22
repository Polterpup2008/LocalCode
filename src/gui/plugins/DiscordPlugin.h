#pragma once
#include <QObject>
#include <QJsonObject>
#include <QString>

class QProcess;
class QTimer;

namespace localcode::gui {

// A versioned child-process boundary; no Rust internals enter the C++ engine.
class DiscordPlugin : public QObject {
    Q_OBJECT
public:
    explicit DiscordPlugin(QObject* parent = nullptr);
    ~DiscordPlugin() override;
    bool available() const;
    bool busy() const;
    bool refreshingStatus() const;
    QString home() const;
    QString instanceName() const;
    QString installPackage(const QString& manifestPath);
    QString selectInstance(const QString& name);
    void request(const QString& command, const QJsonObject& input = {});
    void releaseStatusClient();
    void start();
    static QString validatePackage(const QString& manifestPath, QString* executable = nullptr);

signals:
    void completed(const QString& command, const QJsonObject& result);
    void failed(const QString& message);
    void busyChanged(bool busy);
    void started(qint64 pid);

private:
    void loadRegistration();
    QString saveRegistration();
    QString baseDirectory() const;
    void configureProcess(QProcess* process) const;
    void requestStatusStream(const QString& binary);
    QString manifestPath_;
    QString instance_ = "default";
    QProcess* request_ = nullptr;
    QProcess* statusClient_ = nullptr;
    bool statusPending_ = false;
    bool statusStreamSupported_ = false;
    QByteArray statusOutput_;
    QTimer* timeout_ = nullptr;
    QByteArray output_;
    QString command_;
    bool oversized_ = false;
};
}
