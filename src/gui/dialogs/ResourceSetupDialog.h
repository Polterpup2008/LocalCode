#pragma once
#include <QDialog>
#include <atomic>
#include <memory>
#include "../setup/ResourceInstaller.h"
class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QProgressBar;
class QPushButton;
class QShowEvent;
class QTabWidget;
class QTextBrowser;
class QTimer;

namespace localcode::gui {
struct SetupSelection {
    QString runtime, model, projector;
    bool cpuStartingProfile = false;
};

class ResourceSetupDialog : public QDialog {
    Q_OBJECT
public:
    ResourceSetupDialog(SetupSelection initial, QString runtimeRoot, QString modelRoot,
                        QWidget* parent = nullptr,
                        std::shared_ptr<ResourceInstaller> installer = {},
                        bool allowProjector = true);
    ~ResourceSetupDialog() override;
    SetupSelection selection() const;

protected:
    void showEvent(QShowEvent* event) override;
    void reject() override;

private:
    struct Outcome {
        QList<RuntimePackage> runtimes;
        HubModels models;
        QString path, model, projector;
    };

    struct Progress {
        std::atomic<quint64> received{0}, total{0};
    };

    using Work = std::function<Result<Outcome>(CancellationToken, ResourceProgress)>;
    void job(const QString& label, Work work, std::function<void(const Outcome&)> finished);
    void updateControls();
    void showRuntime();
    void showModel();
    void validateSelection();
    std::shared_ptr<ResourceInstaller> installer_;
    QString runtimeRoot_, modelRoot_;
    QList<RuntimePackage> runtimes_;
    HubModels models_;
    CancellationSource cancel_;
    std::shared_ptr<Progress> progress_;
    QLineEdit *runtime_, *model_, *projector_, *tag_, *repository_;
    QComboBox *runtimeBuild_, *modelFile_;
    QTabWidget* pages_;
    QTextBrowser *runtimeDetail_, *modelDetail_;
    QLabel* status_;
    QProgressBar* bar_;
    QCheckBox *cpu_, *memory_, *reasoning_;
    QPushButton *runtimeFetch_, *runtimeDownload_, *modelFetch_, *modelDownload_, *use_,
        *cancelButton_;
    QTimer* progressTimer_;
    bool busy_ = false, closeAfterCancel_ = false;
    bool allowProjector_ = true;
};
}   // namespace localcode::gui
