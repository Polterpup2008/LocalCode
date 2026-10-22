# ---------------------------------------------------------------------------
# The Qt 6 desktop application.
#
# Everything here is presentation: it talks to localcode_core and holds no
# engine logic of its own. The agent runs on a worker thread and reaches the UI
# only through queued signals, so no model call, build, or filesystem scan can
# block the interface.
# ---------------------------------------------------------------------------
# Concurrent belongs here because MainWindow runs the workspace scan and the
# model load through QtConcurrent::run. It was missing, and the omission was
# invisible for as long as nobody had a Qt to build against.
find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui Widgets Concurrent)

qt_standard_project_setup()

# ZIP extraction for explicit user-selected runtime installation. Kept in the
# desktop host; the inference engines and headless core do not depend on it.
include(FetchContent)
FetchContent_Declare(localcode_miniz
    URL https://github.com/richgel999/miniz/archive/refs/tags/3.1.2.zip
    URL_HASH SHA256=e7a0334927a069dc5aeb69616d371542a4376e1569290d7102b88c6f3992d6cb
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SOURCE_SUBDIR localcode-no-upstream-cmake)
FetchContent_MakeAvailable(localcode_miniz)
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/miniz-headers")
# Keep unchanged headers' timestamps so reconfiguration does not rebuild consumers.
file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/miniz-headers/miniz_export.h"
    CONTENT "#pragma once\n#define MINIZ_EXPORT\n" @ONLY NEWLINE_STYLE LF)
add_library(localcode_resource_zip STATIC
    "${localcode_miniz_SOURCE_DIR}/miniz.c"
    "${localcode_miniz_SOURCE_DIR}/miniz_zip.c"
    "${localcode_miniz_SOURCE_DIR}/miniz_tinfl.c"
    "${localcode_miniz_SOURCE_DIR}/miniz_tdef.c")
target_include_directories(localcode_resource_zip SYSTEM PUBLIC
    "${localcode_miniz_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/miniz-headers")
target_compile_definitions(localcode_resource_zip PUBLIC MINIZ_NO_ZLIB_COMPATIBLE_NAMES)
if(MSVC)
    target_compile_options(localcode_resource_zip PRIVATE /W0)
endif()
install(FILES "${localcode_miniz_SOURCE_DIR}/LICENSE" DESTINATION share/localcode/licenses RENAME miniz-LICENSE.txt)

set(LOCALCODE_GUI_SOURCES
    main.cpp
    MainWindow.cpp
    Theme.cpp
    ScreenGuard.cpp
    AgentController.cpp
    plugins/DiscordPlugin.cpp
    setup/ResourceInstaller.cpp
    setup/RuntimeMemory.cpp
    widgets/PluginsPanel.cpp

    widgets/ChatView.cpp
    widgets/MarkdownRenderer.cpp
    widgets/PromptComposer.cpp
    widgets/ProjectExplorer.cpp
    widgets/ActivityTimeline.cpp
    widgets/TaskPanel.cpp
    widgets/VerificationPanel.cpp
    widgets/SourceEditor.cpp
    widgets/SyntaxHighlighter.cpp
    widgets/DiffView.cpp
    widgets/OutputPanel.cpp
    widgets/SkillsPanel.cpp
    widgets/RepositoryPanel.cpp
    widgets/ModelPanel.cpp
    widgets/StatusStrip.cpp

    dialogs/SettingsDialog.cpp
    dialogs/ModelImportDialog.cpp
    dialogs/InferenceTuningDialog.cpp
    dialogs/InferenceBenchmarkDialog.cpp
    dialogs/ResourceSetupDialog.cpp
    dialogs/ConfirmationDialog.cpp
)

qt_add_executable(localcode WIN32 ${LOCALCODE_GUI_SOURCES})

