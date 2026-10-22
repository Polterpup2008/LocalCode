// GGUF parsing, capability inference and backend selection.
#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <fstream>

#include "model/GgufMetadata.h"
#include "model/ModelProfile.h"
#include "platform/PathEncoding.h"
#include "support/TempDirectory.h"

using namespace localcode;
using localcode::test::TempDirectory;

TEST_CASE("weight repacking preserves defaults explicit false and invalid saved choices",
          "[model][repacking]") {
    ModelProfile profile;
    CHECK(profile.weight_repacking == WeightRepacking::RuntimeDefault);
    auto json = to_json(profile);
    CHECK_FALSE(json.contains("weight_repacking"));
    CHECK(model_profile_from_json(json).weight_repacking == WeightRepacking::RuntimeDefault);
    json["weight_repacking"] = nullptr;
    CHECK(model_profile_from_json(json).weight_repacking == WeightRepacking::RuntimeDefault);
    for (const auto mode : {WeightRepacking::Enabled, WeightRepacking::Disabled}) {
        profile.weight_repacking = mode;
        const auto encoded = to_json(profile);
        REQUIRE(encoded["weight_repacking"].is_boolean());
        CHECK(encoded["weight_repacking"].get<bool>() == (mode == WeightRepacking::Enabled));
        CHECK(model_profile_from_json(encoded).weight_repacking == mode);
    }
    for (const Json value : {Json(0), Json(1), Json("false"), Json::array(), Json::object()}) {
        json["weight_repacking"] = value;
        const auto invalid = model_profile_from_json(json);
        CHECK(invalid.weight_repacking == WeightRepacking::Invalid);
        CHECK(model_profile_from_json(to_json(invalid)).weight_repacking == WeightRepacking::Invalid);
    }
}

TEST_CASE("prompt cache budgets preserve legacy defaults and explicit zero", "[model][cache-ram]") {
    ModelProfile profile;
    CHECK_FALSE(profile.prompt_cache_mib.has_value());
    auto legacy = to_json(profile);
    CHECK_FALSE(legacy.contains("prompt_cache_mib"));
    CHECK_FALSE(model_profile_from_json(legacy).prompt_cache_mib.has_value());
    legacy["prompt_cache_mib"] = nullptr;
    CHECK_FALSE(model_profile_from_json(legacy).prompt_cache_mib.has_value());
    for (const auto value : {0, 64, 8192, 2147483647}) {
        profile.prompt_cache_mib = value;
        const auto restored = model_profile_from_json(to_json(profile));
        REQUIRE(restored.prompt_cache_mib.has_value());
        CHECK(*restored.prompt_cache_mib == value);
    }
    for (const Json value : {Json(-1), Json(2147483648LL), Json("64"), Json(64.5), Json(true)}) {
        legacy["prompt_cache_mib"] = value;
        const auto restored = model_profile_from_json(legacy);
        REQUIRE(restored.prompt_cache_mib.has_value());
        CHECK((*restored.prompt_cache_mib < 0 || *restored.prompt_cache_mib > 2147483647LL));
    }
}

namespace {

// Builds a minimal but genuinely well-formed GGUF v3 header, so the parser is
// tested against the real format rather than a mock.
class GgufBuilder {
public:
    explicit GgufBuilder(std::uint32_t version = 3) : version_(version) {
        put_u32(0x46554747);   // "GGUF"
        put_u32(version);      // version
        put_length(0);         // tensor count
        kv_count_offset_ = data_.size();
        put_length(0);   // metadata count, patched by finish()
    }

    GgufBuilder& string_value(const std::string& key, const std::string& value) {
        put_string(key);
        put_u32(8);   // String
        put_string(value);
        ++count_;
        return *this;
    }

    GgufBuilder& uint32_value(const std::string& key, std::uint32_t value) {
        put_string(key);
        put_u32(4);   // Uint32
        put_u32(value);
        ++count_;
        return *this;
    }

    GgufBuilder& string_array(const std::string& key, const std::vector<std::string>& values) {
        put_string(key);
        put_u32(9);   // Array
        put_u32(8);   // of String
        put_length(values.size());
        for (const auto& value : values)
            put_string(value);
        ++count_;
        return *this;
    }

