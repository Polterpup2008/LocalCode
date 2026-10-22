// The text-protocol fallback for models without native tool calling.
#include <catch2/catch_test_macros.hpp>

#include "agent/ToolCallParser.h"

using namespace localcode;

TEST_CASE("the documented form is parsed", "[toolcall]") {
    const std::string text =
        "I will look at the file.\n"
        "<tool_call>\n"
        R"({"name": "read_file", "arguments": {"path": "src/main.cpp"}})" "\n"
        "</tool_call>";

    auto parsed = ToolCallParser::parse(text);
    REQUIRE(parsed.calls.size() == 1);
    CHECK(parsed.calls[0].name == "read_file");
    CHECK(parsed.calls[0].arguments_json.find("src/main.cpp") != std::string::npos);
    CHECK(parsed.remaining_text == "I will look at the file.");
    CHECK(parsed.malformed.empty());
}

TEST_CASE("several calls in one reply are all found", "[toolcall]") {
    const std::string text =
        "<tool_call>\n" R"({"name":"a","arguments":{}})" "\n</tool_call>\n"
        "some text between\n"
        "<tool_call>\n" R"({"name":"b","arguments":{"x":1}})" "\n</tool_call>";

    auto parsed = ToolCallParser::parse(text);
    REQUIRE(parsed.calls.size() == 2);
    CHECK(parsed.calls[0].name == "a");
    CHECK(parsed.calls[1].name == "b");
    CHECK(parsed.calls[0].id != parsed.calls[1].id);
    CHECK(parsed.remaining_text.find("some text between") != std::string::npos);
}

TEST_CASE("the delimiters models actually use are accepted", "[toolcall]") {
    SECTION("function_call tags") {
        auto parsed = ToolCallParser::parse(
            "<function_call>" R"({"name":"x","arguments":{}})" "</function_call>");
        REQUIRE(parsed.calls.size() == 1);
    }
    SECTION("bracketed markers") {
        auto parsed = ToolCallParser::parse(
            "[TOOL_CALL]" R"({"name":"x","arguments":{}})" "[/TOOL_CALL]");
        REQUIRE(parsed.calls.size() == 1);
    }
    SECTION("a fenced code block") {
        auto parsed = ToolCallParser::parse(
            "```tool_call\n" R"({"name":"x","arguments":{}})" "\n```");
        REQUIRE(parsed.calls.size() == 1);
    }
}

TEST_CASE("alternative key names are understood", "[toolcall]") {
    SECTION("tool instead of name") {
        auto parsed = ToolCallParser::parse(
            R"(<tool_call>{"tool":"read_file","arguments":{"path":"a"}}</tool_call>)");
        REQUIRE(parsed.calls.size() == 1);
        CHECK(parsed.calls[0].name == "read_file");
    }
    SECTION("parameters instead of arguments") {
        auto parsed = ToolCallParser::parse(
            R"(<tool_call>{"name":"read_file","parameters":{"path":"a"}}</tool_call>)");
        REQUIRE(parsed.calls.size() == 1);
        CHECK(parsed.calls[0].arguments_json.find("path") != std::string::npos);
    }
    SECTION("arguments given as a JSON string") {
        auto parsed = ToolCallParser::parse(
            R"(<tool_call>{"name":"read_file","arguments":"{\"path\":\"a\"}"}</tool_call>)");
        REQUIRE(parsed.calls.size() == 1);
        CHECK(parsed.calls[0].arguments_json.find("path") != std::string::npos);
    }
    SECTION("arguments inlined as siblings of name") {
        auto parsed = ToolCallParser::parse(
            R"(<tool_call>{"name":"read_file","path":"src/a.cpp"}</tool_call>)");
        REQUIRE(parsed.calls.size() == 1);
        CHECK(parsed.calls[0].arguments_json.find("src/a.cpp") != std::string::npos);
    }
}

TEST_CASE("braces inside strings do not end the object", "[toolcall]") {
    auto parsed = ToolCallParser::parse(
        R"(<tool_call>{"name":"write_file","arguments":{"content":"int main() { return 0; }"}}</tool_call>)");
    REQUIRE(parsed.calls.size() == 1);
    CHECK(parsed.calls[0].arguments_json.find("return 0") != std::string::npos);
}

TEST_CASE("escaped quotes are handled", "[toolcall]") {
    auto parsed = ToolCallParser::parse(
        R"(<tool_call>{"name":"write_file","arguments":{"content":"say \"hi\" here"}}</tool_call>)");
    REQUIRE(parsed.calls.size() == 1);
}

