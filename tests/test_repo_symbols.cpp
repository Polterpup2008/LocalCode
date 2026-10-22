// ---------------------------------------------------------------------------
// What the symbol reader finds, and what it must not claim.
//
// It is lexical, and the tests are written to hold it to that: it should find
// the definitions a reader would point at, it should not invent one from a
// comment or a string, and everything it produces should be marked lexical
// unless it came out of a file format that actually declares things.
// ---------------------------------------------------------------------------
#include <catch2/catch_test_macros.hpp>

#include <algorithm>

#include "repo/Languages.h"
#include "repo/SymbolIndexer.h"

using namespace localcode::repo;

namespace {

bool has(const IndexedFile& file, std::string_view name, SymbolKind kind) {
    return std::any_of(file.symbols.begin(), file.symbols.end(), [&](const RepoSymbol& s) {
        return s.name == name && s.kind == kind;
    });
}

bool has_name(const IndexedFile& file, std::string_view name) {
    return std::any_of(file.symbols.begin(), file.symbols.end(),
                       [&](const RepoSymbol& s) { return s.name == name; });
}

const RepoSymbol* find(const IndexedFile& file, std::string_view name) {
    const auto it = std::find_if(file.symbols.begin(), file.symbols.end(),
                                 [&](const RepoSymbol& s) { return s.name == name; });
    return it == file.symbols.end() ? nullptr : &*it;
}

}  // namespace

TEST_CASE("inline control flow and spaced qualified calls are not C++ definitions",
          "[repo][symbols]") {
    const auto indexed = index_source("src/calls.cpp", "c++", R"(
int declared (int value);
int ns::defined (int value) { return value; }
void example() {
    try { discounted_price(-1, 10); }
    catch (const std::invalid_argument&) {}
    spaced_call (1);
    ns::qualified_call(1);
    ns::another_spaced_call (1);
    first(); second_call(1);
}
)");
    CHECK(has_name(indexed, "declared"));
    CHECK(has_name(indexed, "defined"));
    CHECK(has_name(indexed, "example"));
    for (const char* name : {"discounted_price", "spaced_call", "qualified_call",
                             "another_spaced_call", "second_call"}) {
        INFO(name);
        CHECK_FALSE(has_name(indexed, name));
    }
}

TEST_CASE("C++ definitions are found and attributed to their class", "[repo][symbols]") {
    const std::string source = R"(#include <string>
#include "Other.h"

namespace app {

class Engine {
public:
    void start();
    int  count() const;
};

void Engine::start() {
    helper(1);
}

struct Point { int x; };

enum class Mode { On, Off };

using Alias = int;

int free_function(int a) { return a; }

}  // namespace app
)";
    const auto indexed = index_source("src/Engine.cpp", "c++", source);

    CHECK(has(indexed, "Engine", SymbolKind::Class));
    CHECK(has(indexed, "Point", SymbolKind::Struct));
    CHECK(has(indexed, "Mode", SymbolKind::Enum));
    CHECK(has(indexed, "Alias", SymbolKind::TypeAlias));
    CHECK(has(indexed, "app", SymbolKind::Namespace));
    CHECK(has_name(indexed, "start"));
    CHECK(has_name(indexed, "free_function"));

    // A call is not a definition.
    CHECK_FALSE(has_name(indexed, "helper"));

    // Includes become references.
    CHECK(std::any_of(indexed.references.begin(), indexed.references.end(),
                      [](const RepoReference& r) { return r.to_file == "Other.h"; }));

    // Nothing here is stronger than lexical.
    for (const auto& symbol : indexed.symbols) CHECK(symbol.evidence == Evidence::Lexical);
}

TEST_CASE("a name in a comment or a string is not a definition", "[repo][symbols]") {
    // The failure that makes a lexical index untrustworthy: prose that looks
    // like code becoming an entry the agent is then sent to open.
    const std::string source = R"(// class GhostFromComment { };
/* void ghost_block(); */
const char* text = "class GhostInString {};";
void real_function() {}
)";
    const auto indexed = index_source("src/a.cpp", "c++", source);

    CHECK(has_name(indexed, "real_function"));
    CHECK_FALSE(has_name(indexed, "GhostFromComment"));
    CHECK_FALSE(has_name(indexed, "ghost_block"));
    CHECK_FALSE(has_name(indexed, "GhostInString"));
}

TEST_CASE("Python functions, classes and methods are separated", "[repo][symbols]") {
    const std::string source = R"(import os
from pathlib import Path

CONSTANT = 3

class Runner:
    def start(self):
        pass

    async def stop(self):
        pass

def module_level():
    pass
)";
    const auto indexed = index_source("run.py", "python", source);

    CHECK(has(indexed, "Runner", SymbolKind::Class));
    CHECK(has(indexed, "module_level", SymbolKind::Function));
    CHECK(has(indexed, "start", SymbolKind::Method));
    CHECK(has(indexed, "stop", SymbolKind::Method));
    CHECK(has(indexed, "CONSTANT", SymbolKind::Variable));

    const auto* start = find(indexed, "start");
    REQUIRE(start != nullptr);
    CHECK(start->container == "Runner");

    CHECK(std::any_of(indexed.references.begin(), indexed.references.end(),
                      [](const RepoReference& r) { return r.to_file == "pathlib"; }));
}

