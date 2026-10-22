# ---------------------------------------------------------------------------
# Third-party dependencies.
#
# LocalCode keeps its dependency tree deliberately small:
#   * nlohmann/json  - JSON for tool schemas, the OpenAI-compatible wire format
#                      and on-disk configuration.
#   * SQLite         - embedded persistence (official amalgamation, one .c file).
#   * Catch2         - unit tests only.
#   * Qt 6           - desktop GUI only; the core library never links Qt.
#
# Everything is pulled with FetchContent and pinned by version + hash. Set
# FETCHCONTENT_SOURCE_DIR_<NAME> to build fully offline from a local copy.
# ---------------------------------------------------------------------------
include(FetchContent)
set(FETCHCONTENT_QUIET OFF)

# --- nlohmann/json ---------------------------------------------------------
find_package(nlohmann_json 3.11 QUIET)
if(NOT nlohmann_json_FOUND)
    FetchContent_Declare(nlohmann_json
        URL      https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
        URL_HASH SHA256=d6c65aca6b1ed68e7a182f4757257b107ae403032760ed6ef121c9d55e81757d
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    set(JSON_BuildTests OFF CACHE INTERNAL "")
    set(JSON_Install OFF CACHE INTERNAL "")
    FetchContent_MakeAvailable(nlohmann_json)
endif()

# --- SQLite (official amalgamation) ----------------------------------------
FetchContent_Declare(sqlite3_amalgamation
    URL      https://sqlite.org/2024/sqlite-amalgamation-3460100.zip
    URL_HASH SHA256=77823cb110929c2bcb0f5d48e4833b5c59a8a6e40cdea3936b99e199dbbe5784
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(sqlite3_amalgamation)

add_library(sqlite3 STATIC "${sqlite3_amalgamation_SOURCE_DIR}/sqlite3.c")
target_include_directories(sqlite3 SYSTEM PUBLIC "${sqlite3_amalgamation_SOURCE_DIR}")
target_compile_definitions(sqlite3 PUBLIC
    SQLITE_THREADSAFE=1
    SQLITE_ENABLE_FTS5
    SQLITE_ENABLE_JSON1
    SQLITE_DQS=0
    SQLITE_OMIT_DEPRECATED
    SQLITE_DEFAULT_FOREIGN_KEYS=1)
if(MSVC)
    target_compile_options(sqlite3 PRIVATE /W0)
else()
    target_compile_options(sqlite3 PRIVATE -w)
    find_package(Threads REQUIRED)
    target_link_libraries(sqlite3 PUBLIC Threads::Threads ${CMAKE_DL_LIBS})
endif()
add_library(LocalCode::sqlite3 ALIAS sqlite3)

# --- HTTP backend selection ------------------------------------------------
# Windows uses WinHTTP (ships with the OS, TLS included, no extra dependency).
# Elsewhere we use libcurl when available. Without either, the web tools report
# an explicit "no HTTP transport" error instead of silently pretending to work.
if(LOCALCODE_WITH_CURL)
    find_package(CURL 7.66 REQUIRED)
    set(LOCALCODE_HTTP_BACKEND "libcurl" CACHE INTERNAL "")
elseif(WIN32)
    set(LOCALCODE_HTTP_BACKEND "WinHTTP" CACHE INTERNAL "")
else()
    find_package(CURL 7.66 QUIET)
    if(CURL_FOUND)
        set(LOCALCODE_HTTP_BACKEND "libcurl" CACHE INTERNAL "")
    else()
        set(LOCALCODE_HTTP_BACKEND "none" CACHE INTERNAL "")
        message(WARNING "No HTTP transport found (install libcurl); web tools and "
                        "the inference backends will be unavailable at runtime.")
    endif()
endif()

# --- Catch2 (tests only) ---------------------------------------------------
if(LOCALCODE_BUILD_TESTS)
    find_package(Catch2 3 QUIET)
    if(NOT Catch2_FOUND)
        FetchContent_Declare(Catch2
            URL      https://github.com/catchorg/Catch2/archive/refs/tags/v3.7.1.tar.gz
            URL_HASH SHA256=c991b247a1a0d7bb9c39aa35faf0fe9e19764213f28ffba3109388e62ee0269c
            DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
        set(CATCH_INSTALL_DOCS OFF CACHE INTERNAL "")
        set(CATCH_INSTALL_EXTRAS OFF CACHE INTERNAL "")
        FetchContent_MakeAvailable(Catch2)
        list(APPEND CMAKE_MODULE_PATH "${catch2_SOURCE_DIR}/extras")
    endif()
endif()
