// ---------------------------------------------------------------------------
// The engine must outlive everything that borrows it.
//
// The GUI got this wrong and the cost was an access violation on every single
// exit - six sessions, six crash dumps, one fault bucket. `main` called
// `engine.shutdown()`, which does `agent_.reset()`, while the main window was
// still alive; the window's destructor then ran `~AgentController`, which
// unregisters its event sink through `application_.agent()`. That accessor
// dereferences the owning pointer without checking, so the unregistration
// locked a mutex belonging to a destroyed object:
//
//     msvcp140!Mtx_lock+0x13        rbx=0, mov edx,dword ptr [rbx]
//     localcode::Agent::set_event_sink+0x2e
//     localcode::gui::AgentController::~AgentController+0x75
//     localcode::gui::MainWindow::~MainWindow+0x3f
//     LocalCode!main+0x7b9
//
// The fix is ordering: the window gets its own scope so it is destroyed before
// shutdown() runs. Nothing links the GUI into this binary, so the only way to
// stop that ordering being undone is to read the entry point and check it.
// ---------------------------------------------------------------------------
#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "Application.h"

using namespace localcode;

namespace {

std::string read_source(const std::string& relative) {
    const auto path = std::filesystem::path{LOCALCODE_TEST_SOURCE_DIR} / relative;
    std::ifstream in(path, std::ios::binary);
    REQUIRE(in.good());
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

// Depth of nesting at `position`, counting braces from the start of the text.
// Crude, but the entry point has no braces inside string or character literals,
// and a test that needs a C++ parser to state a one-line rule is the wrong test.
int brace_depth_at(const std::string& text, std::size_t position) {
    int depth = 0;
    for (std::size_t i = 0; i < position && i < text.size(); ++i) {
        if (text[i] == '{') ++depth;
        else if (text[i] == '}') --depth;
    }
    return depth;
}

}  // namespace

TEST_CASE("the GUI destroys its window before shutting the engine down", "[shutdown]") {
    const auto source = read_source("src/gui/main.cpp");

    const auto window = source.find("MainWindow window(engine)");
    const auto shutdown = source.find("engine.shutdown()");
    REQUIRE(window != std::string::npos);
    REQUIRE(shutdown != std::string::npos);

    // shutdown() must come last in the text...
    REQUIRE(window < shutdown);

    // ...and, the part that actually matters, the window must live in a deeper
    // scope than the shutdown call, so that its destructor has already run by
    // the time the engine is torn down. Same depth means same scope, which is
    // the arrangement that crashed.
    const int window_depth = brace_depth_at(source, window);
    const int shutdown_depth = brace_depth_at(source, shutdown);
    INFO("window declared at brace depth " << window_depth << ", engine.shutdown() at "
                                           << shutdown_depth);
    REQUIRE(window_depth > shutdown_depth);
}

TEST_CASE("an Application reports when its subsystems are gone", "[shutdown]") {
    // The accessors hand back references into members that shutdown() destroys,
    // so callers need a way to ask. Without this the only signal that a
    // subsystem has gone is the access violation from using it.
    Application application;
    REQUIRE_FALSE(application.is_initialised());

    if (auto status = application.initialise(); status) {
        REQUIRE(application.is_initialised());
        application.shutdown();
        REQUIRE_FALSE(application.is_initialised());

        // Idempotent: a second shutdown must not try to destroy it all again.
        application.shutdown();
        REQUIRE_FALSE(application.is_initialised());
    }
}
