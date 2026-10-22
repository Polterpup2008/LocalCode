#include "ConfirmationDialog.h"

#include <QDialogButtonBox>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QVBoxLayout>

#include "../Theme.h"

namespace localcode::gui {

ConfirmationDialog::ConfirmationDialog(const QString& title, const QString& detail,
                                       const QString& command, const QString& reason,
                                       QWidget* parent)
    : QDialog(parent) {
    setWindowTitle(tr("Approval required"));
    setModal(true);
    setMinimumWidth(560);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(16, 14, 16, 14);
    layout->setSpacing(10);

    auto* heading = new QLabel(title.isEmpty() ? tr("Run this command?") : title, this);
    heading->setStyleSheet("font-size:14px;font-weight:600");
    layout->addWidget(heading);

    if (!command.isEmpty()) {
        auto* commandView = new QPlainTextEdit(command, this);
        commandView->setReadOnly(true);
        commandView->setFont(Theme::instance().monoFont());
        commandView->setMaximumHeight(90);
        layout->addWidget(commandView);
    }

    if (!detail.isEmpty()) {
        auto* detailLabel = new QLabel(detail, this);
        detailLabel->setObjectName("Muted");
        detailLabel->setWordWrap(true);
        detailLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
        layout->addWidget(detailLabel);
    }

    if (!reason.isEmpty()) {
        auto* reasonLabel = new QLabel(tr("Why this needs approval: %1").arg(reason), this);
        reasonLabel->setWordWrap(true);
        reasonLabel->setStyleSheet(
            QString("color:%1").arg(colours().warning.name(QColor::HexRgb)));
        layout->addWidget(reasonLabel);
    }

    auto* buttons = new QDialogButtonBox(this);
    auto* allow = buttons->addButton(tr("Run it"), QDialogButtonBox::AcceptRole);
    allow->setObjectName("Primary");
    auto* refuse = buttons->addButton(tr("Don't run"), QDialogButtonBox::RejectRole);
    refuse->setDefault(true);   // the safe answer is the default
    refuse->setFocus();

    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
}

}  // namespace localcode::gui
