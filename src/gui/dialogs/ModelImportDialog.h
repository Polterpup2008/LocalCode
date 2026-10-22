#pragma once
// ---------------------------------------------------------------------------
// Importing a GGUF file: pick it, read its header, show what was found, confirm.
// ---------------------------------------------------------------------------
#include <QDialog>
#include <QString>

#include "Application.h"

class QLabel;
class QLineEdit;
class QTextBrowser;
class QPushButton;

namespace localcode::gui {

class ModelImportDialog : public QDialog {
    Q_OBJECT

public:
    explicit ModelImportDialog(Application& application, QWidget* parent = nullptr);

    [[nodiscard]] QString importedProfileId() const { return profileId_; }

protected:
    // Same reason as the settings dialog: a tall dialog can be placed with its
    // buttons off the bottom of the screen.
    void showEvent(QShowEvent* event) override;

private slots:
    void browse();
    void inspect();
    void import();

private:
    Application&  application_;
    QLineEdit*    path_ = nullptr;
    QTextBrowser* detail_ = nullptr;
    QPushButton*  import_ = nullptr;
    QString       profileId_;
};

}  // namespace localcode::gui
