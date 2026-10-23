#include "StatusStrip.h"

#include <QStyle>

#include <QHBoxLayout>

#include "../Theme.h"

namespace localcode::gui {
namespace {

QString dot(const QColor& colour) {
    return QString("<span style=\"color:%1\">●</span>").arg(colour.name(QColor::HexRgb));
}

// A colour blended towards the background, for the pulse.
QColor faded(const QColor& colour, const QColor& towards, double amount) {
    const double keep = 1.0 - amount;
    return QColor(static_cast<int>(colour.red() * keep + towards.red() * amount),
                  static_cast<int>(colour.green() * keep + towards.green() * amount),
                  static_cast<int>(colour.blue() * keep + towards.blue() * amount));
}

}   // namespace

StatusStrip::StatusStrip(QWidget* parent) : QWidget(parent) {
    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(10, 3, 10, 3);
    layout->setSpacing(14);

    indicator_ = new QLabel(this);
    indicator_->setTextFormat(Qt::RichText);
    layout->addWidget(indicator_);

    modelLabel_ = new QLabel(this);
    modelLabel_->setObjectName("Muted");
    modelLabel_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    modelLabel_->setMaximumWidth(240);
    layout->addWidget(modelLabel_, 1);

    layout->addStretch(1);

    activityLabel_ = new QLabel(this);
    activityLabel_->setObjectName("Faint");
    activityLabel_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    activityLabel_->setMaximumWidth(300);
    layout->addWidget(activityLabel_, 1);

    outcomeLabel_ = new QLabel(this);
    outcomeLabel_->setObjectName("Muted");
    outcomeLabel_->setToolTip(
        tr("The last run's engineering verdict and whether a summary "
           "of it was produced. These fail independently."));
    layout->addWidget(outcomeLabel_);

    rateLabel_ = new QLabel(this);
    rateLabel_->setObjectName("Muted");
    rateLabel_->setToolTip(
        tr("Generation speed of the last turn, excluding prompt "
           "processing"));
    layout->addWidget(rateLabel_);

    workspaceLabel_ = new QLabel(this);
    workspaceLabel_->setObjectName("Faint");
    workspaceLabel_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    workspaceLabel_->setMaximumWidth(260);
    layout->addWidget(workspaceLabel_, 1);

    contextBar_ = new QProgressBar(this);
    contextBar_->setTextVisible(false);
    contextBar_->setFixedSize(90, 6);
    contextBar_->setRange(0, 100);
    contextBar_->setValue(0);
    contextBar_->hide();
    layout->addWidget(contextBar_);

    contextLabel_ = new QLabel(this);
    contextLabel_->setObjectName("Muted");
    contextLabel_->setMinimumWidth(0);
    layout->addWidget(contextLabel_);

    pulseTimer_ = new QTimer(this);
    pulseTimer_->setInterval(250);
    connect(pulseTimer_, &QTimer::timeout, this, [this] {
        ++pulseFrame_;
        redrawIndicator();
    });

    setModelState(ModelState::Unloaded, tr("No model loaded"));
}

void StatusStrip::setModelState(ModelState state, const QString& detail) {
    state_ = state;
    switch (state) {
        case ModelState::Unloaded: stateText_ = tr("No model"); break;
        case ModelState::Loading: stateText_ = tr("Starting"); break;
        case ModelState::Unloading: stateText_ = tr("Unloading"); break;
        case ModelState::Switching: stateText_ = tr("Switching"); break;
        case ModelState::Restarting: stateText_ = tr("Restarting"); break;
        case ModelState::Ready: stateText_ = tr("Ready"); break;
        case ModelState::Failed: stateText_ = tr("Failed"); break;
    }
    indicator_->setToolTip(detail);

    // Only the loading state animates. A dot that pulses forever is noise; one
    // that pulses exactly while a multi-minute load is in progress is a signal.
    if (state == ModelState::Loading || state == ModelState::Unloading ||
        state == ModelState::Switching || state == ModelState::Restarting) {
        pulseFrame_ = 0;
        pulseTimer_->start();
    } else {
        pulseTimer_->stop();
    }
    redrawIndicator();
}

void StatusStrip::redrawIndicator() {
    const auto& p = colours();
    QColor colour;
    switch (state_) {
        case ModelState::Unloaded: colour = p.textFaint; break;
        case ModelState::Loading: colour = p.warning; break;
        case ModelState::Unloading: colour = p.warning; break;
        case ModelState::Switching:
        case ModelState::Restarting: colour = p.warning; break;
        case ModelState::Ready: colour = p.success; break;
        case ModelState::Failed: colour = p.danger; break;
    }

    if (state_ == ModelState::Loading || state_ == ModelState::Unloading ||
        state_ == ModelState::Switching || state_ == ModelState::Restarting) {
        // A slow triangle wave between full and half strength - a breath rather
        // than a blink, which is easier to have in the corner of your eye.
        const int period = 20;
        const int phase = pulseFrame_ % period;
        const double up = static_cast<double>(phase < period / 2 ? phase : period - phase);
        const double amount = 0.55 * (up / (period / 2.0));
        colour = faded(colour, p.window, amount);
    }

    indicator_->setText(dot(colour) + " " + stateText_);
}

void StatusStrip::setModelName(const QString& name, const QString& backend) {
    if (name.isEmpty()) {
        modelLabel_->clear();
        return;
    }
    modelLabel_->setText(backend.isEmpty() ? name : QString("%1  ·  %2").arg(name, backend));
    modelLabel_->setToolTip(tr("Running: %1").arg(modelLabel_->text()));
}

void StatusStrip::setWorkspace(const QString& path) {
    workspaceLabel_->setText(workspaceLabel_->fontMetrics().elidedText(path, Qt::ElideMiddle, 260));
    workspaceLabel_->setToolTip(path);
}

void StatusStrip::setContext(int used, int total, bool exact) {
    if (total <= 0) {
        contextLabel_->clear();
        contextBar_->setValue(0);
        contextBar_->hide();
        return;
    }
    contextBar_->show();
    const int percent = qBound(0, used * 100 / total, 100);
    contextBar_->setValue(percent);

    auto thousands = [](int value) {
        return value >= 1000 ? QString::number(value / 1000.0, 'f', 1) + "K"
                             : QString::number(value);
    };
    contextLabel_->setText(
        QString("%1%2 / %3").arg(thousands(used), exact ? "" : "~", thousands(total)));
    contextLabel_->setToolTip(exact ? tr("Token counts come from the model's own tokenizer")
                                    : tr("Token counts are estimated; the running server does "
                                         "not expose a tokenizer"));

    const auto& p = colours();
    const QColor colour = percent > 90 ? p.danger : percent > 75 ? p.warning : p.accent;
    contextBar_->setStyleSheet(QString("QProgressBar::chunk{background:%1;border-radius:2px}")
                                   .arg(colour.name(QColor::HexRgb)));
}

void StatusStrip::setActivity(const QString& text) {
    activityLabel_->setText(activityLabel_->fontMetrics().elidedText(text, Qt::ElideMiddle, 300));
    activityLabel_->setToolTip(text);
}

void StatusStrip::setOutcome(bool objectiveVerified, const QString& presentation,
                             bool presentationFailed) {
    // Objective first and always, because it is the one that says whether the
    // work happened. Presentation is only worth a word when it went wrong -
    // naming it on every successful run would teach people to ignore it.
    const QString objective = objectiveVerified ? tr("verified") : tr("not verified");
    if (!presentationFailed) {
        outcomeLabel_->setText(tr("objective: %1").arg(objective));
        outcomeLabel_->setObjectName(objectiveVerified ? "Muted" : "Warning");
    } else {
        outcomeLabel_->setText(
            tr("objective: %1  ·  summary: %2")
                .arg(objective, presentation.isEmpty() ? tr("none") : presentation));
        outcomeLabel_->setObjectName("Warning");
    }
    outcomeLabel_->style()->unpolish(outcomeLabel_);
    outcomeLabel_->style()->polish(outcomeLabel_);
}

void StatusStrip::clearOutcome() {
    outcomeLabel_->clear();
}

void StatusStrip::setTokenRate(double decodeTokensPerSecond, bool measured) {
    if (decodeTokensPerSecond <= 0.0) {
        rateLabel_->clear();
        return;
    }
    rateLabel_->setText(
        QString("%1%2 tok/s").arg(measured ? "" : "~").arg(decodeTokensPerSecond, 0, 'f', 1));
    rateLabel_->setToolTip(measured ? tr("Decode throughput reported by the runtime. Excludes "
                                         "prompt evaluation.")
                                    : tr("Decode throughput measured by LocalCode, because the "
                                         "runtime reported none. Excludes prompt evaluation."));
}

}   // namespace localcode::gui
