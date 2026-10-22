#include "Skill.h"
#include "../platform/PathEncoding.h"

#include <algorithm>

#include "../util/Strings.h"

namespace localcode {
namespace {

// Splits "[a, b, "c d"]" or "a, b" into items. The frontmatter is a small,
// deliberately restricted subset of YAML - enough to be familiar, small enough
// that it needs no dependency.
std::vector<std::string> parse_list(std::string_view raw) {
    auto text = strings::trim(raw);
    if (text.size() >= 2 && text.front() == '[' && text.back() == ']') {
        text = strings::trim(text.substr(1, text.size() - 2));
    }
    std::vector<std::string> items;
    std::string current;
    bool in_single = false, in_double = false;
    for (char c : text) {
        if (c == '\'' && !in_double) { in_single = !in_single; continue; }
        if (c == '"' && !in_single) { in_double = !in_double; continue; }
        if (c == ',' && !in_single && !in_double) {
            auto item = strings::trim(current);
            if (!item.empty()) items.push_back(item);
            current.clear();
            continue;
        }
        current.push_back(c);
    }
    auto item = strings::trim(current);
    if (!item.empty()) items.push_back(item);
    return items;
}

bool parse_bool(std::string_view raw, bool fallback) {
    const auto value = strings::to_lower(strings::trim(raw));
    if (value == "true" || value == "yes" || value == "on" || value == "1") return true;
    if (value == "false" || value == "no" || value == "off" || value == "0") return false;
    return fallback;
}

std::string strip_quotes(std::string_view raw) {
    auto text = strings::trim(raw);
    if (text.size() >= 2 && ((text.front() == '"' && text.back() == '"') ||
                             (text.front() == '\'' && text.back() == '\''))) {
        return std::string{text.substr(1, text.size() - 2)};
    }
    return std::string{text};
}

std::string quote_if_needed(const std::string& value) {
    if (value.find_first_of(",:[]#\"'") != std::string::npos) {
        return "\"" + strings::replace_all(value, "\"", "\\\"") + "\"";
    }
    return value;
}

}  // namespace

std::string Skill::to_prompt_section() const {
    std::string out = "### Skill: " + (name.empty() ? id : name) + "\n";
    if (!description.empty()) out += description + "\n\n";
    out += strings::trim(body);
    out += "\n";
    return out;
}

std::vector<std::string> Skill::validate() const {
    std::vector<std::string> problems;

    if (id.empty()) {
        problems.emplace_back("missing 'id'");
    } else {
        for (char c : id) {
            const auto uc = static_cast<unsigned char>(c);
            if (!std::isalnum(uc) && c != '-' && c != '_') {
                problems.emplace_back("id must contain only letters, digits, '-' and '_': " + id);
                break;
            }
        }
    }
    if (name.empty()) problems.emplace_back("missing 'name'");
    if (description.empty()) {
        problems.emplace_back("missing 'description' (it is what the selector matches on)");
    } else if (description.size() > 300) {
        problems.emplace_back("'description' should be one line, under 300 characters");
    }

    const auto trimmed_body = strings::trim(body);
    if (trimmed_body.size() < 80) {
        problems.emplace_back("body is too short to be a usable procedure");
    }
    if (triggers.empty() && tags.empty() && !always) {
        problems.emplace_back(
            "a skill needs 'triggers' or 'tags', otherwise it can never be selected");
    }
    for (const auto& trigger : triggers) {
        if (trigger.size() < 3) {
            problems.emplace_back("trigger '" + trigger + "' is too short and will match noise");
        }
    }
    if (priority < 0 || priority > 100) problems.emplace_back("'priority' must be 0-100");
    return problems;
}

Result<Skill> parse_skill(std::string_view text, const std::filesystem::path& source) {
    Skill skill;
    skill.source_path = source;

    const auto lines = strings::split_lines(text);
    std::size_t index = 0;

    // Skip a leading UTF-8 BOM and blank lines before the frontmatter.
    while (index < lines.size() && strings::trim(lines[index]).empty()) ++index;

    if (index >= lines.size() || strings::trim(lines[index]) != "---") {
        return make_error(ErrorCode::ParseError,
                          "skill is missing its '---' frontmatter block",
                          path_to_utf8(source));
    }
    ++index;

    std::string pending_key;
    bool in_block_list = false;

    for (; index < lines.size(); ++index) {
        const auto& raw = lines[index];
        if (strings::trim(raw) == "---") {
            ++index;
            break;
        }

        // Block list continuation: "  - item"
        const auto trimmed = strings::trim(raw);
        if (!pending_key.empty() && strings::starts_with(trimmed, "- ")) {
            in_block_list = true;
            const auto item = strip_quotes(trimmed.substr(2));
            if (item.empty()) continue;
            if (pending_key == "tags") skill.tags.push_back(item);
            else if (pending_key == "triggers") skill.triggers.push_back(item);
            else if (pending_key == "tools") skill.tools.push_back(item);
            continue;
        }
        in_block_list = false;

        if (trimmed.empty() || trimmed[0] == '#') continue;

        const auto colon = trimmed.find(':');
        if (colon == std::string::npos) continue;

        const auto key = strings::to_lower(strings::trim(trimmed.substr(0, colon)));
        const auto value = strings::trim(trimmed.substr(colon + 1));
        pending_key = key;

        if (key == "id") skill.id = strip_quotes(value);
        else if (key == "name") skill.name = strip_quotes(value);
        else if (key == "description") skill.description = strip_quotes(value);
        else if (key == "tags") skill.tags = parse_list(value);
        else if (key == "triggers") skill.triggers = parse_list(value);
        else if (key == "tools") skill.tools = parse_list(value);
        else if (key == "priority") {
            try { skill.priority = std::stoi(value); } catch (...) { /* keep default */ }
        }
        else if (key == "enabled") skill.enabled = parse_bool(value, true);
        else if (key == "always") skill.always = parse_bool(value, false);
        else if (key == "validated") skill.validated = parse_bool(value, true);
    }
    (void)in_block_list;

    std::vector<std::string> body_lines{lines.begin() + static_cast<std::ptrdiff_t>(index),
                                        lines.end()};
    skill.body = strings::join(body_lines, "\n");

    if (skill.id.empty()) {
        // Fall back to the directory name, which is how skills are addressed.
        const auto parent = path_to_utf8(source.parent_path().filename());
        skill.id = parent.empty() ? path_to_utf8(source.stem()) : parent;
    }
    if (skill.name.empty()) skill.name = skill.id;

    return skill;
}

std::string serialize_skill(const Skill& skill) {
    auto list = [](const std::vector<std::string>& items) {
        std::vector<std::string> quoted;
        quoted.reserve(items.size());
        for (const auto& item : items) quoted.push_back(quote_if_needed(item));
        return "[" + strings::join(quoted, ", ") + "]";
    };

    std::string out = "---\n";
    out += "id: " + skill.id + "\n";
    out += "name: " + quote_if_needed(skill.name) + "\n";
    out += "description: " + quote_if_needed(skill.description) + "\n";
    if (!skill.tags.empty())     out += "tags: " + list(skill.tags) + "\n";
    if (!skill.triggers.empty()) out += "triggers: " + list(skill.triggers) + "\n";
    if (!skill.tools.empty())    out += "tools: " + list(skill.tools) + "\n";
    out += "priority: " + std::to_string(skill.priority) + "\n";
    if (!skill.enabled)   out += "enabled: false\n";
    if (skill.always)     out += "always: true\n";
    if (!skill.validated) out += "validated: false\n";
    out += "---\n\n";
    out += strings::trim(skill.body);
    out += "\n";
    return out;
}

}  // namespace localcode