TEST_CASE("Rust items are recognised including impl blocks", "[repo][symbols]") {
    const std::string source = R"(use std::io::Read;

pub struct Parser {
    depth: usize,
}

pub trait Visit {
    fn visit(&self);
}

impl Parser {
    pub fn new() -> Self { Parser { depth: 0 } }
    fn helper(&self) {}
}

pub enum Mode { A, B }

pub const LIMIT: usize = 4;

pub fn free() {}
)";
    const auto indexed = index_source("src/lib.rs", "rust", source);

    CHECK(has(indexed, "Parser", SymbolKind::Struct));
    CHECK(has(indexed, "Visit", SymbolKind::Interface));
    CHECK(has(indexed, "Mode", SymbolKind::Enum));
    CHECK(has(indexed, "LIMIT", SymbolKind::Variable));
    CHECK(has_name(indexed, "free"));

    const auto* created = find(indexed, "new");
    REQUIRE(created != nullptr);
    CHECK(created->container == "Parser");
    CHECK(created->kind == SymbolKind::Method);
}

TEST_CASE("TypeScript and Go shapes are recognised", "[repo][symbols]") {
    const std::string ts = R"(import { thing } from "./other";
export class Widget {
    render(): void {}
}
export function build(): number { return 1; }
export const handler = async (event: string) => { return event; };
interface Shape { area(): number; }
)";
    const auto typescript = index_source("src/app.ts", "typescript", ts);
    CHECK(has(typescript, "Widget", SymbolKind::Class));
    CHECK(has(typescript, "build", SymbolKind::Function));
    CHECK(has(typescript, "handler", SymbolKind::Function));
    CHECK(has(typescript, "Shape", SymbolKind::Interface));
    CHECK(std::any_of(typescript.references.begin(), typescript.references.end(),
                      [](const RepoReference& r) { return r.to_file == "./other"; }));

    const std::string go = R"(package main

import "fmt"

type Server struct { port int }

func (s *Server) Start() error { return nil }

func main() { fmt.Println("hi") }
)";
    const auto golang = index_source("main.go", "go", go);
    CHECK(has(golang, "Server", SymbolKind::Struct));
    CHECK(has_name(golang, "main"));
    const auto* start = find(golang, "Start");
    REQUIRE(start != nullptr);
    CHECK(start->container == "Server");
}

TEST_CASE("CMake targets are structural, not guessed", "[repo][symbols]") {
    // A target is declared in so many words, so this is one of the few places
    // the index can say something stronger than "the text looked like this".
    const std::string source = R"(cmake_minimum_required(VERSION 3.24)
project(demo)

add_executable(app main.cpp)
add_library(engine STATIC engine.cpp)
add_subdirectory(src/core)
function(helper name)
endfunction()
)";
    const auto indexed = index_source("CMakeLists.txt", "cmake", source);

    const auto* app = find(indexed, "app");
    REQUIRE(app != nullptr);
    CHECK(app->kind == SymbolKind::Target);
    CHECK(app->evidence == Evidence::Structural);

    CHECK(has(indexed, "engine", SymbolKind::Target));
    CHECK(has(indexed, "helper", SymbolKind::Function));
    CHECK(std::any_of(indexed.references.begin(), indexed.references.end(),
                      [](const RepoReference& r) { return r.to_file == "src/core"; }));
}

TEST_CASE("a language with no reader still yields a line count", "[repo][symbols]") {
    // Partial support has to look like partial support: no symbols, but the
    // file is still counted rather than quietly dropped.
    const auto indexed = index_source("notes.md", "markdown", "# Title\n\ntext\n");
    CHECK(indexed.symbols.empty());
    CHECK(indexed.line_count == 3);
    CHECK_FALSE(has_symbol_support("markdown"));
    CHECK(has_symbol_support("c++"));
}

TEST_CASE("line numbers point at the definition", "[repo][symbols]") {
    const std::string source = "// header\n\nvoid target_function() {}\n";
    const auto indexed = index_source("a.cpp", "c++", source);
    const auto* symbol = find(indexed, "target_function");
    REQUIRE(symbol != nullptr);
    CHECK(symbol->line == 3);
    CHECK(symbol->signature.find("target_function") != std::string::npos);
}

// ---------------------------------------------------------------------------
// Calls are not definitions
//
// "Where is X defined" is the question the symbol index exists to answer, and
// answering it with the places X is *used* is worse than answering it with
// nothing: the agent opens a file, finds a call, and has learned nothing while
// believing it has.
// ---------------------------------------------------------------------------
TEST_CASE("a call through an object is not indexed as a definition", "[repo][symbols]") {
    const std::string source = R"(#include "app.h"

namespace demo {

void MainWindow::openProject(const std::string& path) {
    application_.refresh_repository_index(false);
    panel_->workspaceChanged();
    return make_result(path);
}

}  // namespace demo
)";
    const auto indexed = index_source("src/window.cpp", "c++", source);

    // The one real definition.
    CHECK(has_name(indexed, "openProject"));
    // The call sites, which used to be indexed because something preceded the
    // name on the line.
    CHECK_FALSE(has_name(indexed, "refresh_repository_index"));
    CHECK_FALSE(has_name(indexed, "workspaceChanged"));
    CHECK_FALSE(has_name(indexed, "make_result"));
}

TEST_CASE("a function in a namespace is a function, not a method", "[repo][symbols]") {
    const std::string source = R"(namespace localcode {

int free_function(int value) {
    return value * 2;
}

class Holder {
public:
    int member_function(int value);
};

int Holder::member_function(int value) {
    return value + 1;
}

}  // namespace localcode
)";
    const auto indexed = index_source("src/thing.cpp", "c++", source);

    const auto* free_one = find(indexed, "free_function");
    REQUIRE(free_one != nullptr);
    // Everything in this codebase sits inside `namespace localcode`, so
    // treating any container as a class made every free function a method.
    CHECK(free_one->kind == SymbolKind::Function);
    CHECK(free_one->container == "localcode");

    const auto* member = find(indexed, "member_function");
    REQUIRE(member != nullptr);
    CHECK(member->kind == SymbolKind::Method);
}