if(WIN32)
    enable_language(RC)
    set(LOCALCODE_APP_ICON "${CMAKE_SOURCE_DIR}/packaging/windows/localcode.ico")
    configure_file("${CMAKE_SOURCE_DIR}/packaging/windows/LocalCode.rc.in"
                   "${CMAKE_CURRENT_BINARY_DIR}/LocalCode.rc" @ONLY)
    target_sources(localcode PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/LocalCode.rc")
    qt_add_resources(localcode "application_icon"
        PREFIX "/icons" BASE "${CMAKE_SOURCE_DIR}/packaging/windows"
        FILES "${LOCALCODE_APP_ICON}")
endif()

target_include_directories(localcode PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")

target_link_libraries(localcode
    PRIVATE LocalCode::core
            Qt6::Core
            Qt6::Gui
            Qt6::Widgets
            Qt6::Concurrent
            localcode_resource_zip
            LocalCode::warnings)

set_target_properties(localcode PROPERTIES
    OUTPUT_NAME "LocalCode"
    WIN32_EXECUTABLE TRUE
    MACOSX_BUNDLE FALSE)

# On Windows, gather the Qt runtime beside the executable so it can be run and
# zipped without a Qt installation on the target machine.
if(WIN32)
    find_program(LOCALCODE_WINDEPLOYQT windeployqt HINTS "${Qt6_DIR}/../../../bin")
    if(LOCALCODE_WINDEPLOYQT)
        add_custom_command(TARGET localcode POST_BUILD
            COMMAND "${CMAKE_COMMAND}"
                    "-DLOCALCODE_DEPLOY_TOOL=${LOCALCODE_WINDEPLOYQT}"
                    "-DLOCALCODE_DEPLOY_EXECUTABLE=$<TARGET_FILE:localcode>"
                    -P "${CMAKE_SOURCE_DIR}/cmake/DeployQtRuntime.cmake"
            COMMENT "Deploying the Qt runtime" VERBATIM)
    endif()
endif()

add_dependencies(localcode localcode_skills)
if(TARGET localcode_compiler_runtime)
    add_dependencies(localcode localcode_compiler_runtime)
endif()

if(LOCALCODE_BUILD_TESTS)
    set(LOCALCODE_GUI_TEST_SOURCES ${LOCALCODE_GUI_SOURCES})
    list(REMOVE_ITEM LOCALCODE_GUI_TEST_SOURCES main.cpp)
    qt_add_executable(localcode_gui_tests
        ${LOCALCODE_GUI_TEST_SOURCES}
        "${CMAKE_SOURCE_DIR}/tests/test_gui.cpp"
        "${CMAKE_SOURCE_DIR}/tests/test_gui_projectless.cpp"
        "${CMAKE_SOURCE_DIR}/tests/test_gui_runtime_choice.cpp"
        "${CMAKE_SOURCE_DIR}/tests/test_gui_benchmark.cpp"
        "${CMAKE_SOURCE_DIR}/tests/test_plugin_status.cpp"
        "${CMAKE_SOURCE_DIR}/tests/test_setup.cpp"
        "${CMAKE_SOURCE_DIR}/tests/test_runtime_memory.cpp"
        "${CMAKE_SOURCE_DIR}/tests/support/MockBackend.cpp"
        "${CMAKE_SOURCE_DIR}/tests/support/TempDirectory.cpp")
    target_include_directories(localcode_gui_tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_SOURCE_DIR}/tests")
    target_link_libraries(localcode_gui_tests PRIVATE LocalCode::core Catch2::Catch2
        Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Concurrent localcode_resource_zip LocalCode::warnings)
    add_dependencies(localcode_gui_tests localcode_skills)
    if(TARGET localcode_compiler_runtime)
        add_dependencies(localcode_gui_tests localcode_compiler_runtime)
    endif()
    add_executable(localcode_benchmark_fixture "${CMAKE_SOURCE_DIR}/tests/benchmark_fixture.cpp")
    add_dependencies(localcode_gui_tests localcode_benchmark_fixture)
    add_executable(localcode_benchmark_utf8_fixture
        "${CMAKE_SOURCE_DIR}/tests/benchmark_fixture.cpp"
        "${CMAKE_SOURCE_DIR}/tools/llama-bench-utf8/main.cpp")
    target_compile_definitions(localcode_benchmark_utf8_fixture PRIVATE LOCALCODE_BENCH_UTF8_FIXTURE)
    add_dependencies(localcode_gui_tests localcode_benchmark_utf8_fixture)
    add_executable(localcode_plugin_status_fixture "${CMAKE_SOURCE_DIR}/tests/plugin_status_fixture.cpp")
    add_dependencies(localcode_gui_tests localcode_plugin_status_fixture)
    if(WIN32 AND LOCALCODE_WINDEPLOYQT)
        # The test target is also runnable when built without the application.
        add_custom_command(TARGET localcode_gui_tests POST_BUILD
            COMMAND "${CMAKE_COMMAND}"
                    "-DLOCALCODE_DEPLOY_TOOL=${LOCALCODE_WINDEPLOYQT}"
                    "-DLOCALCODE_DEPLOY_EXECUTABLE=$<TARGET_FILE:localcode_gui_tests>"
                    -P "${CMAKE_SOURCE_DIR}/cmake/DeployQtRuntime.cmake"
            COMMENT "Deploying the Qt test runtime" VERBATIM)
    endif()
endif()

install(TARGETS localcode RUNTIME DESTINATION bin)
if(WIN32)
    qt_generate_deploy_app_script(TARGET localcode OUTPUT_SCRIPT localcode_deploy
        NO_TRANSLATIONS NO_COMPILER_RUNTIME
        DEPLOY_TOOL_OPTIONS --no-system-d3d-compiler --no-opengl-sw)
    install(SCRIPT ${localcode_deploy})
endif()

# Observational benchmarks use the actual production widgets and core APIs.
# They record raw samples; machine-dependent timings are not a CI pass gate.
if(LOCALCODE_BUILD_PERFORMANCE)
    qt_add_executable(localcode_editor_performance
        "${CMAKE_SOURCE_DIR}/tests/editor_performance_main.cpp"
        Theme.cpp widgets/SourceEditor.cpp widgets/SyntaxHighlighter.cpp)
    target_include_directories(localcode_editor_performance PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(localcode_editor_performance PRIVATE LocalCode::core
        Qt6::Core Qt6::Gui Qt6::Widgets LocalCode::warnings)
    set(LOCALCODE_APP_PERFORMANCE_SOURCES ${LOCALCODE_GUI_SOURCES})
    list(REMOVE_ITEM LOCALCODE_APP_PERFORMANCE_SOURCES main.cpp)
    qt_add_executable(localcode_app_performance
        ${LOCALCODE_APP_PERFORMANCE_SOURCES}
        "${CMAKE_SOURCE_DIR}/tests/app_performance_main.cpp")
    target_include_directories(localcode_app_performance PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(localcode_app_performance PRIVATE LocalCode::core
        Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Concurrent localcode_resource_zip LocalCode::warnings)
    add_dependencies(localcode_app_performance localcode_skills)
    qt_add_executable(localcode_performance
        "${CMAKE_SOURCE_DIR}/tests/performance_main.cpp"
        Theme.cpp widgets/ChatView.cpp widgets/MarkdownRenderer.cpp widgets/OutputPanel.cpp)
    target_include_directories(localcode_performance PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(localcode_performance PRIVATE LocalCode::core
        Qt6::Core Qt6::Gui Qt6::Widgets LocalCode::warnings)
    if(WIN32)
        target_link_libraries(localcode_performance PRIVATE psapi)
    endif()
endif()
