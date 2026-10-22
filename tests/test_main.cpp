// Test entry point. Defined explicitly rather than using Catch2WithMain so the
// logger can be quieted before any test runs - otherwise a run's output is
// buried under the engine's own log lines.
#include <catch2/catch_session.hpp>

#include "platform/Paths.h"
#include "support/TempDirectory.h"
#include "util/Log.h"

int main(int argc, char* argv[]) {
    // Application tests may relocate only their database. Their logs, caches
    // and runtime probes must still stay clear of a concurrently running GUI.
    localcode::test::TempDirectory runtime{"lc-core-runtime"};
    localcode::app_paths().set_root(runtime.path());
    localcode::Logger::instance().set_min_level(localcode::LogLevel::Error);
    const int result = Catch::Session().run(argc, argv);
    localcode::Logger::instance().configure({}, localcode::LogLevel::Error);
    return result;
}
