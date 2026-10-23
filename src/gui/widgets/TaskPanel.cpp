#include "TaskPanel.h"

#include <QUrl>
#include <QVBoxLayout>

#include "../Theme.h"
#include "MarkdownRenderer.h"

namespace localcode::gui {
namespace {

QString colour(const QColor& c) { return c.name(QColor::HexRgb); }

}  // namespace

TaskPanel::TaskPanel(QWidget* parent) : QWidget(parent) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    auto* title = new QLabel(tr("TASK"), this);
    title->setObjectName("PanelTitle");
    layout->addWidget(title);

    body_ = new QTextBrowser(this);
    body_->setFrameShape(QFrame::NoFrame);
    body_->setOpenLinks(false);
    layout->addWidget(body_, 1);

    verification_ = new QLabel(this);
    verification_->setWordWrap(true);
    verification_->setContentsMargins(8, 6, 8, 8);
    layout->addWidget(verification_);

    connect(body_, &QTextBrowser::anchorClicked, this, [this](const QUrl& url) {
        const QString href = url.toString();
        if (href.startsWith("file:")) emit fileActivated(href.mid(5));
    });

    clear();
}

void TaskPanel::setTaskBlock(const QString& block) {
    taskBlock_ = block;
    rebuild();
}

void TaskPanel::setChangedFiles(const QStringList& changed, const QStringList& conflicting) {
    changed_ = changed;
    conflicting_ = conflicting;
    rebuild();
}

void TaskPanel::setGitSummary(const QString& branch, int changedFiles, bool clean) {
    gitBranch_ = branch;
    gitChanged_ = changedFiles;
    gitClean_ = clean;
    rebuild();
}

void TaskPanel::setVerification(const QString& state, const QStringList& unsupportedClaims,
                                bool objectiveVerified, bool hadPlan) {
    const auto& p = colours();
    QColor tone = p.textMuted;
    QString text = state;

    if (objectiveVerified) {
        tone = p.success;
        text = tr("VERIFIED — every acceptance criterion is settled.");
    } else if (!hadPlan) {
        text.clear();
    } else if (state.compare("verified", Qt::CaseInsensitive) == 0) {
        text = tr("CHECKS PASSED — the objective is not verified.");
    } else if (state.compare("failing", Qt::CaseInsensitive) == 0) {
        tone = p.danger;
        text = tr("NOT VERIFIED — the last build or test run failed.");
    } else if (state.compare("partially verified", Qt::CaseInsensitive) == 0) {
        tone = p.warning;
        text = tr("SOME CHECKS PASSED — the objective is not verified.");
    } else if (state.compare("not verified", Qt::CaseInsensitive) == 0) {
        tone = p.textMuted;
        text = tr("NOT VERIFIED — no classified build or test evidence confirms the current source.");
    }

    if (!unsupportedClaims.isEmpty()) {
        tone = p.danger;
        text += "\n" + tr("Unsupported claims: %1").arg(unsupportedClaims.join("; "));
    }

    verification_->setText(text);
    verification_->setStyleSheet(QString("color:%1;font-size:9pt").arg(colour(tone)));
}

void TaskPanel::clearVerification() {
    verification_->clear();
}

void TaskPanel::rebuild() {
    const auto& p = colours();
    QString html = QString("<body style=\"color:%1;%2\">")
                       .arg(colour(p.text), Theme::instance().richTextStyle());

    if (taskBlock_.trimmed().isEmpty() && changed_.isEmpty()) {
        html += QString("<div style=\"color:%1;padding:6px\">%2</div>")
                    .arg(colour(p.textFaint),
                         tr("No task is running. Describe what you want done."));
    } else {
        html += MarkdownRenderer::toHtml(taskBlock_);
    }

    if (!changed_.isEmpty()) {
        html += QString(
                    "<div style=\"color:%1;font-weight:600;font-size:9pt;"
                    "letter-spacing:0.05em;margin:12px 0 4px 0\">CHANGED FILES</div>")
                    .arg(colour(p.textMuted));
        for (const auto& file : changed_) {
            const bool conflicts = conflicting_.contains(file);
            html += QString("<div style=\"margin:2px 0\">"
                            "<a href=\"file:%1\" style=\"color:%2;text-decoration:none\">%3</a>%4"
                            "</div>")
                        .arg(file, colour(conflicts ? p.warning : p.accent),
                             MarkdownRenderer::escape(file),
                             conflicts ? QString(" <span style=\"color:%1\">%2</span>")
                                             .arg(colour(p.warning),
                                                  tr("(had uncommitted changes)"))
                                       : QString());
        }
    }

    if (!gitBranch_.isEmpty()) {
        html += QString(
                    "<div style=\"color:%1;margin-top:12px;font-size:9pt\">"
                    "git: %2 &nbsp;·&nbsp; %3</div>")
                    .arg(colour(p.textFaint), MarkdownRenderer::escape(gitBranch_),
                         gitClean_ ? tr("clean") : tr("%1 changed").arg(gitChanged_));
    }

    html += "</body>";
    body_->setHtml(html);
}

void TaskPanel::clear() {
    taskBlock_.clear();
    changed_.clear();
    conflicting_.clear();
    gitBranch_.clear();
    gitChanged_ = 0;
    gitClean_ = true;
    verification_->clear();
    rebuild();
}

}  // namespace localcode::gui
