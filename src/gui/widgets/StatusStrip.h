#pragma once
// ---------------------------------------------------------------------------
// The status line: model state, context usage, workspace, backend.
// ---------------------------------------------------------------------------
#include <QLabel>
#include <QProgressBar>
#include <QTimer>
#include <QWidget>

namespace localcode::gui {

enum class ModelState { Unloaded, Loading, Switching, Restarting, Ready, Failed, Unloading };

class StatusStrip : public QWidget {
    Q_OBJECT

public:
    explicit StatusStrip(QWidget* parent = nullptr);

    void setModelState(ModelState state, const QString& detail);
    void setModelName(const QString& name, const QString& backend);
    void setWorkspace(const QString& path);
    void setContext(int used, int total, bool exact);
    void setActivity(const QString& text);
    // The last run's two verdicts, shown side by side because they are two
    // questions: was the work established, and was it described. A run can pass
    // the first and fail the second, and a status line that shows only one word
    // cannot say so.
    void setOutcome(bool objectiveVerified, const QString& presentation, bool presentationFailed);
    void clearOutcome();
    // Decode throughput of the last turn. `measured` false means it was
    // derived here rather than reported by the runtime, and is shown with a
    // tilde so it cannot be mistaken for the runtime's own figure.
    void setTokenRate(double decodeTokensPerSecond, bool measured);

private:
    void redrawIndicator();

    QLabel* indicator_ = nullptr;
    QLabel* modelLabel_ = nullptr;
    QLabel* workspaceLabel_ = nullptr;
    QLabel* contextLabel_ = nullptr;
    QProgressBar* contextBar_ = nullptr;
    QLabel* activityLabel_ = nullptr;
    QLabel* outcomeLabel_ = nullptr;
    // Throughput has its own label: it used to share one with the activity note,
    // so whichever arrived second erased the other.
    QLabel* rateLabel_ = nullptr;

    // The indicator breathes while the model is loading, which is the one state
    // that can last minutes with nothing else on screen changing.
    QTimer* pulseTimer_ = nullptr;
    ModelState state_ = ModelState::Unloaded;
    QString stateText_;
    int pulseFrame_ = 0;
};

}   // namespace localcode::gui
