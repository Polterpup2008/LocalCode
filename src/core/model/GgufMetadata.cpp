#include "GgufMetadata.h"
#include "../platform/PathEncoding.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <limits>

#include "../util/Log.h"
#include "../util/Strings.h"

namespace localcode {
namespace {

constexpr std::uint32_t kGgufMagic = 0x46554747u;   // "GGUF" little-endian
// The header of even a very large model is small; refuse to chase a corrupt
// length field into gigabytes of reads.
constexpr std::uint64_t kMaxStringLength = 16u * 1024 * 1024;
constexpr std::uint64_t kMaxKvCount = 1u << 20;
// Keep small string arrays; skip vocabularies.
constexpr std::uint64_t kMaxKeptArrayStrings = 64;

// Sequential reader over the file with bounds checking on every field.
class HeaderReader {
public:
    HeaderReader(std::ifstream& stream, std::uint64_t file_size)
        : stream_(stream), file_size_(file_size) {}

    [[nodiscard]] bool ok() const { return ok_; }

    [[nodiscard]] const std::string& error() const { return error_; }

    template <typename T>
    T read_scalar() {
        T value{};
        return read_bytes(reinterpret_cast<char*>(&value), sizeof(T)) ? value : T{};
    }

    // GGUF v1 used 32-bit lengths; v2 and v3 use 64-bit.
    std::uint64_t read_length(std::uint32_t version) {
        return version == 1 ? static_cast<std::uint64_t>(read_scalar<std::uint32_t>())
                            : read_scalar<std::uint64_t>();
    }

    std::string read_string(std::uint32_t version) {
        const std::uint64_t length = read_length(version);
        if (!ok_) return {};
        if (length > kMaxStringLength) {
            fail("string length " + std::to_string(length) + " is implausible");
            return {};
        }
        std::string value(static_cast<std::size_t>(length), '\0');
        if (!read_bytes(value.data(), static_cast<std::size_t>(length))) return {};
        return value;
    }

    void skip(std::uint64_t bytes) {
        if (!ok_ || bytes == 0) return;
        if (bytes > file_size_ - position_) {
            fail("unexpected end of file while skipping");
            return;
        }
        while (bytes > 0) {
            const auto buffered = std::min<std::uint64_t>(bytes, filled_ - cursor_);
            cursor_ += static_cast<std::size_t>(buffered);
            position_ += buffered;
            bytes -= buffered;
            if (bytes == 0) return;
            // Long numeric arrays can still be skipped with one seek. Small
            // vocabulary strings stay in the sequential read buffer.
            if (bytes >= buffer_.size()) {
                if (bytes >
                    static_cast<std::uint64_t>(std::numeric_limits<std::streamoff>::max())) {
                    fail("skip length exceeds the stream offset range");
                    return;
                }
                stream_.seekg(static_cast<std::streamoff>(bytes), std::ios::cur);
                if (!stream_) {
                    fail("unexpected end of file while skipping");
                    return;
                }
                position_ += bytes;
                cursor_ = filled_ = 0;
                return;
            }
            if (!refill()) return;
        }
    }

    void fail(std::string message) {
        if (ok_) {
            ok_ = false;
            error_ = std::move(message);
        }
    }

private:
    bool refill() {
        const auto count = static_cast<std::size_t>(
            std::min<std::uint64_t>(buffer_.size(), file_size_ - position_));
        if (count == 0) {
            fail("unexpected end of file");
            return false;
        }
        stream_.read(buffer_.data(), static_cast<std::streamsize>(count));
        if (stream_.gcount() != static_cast<std::streamsize>(count)) {
            fail("unexpected end of file");
            return false;
        }
        cursor_ = 0;
        filled_ = count;
        return true;
    }

    bool read_bytes(char* destination, std::size_t bytes) {
        if (!ok_) return false;
        if (bytes > file_size_ - position_) {
            fail("unexpected end of file");
            return false;
        }
        while (bytes > 0) {
            if (cursor_ == filled_ && !refill()) return false;
            const auto count = std::min(bytes, filled_ - cursor_);
            std::memcpy(destination, buffer_.data() + cursor_, count);
            destination += count;
            cursor_ += count;
            position_ += count;
            bytes -= count;
        }
        return true;
    }

