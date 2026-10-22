#pragma once
// Single include point for JSON so the rest of the code never names the
// third-party library directly.
#include <nlohmann/json.hpp>

#include <string>
#include <string_view>

#include "Result.h"

namespace localcode {

using Json = nlohmann::json;

// Parses without throwing. `context` is used in the error message only.
Result<Json> parse_json(std::string_view text, std::string_view context = "json");

// Reads a field with a default, tolerating a wrong type instead of throwing -
// small local models emit numbers as strings and booleans as "true" often
// enough that strict typing would break perfectly recoverable tool calls.
std::string  json_string(const Json& j, std::string_view key, std::string fallback = {});
long long    json_int(const Json& j, std::string_view key, long long fallback = 0);
double       json_double(const Json& j, std::string_view key, double fallback = 0.0);
bool         json_bool(const Json& j, std::string_view key, bool fallback = false);

}  // namespace localcode
