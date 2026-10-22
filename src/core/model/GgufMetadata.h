#pragma once
// ---------------------------------------------------------------------------
// GGUF header reader.
//
// LocalCode reads the model file itself rather than asking the user to type in
// its architecture and context length. The header carries everything needed to
// build a sensible default profile: architecture, parameter counts, trained
// context length, quantization, and - most usefully - the chat template, which
// is the only honest evidence of whether a model was trained to emit tool calls
// or reasoning content.
//
// Vocabularies are streamed with bounded working memory and 64 KiB read-ahead;
// tensor payloads are not parsed.
// ---------------------------------------------------------------------------
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "../util/Result.h"

namespace localcode {

enum class GgufValueType : std::uint32_t {
    Uint8 = 0, Int8 = 1, Uint16 = 2, Int16 = 3, Uint32 = 4, Int32 = 5,
    Float32 = 6, Bool = 7, String = 8, Array = 9, Uint64 = 10, Int64 = 11,
    Float64 = 12,
};

struct GgufValue {
    GgufValueType type = GgufValueType::String;
    // Scalars are kept in whichever field applies; arrays record only their
    // element type and length, because token vocabularies run to hundreds of
    // thousands of entries and are of no use to us.
    std::string   string_value;
    double        number_value = 0.0;
    bool          bool_value = false;
    bool          is_array = false;
    GgufValueType array_element_type = GgufValueType::String;
    std::uint64_t array_length = 0;
    // Small string arrays are kept in full (chat template lists, for example).
    std::vector<std::string> array_strings;

    [[nodiscard]] std::string to_display_string() const;
};

struct GgufMetadata {
    std::filesystem::path path;
    std::uint32_t         version = 0;
    std::uint64_t         tensor_count = 0;
    std::uint64_t         kv_count = 0;
    std::uint64_t         file_size = 0;

    std::string           architecture;      // general.architecture
    std::string           name;              // general.name
    std::string           basename;
    std::string           size_label;        // e.g. "7B"
    std::string           quantization;      // decoded from general.file_type
    std::uint32_t         file_type = 0;

    std::uint64_t         context_length = 0;      // <arch>.context_length
    std::uint64_t         embedding_length = 0;
    std::uint64_t         block_count = 0;
    std::uint64_t         attention_head_count = 0;
    std::uint64_t         attention_head_count_kv = 0;
    double                rope_freq_base = 0.0;
    std::uint64_t         expert_count = 0;        // > 0 for mixture-of-experts

    std::string           chat_template;           // tokenizer.chat_template
    std::string           tokenizer_model;         // tokenizer.ggml.model
    std::uint64_t         vocab_size = 0;

    std::map<std::string, GgufValue> values;       // every scalar key

    [[nodiscard]] const GgufValue* find(std::string_view key) const;
    // Human-readable summary used by the model inspector.
    [[nodiscard]] std::vector<std::pair<std::string, std::string>> summary() const;
};

// Reads the header. Fails with ParseError for anything that is not a GGUF file.
Result<GgufMetadata> read_gguf_metadata(const std::filesystem::path& path);

// Maps a general.file_type value to its llama.cpp quantization name.
[[nodiscard]] std::string gguf_file_type_name(std::uint32_t file_type);

}  // namespace localcode
