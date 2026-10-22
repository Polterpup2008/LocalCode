#include "Json.h"

#include "Strings.h"

namespace localcode {

Result<Json> parse_json(std::string_view text, std::string_view context) {
    Json parsed = Json::parse(text, nullptr, /*allow_exceptions=*/false, /*ignore_comments=*/true);
    if (parsed.is_discarded()) {
        return make_error(ErrorCode::ParseError,
                          "invalid JSON in " + std::string{context},
                          strings::utf8_truncate(text, 400));
    }
    return parsed;
}

namespace {

const Json* find(const Json& j, std::string_view key) {
    if (!j.is_object()) return nullptr;
    const auto it = j.find(std::string{key});
    if (it == j.end() || it->is_null()) return nullptr;
    return &*it;
}

}  // namespace

std::string json_string(const Json& j, std::string_view key, std::string fallback) {
    const Json* v = find(j, key);
    if (!v) return fallback;
    if (v->is_string()) return v->get<std::string>();
    if (v->is_number_integer()) return std::to_string(v->get<long long>());
    if (v->is_number_float()) return std::to_string(v->get<double>());
    if (v->is_boolean()) return v->get<bool>() ? "true" : "false";
    return v->dump();
}

long long json_int(const Json& j, std::string_view key, long long fallback) {
    const Json* v = find(j, key);
    if (!v) return fallback;
    if (v->is_number_integer()) return v->get<long long>();
    if (v->is_number_float()) return static_cast<long long>(v->get<double>());
    if (v->is_boolean()) return v->get<bool>() ? 1 : 0;
    if (v->is_string()) {
        try {
            return std::stoll(v->get<std::string>());
        } catch (...) {
            return fallback;
        }
    }
    return fallback;
}

double json_double(const Json& j, std::string_view key, double fallback) {
    const Json* v = find(j, key);
    if (!v) return fallback;
    if (v->is_number()) return v->get<double>();
    if (v->is_string()) {
        try {
            return std::stod(v->get<std::string>());
        } catch (...) {
            return fallback;
        }
    }
    return fallback;
}

bool json_bool(const Json& j, std::string_view key, bool fallback) {
    const Json* v = find(j, key);
    if (!v) return fallback;
    if (v->is_boolean()) return v->get<bool>();
    if (v->is_number()) return v->get<double>() != 0.0;
    if (v->is_string()) {
        const auto s = strings::to_lower(v->get<std::string>());
        if (s == "true" || s == "yes" || s == "1") return true;
        if (s == "false" || s == "no" || s == "0") return false;
    }
    return fallback;
}

}  // namespace localcode
