#include "VerificationPanel.h"

#include <QVBoxLayout>

#include "../Theme.h"
#include "MarkdownRenderer.h"

namespace localcode::gui {
namespace {

QString colour(const QColor& c) {
    return c.name(QColor::HexRgb);
}

QColor toneFor(const QString& status, const Palette& p) {
    if (status == "pass") return p.success;
    if (status == "fail") return p.danger;
    if (status == "stale") return p.warning;
    if (status == "not applicable") return p.textFaint;
    return p.textMuted;   // not established
}

// What each stage means, in one line, because "execution: pass" is exactly the
// fact a reader is most likely to mistake for "it works".
QString meaningOf(const QString& stage) {
    if (stage == "configure") return QObject::tr("a build system accepted the project");
    if (stage == "build") return QObject::tr("it compiled and linked");
    if (stage == "tests") return QObject::tr("the project's own tests ran and passed");
    if (stage == "execution") return QObject::tr("the program ran without crashing");
    if (stage == "behaviour") return QObject::tr("its output was checked against the task");
    if (stage == "objective") return QObject::tr("every acceptance criterion is settled");
    return {};
}

}   // namespace

VerificationPanel::VerificationPanel(QWidget* parent) : QWidget(parent) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    auto* title = new QLabel(tr("VERIFICATION"), this);
    title->setObjectName("PanelTitle");
    layout->addWidget(title);

    headline_ = new QLabel(this);
    headline_->setWordWrap(true);
    headline_->setContentsMargins(8, 6, 8, 6);
    layout->addWidget(headline_);

    body_ = new QTextBrowser(this);
    body_->setFrameShape(QFrame::NoFrame);
    body_->setOpenLinks(false);
    layout->addWidget(body_, 1);

    clear();
}

void VerificationPanel::setVerification(const UiVerification& verification) {
    state_ = verification;
    hasState_ = true;
    rebuild();
}

void VerificationPanel::clear() {
    state_ = UiVerification{};
    hasState_ = false;
    rebuild();
}

void VerificationPanel::rebuild() {
    const auto& p = colours();

    if (!hasState_) {
        headline_->setText(tr("Nothing has been established yet."));
        headline_->setStyleSheet(QString("color:%1;font-size:11px").arg(colour(p.textFaint)));
        body_->setHtml(QString("<body style=\"color:%1;font-size:11px;padding:6px\">%2</body>")
                           .arg(colour(p.textFaint),
                                tr("A run records what it proved here, stage by stage.")));
        return;
    }

    // The headline is the objective, stated in the terms a reader cares about
    // and never softened. "Not established" is the honest word for a run that
    // built and ran something without ever checking the answer.
    QString objectiveStatus = "not established";
    for (const auto& stage : state_.stages) {
        if (stage.name == "objective") objectiveStatus = stage.status;
    }
    QString headline;
    QColor headlineTone = p.textMuted;
    if (objectiveStatus == "pass") {
        headline = tr("VERIFIED — every acceptance criterion was settled by evidence.");
        headlineTone = p.success;
    } else if (objectiveStatus == "fail") {
        headline = tr("FAILED — an acceptance criterion was checked and not met.");
        headlineTone = p.danger;
    } else if (objectiveStatus == "stale") {
        headline = tr("OUT OF DATE — files changed after the last thing that was proved.");
        headlineTone = p.warning;
    } else {
        headline = tr("NOT ESTABLISHED — nothing yet shows the task is done.");
        headlineTone = p.textMuted;
    }
    headline_->setText(headline);
    headline_->setStyleSheet(
        QString("color:%1;font-size:11px;font-weight:600").arg(colour(headlineTone)));

    QString html = QString("<body style=\"color:%1;font-size:11px\">").arg(colour(p.text));
    html += QString("<div style=\"color:%1;font-size:10px;margin-bottom:6px\">%2</div>")
                .arg(colour(p.textFaint), tr("source revision r%1").arg(state_.revision));

    html += "<table cellspacing=\"0\" cellpadding=\"2\" width=\"100%\">";
    for (const auto& stage : state_.stages) {
        const auto tone = toneFor(stage.status, p);
        QString note = stage.status == "stale" ? tr("passed at r%1, source is now r%2")
                                                     .arg(stage.establishedAtRevision)
                                                     .arg(state_.revision)
                                               : meaningOf(stage.name);
        if (!stage.detail.isEmpty() &&
            (stage.status == "fail" || (stage.name == "objective" && stage.status == "pass")))
            note = stage.detail;

        html += QString(
                    "<tr>"
                    "<td style=\"color:%1\">%2</td>"
                    "<td style=\"color:%3;font-weight:600\">%4</td></tr>"
                    "<tr><td colspan=\"2\" style=\"color:%5;padding-bottom:8px\">%6</td></tr>")
                    .arg(colour(p.textMuted), MarkdownRenderer::escape(stage.name), colour(tone),
                         MarkdownRenderer::escape(stage.status.toUpper()), colour(p.textFaint),
                         MarkdownRenderer::escape(note));
    }
    html += "</table>";

    if (!state_.plan.isEmpty()) {
        html += QString(
                    "<div style=\"color:%1;font-weight:600;font-size:10px;"
                    "letter-spacing:0.05em;margin:12px 0 4px 0\">%2</div>")
                    .arg(colour(p.textMuted), tr("ACCEPTANCE CRITERIA"));
        int index = 0;
        for (const auto& check : state_.plan) {
            ++index;
            QString marker = "○";
            QColor tone = p.textMuted;
            if (check.settled) {
                marker = check.satisfied ? "✓" : "✗";
                tone = check.satisfied ? p.success : p.danger;
            }
            html += QString(
                        "<div style=\"margin:2px 0;color:%1\">"
                        "<span style=\"color:%2;font-weight:600\">%3</span> %4. %5%6</div>")
                        .arg(colour(p.text), colour(tone), marker)
                        .arg(index)
                        .arg(MarkdownRenderer::escape(check.description),
                             check.evidenceId.isEmpty()
                                 ? QString()
                                 : QString(" <span style=\"color:%1\">(%2)</span>")
                                       .arg(colour(p.textFaint),
                                            MarkdownRenderer::escape(check.evidenceId)));
        }
    }

    if (!state_.recentEvidence.isEmpty()) {
        html += QString(
                    "<div style=\"color:%1;font-weight:600;font-size:10px;"
                    "letter-spacing:0.05em;margin:12px 0 4px 0\">%2</div>")
                    .arg(colour(p.textMuted), tr("EVIDENCE"));
        for (const auto& line : state_.recentEvidence) {
            html += QString("<div style=\"color:%1;margin:1px 0\">%2</div>")
                        .arg(colour(p.textFaint), MarkdownRenderer::escape(line));
        }
    }

    html += "</body>";
    body_->setHtml(html);
}

}   // namespace localcode::gui
