#include "../ScreenGuard.h"
#include <QShowEvent>
#include "ModelImportDialog.h"

#include <QDialogButtonBox>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTextBrowser>
#include <QVBoxLayout>

#include "../Theme.h"
#include "../widgets/MarkdownRenderer.h"
#include "model/GgufMetadata.h"
#include "platform/PathEncoding.h"

namespace localcode::gui {

ModelImportDialog::ModelImportDialog(Application& application, QWidget* parent)
    : QDialog(parent), application_(application) {
    setWindowTitle(tr("Add a GGUF model"));
    setModal(true);
    setMinimumSize(620, 480);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(14, 14, 14, 14);
    layout->setSpacing(10);

    auto* row = new QHBoxLayout();
    path_ = new QLineEdit(this);
    path_->setPlaceholderText(tr("Path to a .gguf file"));
    row->addWidget(path_, 1);

    auto* browse = new QPushButton(tr("Browse..."), this);
    row->addWidget(browse);
    layout->addLayout(row);

    detail_ = new QTextBrowser(this);
    detail_->setPlaceholderText(
        tr("Choose a model file. LocalCode reads its header to work out the architecture, "
           "context length, quantization and whether it supports tool calling."));
    layout->addWidget(detail_, 1);

    auto* buttons = new QDialogButtonBox(this);
    import_ = buttons->addButton(tr("Add model"), QDialogButtonBox::AcceptRole);
    import_->setObjectName("Primary");
    import_->setEnabled(false);
    buttons->addButton(QDialogButtonBox::Cancel);
    layout->addWidget(buttons);

    connect(browse, &QPushButton::clicked, this, &ModelImportDialog::browse);
    connect(path_, &QLineEdit::textChanged, this, &ModelImportDialog::inspect);
    connect(buttons, &QDialogButtonBox::accepted, this, &ModelImportDialog::import);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
}

void ModelImportDialog::browse() {
    const auto file = QFileDialog::getOpenFileName(this, tr("Select a GGUF model"), QString(),
                                                   tr("GGUF models (*.gguf);;All files (*)"));
    if (!file.isEmpty()) path_->setText(file);
}

void ModelImportDialog::inspect() {
    const QString file = path_->text().trimmed();
    import_->setEnabled(false);
    if (file.isEmpty()) {
        detail_->clear();
        return;
    }

    auto metadata = read_gguf_metadata(path_from_utf8(file.toStdString()));
    const auto& p = colours();

    if (!metadata) {
        detail_->setHtml(QString("<div style=\"color:%1\">%2</div>")
                             .arg(p.danger.name(QColor::HexRgb),
                                  MarkdownRenderer::escape(
                                      QString::fromStdString(metadata.error().to_string()))));
        return;
    }

    QString html = QString("<body style=\"color:%1;font-size:11px\">")
                       .arg(p.text.name(QColor::HexRgb));
    html += "<table cellspacing=\"0\" cellpadding=\"3\">";
    for (const auto& [key, value] : metadata->summary()) {
        html += QString("<tr><td style=\"color:%1\">%2</td><td>%3</td></tr>")
                    .arg(p.textMuted.name(QColor::HexRgb),
                         MarkdownRenderer::escape(QString::fromStdString(key)),
                         MarkdownRenderer::escape(QString::fromStdString(value)));
    }
    html += "</table>";

    const auto capabilities = ModelCapabilities::infer(*metadata);
    html += QString("<div style=\"font-weight:600;color:%1;margin:12px 0 4px 0\">%2</div>")
                .arg(p.textMuted.name(QColor::HexRgb), tr("CAPABILITIES"));
    for (const auto& [key, value] : capabilities.describe()) {
        if (key == "evidence") continue;
        const bool yes = value == "yes";
        html += QString("<div><span style=\"color:%1\">%2</span> %3</div>")
                    .arg((yes ? p.success : p.textFaint).name(QColor::HexRgb), yes ? "✓" : "·",
                         MarkdownRenderer::escape(QString::fromStdString(key)));
    }
    if (!capabilities.evidence.empty()) {
        html += QString("<div style=\"color:%1;font-size:10px;margin-top:6px\">%2</div>")
                    .arg(p.textFaint.name(QColor::HexRgb),
                         MarkdownRenderer::escape(
                             QString::fromStdString(capabilities.evidence)));
    }
    if (!capabilities.supports_tools) {
        html += QString("<div style=\"color:%1;margin-top:8px\">%2</div>")
                    .arg(p.warning.name(QColor::HexRgb),
                         tr("This model's chat template does not declare tool calling. "
                            "LocalCode will use its documented text protocol instead, which "
                            "works but is less reliable than native tool calls."));
    }
    html += "</body>";
    detail_->setHtml(html);
    import_->setEnabled(true);
}

void ModelImportDialog::import() {
    const QString file = path_->text().trimmed();
    if (file.isEmpty()) return;

    auto profile = application_.import_model(path_from_utf8(file.toStdString()));
    if (!profile) {
        detail_->setHtml(QString("<div style=\"color:%1\">%2</div>")
                             .arg(colours().danger.name(QColor::HexRgb),
                                  MarkdownRenderer::escape(
                                      QString::fromStdString(profile.error().to_string()))));
        return;
    }
    profileId_ = QString::fromStdString(profile->id);
    accept();
}

void ModelImportDialog::showEvent(QShowEvent* event) {
    QDialog::showEvent(event);
    // After the base class has sized and centred it, not before:
    // the geometry being corrected does not exist until then.
    clampToScreen(*this);
}

}  // namespace localcode::gui
