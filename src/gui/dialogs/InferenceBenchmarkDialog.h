#pragma once
#include <QDialog>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QFutureWatcher>
#include <QStringList>
#include <atomic>
#include <memory>
#include "platform/Process.h"

class QLabel;
class QComboBox;
class QLineEdit;
class QProcess;
class QPushButton;
class QSpinBox;
class QTableWidget;
class QTimer;

namespace localcode::gui {
class InferenceBenchmarkDialog : public QDialog {
    Q_OBJECT
public:
    explicit InferenceBenchmarkDialog(const QString& model, const QString& server,
                                      QWidget* parent = nullptr);
    ~InferenceBenchmarkDialog() override;
    QJsonObject report() const;
    void setWeightRepacking(const QJsonValue& value);
    void setCpuPlacement(const QString& value);
    void setThreadSelectionEnabled(bool enabled, int maximumThreads = 256);
    QJsonObject selectedThreadSettings() const;
    void reject() override;

protected:
    void showEvent(QShowEvent* event) override;

private:
    enum class Stage { Idle, Help, Run };
    void start();
    void next();
    void readOutput();
    void finished(int exitCode, bool normalExit);
    void stop(const QString& reason);
    void settle(const QString& message);
    void saveReport();
    void summarizeThreads();
    void updateThreadSelection();
    bool threadSelectionMatches() const;
    bool selectedPoolsMatch() const;
    bool measurementInputsUnchanged() const;

    QLineEdit *bench_, *model_, *threads_;
    QSpinBox *prompt_, *generated_, *repetitions_;
    QLabel* status_;
    QTableWidget* table_;
    QPushButton *start_, *stop_, *save_;
    QProcess* process_;
    QFutureWatcher<Result<ProcessResult>>* ownedProcess_ = nullptr;
    CancellationSource ownedCancellation_;
    QStringList trialArguments_;
    std::uint32_t trialProcessId_ = 0;
    std::shared_ptr<std::atomic<std::uint32_t>> ownedProcessId_;
    QTimer *deadline_, *overallDeadline_;
    QElapsedTimer phaseTime_;
    Stage stage_ = Stage::Idle;
    QList<int> trials_;
    int trial_ = 0;
    quint64 comparison_ = 0;
    QString help_, stopReason_, modelDirectory_, modelName_, program_;
    QByteArray stdout_, stderr_;
    QJsonObject report_;
    QJsonObject targetModel_, targetBenchmark_, targetServer_;
    QJsonArray targetLibraries_;
    QJsonValue weightRepacking_{QJsonValue::Null};
    QString targetServerPath_;
    bool threadSelectionEnabled_ = false;
    int maximumThreads_ = 256;
    QComboBox *generationChoice_, *promptChoice_;
    QLabel* selectionNote_;
    QLabel* repackingNote_;
    QLabel* placementNote_;
    QString cpuPlacement_ = "os", defaultThreadInput_;
    QJsonObject placementMasks_;
    QJsonObject placementPools_;
    QPushButton* useThreads_;
};
}   // namespace localcode::gui
