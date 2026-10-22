#pragma once
// ---------------------------------------------------------------------------
// The bridge between the engine and the interface.
//
// The agent runs on a worker thread. Its events arrive on that thread and are
// re-emitted here as queued Qt signals, so every slot in the UI runs on the GUI
// thread without any widget ever touching a core object across a thread
// boundary.
//
// Confirmation is the one call that goes the other way: the tool thread has to
// wait until a human answers or cancellation releases it. The dialog is queued
// onto the GUI thread; the worker's wait must remain cancellable during shutdown.
// ---------------------------------------------------------------------------
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>

#include <atomic>
#include <memory>
#include <thread>

#include "Application.h"
#include "agent/Agent.h"

namespace localcode::gui {

// Qt-friendly mirrors of the core types, so widgets never include engine
// headers in their own headers and moc has plain types to work with.
struct UiToolEvent {
    QString name;
    QString arguments;
    QString summary;
    QString detail;
    bool ok = false;
    int exitCode = -1;
    qint64 durationMs = 0;
    QString logPath;
};

// What one completed turn cost, mirroring core TurnStats field for field.
//
// The provenance flags are not decoration: a display that shows an estimate the
// same way it shows a measurement is worse than one that shows nothing.
struct UiTurnStats {
    bool usageFromServer = false;
    int promptTokens = 0;
    int cachedPromptTokens = 0;
    int completionTokens = 0;   // reasoning + visible answer together

    bool timingsFromServer = false;
    int promptTokensEvaluated = 0;
    double promptMs = 0.0;
    double promptTokensPerSecond = 0.0;
    double decodeMs = 0.0;
    double decodeTokensPerSecond = 0.0;

    qint64 totalRequestMs = 0;
    qint64 timeToFirstTokenMs = 0;

    int reasoningCharacters = 0;
    int estimatedReasoningTokens = 0;
    qint64 observedReasoningStreamMs = 0;
    int reasoningDeltaCount = 0;
};

// One verification stage as the panel draws it. Mirrors core so the widget
// never has to interpret the model's text block for itself.
struct UiStage {
    QString name;
    QString status;   // pass / fail / stale / not established / not applicable
    QString detail;
    QString evidenceId;
    int establishedAtRevision = -1;
};

struct UiAcceptanceCheck {
    QString description;
    bool settled = false;
    bool satisfied = false;
    QString evidenceId;
};

struct UiVerification {
    int revision = 0;
    QList<UiStage> stages;
    QList<UiAcceptanceCheck> plan;
    QStringList recentEvidence;
};

// How a run ended, with the engineering verdict and the account of it kept
// apart - because they fail independently and a window that collapses them into
// one word will tell a user their work failed when it did not.
//
// Every field is copied from what the engine decided. Nothing here is
// re-derived: the interface renders this, it does not compute it.
struct UiOutcome {
    // What the engine calls this run: verified / verified, no summary /
    // not verified / cancelled, with the one line that goes with it.
    QString kind;
    QString headline;

    // --- Engineering ------------------------------------------------------
    bool objectiveVerified = false;   // the ledger's Objective stage holds
    QString objectiveStatus;          // that stage's own word for its state
    // TaskState's build-and-test summary. A different question from the
    // objective - a run can settle every criterion by running a command without
    // ever building, which reads as partial here and pass there.
    QString taskVerification;

    // --- Presentation -----------------------------------------------------
    QString presentation;   // none / pending / reasoning only / ...
    bool presentationFailed = false;
    int finalizationAttempts = 0;

    // True when finalization never ran because the work was not established
    // enough to describe. That is the guard doing its job, and it must not be
    // shown as the finalizer having failed.
    bool finalizationGatedOff = false;
    bool planComplete = false;
    bool planHasFailure = false;
    // No acceptance criteria were set: the run was asked a question, not given
    // a task, so there is nothing for "not verified" to be about.
    bool hadPlan = false;
    QStringList unsettledCriteria;
    QStringList disputedCriteria;

    // --- What to show -----------------------------------------------------
    bool completed = false;
    QString finalMessage;
    QString stopReason;
    QStringList unsupportedClaims;
    // The engine already reported this failure as it happened, so the reason
    // is in the transcript and must not be printed a second time.
    bool alreadyReported = false;
};

struct UiContext {
    int total = 0;
    int used = 0;
    int systemPrompt = 0;
    int skills = 0;
    int taskState = 0;
    int tools = 0;
    int conversation = 0;
    bool exact = false;
};

class AgentController : public QObject {
    Q_OBJECT

public:
    explicit AgentController(Application& application, QObject* parent = nullptr);
    ~AgentController() override;

    [[nodiscard]] bool busy() const { return busy_.load(); }

    void restoreTaskDisplay();

public slots:
    // Starts a run on the worker thread. Ignored while one is in flight.
    void send(const QString& message);
    // Cancels generation, the running command and the loop.
    void stop();

signals:
    void runStarted(const QString& userMessage);
    void promptProcessing();
    void reasoningDelta(const QString& text);
    void contentDelta(const QString& text);
    void assistantFinished();
    void toolStarted(const UiToolEvent& event);
    void toolProgress(const QString& note);
    void toolOutput(const QString& chunk, bool isStderr);
    void toolFinished(const UiToolEvent& event);
    void contextUpdated(const UiContext& context, const QString& note);
    void generationStats(const UiTurnStats& stats);
    void progressWarning(const QString& guidance);
    // The run is producing its final summary. `retry` marks the second,
    // stricter attempt.
    void finalizationStarted(const QString& note, bool retry);
    void taskStateUpdated(const QString& block);
    void verificationUpdated(const UiVerification& verification);
    void iterationFinished(int iteration);
    // One terminal signal carrying the whole outcome, so the window decides how
    // to present verified-work-without-a-summary rather than being handed a
    // single word that cannot express it.
    void runFinished(const localcode::gui::UiOutcome& outcome);
    // Emitted by the engine as a failure happens, for the live transcript.
    void runFailed(const QString& reason);
    void runCancelled();
    void busyChanged(bool busy);
    // Delivered after the terminal UI outcome, once this run can no longer
    // append history. A live RunFailed/RunCancelled event can arrive earlier.
    void workerFinished();

    // Emitted from the worker after the cancellable dialog request is queued.
    void confirmationRequested(const QString& title, const QString& detail, const QString& command,
                               const QString& reason);

private:
    void dispatch(const AgentEvent& event);

    Application& application_;
    CancellationSource cancel_;
    std::thread worker_;
    std::atomic<bool> busy_{false};
    // The engine emits RunFailed or RunCancelled at the point it happens. The
    // tail of send() emits one too, for the paths where the engine emits none -
    // and without this guard, the ones where it does emit printed the same
    // thing twice. Observed both ways in the live GUI session: a stop
    // explanation three times over, and "Cancelled" twice on every Esc.
    std::atomic<bool> sawTerminalEvent_{false};
};

}   // namespace localcode::gui

Q_DECLARE_METATYPE(localcode::gui::UiToolEvent)
Q_DECLARE_METATYPE(localcode::gui::UiContext)
Q_DECLARE_METATYPE(localcode::gui::UiOutcome)
