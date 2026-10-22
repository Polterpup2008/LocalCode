# ---------------------------------------------------------------------------
# The core test executable needs no Qt, display or model server. Optional GUI
# tests are registered separately with the platform plugin shipped by Qt.
# ---------------------------------------------------------------------------
add_executable(localcode_server_fixture server_fixture_main.cpp)
target_link_libraries(localcode_server_fixture PRIVATE LocalCode::warnings)

add_executable(localcode_tests
    test_main.cpp
    test_strings.cpp
    test_path_policy.cpp
    test_command_contract.cpp
    test_criterion_strength.cpp
    test_command_policy.cpp
    test_process.cpp
    test_cpu_allowance.cpp
    test_filesystem.cpp
    test_storage.cpp
    test_model_profile.cpp
    test_model_lifecycle.cpp
    test_server_recovery.cpp
    test_skills.cpp
    test_tool_call_parser.cpp
    test_tool_catalog.cpp
    test_task_state.cpp
    test_context_manager.cpp
    test_source_witnesses.cpp
    test_system_prompt.cpp
    test_git_parsing.cpp
    test_web.cpp
    test_http_transport.cpp
    test_tools.cpp
    test_progress_monitor.cpp
    test_agent_loop.cpp
    test_final_response_budget.cpp
    test_backend_flags.cpp
    test_cpu_placement.cpp
    test_generation_metrics.cpp
    test_verification.cpp
    test_evidence_interpreter.cpp
    test_diagnostics.cpp
    test_code_integrity.cpp
    test_shutdown_order.cpp
    test_outcome.cpp
    test_screen_fit.cpp
    test_vram_preflight.cpp
    test_repo_symbols.cpp
    test_repo_index.cpp
    test_repo_tools.cpp
    bench_repo_index.cpp
    test_foreign_toolchain.cpp
    test_output_expectation.cpp
    test_completion_gate.cpp
    test_finalization.cpp
    test_reference_check.cpp
    test_adversarial_verification.cpp
    support/MockBackend.cpp
    support/TempDirectory.cpp
)

target_include_directories(localcode_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")

target_link_libraries(localcode_tests
    PRIVATE LocalCode::core
            Catch2::Catch2
            LocalCode::warnings)

# Point the test binary at the repository's skills so the skill tests exercise
# the real bundled content rather than fixtures.
target_compile_definitions(localcode_tests PRIVATE
    LOCALCODE_TEST_SERVER_FIXTURE="$<TARGET_FILE:localcode_server_fixture>"
    LOCALCODE_TEST_SKILLS_DIR="${CMAKE_SOURCE_DIR}/skills"
    LOCALCODE_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data"
    # The GUI is not linked into the test binary, so a lifetime rule that lives
    # in its entry point can only be checked by reading the source.
    LOCALCODE_TEST_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
add_dependencies(localcode_tests localcode_server_fixture)

include(Catch)
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
    add_test(NAME localcode_http_transport
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/scripts/test-http-transport.py"
                --binary "$<TARGET_FILE:localcode_tests>")
    set_tests_properties(localcode_http_transport PROPERTIES TIMEOUT 180)
endif()
if(TARGET localcode_gui_tests)
    add_dependencies(localcode_gui_tests localcode_server_fixture)
    add_test(NAME localcode_gui_rendering COMMAND localcode_gui_tests)
    # Windows deployments include qwindows, but need not include qoffscreen.
    # Keep headless offscreen rendering for the other supported platforms.
    if(WIN32)
        set(localcode_test_qpa_platform windows)
    else()
        set(localcode_test_qpa_platform offscreen)
    endif()
    set_tests_properties(localcode_gui_rendering PROPERTIES
        # The complete suite starts many isolated native processes. Keep a
        # bounded allowance for loaded CI/developer hosts; individual waits
        # and owned-process termination are still enforced by each test.
        ENVIRONMENT "QT_QPA_PLATFORM=${localcode_test_qpa_platform};QT_PLUGIN_PATH=${Qt6_DIR}/../../../plugins" TIMEOUT 300)
endif()
catch_discover_tests(localcode_tests
    DISCOVERY_MODE PRE_TEST
    PROPERTIES TIMEOUT 300)
