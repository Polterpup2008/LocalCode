#pragma once
// ---------------------------------------------------------------------------
// The approval prompt for a command the policy classified as Confirm.
//
// It shows the exact command, the working directory, and why approval was
// required - so the answer is an informed one rather than a reflex.
// ---------------------------------------------------------------------------
#include <QDialog>
#include <QString>

namespace localcode::gui {

class ConfirmationDialog : public QDialog {
    Q_OBJECT

public:
    ConfirmationDialog(const QString& title, const QString& detail, const QString& command,
                       const QString& reason, QWidget* parent = nullptr);
};

}  // namespace localcode::gui
