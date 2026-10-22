#include <catch2/catch_test_macros.hpp>
#include "agent/ToolCatalog.h"
#include "tools/ToolRegistry.h"

using namespace localcode;

TEST_CASE("every permitted tool remains discoverable with its complete parameter set",
          "[tools][catalog][budget]") {
    ToolRegistry registry;
    registry.register_builtin_tools();
    const auto full = registry.definitions();
    ToolCatalog catalog(full);
    auto lean_cost = [](const auto& tools) {
        // Force prose compaction without depending on English byte length.
        return tools.back().description.size() > 125 ? 100 : 10;
    };
    for (const auto& tool : full) {
        INFO(tool.name);
        REQUIRE(catalog.select({{"names", Json::array({tool.name})}}, 20, lean_cost));
        const auto advertised = catalog.definitions();
        REQUIRE(advertised.size() == 2);
        CHECK(advertised.back().name == tool.name);
        const auto& original = tool.parameters["properties"];
        const auto& properties = advertised.back().parameters["properties"];
        CHECK(properties.size() == original.size());
        for (auto field = original.begin(); field != original.end(); ++field) {
            REQUIRE(properties.contains(field.key()));
            if (field->contains("enum")) CHECK(properties[field.key()]["enum"] == (*field)["enum"]);
        }
        if (tool.parameters.contains("required"))
            CHECK(advertised.back().parameters["required"] == tool.parameters["required"]);
    }
}

TEST_CASE("invalid or oversized discovery requests preserve the previous active set",
          "[tools][catalog][budget]") {
    ToolDefinition tool{"custom", std::string(200, 'a'),
                        {{"type", "object"}, {"additionalProperties", false},
                         {"oneOf", Json::array({{{"required", Json::array({"x"})}},
                                                {{"required", Json::array({"y"})}}})},
                         {"properties", {{"x", {{"type", "string"}}},
                                         {"y", {{"type", "integer"}, {"minimum", 1}}}}}}};
    ToolCatalog catalog({tool});
    auto cost = [](const auto& tools) { return static_cast<int>(tools.size()) * 10; };
    REQUIRE(catalog.select({{"names", Json::array({"custom", "custom"})}}, 20, cost));
    CHECK(catalog.definitions().size() == 2);
    const auto before = catalog.definitions().back().to_openai_json();
    for (const auto& arguments : {Json{{"names", "custom"}},
                                 Json{{"names", Json::array({"custom", 2})}},
                                 Json{{"names", Json::array({"outside"})}}}) {
        CHECK_FALSE(catalog.select(arguments, 100, cost));
        CHECK(catalog.definitions().back().to_openai_json() == before);
    }
    CHECK_FALSE(catalog.select({{"names", Json::array({"custom"})}}, 19, cost));
    CHECK(catalog.definitions().back().to_openai_json() == before);
    REQUIRE(catalog.select({{"names", Json::array({"custom"})}}, 10,
                           [](const auto& tools) { return tools.back().description.size() > 125 ? 20 : 10; }));
    const auto compact = catalog.definitions().back();
    CHECK(compact.parameters["additionalProperties"] == false);
    CHECK(compact.parameters["oneOf"] == tool.parameters["oneOf"]);
    CHECK(compact.parameters["properties"]["y"]["minimum"] == 1);
    REQUIRE(catalog.select({{"names", Json::array()}}, 10, cost));
    CHECK(catalog.definitions().size() == 1);
}

TEST_CASE("fixed dispatch only expands loaded permitted calls with the original call id",
          "[tools][stable-dispatch][budget]") {
    ToolRegistry registry;
    registry.register_builtin_tools();
    ToolCatalog catalog(registry.definitions(false), true);
    const auto before = catalog.definitions();
    ToolCall call{"request-id", "call_tool", Json{{"name", "read_file"}, {"arguments", {{"path", "owned.txt"}}}}.dump()};
    CHECK_FALSE(catalog.expand_call(call));
    REQUIRE(catalog.select({{"names", Json::array({"read_file"})}}, 2000,
                           [](const auto& tools) { return estimate_tokens(ToolCatalog::text_schemas(tools)); }));
    const auto expanded = catalog.expand_call(call);
    REQUIRE(expanded);
    CHECK(expanded->id == call.id);
    CHECK(expanded->name == "read_file");
    CHECK(*expanded->parsed_arguments() == Json{{"path", "owned.txt"}});
    const auto after = catalog.definitions();
    REQUIRE(before.size() == after.size());
    for (std::size_t i = 0; i < before.size(); ++i)
        CHECK(before[i].to_openai_json() == after[i].to_openai_json());
    CHECK_FALSE(catalog.select({{"names", Json::array({"write_file"})}}, 2000,
                              [](const auto&) { return 1; }));
    call.arguments_json = Json{{"name", "write_file"}, {"arguments", {{"path", "forbidden.txt"}, {"content", "forbidden"}}}}.dump();
    CHECK_FALSE(catalog.expand_call(call));
    call.arguments_json = Json{{"name", "read_file"}, {"arguments", Json::array({"owned.txt"})}}.dump();
    CHECK_FALSE(catalog.expand_call(call));
}
