#pragma once
#include <QDialog>
#include <QJsonObject>
class QComboBox;
class QLabel;
class QSpinBox;
class QShowEvent;

namespace localcode::gui {
class InferenceTuningDialog : public QDialog {
    Q_OBJECT
public:
    explicit InferenceTuningDialog(const QJsonObject& settings, bool plugin,
                                   QWidget* parent = nullptr);
    QJsonObject settings() const;
    void setBenchmarkFiles(const QString& model, const QString& server,
                           bool allowThreadTransfer = true);

protected:
    void showEvent(QShowEvent* event) override;

private:
    bool plugin_;
    QSpinBox *context_, *gpu_, *threads_, *batch_, *threadsBatch_ = nullptr, *ubatch_ = nullptr,
                                                   *cacheRam_ = nullptr;
    QComboBox *key_, *value_, *flash_;
    QComboBox* speculation_ = nullptr;
    QComboBox* repacking_ = nullptr;
    QComboBox* placement_ = nullptr;
    bool placementSupported_ = true;
    bool repackingSupported_ = true;
    QLabel* hardware_;
    QString benchmarkModel_, benchmarkServer_;
    bool benchmarkThreadTransfer_ = true;
};
}   // namespace localcode::gui
