#include "SkillsPanel.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QListWidgetItem>
#include <QPushButton>
#include <QSplitter>
#include <QVBoxLayout>

#include "../Theme.h"
#include "MarkdownRenderer.h"

namespace localcode::gui {

SkillsPanel::SkillsPanel(SkillRegistry& registry, QWidget* parent)
    : QWidget(parent), registry_(registry) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    // The enclosing tab is already labelled "Skills".

    auto* splitter = new QSplitter(Qt::Vertical, this);

    list_ = new QListWidget(splitter);
    list_->setFrameShape(QFrame::NoFrame);
    splitter->addWidget(list_);

    detail_ = new QTextBrowser(splitter);
    detail_->setFrameShape(QFrame::NoFrame);
    detail_->setOpenExternalLinks(false);
    splitter->addWidget(detail_);

    splitter->setSizes({200, 320});
    layout->addWidget(splitter, 1);

    auto* buttons = new QWidget(this);
    auto* buttonLayout = new QHBoxLayout(buttons);
    buttonLayout->setContentsMargins(6, 4, 6, 6);
    buttonLayout->setSpacing(6);

    toggle_ = new QPushButton(tr("Disable"), buttons);
    review_ = new QPushButton(tr("Mark reviewed"), buttons);
    review_->setToolTip(tr("A skill written by the agent is not used until a person has read "
                           "it and marked it reviewed."));
    buttonLayout->addWidget(toggle_);
    buttonLayout->addWidget(review_);
    buttonLayout->addStretch(1);
    layout->addWidget(buttons);

    connect(list_, &QListWidget::currentRowChanged, this, &SkillsPanel::showSelected);
    connect(toggle_, &QPushButton::clicked, this, &SkillsPanel::toggleEnabled);
    connect(review_, &QPushButton::clicked, this, &SkillsPanel::markReviewed);

    refresh();
}

void SkillsPanel::refresh() {
    const QString previous = list_->currentItem() ? list_->currentItem()->data(Qt::UserRole).toString()
                                                  : QString();
    list_->clear();

    const auto& p = colours();
    for (const auto& skill : registry_.skills()) {
        const QString id = QString::fromStdString(skill.id);
        auto* item = new QListWidgetItem(id, list_);
        item->setData(Qt::UserRole, id);

        QString suffix;
        if (!skill.validated) {
            suffix = tr("  (awaiting review)");
            item->setForeground(p.warning);
        } else if (!skill.enabled) {
            suffix = tr("  (disabled)");
            item->setForeground(p.textFaint);
        } else if (skill.always) {
            suffix = tr("  (always on)");
            item->setForeground(p.accent);
        }
        item->setText(id + suffix);
        item->setToolTip(QString::fromStdString(skill.description));
    }

    if (!previous.isEmpty()) {
        for (int i = 0; i < list_->count(); ++i) {
            if (list_->item(i)->data(Qt::UserRole).toString() == previous) {
                list_->setCurrentRow(i);
                break;
            }
        }
    } else if (list_->count() > 0) {
        list_->setCurrentRow(0);
    }
}

const Skill* SkillsPanel::selectedSkill() const {
    auto* item = list_->currentItem();
    if (!item) return nullptr;
    return registry_.find(item->data(Qt::UserRole).toString().toStdString());
}

void SkillsPanel::showSelected() {
    const auto* skill = selectedSkill();
    if (!skill) {
        detail_->clear();
        toggle_->setEnabled(false);
        review_->setEnabled(false);
        return;
    }

    toggle_->setEnabled(true);
    toggle_->setText(skill->enabled ? tr("Disable") : tr("Enable"));
    review_->setEnabled(!skill->validated);

    const auto& p = colours();
    QString html = QString("<body style=\"color:%1;font-size:11px\">").arg(
        p.text.name(QColor::HexRgb));
    html += QString("<div style=\"font-size:14px;font-weight:600\">%1</div>")
                .arg(MarkdownRenderer::escape(QString::fromStdString(skill->name)));
    html += QString("<div style=\"color:%1;margin:2px 0 8px 0\">%2</div>")
                .arg(p.textMuted.name(QColor::HexRgb),
                     MarkdownRenderer::escape(QString::fromStdString(skill->description)));

    if (!skill->triggers.empty()) {
        QStringList triggers;
        for (const auto& trigger : skill->triggers) triggers << QString::fromStdString(trigger);
        html += QString("<div style=\"color:%1;font-size:10px\">TRIGGERS: %2</div>")
                    .arg(p.textFaint.name(QColor::HexRgb),
                         MarkdownRenderer::escape(triggers.join(", ")));
    }
    if (!skill->validated) {
        html += QString("<div style=\"color:%1;margin-top:8px\">%2</div>")
                    .arg(p.warning.name(QColor::HexRgb),
                         tr("This skill was written by the agent and has not been reviewed. It "
                            "will not be used until you mark it reviewed."));
    }
    html += "<hr>";
    html += MarkdownRenderer::toHtml(QString::fromStdString(skill->body));
    html += "</body>";
    detail_->setHtml(html);
}

void SkillsPanel::toggleEnabled() {
    const auto* skill = selectedSkill();
    if (!skill) return;
    const bool enabled = !skill->enabled;
    const QString id = QString::fromStdString(skill->id);
    registry_.set_enabled(skill->id, enabled);
    emit skillStateChanged(id, enabled, skill->validated);
    refresh();
}

void SkillsPanel::markReviewed() {
    const auto* skill = selectedSkill();
    if (!skill || skill->validated) return;

    Skill updated = *skill;
    updated.validated = true;
    const QString id = QString::fromStdString(updated.id);
    registry_.save_user_skill(updated);
    emit skillStateChanged(id, updated.enabled, true);
    refresh();
}

}  // namespace localcode::gui
