#pragma once
// ---------------------------------------------------------------------------
// Settings.
//
// Everything configurable in one dialog, grouped by what it affects. The
// security page is deliberately explicit about what each autonomy level means,
// because that is the setting most worth understanding before changing.
// ---------------------------------------------------------------------------
#include <QDialog>

#include "Application.h"

class QCheckBox;
class QComboBox;
class QLineEdit;
class QPlainTextEdit;
class QSpinBox;
class QListWidget;
class QStackedWidget;
template <typename T>
class QFutureWatcher;

namespace localcode::gui {

class SettingsDialog : public QDialog {
    Q_OBJECT

public:
    explicit SettingsDialog(Application& application, QWidget* parent = nullptr);
    ~SettingsDialog() override;

signals:
    void settingsApplied();

protected:
    // A dialog centred on a tall parent can be placed with its buttons below
    // the bottom of the screen, where nothing can reach them.
    void showEvent(QShowEvent* event) override;

private slots:
    void apply();
    void browseLlamaServer();
    void browseIkLlamaServer();
    void browseRuntimeDirectory();
    void detectRuntimes();
    void updateRuntimeChoice();

private:
    QWidget* buildBackendPage();
    QWidget* buildAgentPage();
    QWidget* buildWebPage();
    QWidget* buildInterfacePage();
    QWidget* buildPathsPage();
    QWidget* buildPerformancePage();
    QWidget* buildAdvancedPage();
    QWidget* buildRepositoryPage();
    void load();

    Application& application_;
    QListWidget* navigation_ = nullptr;
    QStackedWidget* pages_ = nullptr;

    // Backends
    QComboBox* llamaSource_ = nullptr;
    QWidget* customLlamaRow_ = nullptr;
    QLineEdit* llamaServer_ = nullptr;
    QLineEdit* ikLlamaServer_ = nullptr;
    QComboBox* defaultBackend_ = nullptr;
    QCheckBox* useExternal_ = nullptr;
    QLineEdit* externalUrl_ = nullptr;
    QLineEdit* externalKey_ = nullptr;
    QPlainTextEdit* discovered_ = nullptr;
    QFutureWatcher<std::vector<RuntimeInstall>>* runtimeScan_ = nullptr;
    CancellationSource runtimeCancel_;

    // Agent
    QComboBox* autonomy_ = nullptr;
    QSpinBox* maxIterations_ = nullptr;
    QSpinBox* timeBudget_ = nullptr;
    QCheckBox* readOnly_ = nullptr;
    QCheckBox* streaming_ = nullptr;

    // Web
    QCheckBox* webEnabled_ = nullptr;
    QComboBox* provider_ = nullptr;
    QLineEdit* endpoint_ = nullptr;
    QLineEdit* apiKey_ = nullptr;
    QPlainTextEdit* trustedHosts_ = nullptr;

    // Interface
    QComboBox* theme_ = nullptr;
    QSpinBox* fontSize_ = nullptr;
    QCheckBox* showReasoning_ = nullptr;
    QComboBox* logLevel_ = nullptr;

    // Paths
    QLineEdit* runtimeDirectory_ = nullptr;
    QLineEdit* dataDirectory_ = nullptr;
    QLineEdit* cacheDirectory_ = nullptr;
};

}   // namespace localcode::gui