    std::ifstream& stream_;
    std::array<char, 64u * 1024> buffer_;
    std::uint64_t file_size_ = 0;
    std::uint64_t position_ = 0;
    std::size_t cursor_ = 0;
    std::size_t filled_ = 0;
    bool ok_ = true;
    std::string error_;
};

std::optional<std::size_t> scalar_size(GgufValueType type) {
    switch (type) {
        case GgufValueType::Uint8:
        case GgufValueType::Int8:
        case GgufValueType::Bool:    return 1;
        case GgufValueType::Uint16:
        case GgufValueType::Int16:   return 2;
        case GgufValueType::Uint32:
        case GgufValueType::Int32:
        case GgufValueType::Float32: return 4;
        case GgufValueType::Uint64:
        case GgufValueType::Int64:
        case GgufValueType::Float64: return 8;
        case GgufValueType::String:
        case GgufValueType::Array:   return std::nullopt;
    }
    return std::nullopt;
}

GgufValue read_scalar_value(HeaderReader& reader, GgufValueType type, std::uint32_t version) {
    GgufValue value;
    value.type = type;
    switch (type) {
        case GgufValueType::Uint8:   value.number_value = reader.read_scalar<std::uint8_t>(); break;
        case GgufValueType::Int8:    value.number_value = reader.read_scalar<std::int8_t>(); break;
        case GgufValueType::Uint16:  value.number_value = reader.read_scalar<std::uint16_t>(); break;
        case GgufValueType::Int16:   value.number_value = reader.read_scalar<std::int16_t>(); break;
        case GgufValueType::Uint32:  value.number_value = reader.read_scalar<std::uint32_t>(); break;
        case GgufValueType::Int32:   value.number_value = reader.read_scalar<std::int32_t>(); break;
        case GgufValueType::Float32: value.number_value = reader.read_scalar<float>(); break;
        case GgufValueType::Uint64:
            value.number_value = static_cast<double>(reader.read_scalar<std::uint64_t>());
            break;
        case GgufValueType::Int64:
            value.number_value = static_cast<double>(reader.read_scalar<std::int64_t>());
            break;
        case GgufValueType::Float64: value.number_value = reader.read_scalar<double>(); break;
        case GgufValueType::Bool:
            value.bool_value = reader.read_scalar<std::uint8_t>() != 0;
            value.number_value = value.bool_value ? 1.0 : 0.0;
            break;
        case GgufValueType::String:  value.string_value = reader.read_string(version); break;
        case GgufValueType::Array:   reader.fail("nested arrays are not supported"); break;
    }
    return value;
}

std::uint64_t as_uint(const GgufValue& value) {
    if (value.number_value < 0) return 0;
    return static_cast<std::uint64_t>(value.number_value);
}

}  // namespace

std::string GgufValue::to_display_string() const {
    if (is_array) {
        std::string out = "[" + std::to_string(array_length) + " items]";
        if (!array_strings.empty()) {
            out += " " + strings::join(array_strings, ", ");
        }
        return out;
    }
    switch (type) {
        case GgufValueType::String: return string_value;
        case GgufValueType::Bool:   return bool_value ? "true" : "false";
        case GgufValueType::Float32:
        case GgufValueType::Float64: {
            char buffer[64];
            std::snprintf(buffer, sizeof buffer, "%g", number_value);
            return buffer;
        }
        default: {
            char buffer[64];
            std::snprintf(buffer, sizeof buffer, "%lld", static_cast<long long>(number_value));
            return buffer;
        }
    }
}

const GgufValue* GgufMetadata::find(std::string_view key) const {
    const auto it = values.find(std::string{key});
    return it == values.end() ? nullptr : &it->second;
}

std::vector<std::pair<std::string, std::string>> GgufMetadata::summary() const {
    std::vector<std::pair<std::string, std::string>> rows;
    auto add = [&rows](std::string key, std::string value) {
        if (!value.empty() && value != "0") rows.emplace_back(std::move(key), std::move(value));
    };
    add("Name", name);
    add("Architecture", architecture);
    add("Quantization", quantization);
    add("Parameters", size_label);
    add("File size", strings::human_bytes(file_size));
    add("Trained context", context_length ? std::to_string(context_length) + " tokens" : "");
    add("Layers", block_count ? std::to_string(block_count) : "");
    add("Embedding size", embedding_length ? std::to_string(embedding_length) : "");
    add("Attention heads", attention_head_count ? std::to_string(attention_head_count) : "");
    add("KV heads", attention_head_count_kv ? std::to_string(attention_head_count_kv) : "");
    add("Experts", expert_count ? std::to_string(expert_count) : "");
    add("Vocabulary", vocab_size ? std::to_string(vocab_size) : "");
    add("Tokenizer", tokenizer_model);
    add("Chat template", chat_template.empty() ? "none" : "present");
    add("GGUF version", std::to_string(version));
    add("Tensors", std::to_string(tensor_count));
    return rows;
}

std::string gguf_file_type_name(std::uint32_t file_type) {
    // Mirrors llama.cpp's llama_ftype enumeration.
    switch (file_type) {
        case 0:  return "F32";
        case 1:  return "F16";
        case 2:  return "Q4_0";
        case 3:  return "Q4_1";
        case 4:  return "Q4_1_SOME_F16";
        case 7:  return "Q8_0";
        case 8:  return "Q5_0";
        case 9:  return "Q5_1";
        case 10: return "Q2_K";
        case 11: return "Q3_K_S";
        case 12: return "Q3_K_M";
        case 13: return "Q3_K_L";
        case 14: return "Q4_K_S";
        case 15: return "Q4_K_M";
        case 16: return "Q5_K_S";
        case 17: return "Q5_K_M";
        case 18: return "Q6_K";
        case 19: return "IQ2_XXS";
        case 20: return "IQ2_XS";
        case 21: return "Q2_K_S";
        case 22: return "IQ3_XS";
        case 23: return "IQ3_XXS";
        case 24: return "IQ1_S";
        case 25: return "IQ4_NL";
        case 26: return "IQ3_S";
        case 27: return "IQ3_M";
        case 28: return "IQ2_S";
        case 29: return "IQ2_M";
        case 30: return "IQ4_XS";
        case 31: return "IQ1_M";
        case 32: return "BF16";
        case 36: return "TQ1_0";
        case 37: return "TQ2_0";
        default: return "type " + std::to_string(file_type);
    }
}

Result<GgufMetadata> read_gguf_metadata(const std::filesystem::path& path) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec)) {
        return make_error(ErrorCode::NotFound, "model file not found: " + path_to_utf8(path));
    }

    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        return make_error(ErrorCode::IoError, "cannot open model file: " + path_to_utf8(path));
    }

    GgufMetadata metadata;
    metadata.path = path;
    metadata.file_size = std::filesystem::file_size(path, ec);
    if (ec) return make_error(ErrorCode::IoError, "cannot read model file size: " + path_to_utf8(path));

    HeaderReader reader{stream, metadata.file_size};

    const auto magic = reader.read_scalar<std::uint32_t>();
    if (!reader.ok() || magic != kGgufMagic) {
        return make_error(ErrorCode::ParseError, "not a GGUF file: " + path_to_utf8(path),
                          "expected magic 'GGUF'");
    }

    metadata.version = reader.read_scalar<std::uint32_t>();
    if (!reader.ok() || metadata.version == 0 || metadata.version > 3) {
        return make_error(ErrorCode::NotSupported,
                          "unsupported GGUF version " + std::to_string(metadata.version),
                          "LocalCode reads GGUF v1 to v3");
    }

    metadata.tensor_count = reader.read_length(metadata.version);
    metadata.kv_count = reader.read_length(metadata.version);
    if (!reader.ok()) {
        return make_error(ErrorCode::ParseError, "truncated GGUF header", reader.error());
    }
    if (metadata.kv_count > kMaxKvCount) {
        return make_error(ErrorCode::ParseError,
                          "implausible metadata count in " + path_to_utf8(path),
                          std::to_string(metadata.kv_count) + " entries");
    }

    for (std::uint64_t i = 0; i < metadata.kv_count && reader.ok(); ++i) {
        const std::string key = reader.read_string(metadata.version);
        if (!reader.ok()) break;

        const auto type = static_cast<GgufValueType>(reader.read_scalar<std::uint32_t>());
        if (!reader.ok()) break;

        GgufValue value;
        if (type == GgufValueType::Array) {
            const auto element_type =
                static_cast<GgufValueType>(reader.read_scalar<std::uint32_t>());
            const std::uint64_t length = reader.read_length(metadata.version);
            if (!reader.ok()) break;

            value.type = GgufValueType::Array;
            value.is_array = true;
            value.array_element_type = element_type;
            value.array_length = length;

            if (element_type == GgufValueType::String) {
                // Vocabularies are hundreds of thousands of entries; read the
                // first few for display and skip the rest by streaming past
                // each length-prefixed item.
                for (std::uint64_t k = 0; k < length && reader.ok(); ++k) {
                    if (k < kMaxKeptArrayStrings) {
                        value.array_strings.push_back(reader.read_string(metadata.version));
                    } else {
                        const std::uint64_t item_length = reader.read_length(metadata.version);
                        if (item_length > kMaxStringLength) {
                            reader.fail("implausible string length inside array");
                            break;
                        }
                        reader.skip(item_length);
                    }
                }
            } else if (element_type == GgufValueType::Array) {
                reader.fail("nested arrays are not supported");
            } else if (auto size = scalar_size(element_type)) {
                if (length > std::numeric_limits<std::uint64_t>::max() / *size) {
                    reader.fail("implausible scalar array length");
                } else {
                    reader.skip(length * *size);
                }
            } else {
                reader.fail("unknown array element type " +
                            std::to_string(static_cast<std::uint32_t>(element_type)));
            }

        } else {
            value = read_scalar_value(reader, type, metadata.version);
        }

        if (!reader.ok()) break;
        if (type == GgufValueType::Array && key == "tokenizer.ggml.tokens") {
            metadata.vocab_size = value.array_length;
        }
        metadata.values.emplace(key, std::move(value));
    }

    if (!reader.ok()) {
        // A partially read header is still worth returning when the interesting
        // keys arrived before the corruption, but say so loudly.
        log_warn(LogChannel::App, "GGUF header read stopped early",
                 {{"file", path_to_utf8(path.filename())}, {"reason", reader.error()}});
        if (metadata.values.empty()) {
            return make_error(ErrorCode::ParseError,
                              "could not read GGUF metadata from " + path_to_utf8(path),
                              reader.error());
        }
    }

    // --- Promote the keys we care about -----------------------------------
    auto string_of = [&metadata](std::string_view key) -> std::string {
        const auto* v = metadata.find(key);
        return v ? v->string_value : std::string{};
    };
    auto uint_of = [&metadata](std::string_view key) -> std::uint64_t {
        const auto* v = metadata.find(key);
        return v ? as_uint(*v) : 0;
    };

    metadata.architecture = string_of("general.architecture");
    metadata.name = string_of("general.name");
    metadata.basename = string_of("general.basename");
    metadata.size_label = string_of("general.size_label");
    metadata.tokenizer_model = string_of("tokenizer.ggml.model");
    metadata.chat_template = string_of("tokenizer.chat_template");

    if (const auto* ft = metadata.find("general.file_type")) {
        metadata.file_type = static_cast<std::uint32_t>(as_uint(*ft));
        metadata.quantization = gguf_file_type_name(metadata.file_type);
    }

    if (!metadata.architecture.empty()) {
        const std::string prefix = metadata.architecture + ".";
        metadata.context_length = uint_of(prefix + "context_length");
        metadata.embedding_length = uint_of(prefix + "embedding_length");
        metadata.block_count = uint_of(prefix + "block_count");
        metadata.attention_head_count = uint_of(prefix + "attention.head_count");
        metadata.attention_head_count_kv = uint_of(prefix + "attention.head_count_kv");
        metadata.expert_count = uint_of(prefix + "expert_count");
        if (const auto* rope = metadata.find(prefix + "rope.freq_base")) {
            metadata.rope_freq_base = rope->number_value;
        }
    }

    if (metadata.name.empty()) metadata.name = path_to_utf8(path.stem());
    if (metadata.quantization.empty()) {
        // Some converters omit general.file_type; fall back to the filename,
        // which by convention carries the quantization.
        const auto stem = strings::to_upper(path_to_utf8(path.stem()));
        for (const char* candidate : {"Q2_K", "Q3_K_S", "Q3_K_M", "Q3_K_L", "Q4_0", "Q4_1",
                                      "Q4_K_S", "Q4_K_M", "Q5_0", "Q5_1", "Q5_K_S", "Q5_K_M",
                                      "Q6_K", "Q8_0", "IQ4_XS", "IQ4_NL", "IQ3_M", "IQ3_S",
                                      "IQ2_M", "IQ2_S", "BF16", "F16", "F32"}) {
            if (stem.find(candidate) != std::string::npos) {
                metadata.quantization = candidate;
                break;
            }
        }
    }

    log_info(LogChannel::App, "read GGUF metadata",
             {{"file", path_to_utf8(path.filename())},
              {"arch", metadata.architecture},
              {"quant", metadata.quantization},
              {"ctx", std::to_string(metadata.context_length)}});
    return metadata;
}

}  // namespace localcode