    GgufBuilder& uint32_array(const std::string& key, const std::vector<std::uint32_t>& values) {
        put_string(key);
        put_u32(9);
        put_u32(4);
        put_length(values.size());
        for (const auto value : values)
            put_u32(value);
        ++count_;
        return *this;
    }

    std::string finish() {
        std::string out = data_;
        std::uint64_t count = count_;
        if (version_ == 1) {
            const auto short_count = static_cast<std::uint32_t>(count);
            std::memcpy(out.data() + kv_count_offset_, &short_count, sizeof short_count);
        } else {
            std::memcpy(out.data() + kv_count_offset_, &count, sizeof count);
        }
        return out;
    }

private:
    void put_u32(std::uint32_t value) {
        data_.append(reinterpret_cast<const char*>(&value), sizeof value);
    }

    void put_u64(std::uint64_t value) {
        data_.append(reinterpret_cast<const char*>(&value), sizeof value);
    }

    void put_string(const std::string& value) {
        put_length(value.size());
        data_.append(value);
    }

    void put_length(std::uint64_t value) {
        if (version_ == 1)
            put_u32(static_cast<std::uint32_t>(value));
        else
            put_u64(value);
    }

    std::string data_;
    std::uint32_t version_ = 3;
    std::size_t kv_count_offset_ = 0;
    std::uint64_t count_ = 0;
};

}   // namespace

TEST_CASE("GGUF vocabulary skipping and retained strings span read buffers", "[gguf][model]") {
    TempDirectory temp{"lc-gguf-buffer"};
    for (const auto version : {1u, 2u, 3u}) {
        CAPTURE(version);
        std::vector<std::string> tokens;
        for (unsigned i = 0; i < 130; ++i) {
            tokens.push_back(std::to_string(i) + std::string(1100, 'x'));
        }
        const std::string chat_template(150000, 't');
        const auto file = temp.write(
            "buffer.gguf",
            GgufBuilder(version)
                .string_value("general.architecture", "llama")
                .string_array("tokenizer.ggml.tokens", tokens)
                .uint32_array("tokenizer.ggml.token_type", std::vector<std::uint32_t>(80000, 1))
                .uint32_value("llama.context_length", 32768)
                .string_value("tokenizer.chat_template", chat_template)
                .finish());
        const auto metadata = read_gguf_metadata(file);
        REQUIRE(metadata);
        CHECK(metadata->version == version);
        CHECK(metadata->vocab_size == tokens.size());
        CHECK(metadata->context_length == 32768);
        CHECK(metadata->chat_template == chat_template);
        const auto* vocabulary = metadata->find("tokenizer.ggml.tokens");
        REQUIRE(vocabulary);
        REQUIRE(vocabulary->array_strings.size() == 64);
        CHECK(vocabulary->array_strings.front() == tokens.front());
        CHECK(vocabulary->array_strings.back() == tokens[63]);
        const auto* types = metadata->find("tokenizer.ggml.token_type");
        REQUIRE(types);
        CHECK(types->array_length == 80000);
    }
}

TEST_CASE("GGUF scalar length fields can cross a read-buffer boundary", "[gguf][model]") {
    TempDirectory temp{"lc-gguf-split-scalar"};
    GgufBuilder builder;
    const auto overhead =
        builder.finish().size() + 8 + std::string("general.padding").size() + 4 + 8;
    builder.string_value("general.padding", std::string(65533 - overhead, 'p'));
    REQUIRE(builder.finish().size() == 65533);
    builder.string_value("general.architecture", "llama").uint32_value("llama.block_count", 32);
    const auto metadata = read_gguf_metadata(temp.write("scalar.gguf", builder.finish()));
    REQUIRE(metadata);
    CHECK(metadata->architecture == "llama");
    CHECK(metadata->block_count == 32);
}