TEST_CASE("a malformed block is reported, not silently dropped", "[toolcall]") {
    auto parsed = ToolCallParser::parse("<tool_call>\nthis is not json at all\n</tool_call>");
    CHECK(parsed.calls.empty());
    CHECK_FALSE(parsed.malformed.empty());
}

TEST_CASE("a nameless object is malformed", "[toolcall]") {
    auto parsed = ToolCallParser::parse(R"(<tool_call>{"arguments":{"a":1}}</tool_call>)");
    CHECK(parsed.calls.empty());
    CHECK_FALSE(parsed.malformed.empty());
}

TEST_CASE("plain prose is left alone", "[toolcall]") {
    const std::string prose =
        "The bug is in render.cpp where the index is off by one. I suggest changing the loop "
        "bound.";
    auto parsed = ToolCallParser::parse(prose);
    CHECK(parsed.calls.empty());
    CHECK(parsed.malformed.empty());
    CHECK(parsed.remaining_text == prose);
}

TEST_CASE("a bare JSON object that is clearly a call is accepted", "[toolcall]") {
    auto parsed = ToolCallParser::parse(R"({"name": "list_directory", "arguments": {}})");
    REQUIRE(parsed.calls.size() == 1);
    CHECK(parsed.calls[0].name == "list_directory");
}

TEST_CASE("an unterminated block at the end of a stream still parses", "[toolcall]") {
    auto parsed = ToolCallParser::parse(
        "<tool_call>\n" R"({"name":"read_file","arguments":{"path":"a"}})");
    REQUIRE(parsed.calls.size() == 1);
    CHECK(parsed.calls[0].name == "read_file");
}

TEST_CASE("the protocol instructions name every tool", "[toolcall]") {
    std::vector<ToolDefinition> tools;
    ToolDefinition read;
    read.name = "read_file";
    read.description = "Reads a file.";
    read.parameters = Json{{"type", "object"}};
    tools.push_back(read);

    const auto instructions = ToolCallParser::protocol_instructions(tools);
    CHECK(instructions.find("read_file") != std::string::npos);
    CHECK(instructions.find("<tool_call>") != std::string::npos);
    CHECK(instructions.find("Reads a file.") != std::string::npos);
}

TEST_CASE("looks_like_tool_call is a cheap pre-filter", "[toolcall]") {
    CHECK(ToolCallParser::looks_like_tool_call("<tool_call>{}</tool_call>"));
    CHECK(ToolCallParser::looks_like_tool_call(R"({"name":"x","arguments":{}})"));
    CHECK_FALSE(ToolCallParser::looks_like_tool_call("just some prose about code"));
}

TEST_CASE("unquoted tool intent catches mixed operative prose and raw envelopes",
          "[toolcall][tool-intent][recovery]") {
    const std::vector<std::string> tools{"read_file", "read_file_range"};
    for (const std::string text : {
             "I will inspect the implementation next.\n<tool_call>\n"
             "<function=read_file_range>\n<parameter=path>src/main.cpp</parameter>\n"
             "<parameter=start_line>42</parameter>\n</function>\n</tool_call>",
             "The owner is not established yet. "
             "<tool_call>{\"name\":\"read_file\",\"arguments\":{\"path\":\"a.cpp\"}}</tool_call>",
             "Read the source now. <function_call>{\"name\":\"read_file\"}</function_call>",
             "Still investigating. [TOOL_CALL]{\"name\":\"read_file\"}[/TOOL_CALL]",
             "<|tool_call|>{\"name\":\"read_file\",\"arguments\":{}}<|/tool_call|>",
             "<tool_call>{\"name\":\"read_file\"}<|tool_call_end|>",
             "I will inspect it. <function=read_file><parameter=path>a.cpp</parameter></function>",
         }) {
        INFO(text);
        CHECK(ToolCallParser::contains_unquoted_tool_intent(text));
        CHECK(ToolCallParser::contains_unquoted_tool_intent(text, tools));
    }
}

