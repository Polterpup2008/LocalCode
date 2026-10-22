#include "RepoTypes.h"

namespace localcode::repo {

std::string_view to_string(Evidence evidence) noexcept {
    switch (evidence) {
        case Evidence::Heuristic:  return "heuristic";
        case Evidence::Lexical:    return "lexical";
        case Evidence::Structural: return "structural";
        case Evidence::Verified:   return "verified";
    }
    return "heuristic";
}

std::optional<Evidence> evidence_from_string(std::string_view text) {
    if (text == "heuristic")  return Evidence::Heuristic;
    if (text == "lexical")    return Evidence::Lexical;
    if (text == "structural") return Evidence::Structural;
    if (text == "verified")   return Evidence::Verified;
    return std::nullopt;
}

std::string_view to_string(FileRole role) noexcept {
    switch (role) {
        case FileRole::Source:        return "source";
        case FileRole::Test:          return "test";
        case FileRole::Build:         return "build";
        case FileRole::Config:        return "config";
        case FileRole::Documentation: return "documentation";
        case FileRole::Asset:         return "asset";
        case FileRole::Generated:     return "generated";
        case FileRole::Vendored:      return "vendored";
        case FileRole::Unknown:       return "unknown";
    }
    return "unknown";
}

std::string_view to_string(SymbolKind kind) noexcept {
    switch (kind) {
        case SymbolKind::Function:  return "function";
        case SymbolKind::Method:    return "method";
        case SymbolKind::Class:     return "class";
        case SymbolKind::Struct:    return "struct";
        case SymbolKind::Enum:      return "enum";
        case SymbolKind::Interface: return "interface";
        case SymbolKind::Namespace: return "namespace";
        case SymbolKind::Variable:  return "variable";
        case SymbolKind::TypeAlias: return "type";
        case SymbolKind::Macro:     return "macro";
        case SymbolKind::Target:    return "target";
        case SymbolKind::Unknown:   return "unknown";
    }
    return "unknown";
}

std::optional<SymbolKind> symbol_kind_from_string(std::string_view text) {
    if (text == "function")  return SymbolKind::Function;
    if (text == "method")    return SymbolKind::Method;
    if (text == "class")     return SymbolKind::Class;
    if (text == "struct")    return SymbolKind::Struct;
    if (text == "enum")      return SymbolKind::Enum;
    if (text == "interface") return SymbolKind::Interface;
    if (text == "namespace") return SymbolKind::Namespace;
    if (text == "variable")  return SymbolKind::Variable;
    if (text == "type")      return SymbolKind::TypeAlias;
    if (text == "macro")     return SymbolKind::Macro;
    if (text == "target")    return SymbolKind::Target;
    if (text == "unknown")   return SymbolKind::Unknown;
    return std::nullopt;
}

std::string_view to_string(ReferenceKind kind) noexcept {
    switch (kind) {
        case ReferenceKind::Include:     return "include";
        case ReferenceKind::Mention:     return "mention";
        case ReferenceKind::TestSubject: return "test-subject";
        case ReferenceKind::BuildInput:  return "build-input";
    }
    return "mention";
}

}  // namespace localcode::repo