TEST_CASE("GGUF skipped array truncation does not retain the incomplete key", "[gguf][model]") {
    TempDirectory temp{"lc-gguf-truncated-skip"};
    for (const bool strings : {false, true}) {
        CAPTURE(strings);
        GgufBuilder builder;
        builder.string_value("general.architecture", "llama");
        if (strings)
            builder.string_array("tokenizer.ggml.tokens",
                                 std::vector<std::string>(65, std::string(1100, 'x')));
        else
            builder.uint32_array("tokenizer.ggml.token_type", std::vector<std::uint32_t>(80000, 1));
        auto bytes = builder.finish();
        bytes.pop_back();
        const auto metadata = read_gguf_metadata(temp.write("truncated-array.gguf", bytes));
        REQUIRE(metadata);
        CHECK(metadata->architecture == "llama");
        CHECK(metadata->find(strings ? "tokenizer.ggml.tokens" : "tokenizer.ggml.token_type") ==
              nullptr);
        CHECK(metadata->vocab_size == 0);
    }
}

TEST_CASE("GGUF scalar array byte lengths cannot wrap around", "[gguf][model][adversarial]") {
    TempDirectory temp{"lc-gguf-overflow-skip"};
    auto bytes = GgufBuilder()
                     .string_value("general.architecture", "llama")
                     .uint32_array("overflow", {})
                     .finish();
    const std::uint64_t length = (std::uint64_t{1} << 62);
    std::memcpy(bytes.data() + bytes.size() - sizeof length, &length, sizeof length);
    const auto metadata = read_gguf_metadata(temp.write("overflow.gguf", bytes));
    REQUIRE(metadata);
    CHECK(metadata->architecture == "llama");
    CHECK(metadata->find("overflow") == nullptr);
}

TEST_CASE("a well-formed GGUF header is parsed", "[gguf][model]") {
    TempDirectory temp{"lc-gguf"};

    GgufBuilder builder;
    builder.string_value("general.architecture", "llama")
        .string_value("general.name", "Test Model")
        .string_value("general.size_label", "7B")
        .uint32_value("general.file_type", 15)   // Q4_K_M
        .uint32_value("llama.context_length", 32768)
        .uint32_value("llama.embedding_length", 4096)
        .uint32_value("llama.block_count", 32)
        .uint32_value("llama.attention.head_count", 32)
        .string_value("tokenizer.ggml.model", "gpt2")
        .string_array("tokenizer.ggml.tokens", {"a", "b", "c"});

    const auto file = temp.write("model.gguf", builder.finish());

    auto metadata = read_gguf_metadata(file);
    REQUIRE(metadata.ok());
    CHECK(metadata->version == 3);
    CHECK(metadata->architecture == "llama");
    CHECK(metadata->name == "Test Model");
    CHECK(metadata->size_label == "7B");
    CHECK(metadata->quantization == "Q4_K_M");
    CHECK(metadata->context_length == 32768);
    CHECK(metadata->embedding_length == 4096);
    CHECK(metadata->block_count == 32);
    CHECK(metadata->attention_head_count == 32);
    CHECK(metadata->tokenizer_model == "gpt2");
    CHECK(metadata->vocab_size == 3);
}

TEST_CASE("a file that is not GGUF is rejected", "[gguf][model]") {
    TempDirectory temp{"lc-gguf"};
    const auto file = temp.write("not-a-model.bin", "this is definitely not a GGUF file at all");

    auto metadata = read_gguf_metadata(file);
    REQUIRE_FALSE(metadata.ok());
    CHECK(metadata.error().code == ErrorCode::ParseError);
}

TEST_CASE("a truncated header does not crash the parser", "[gguf][model]") {
    TempDirectory temp{"lc-gguf"};
    auto full = GgufBuilder().string_value("general.architecture", "llama").finish();
    // Cut it off partway through.
    const auto file = temp.write("truncated.gguf", full.substr(0, full.size() / 2));

    auto metadata = read_gguf_metadata(file);
    // Either an error or partial data - but never a crash or a hang.
    if (metadata.ok()) CHECK(metadata->version == 3);
}

TEST_CASE("an implausible length field is refused", "[gguf][model][adversarial]") {
    TempDirectory temp{"lc-gguf"};

    // Magic, version, tensor count, then a metadata count that would need
    // gigabytes of allocation.
    std::string data;
    auto append_u32 = [&data](std::uint32_t v) {
        data.append(reinterpret_cast<const char*>(&v), sizeof v);
    };
    auto append_u64 = [&data](std::uint64_t v) {
        data.append(reinterpret_cast<const char*>(&v), sizeof v);
    };
    append_u32(0x46554747);
    append_u32(3);
    append_u64(0);
    append_u64(0xFFFFFFFFFFFFFFFFull);

    const auto file = temp.write("hostile.gguf", data);
    auto metadata = read_gguf_metadata(file);
    REQUIRE_FALSE(metadata.ok());
}

