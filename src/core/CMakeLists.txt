# ---------------------------------------------------------------------------
# localcode_core - the portable heart of the application.
#
# This target must never link Qt. Everything the agent does - policy, tools,
# process execution, inference, storage - lives here so it can be unit tested
# headlessly and reused by a future CLI or server front end.
# ---------------------------------------------------------------------------
set(LOCALCODE_CORE_SOURCES
    util/Result.cpp
    util/Strings.cpp
    util/Log.cpp
    util/Cancellation.cpp
    util/Json.cpp
    platform/Paths.cpp
    platform/FileSystem.cpp
    platform/Process.cpp
    platform/Environment.cpp
    platform/CpuPlacement.cpp
    security/PathPolicy.cpp
    security/CommandPolicy.cpp
    security/TaskPolicy.cpp
    storage/Database.cpp
    storage/Repositories.cpp
    model/GgufMetadata.cpp
    model/ModelProfile.cpp
    inference/HttpClient.cpp
    inference/ChatTypes.cpp
    inference/OpenAiCompatBackend.cpp
    inference/RequestTrace.cpp
    inference/ServerManager.cpp
    inference/BackendManager.cpp
    inference/RuntimeDiagnostics.cpp
    agent/Presentation.cpp
    agent/ToolCatalog.cpp
    agent/Outcome.cpp
    platform/ScreenFit.cpp
    repo/RepoTypes.cpp
    repo/Hashing.cpp
    repo/RepoStore.cpp
    repo/RepoScanner.cpp
    repo/RepoIndex.cpp
    repo/RepoQuery.cpp
    tools/RepoTools.cpp
    repo/Languages.cpp
    repo/SymbolIndexer.cpp
    platform/GpuProbe.cpp
    inference/VramPreflight.cpp
    agent/TaskState.cpp
    agent/Verification.cpp
    agent/EvidenceInterpreter.cpp
    agent/CriterionStrength.cpp
    agent/ReferenceCheck.cpp
    agent/ContextManager.cpp
    agent/SourceWitnesses.cpp
    agent/ProgressMonitor.cpp
    agent/ToolCallParser.cpp
    agent/SystemPrompt.cpp
    agent/Agent.cpp
    agent/Conversation.cpp
    agent/AgentConversation.cpp
    tools/Tool.cpp
    tools/ToolRegistry.cpp
    tools/FileTools.cpp
    tools/SearchTools.cpp
    tools/EditTools.cpp
    tools/CodeIntegrity.cpp
    tools/BuildDirectory.cpp
    tools/CommandInspection.cpp
    tools/CommandRequest.cpp
    tools/PathOrigin.cpp
    tools/Diagnostics.cpp
    tools/ForeignToolchain.cpp
    tools/OutputExpectation.cpp
    tools/ProcessTools.cpp
    tools/GitTools.cpp
    tools/WebTools.cpp
    tools/EnvironmentTools.cpp
    tools/SkillTools.cpp
    tools/VerificationTools.cpp
    web/WebService.cpp
    git/GitService.cpp
    workspace/Workspace.cpp
    Application.cpp
    skills/Skill.cpp
    skills/SkillRegistry.cpp
)

if(LOCALCODE_HTTP_BACKEND STREQUAL "libcurl")
    list(APPEND LOCALCODE_CORE_SOURCES inference/CurlHttpClient.cpp)
elseif(WIN32)
    list(APPEND LOCALCODE_CORE_SOURCES inference/WinHttpClient.cpp)
else()
    list(APPEND LOCALCODE_CORE_SOURCES inference/UnavailableHttpClient.cpp)
endif()

add_library(localcode_core STATIC ${LOCALCODE_CORE_SOURCES})
add_library(LocalCode::core ALIAS localcode_core)

target_include_directories(localcode_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")

target_link_libraries(localcode_core
    PUBLIC  nlohmann_json::nlohmann_json
            LocalCode::sqlite3
    PRIVATE LocalCode::warnings)

target_compile_definitions(localcode_core PUBLIC
    LOCALCODE_VERSION="${PROJECT_VERSION}")

if(WIN32)
    target_link_libraries(localcode_core PUBLIC winhttp ws2_32 shlwapi shell32 ole32 advapi32 dxgi)
endif()
if(LOCALCODE_HTTP_BACKEND STREQUAL "libcurl")
    target_link_libraries(localcode_core PUBLIC CURL::libcurl)
    target_compile_definitions(localcode_core PUBLIC LOCALCODE_HTTP_CURL)
elseif(WIN32)
    target_compile_definitions(localcode_core PUBLIC LOCALCODE_HTTP_WINHTTP)
endif()

if(UNIX)
    find_package(Threads REQUIRED)
    target_link_libraries(localcode_core PUBLIC Threads::Threads)
endif()