TEST_CASE("malformed action syntax remains intent without pretending it is executable",
          "[toolcall][tool-intent][recovery]") {
    for (const std::string text : {
             "<tool_call>\nnot valid JSON\n</tool_call>",
             "I will read it. <tool_call>{\"name\":\"read_file\",\"arguments\":",
             "<tool_call><function=read_file>",
             "```tool_call\nnot valid JSON\n```",
             "~~~tool_code\n{broken\n~~~",
             "```json\ttool_call\n{\"name\":\"read_file\"}\n```",
             "```tool_call\n{unfinished",
         }) {
        INFO(text);
        CHECK(ToolCallParser::contains_unquoted_tool_intent(text));
    }
    CHECK_FALSE(ToolCallParser::contains_unquoted_tool_intent(
        "The <tool_call> marker opens the text fallback format."));
    CHECK_FALSE(ToolCallParser::contains_unquoted_tool_intent(
        "Use <tool_call> and </tool_call> to delimit a call."));
    CHECK_FALSE(ToolCallParser::contains_unquoted_tool_intent("<tool_call>"));
    CHECK_FALSE(ToolCallParser::contains_unquoted_tool_intent("```tool_call\n```"));
}

TEST_CASE("ordinary fenced and inline protocol documentation is not action intent",
          "[toolcall][tool-intent][recovery]") {
    const std::vector<std::string> tools{"read_file"};
    for (const std::string text : {
             "The protocol can be documented as follows.\n```xml\n"
             "<tool_call><function=read_file><parameter=path>a</parameter></function></tool_call>\n"
             "```\nThis is an example of its syntax.",
             "Example JSON:\n~~~json\n{\"name\":\"read_file\",\"arguments\":{}}\n~~~",
             "A code sample:\n```\n<tool_call>{broken</tool_call>\n```",
             "Use `<tool_call>{\"name\":\"read_file\"}</tool_call>` in the fallback protocol.",
             "Use ``<tool_call><function=read_file><parameter=path>`a`</parameter></function>"
             "</tool_call>`` as an example.",
             "Quoted JSON: `{\"name\":\"read_file\",\"arguments\":{}}`.",
             "> <tool_call>{\"name\":\"read_file\"}</tool_call>\n"
             "> <function=read_file><parameter=path>a</parameter></function>\n"
             "> ```tool_call\n> {\"name\":\"read_file\"}\n> ```\n\nThat was quoted.",
             "Unfinished code sample:\n```xml\n<tool_call>{broken",
         }) {
        INFO(text);
        CHECK_FALSE(ToolCallParser::contains_unquoted_tool_intent(text));
        CHECK_FALSE(ToolCallParser::contains_unquoted_tool_intent(text, tools));
    }
}

TEST_CASE("quoted examples cannot conceal a separate unquoted action",
          "[toolcall][tool-intent][recovery]") {
    CHECK(ToolCallParser::contains_unquoted_tool_intent(
        "```xml\n<tool_call>{example}</tool_call>\n```\n"
        "I will read it now. <tool_call>{\"name\":\"read_file\"}</tool_call>"));
    CHECK(ToolCallParser::contains_unquoted_tool_intent(
        "> <tool_call>{example}</tool_call>\n\n"
        "<tool_call><function=read_file><parameter=path>a</parameter></function></tool_call>"));
    CHECK(ToolCallParser::contains_unquoted_tool_intent(
        "An unmatched ` does not quote the rest of the reply. "
        "<tool_call>{\"name\":\"read_file\"}</tool_call>"));
    CHECK(ToolCallParser::contains_unquoted_tool_intent(
        "The example is `<tool_call>{example}</tool_call>`.\n"
        "```tool_call\n{\"name\":\"read_file\"}\n```"));
}

TEST_CASE("bare JSON intent is bound to a top-level known tool name",
          "[toolcall][tool-intent][recovery]") {
    const std::vector<std::string> tools{"read_file"};
    const std::string call = R"({"name":"read_file","arguments":{"path":"a}b.cpp"}})";
    CHECK_FALSE(ToolCallParser::contains_unquoted_tool_intent(call));
    CHECK(ToolCallParser::contains_unquoted_tool_intent(call, tools));
    CHECK(ToolCallParser::contains_unquoted_tool_intent("I will inspect it. " + call, tools));
    CHECK(ToolCallParser::contains_unquoted_tool_intent(
        R"({"tool":"read_file","parameters":{"path":"a.cpp"}})", tools));
    CHECK_FALSE(ToolCallParser::contains_unquoted_tool_intent(
        R"({"name":"Ada","arguments":{"topic":"history"}})", tools));
    CHECK_FALSE(ToolCallParser::contains_unquoted_tool_intent(
        R"({"name":"read_file_backup","arguments":{}})", tools));
    CHECK_FALSE(ToolCallParser::contains_unquoted_tool_intent(
        R"({"metadata":{"name":"read_file","arguments":{}}})", tools));
    CHECK(ToolCallParser::contains_unquoted_tool_intent(
        R"({"metadata":{"name":"read_file"}} {"name":"read_file","arguments":{}})", tools));
}