TEST_CASE("capabilities come from the chat template", "[model][capabilities]") {
    SECTION("a template that renders tools implies tool support") {
        GgufMetadata metadata;
        metadata.chat_template =
            "{%- if tools %}{{- '<|im_start|>system' }}{%- for tool in tools %}"
            "{{- tool | tojson }}{%- endfor %}{%- endif %}";
        const auto capabilities = ModelCapabilities::infer(metadata);
        CHECK(capabilities.supports_tools);
        CHECK(capabilities.evidence.find("tool") != std::string::npos);
    }

    SECTION("a template with a think block implies native reasoning") {
        GgufMetadata metadata;
        metadata.chat_template = "{{ '<think>' }}{{ content }}{{ '</think>' }}";
        const auto capabilities = ModelCapabilities::infer(metadata);
        CHECK(capabilities.supports_native_reasoning);
        CHECK(capabilities.supports_reasoning_content);
    }

    SECTION("a plain template implies neither") {
        GgufMetadata metadata;
        metadata.chat_template =
            "{% for message in messages %}{{ message['role'] }}: {{ message['content'] }}"
            "{% endfor %}";
        const auto capabilities = ModelCapabilities::infer(metadata);
        CHECK_FALSE(capabilities.supports_tools);
        CHECK_FALSE(capabilities.supports_native_reasoning);
    }

    SECTION("no template at all is recorded as such") {
        GgufMetadata metadata;
        const auto capabilities = ModelCapabilities::infer(metadata);
        CHECK_FALSE(capabilities.supports_tools);
        CHECK(capabilities.evidence.find("no chat template") != std::string::npos);
    }

    SECTION("long context is derived from the trained length") {
        GgufMetadata metadata;
        metadata.context_length = 131072;
        CHECK(ModelCapabilities::infer(metadata).supports_long_context);

        metadata.context_length = 4096;
        CHECK_FALSE(ModelCapabilities::infer(metadata).supports_long_context);
    }

    SECTION("constrained decoding comes from the runtime, not the weights") {
        GgufMetadata metadata;
        const auto capabilities = ModelCapabilities::infer(metadata);
        CHECK(capabilities.supports_grammar);
        CHECK(capabilities.supports_json_schema);
    }
}

TEST_CASE("a profile built from metadata has sensible defaults", "[model]") {
    GgufMetadata metadata;
    metadata.path = "L:/models/some-model-Q4_K_M.gguf";
    metadata.name = "Some Model";
    metadata.architecture = "qwen2";
    metadata.quantization = "Q4_K_M";
    metadata.context_length = 131072;
    metadata.chat_template = "{%- if tools %}{{ tool_calls }}{%- endif %}";

    const auto profile = ModelProfile::from_gguf(metadata);

    CHECK_FALSE(profile.id.empty());
    CHECK(profile.display_name.find("Some Model") != std::string::npos);
    CHECK(profile.display_name.find("Q4_K_M") != std::string::npos);
    // The trained context is capped to something a workstation can hold.
    CHECK(profile.context_size == 32768);
    CHECK(profile.capabilities.supports_tools);
    CHECK(profile.tool_protocol == ToolProtocol::Native);
    CHECK(profile.backend == BackendPreference::Auto);
}

TEST_CASE("a model without tool support falls back to the text protocol", "[model]") {
    GgufMetadata metadata;
    metadata.path = "L:/models/plain.gguf";
    metadata.chat_template = "{{ messages }}";

    const auto profile = ModelProfile::from_gguf(metadata);
    CHECK_FALSE(profile.capabilities.supports_tools);
    CHECK(profile.tool_protocol == ToolProtocol::TextFallback);
    // Reasoning is not faked for a model that has none.
    CHECK_FALSE(profile.reasoning_enabled);
    CHECK_FALSE(profile.show_reasoning);
}

