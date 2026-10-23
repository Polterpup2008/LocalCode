#pragma once
// ---------------------------------------------------------------------------
// Model management.
//
// Lists the imported models with what was read out of their own GGUF headers,
// lets the user tune a profile, and loads one. Loading is asynchronous - a large
// model takes minutes - so the panel reports progress rather than freezing.
// ---------------------------------------------------------------------------
#include <QJsonObject>
#include <QListWidget>
#include <QString>
#include <QTextBrowser>
#include <QWidget>

#include "Application.h"

class QCheckBox;
class QComboBox;
class QLabel;
class QPushButton;
class QSpinBox;

namespace localcode::gui {

class ModelPanel : public QWidget {
    Q_OBJECT

public:
    explicit ModelPanel(Application& application, QWidget* parent = nullptr);

    void refresh();
    [[nodiscard]] QString selectedModelId() const;

signals:
    void loadRequested(const QString& profileId);
    void unloadRequested();
    void importRequested();
    void statusMessage(const QString& text);

private slots:
    void showSelected();
    void applyEdits();
    void removeSelected();

private:
    [[nodiscard]] std::optional<ModelProfile> selectedProfile() const;

    Application&  application_;
    QListWidget*  list_ = nullptr;
    QTextBrowser* detail_ = nullptr;

    QComboBox* backend_ = nullptr;
    QSpinBox*  context_ = nullptr;
    QSpinBox*  gpuLayers_ = nullptr;
    QCheckBox* autoGpuLayers_ = nullptr;
    QCheckBox* flashAttention_ = nullptr;
    QCheckBox* reasoning_ = nullptr;
    QComboBox* toolProtocol_ = nullptr;

    QPushButton* load_ = nullptr;
    QPushButton* unload_ = nullptr;
    QPushButton* tune_ = nullptr;
    QJsonObject tuning_;

    std::vector<ModelProfile> profiles_;
};

}  // namespace localcode::gui
