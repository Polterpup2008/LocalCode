# ---------------------------------------------------------------------------
# LocalCode - a native C++ local coding agent for GGUF models
# ---------------------------------------------------------------------------
cmake_minimum_required(VERSION 3.24)

project(LocalCode
    VERSION 1.2.1
    DESCRIPTION "Native C++ local coding agent for GGUF models (llama.cpp / ik_llama.cpp)"
    HOMEPAGE_URL "https://github.com/Polterpup2008/LocalCode"
    LANGUAGES C CXX)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

# --- Options ---------------------------------------------------------------
option(LOCALCODE_BUILD_GUI       "Build the Qt 6 desktop application" ON)
option(LOCALCODE_BUILD_TESTS     "Build the unit / integration test suite" ON)
option(LOCALCODE_BUILD_PERFORMANCE "Build observational performance benchmarks" OFF)
option(LOCALCODE_WITH_CURL       "Use libcurl for HTTP instead of the platform client" OFF)
option(LOCALCODE_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)

# --- Global settings -------------------------------------------------------
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 11)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

if(MSVC)
    add_compile_options(/utf-8 /permissive- /Zc:__cplusplus /Zc:preprocessor /EHsc /MP)
    add_compile_definitions(
        UNICODE _UNICODE
        NOMINMAX
        WIN32_LEAN_AND_MEAN
        _CRT_SECURE_NO_WARNINGS
        _SILENCE_ALL_CXX23_DEPRECATION_WARNINGS)
endif()

include(CompilerWarnings)
include(Dependencies)

# Bundle the compiler's redistributable runtime beside every native executable.
# A development machine can hide missing VCRUNTIME DLLs via its system install.
if(WIN32 AND MSVC)
    set(CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS_SKIP TRUE)
    include(InstallRequiredSystemLibraries)
    if(NOT CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS)
        message(FATAL_ERROR "MSVC redistributable runtime files were not found")
    endif()
    install(PROGRAMS ${CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS} DESTINATION bin)
    add_custom_target(localcode_compiler_runtime ALL
        COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS} "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
        COMMAND_EXPAND_LISTS
        COMMENT "Staging compiler runtime dependencies")
endif()

# --- The skill library ------------------------------------------------------
# The skills are engine data, not a GUI asset: the agent loop reads them, and the
# command-line tool runs that same loop. They used to be copied by a POST_BUILD
# step on the GUI target, which meant that building only the CLI - or configuring
# with LOCALCODE_BUILD_GUI=OFF - produced a binary that silently ran with no
# skills at all. This target is depended on by everything that needs them, and
# copies on every build so a deleted or edited skill is restored.
set(LOCALCODE_SKILLS_STAGE "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/skills")
add_custom_target(localcode_skills ALL
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_SOURCE_DIR}/skills" "${LOCALCODE_SKILLS_STAGE}"
    COMMENT "Staging the skill library next to the executables"
    VERBATIM)

install(DIRECTORY "${CMAKE_SOURCE_DIR}/skills/" DESTINATION share/localcode/skills)

# Developer support utilities are opt-in and excluded from normal distributions.
option(LOCALCODE_INSTALL_DEVELOPER_TOOLS "Install runtime builders and benchmark scripts" OFF)
if(LOCALCODE_INSTALL_DEVELOPER_TOOLS)
# Optional source builder: users retain model, runtime and backend choices.
install(FILES scripts/build-llama-checkpoint-candidate.py docs/BUILD_CHECKPOINT_CANDIDATE.md
        docs/performance/proofs/checkpoint-sharing-b10655.patch
        docs/performance/proofs/checkpoint-cpu-dispatch-b10655.patch
        docs/performance/proofs/checkpoint-server-lifecycle-b10655.patch
        DESTINATION share/localcode/tools/checkpoint-builder)

if(WIN32)
    install(FILES scripts/benchmark-cpu.ps1 docs/CPU_MEASUREMENT.md
            DESTINATION share/localcode/tools)
endif()
endif()

add_subdirectory(src/core)
add_subdirectory(src/cli)

if(LOCALCODE_BUILD_PERFORMANCE)
    add_executable(localcode_stream_performance tests/stream_performance_main.cpp)
    target_link_libraries(localcode_stream_performance PRIVATE LocalCode::core LocalCode::warnings)
    add_executable(localcode_core_performance tests/core_performance_main.cpp)
    target_link_libraries(localcode_core_performance PRIVATE LocalCode::core LocalCode::warnings)
    add_executable(localcode_model_performance tests/model_performance_main.cpp)
    target_link_libraries(localcode_model_performance PRIVATE LocalCode::core LocalCode::warnings)
    add_executable(localcode_model_recovery tests/model_recovery_main.cpp)
    target_link_libraries(localcode_model_recovery PRIVATE LocalCode::core LocalCode::warnings)
endif()

if(LOCALCODE_BUILD_GUI)
    add_subdirectory(src/gui)
endif()

if(LOCALCODE_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

include(Packaging)

# --- Summary ---------------------------------------------------------------
message(STATUS "")
message(STATUS "LocalCode ${PROJECT_VERSION}")
message(STATUS "  C++ standard ......... ${CMAKE_CXX_STANDARD}")
message(STATUS "  Build type ........... ${CMAKE_BUILD_TYPE}")
message(STATUS "  Qt 6 GUI ............. ${LOCALCODE_BUILD_GUI}")
message(STATUS "  Tests ................ ${LOCALCODE_BUILD_TESTS}")
message(STATUS "  HTTP backend ......... ${LOCALCODE_HTTP_BACKEND}")
message(STATUS "")