TEST_CASE("profile ids are stable and distinguish paths", "[model]") {
    const auto a = ModelProfile::make_id("L:/models/model.gguf");
    const auto b = ModelProfile::make_id("L:/models/model.gguf");
    const auto c = ModelProfile::make_id("D:/other/model.gguf");

    CHECK(a == b);
    CHECK(a != c);   // same filename, different location
    CHECK(a.find("model") != std::string::npos);
}

TEST_CASE("model and runtime paths survive UTF-8 JSON serialization", "[model][unicode]") {
    ModelProfile profile;
    profile.model_path = std::filesystem::path(u8"L:/modèles/日本語/été.gguf");
    profile.server_binary = std::filesystem::path(u8"L:/exécutables/llama-server.exe");
    profile.id = ModelProfile::make_id(profile.model_path);
    GgufMetadata metadata;
    metadata.path = profile.model_path;
    CHECK(ModelProfile::from_gguf(metadata).display_name ==
          path_to_utf8(profile.model_path.stem()));
    const auto wire = to_json(profile).dump();
    const auto restored = model_profile_from_json(nlohmann::json::parse(wire));
    CHECK(restored.model_path == profile.model_path);
    CHECK(restored.server_binary == profile.server_binary);
    CHECK(restored.id == profile.id);
}

TEST_CASE("a tiny context is raised to a usable floor", "[model]") {
    GgufMetadata metadata;
    metadata.path = "L:/models/tiny.gguf";
    metadata.context_length = 512;

    const auto profile = ModelProfile::from_gguf(metadata);
    CHECK(profile.context_size >= 4096);
}

TEST_CASE("backend preference parses the names users type", "[model][backend]") {
    CHECK(backend_preference_from_string("auto") == BackendPreference::Auto);
    CHECK(backend_preference_from_string("llama.cpp") == BackendPreference::LlamaCpp);
    CHECK(backend_preference_from_string("llamacpp") == BackendPreference::LlamaCpp);
    CHECK(backend_preference_from_string("ik_llama.cpp") == BackendPreference::IkLlama);
    CHECK(backend_preference_from_string("ik") == BackendPreference::IkLlama);
    CHECK(backend_preference_from_string("nonsense") == BackendPreference::Auto);
}

TEST_CASE("profile serialisation is lossless", "[model]") {
    ModelProfile profile;
    profile.id = "abc";
    profile.display_name = "Name";
    profile.model_path = "L:/m.gguf";
    profile.backend = BackendPreference::IkLlama;
    profile.context_size = 24576;
    profile.gpu_layers = 33;
    profile.threads = 4;
    profile.threads_batch = 8;
    profile.ubatch_size = 128;
    profile.ngram_speculation = true;
    profile.flash_attention = false;
    profile.cache_type_k = "q8_0";
    profile.tool_protocol = ToolProtocol::TextFallback;
    profile.native_reasoning_effort = "high";
    profile.sampling.temperature = 0.15f;
    profile.sampling.top_k = 20;
    profile.capabilities.supports_tools = true;

    const auto restored = model_profile_from_json(to_json(profile));

    CHECK(restored.id == profile.id);
    CHECK(restored.backend == profile.backend);
    CHECK(restored.context_size == profile.context_size);
    CHECK(restored.gpu_layers == profile.gpu_layers);
    CHECK(restored.threads == profile.threads);
    CHECK(restored.threads_batch == profile.threads_batch);
    CHECK(restored.ubatch_size == profile.ubatch_size);
    CHECK(restored.ngram_speculation);
    auto legacy = to_json(profile);
    legacy.erase("threads_batch");
    legacy.erase("ubatch_size");
    legacy.erase("ngram_speculation");
    CHECK(model_profile_from_json(legacy).threads_batch == 0);
    CHECK(model_profile_from_json(legacy).ubatch_size == 0);
    CHECK_FALSE(model_profile_from_json(legacy).ngram_speculation);
    CHECK(restored.flash_attention == profile.flash_attention);
    CHECK(restored.cache_type_k == profile.cache_type_k);
    CHECK(restored.tool_protocol == profile.tool_protocol);
    CHECK(restored.native_reasoning_effort == profile.native_reasoning_effort);
    CHECK(restored.sampling.temperature == profile.sampling.temperature);
    CHECK(restored.sampling.top_k == profile.sampling.top_k);
    CHECK(restored.capabilities.supports_tools);
}
