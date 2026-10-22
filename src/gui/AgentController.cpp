#include "AgentController.h"

#include "agent/Outcome.h"

#include <QApplication>
#include <QMetaObject>
#include <QThread>
#include <QTimer>

#include <future>

#include "dialogs/ConfirmationDialog.h"
#include "platform/PathEncoding.h"
#include "util/Log.h"
#include "util/Strings.h"

namespace localcode::gui {
namespace {

QString toQt(const std::string& text) {
    return QString::fromStdString(text);
}

}   // namespace

AgentController::AgentController(Application& application, QObject* parent)
    : QObject(parent), application_(application) {
    qRegisterMetaType<UiToolEvent>("UiToolEvent");
    qRegisterMetaType<UiContext>("UiContext");
    qRegisterMetaType<UiOutcome>("UiOutcome");

    application_.agent().set_event_sink([this](const AgentEvent& event) { dispatch(event); });

    // Queue the dialog, but let cancellation release the worker even when the
    // GUI is shutting down and no longer processes queued calls. A blocking
    // connection deadlocks when destruction joins that worker on the GUI thread.
    application_.agent().set_confirmation_handler([this](const ConfirmationRequest& request) {
        const auto token = cancel_.token();
        auto answer = std::make_shared<std::promise<bool>>();
        auto result = answer->get_future();
        QMetaObject::invokeMethod(
            this,
            [request, token, answer] {
                if (token.cancelled()) {
                    answer->set_value(false);
                    return;
                }
                ConfirmationDialog dialog(toQt(request.title), toQt(request.detail),
                                          toQt(request.command), toQt(request.reason),
                                          QApplication::activeWindow());
                QTimer timer;
                QObject::connect(&timer, &QTimer::timeout, &dialog, [&] {
                    if (token.cancelled()) dialog.reject();
                });
                timer.start(50);
                answer->set_value(dialog.exec() == QDialog::Accepted && !token.cancelled());
            },
            Qt::QueuedConnection);
        emit confirmationRequested(toQt(request.title), toQt(request.detail), toQt(request.command),
                                   toQt(request.reason));
        while (result.wait_for(std::chrono::milliseconds(50)) != std::future_status::ready) {
            if (token.cancelled()) return false;
        }
        return result.get();
    });
}

AgentController::~AgentController() {
    cancel_.cancel();
    if (worker_.joinable()) worker_.join();
    application_.agent().set_event_sink({});
    application_.agent().set_confirmation_handler({});
}

void AgentController::dispatch(const AgentEvent& event) {
    // Called from the agent thread. Every emission below is a queued connection
    // because the receivers live on the GUI thread.
    switch (event.type) {
        case AgentEventType::RunStarted: emit runStarted(toQt(event.text)); break;
        case AgentEventType::PromptProcessing: emit promptProcessing(); break;
        case AgentEventType::ReasoningDelta: emit reasoningDelta(toQt(event.text)); break;
        case AgentEventType::ContentDelta: emit contentDelta(toQt(event.text)); break;
        case AgentEventType::AssistantMessage: emit assistantFinished(); break;
        case AgentEventType::ToolCallStarted: {
            UiToolEvent ui;
            ui.name = toQt(event.tool_call.name);
            ui.arguments = toQt(event.tool_call.arguments_json);
            emit toolStarted(ui);
            break;
        }
        case AgentEventType::GenerationStats: {
            UiTurnStats s;
            s.usageFromServer = event.stats.usage_from_server;
            s.promptTokens = event.stats.prompt_tokens;
            s.cachedPromptTokens = event.stats.cached_prompt_tokens;
            s.completionTokens = event.stats.completion_tokens;

            s.timingsFromServer = event.stats.timings_from_server;
            s.promptTokensEvaluated = event.stats.prompt_tokens_evaluated;
            s.promptMs = event.stats.prompt_ms;
            s.promptTokensPerSecond = event.stats.prompt_tokens_per_second;
            s.decodeMs = event.stats.decode_ms;
            s.decodeTokensPerSecond = event.stats.decode_tokens_per_second;

            s.totalRequestMs = event.stats.total_request_ms;
            s.timeToFirstTokenMs = event.stats.time_to_first_token_ms;

            s.reasoningCharacters = event.stats.reasoning_characters;
            s.estimatedReasoningTokens = event.stats.estimated_reasoning_tokens;
            s.observedReasoningStreamMs = event.stats.observed_reasoning_stream_ms;
            s.reasoningDeltaCount = event.stats.reasoning_delta_count;
            emit generationStats(s);
            break;
        }
        case AgentEventType::ToolProgress: emit toolProgress(toQt(event.text)); break;
        case AgentEventType::ToolOutput: emit toolOutput(toQt(event.text), event.is_stderr); break;
        case AgentEventType::ToolCompleted: {
            UiToolEvent ui;
            ui.name = toQt(event.tool_call.name);
            ui.arguments = toQt(event.tool_call.arguments_json);
            ui.summary = toQt(event.tool_result.summary);
            ui.detail =
                toQt(event.tool_result.ok ? event.tool_result.content : event.tool_result.error);
            ui.ok = event.tool_result.ok;
            if (json_bool(event.tool_result.structured, "expectation_checked") &&
                !json_bool(event.tool_result.structured, "expectation_satisfied"))
                ui.ok = false;
            if (event.tool_result.structured.is_object() &&
                event.tool_result.structured.contains("exit_code") &&
                event.tool_result.structured["exit_code"].is_number_integer()) {
                ui.exitCode = event.tool_result.structured["exit_code"].get<int>();
                ui.ok = ui.ok && ui.exitCode == 0;
            }
            ui.durationMs = event.tool_result.duration_ms;
            ui.logPath = toQt(path_to_utf8(event.tool_result.full_log_path));
            emit toolFinished(ui);
            break;
        }
        case AgentEventType::ContextUpdated: {
            UiContext ui;
            ui.total = event.context.total;
            ui.used = event.context.used;
            ui.systemPrompt = event.context.system_prompt;
            ui.skills = event.context.skills;
            ui.taskState = event.context.task_state;
            ui.tools = event.context.tool_definitions;
            ui.conversation = event.context.conversation;
            ui.exact = event.context.exact;
            emit contextUpdated(ui, toQt(event.text));
            break;
        }
        case AgentEventType::FinalizationStarted:
            // `retry` is read off the engine's own wording for the second
            // attempt rather than counted here, so the two stay in step.
            emit finalizationStarted(toQt(event.text),
                                     event.text.find("once more") != std::string::npos);
            break;
        case AgentEventType::ProgressWarning: emit progressWarning(toQt(event.text)); break;
        case AgentEventType::TaskStateUpdated: emit taskStateUpdated(toQt(event.text)); break;
        case AgentEventType::VerificationUpdated: {
            if (!event.ledger) break;
            const auto& ledger = *event.ledger;
            UiVerification ui;
            ui.revision = ledger.revision();
            for (const auto stage : {Stage::Configure, Stage::Build, Stage::Test, Stage::Execution,
                                     Stage::Behaviour, Stage::Objective}) {
                const auto raw = ledger.raw(stage);
                UiStage entry;
                entry.name = toQt(std::string{to_string(stage)});
                // The resolved status, not the stored one: a pass established
                // against an older revision must read as stale here too.
                entry.status = toQt(std::string{to_string(ledger.status(stage))});
                entry.detail = toQt(raw.detail);
                entry.evidenceId = toQt(raw.evidence_id);
                entry.establishedAtRevision = raw.established_at_revision;
                ui.stages.push_back(std::move(entry));
            }
            for (const auto& check : ledger.acceptance_plan()) {
                UiAcceptanceCheck entry;
                entry.description = toQt(check.description);
                entry.settled = check.settled;
                entry.satisfied = check.satisfied;
                entry.evidenceId = toQt(check.evidence_id);
                ui.plan.push_back(std::move(entry));
            }
            const auto& evidence = ledger.evidence();
            const std::size_t start = evidence.size() > 8 ? evidence.size() - 8 : 0;
            for (std::size_t i = start; i < evidence.size(); ++i) {
                ui.recentEvidence.push_back(toQt(evidence[i].describe()));
            }
            emit verificationUpdated(ui);
            break;
        }
        case AgentEventType::IterationFinished: emit iterationFinished(event.iteration); break;
        case AgentEventType::RunCancelled:
            sawTerminalEvent_.store(true);
            emit runCancelled();
            break;
        case AgentEventType::RunFailed:
            sawTerminalEvent_.store(true);
            emit runFailed(toQt(event.text));
            break;
        case AgentEventType::RunFinished:
            // The summary carries the verification wording; the finished signal
            // is emitted from send() once run() returns so it can include it.
            break;
    }
}

void AgentController::send(const QString& message) {
    if (busy_.exchange(true)) return;
    sawTerminalEvent_.store(false);
    emit busyChanged(true);

    // A previous thread may have finished but not been joined yet.
    if (worker_.joinable()) worker_.join();
    cancel_.reset();

    const std::string text = message.toStdString();
    worker_ = std::thread([this, text] {
        AgentRunSummary summary;
        try {
            summary = application_.agent().run(text, cancel_.token());
        } catch (const std::exception& e) {
            // The engine reports failure as data; an exception here would be a
            // bug, but it must not take the application down.
            log_error(LogChannel::Agent, "unhandled exception in the agent loop",
                      {{"what", e.what()}});
            summary.stop_reason = std::string{"internal error: "} + e.what();
        } catch (...) {
            log_error(LogChannel::Agent, "unknown exception in the agent loop");
            summary.stop_reason = "internal error: unknown exception in the agent loop";
        }

        // The decision about what this run should be called is made once, in
        // the engine, and both front ends read it from there. This converts;
        // it does not judge.
        const auto report = describe_outcome(summary);

        UiOutcome outcome;
        outcome.kind = QString::fromStdString(std::string{to_string(report.kind)});
        outcome.headline = QString::fromStdString(report.headline);
        outcome.objectiveVerified = report.objective_verified;
        outcome.objectiveStatus =
            QString::fromStdString(std::string{to_string(summary.ledger.status(Stage::Objective))});
        outcome.taskVerification =
            QString::fromStdString(std::string{to_string(summary.verification)});

        outcome.presentation = QString::fromStdString(std::string{to_string(report.presentation)});
        outcome.presentationFailed = is_presentation_failure(report.presentation);
        outcome.finalizationAttempts = report.finalization_attempts;
        outcome.finalizationGatedOff = report.finalization_gated_off;
        outcome.planComplete = report.plan_complete;
        outcome.planHasFailure = report.plan_has_failure;
        outcome.hadPlan = report.had_plan;
        for (const auto& item : report.unsettled) {
            outcome.unsettledCriteria << QString::fromStdString(item);
        }
        for (const auto& item : report.disputed) {
            outcome.disputedCriteria << QString::fromStdString(item);
        }

        outcome.completed = summary.completed;
        outcome.finalMessage = QString::fromStdString(summary.final_message);
        outcome.stopReason = QString::fromStdString(summary.stop_reason);
        for (const auto& claim : summary.unsupported_claims) {
            outcome.unsupportedClaims << QString::fromStdString(claim);
        }
        outcome.alreadyReported = sawTerminalEvent_.load();

        QMetaObject::invokeMethod(
            this,
            [this, outcome, cancelled = summary.cancelled] {
                busy_.store(false);
                emit busyChanged(false);
                if (cancelled) {
                    // The engine emits RunCancelled where it happens; this covers
                    // the paths where it does not.
                    if (!outcome.alreadyReported) emit runCancelled();
                    emit workerFinished();
                    return;
                }
                // Everything else is an outcome, including the one that used to be
                // reported as plain failure: verified engineering with no summary
                // to show for it.
                emit runFinished(outcome);
                emit workerFinished();
            },
            Qt::QueuedConnection);
    });
}

void AgentController::stop() {
    if (!busy_.load()) return;
    cancel_.cancel();
}

void AgentController::restoreTaskDisplay() {
    emit taskStateUpdated(toQt(application_.agent().task_state().to_prompt_block()));
    AgentEvent event;
    event.type = AgentEventType::VerificationUpdated;
    event.ledger = std::make_shared<VerificationLedger>(application_.agent().verification_ledger());
    dispatch(event);
}

}   // namespace localcode::gui
