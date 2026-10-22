#pragma once
#include <QWidget>
#include <QJsonObject>
class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;
class QTabWidget;
class QTimer;
namespace localcode::gui {
class DiscordPlugin;
class PluginsPanel : public QWidget {
    Q_OBJECT
public:
    explicit PluginsPanel(QWidget* parent = nullptr);
protected:
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;
private:
    void initialise();
    void refresh();
    void updateControls();
    void applySettings(const QJsonObject& settings);
    QJsonObject collectSettings() const;
    void readLog();
    DiscordPlugin* plugin_;
    QTimer* poll_;
    QComboBox* instance_;
    QPushButton *install_, *select_, *save_, *start_, *stop_, *refresh_, *tune_;
    QLabel *status_, *home_, *tokenState_;
    QTabWidget* tabs_;
    QLineEdit *guilds_, *owners_, *channels_, *token_, *runtime_, *model_, *mmproj_;
    QComboBox* language_;
    QSpinBox *context_, *layers_, *reserve_, *idle_;
    QSpinBox *historyMessages_, *replyDepth_, *historyAge_, *historyBudget_;
    QCheckBox *preload_, *asleep_;
    QPlainTextEdit *persona_, *conversationNotes_, *log_;
    qint64 logOffset_ = 0;
    QJsonObject tuning_;
    QJsonValue cpuPlacementModes_{QJsonValue::Undefined};
    bool tuningSupported_ = false;
    bool conversationSupported_ = false;
    bool conversationNotesSupported_ = false;
    bool initialised_ = false, loaded_ = false, running_ = false, tokenConfigured_ = false, startAfterSave_ = false;
};
}
