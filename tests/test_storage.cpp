// SQLite schema, transactions and the repositories.
#include <catch2/catch_test_macros.hpp>

#include "storage/Repositories.h"
#include "support/TempDirectory.h"

using namespace localcode;
using localcode::test::TempDirectory;

namespace {

std::unique_ptr<Database> open_memory_database() {
    auto db = std::make_unique<Database>();
    REQUIRE(db->open_in_memory().ok());
    return db;
}

}  // namespace

TEST_CASE("the schema is created and migrations are idempotent", "[storage]") {
    TempDirectory temp{"lc-db"};
    const auto path = temp.path() / "test.sqlite3";

    {
        Database db;
        REQUIRE(db.open(path).ok());
        auto version = db.schema_version();
        REQUIRE(version.ok());
        CHECK(*version > 0);
    }
    // Re-opening must not re-run the migrations.
    {
        Database db;
        REQUIRE(db.open(path).ok());
        auto integrity = db.integrity_check();
        REQUIRE(integrity.ok());
        CHECK(*integrity == "ok");
    }
}

TEST_CASE("settings round-trip with types", "[storage]") {
    auto db = open_memory_database();
    SettingsRepository settings{*db};

    REQUIRE(settings.set("a.string", "hello").ok());
    CHECK(settings.get("a.string").value_or("") == "hello");
    CHECK(settings.get("missing.key", "fallback").value_or("") == "fallback");

    REQUIRE(settings.set_bool("a.bool", true).ok());
    CHECK(settings.get_bool("a.bool", false));
    CHECK(settings.get_bool("absent.bool", true));

    REQUIRE(settings.set_int("a.int", 4242).ok());
    CHECK(settings.get_int("a.int", 0) == 4242);

    // Setting again overwrites rather than duplicating.
    REQUIRE(settings.set("a.string", "changed").ok());
    CHECK(settings.get("a.string").value_or("") == "changed");

    auto all = settings.all();
    REQUIRE(all.ok());
    CHECK(all->size() == 3);
}

TEST_CASE("a rejected settings batch preserves all existing settings", "[storage][settings]") {
    auto db = open_memory_database();
    SettingsRepository settings{*db};
    REQUIRE(settings.set_all({{"first", "old"}, {"second", "old"}}));
    REQUIRE(
        db->execute("CREATE TRIGGER reject_setting BEFORE INSERT ON settings "
                    "WHEN NEW.key='second' BEGIN SELECT RAISE(ABORT,'fixture failure'); END"));
    CHECK_FALSE(settings.set_all({{"first", "new"}, {"second", "new"}}));
    CHECK(settings.get("first").value_or("") == "old");
    CHECK(settings.get("second").value_or("") == "old");
    REQUIRE(db->execute("DROP TRIGGER reject_setting"));
    REQUIRE(settings.set_all({{"first", "new"}, {"second", "new"}}));
    CHECK(settings.get("first").value_or("") == "new");
    CHECK(settings.get("second").value_or("") == "new");
}

TEST_CASE("workspaces are recorded and ordered by recency", "[storage]") {
    auto db = open_memory_database();
    WorkspaceRepository workspaces{*db};

    auto first = workspaces.touch("C:/projects/alpha", "alpha");
    REQUIRE(first.ok());
    CHECK(first->open_count == 1);

    auto again = workspaces.touch("C:/projects/alpha", "alpha");
    REQUIRE(again.ok());
    CHECK(again->open_count == 2);
    CHECK(again->id == first->id);

    REQUIRE(workspaces.touch("C:/projects/beta", "beta").ok());

    auto recent = workspaces.recent(10);
    REQUIRE(recent.ok());
    CHECK(recent->size() == 2);
}

TEST_CASE("conversations store messages in order with tool calls", "[storage]") {
    auto db = open_memory_database();
    ConversationRepository conversations{*db};

    auto id = conversations.create(0, "Fix the renderer", "test-model");
    REQUIRE(id.ok());

    REQUIRE(conversations.append_message(*id, ChatMessage::user("fix the bug")).ok());

    ChatMessage assistant = ChatMessage::assistant("Looking at it.");
    ToolCall call;
    call.id = "call_1";
    call.name = "read_file";
    call.arguments_json = R"({"path":"src/main.cpp"})";
    assistant.tool_calls.push_back(call);
    REQUIRE(conversations.append_message(*id, assistant).ok());

    REQUIRE(conversations.append_message(
                    *id, ChatMessage::tool_result("call_1", "read_file", "file contents"))
                .ok());

    auto messages = conversations.messages(*id);
    REQUIRE(messages.ok());
    REQUIRE(messages->size() == 3);

    CHECK((*messages)[0].message.role == Role::User);
    CHECK((*messages)[1].message.role == Role::Assistant);
    REQUIRE((*messages)[1].message.tool_calls.size() == 1);
    CHECK((*messages)[1].message.tool_calls[0].name == "read_file");
    CHECK((*messages)[2].message.role == Role::Tool);
    CHECK((*messages)[2].message.tool_call_id == "call_1");

    // Sequence numbers must be strictly increasing.
    CHECK((*messages)[0].sequence < (*messages)[1].sequence);
    CHECK((*messages)[1].sequence < (*messages)[2].sequence);

    auto count = conversations.message_count(*id);
    REQUIRE(count.ok());
    CHECK(*count == 3);
}

TEST_CASE("internal feedback retains its provenance after storage", "[storage][feedback]") {
    auto db = open_memory_database();
    ConversationRepository conversations{*db};
    const auto id = conversations.create(0, "File-only change", "test-model");
    REQUIRE(id);
    const auto human = ChatMessage::user("Edit the file without running a command.");
    const auto feedback = ChatMessage::application_feedback("Report missing verification.");
    REQUIRE(conversations.append_message(*id, human));
    REQUIRE(conversations.append_message(*id, feedback));
    const auto restored = conversations.messages(*id);
    REQUIRE(restored);
    REQUIRE(restored->size() == 2);
    CHECK_FALSE((*restored)[0].message.is_application_feedback());
    CHECK((*restored)[0].message.content == human.content);
    CHECK((*restored)[1].message.is_application_feedback());
    CHECK((*restored)[1].message.content == feedback.content);
}

TEST_CASE("full-text search finds messages", "[storage]") {
    auto db = open_memory_database();
    ConversationRepository conversations{*db};

    auto id = conversations.create(0, "session", "model");
    REQUIRE(id.ok());
    REQUIRE(conversations.append_message(
                    *id, ChatMessage::user("the rasterizer produces black pixels"))
                .ok());
    REQUIRE(conversations.append_message(*id, ChatMessage::user("unrelated chatter")).ok());

    auto hits = conversations.search("rasterizer");
    REQUIRE(hits.ok());
    REQUIRE(hits->size() == 1);
    CHECK((*hits)[0].message.content.find("rasterizer") != std::string::npos);
}

TEST_CASE("deleting a conversation removes its messages", "[storage]") {
    auto db = open_memory_database();
    ConversationRepository conversations{*db};

    auto id = conversations.create(0, "temp", "model");
    REQUIRE(id.ok());
    REQUIRE(conversations.append_message(*id, ChatMessage::user("hello")).ok());
    REQUIRE(conversations.remove(*id).ok());

    auto messages = conversations.messages(*id);
    REQUIRE(messages.ok());
    CHECK(messages->empty());
}

TEST_CASE("model profiles survive a round trip", "[storage]") {
    auto db = open_memory_database();
    ModelProfileRepository models{*db};

    ModelProfile profile;
    profile.id = "test-model-abc";
    profile.display_name = "Test Model (Q4_K_M)";
    profile.model_path = "L:/models/test.gguf";
    profile.context_size = 16384;
    profile.gpu_layers = 40;
    profile.backend = BackendPreference::IkLlama;
    profile.capabilities.supports_tools = true;
    profile.capabilities.supports_native_reasoning = true;
    profile.capabilities.evidence = "chat template renders tool calls";
    profile.sampling.temperature = 0.3f;
    profile.sampling.stop_sequences = {"<|end|>"};
    profile.extra_server_arguments = {"--parallel", "2"};

    REQUIRE(models.save(profile).ok());

    auto loaded = models.find(profile.id);
    REQUIRE(loaded.ok());
    REQUIRE(loaded->has_value());

    const auto& restored = **loaded;
    CHECK(restored.id == profile.id);
    CHECK(restored.display_name == profile.display_name);
    CHECK(restored.context_size == 16384);
    CHECK(restored.gpu_layers == 40);
    CHECK(restored.backend == BackendPreference::IkLlama);
    CHECK(restored.capabilities.supports_tools);
    CHECK(restored.capabilities.supports_native_reasoning);
    CHECK(restored.sampling.temperature == 0.3f);
    REQUIRE(restored.sampling.stop_sequences.size() == 1);
    REQUIRE(restored.extra_server_arguments.size() == 2);
}

TEST_CASE("a transaction rolls back when it is not committed", "[storage]") {
    auto db = open_memory_database();
    SettingsRepository settings{*db};

    REQUIRE(settings.set("keep", "yes").ok());
    {
        Transaction transaction{*db};
        REQUIRE(settings.set("rolled.back", "value").ok());
        // Leaving scope without commit() must undo it.
    }
    CHECK(settings.get("rolled.back", "absent").value_or("") == "absent");
    CHECK(settings.get("keep").value_or("") == "yes");
}

TEST_CASE("a committed transaction persists", "[storage]") {
    auto db = open_memory_database();
    SettingsRepository settings{*db};

    auto status = db->with_transaction([&] { return settings.set("committed", "value"); });
    REQUIRE(status.ok());
    CHECK(settings.get("committed").value_or("") == "value");
}

TEST_CASE("a failed transaction start never executes its write callback", "[storage]") {
    auto db = open_memory_database();
    REQUIRE(db->execute("BEGIN IMMEDIATE"));
    bool entered = false;
    CHECK_FALSE(db->with_transaction([&] {
        entered = true;
        return db->execute(
            "INSERT INTO app_state(key,value,updated_at) VALUES('unsafe','write',0)");
    }));
    CHECK_FALSE(entered);
    REQUIRE(db->execute("COMMIT"));
    AppStateRepository state{*db};
    CHECK(state.get("unsafe").value_or("").empty());
}

TEST_CASE("drafts are stored per workspace", "[storage]") {
    auto db = open_memory_database();
    DraftRepository drafts{*db};

    REQUIRE(drafts.save("C:/a", "unsent text for a").ok());
    REQUIRE(drafts.save("C:/b", "unsent text for b").ok());

    CHECK(drafts.load("C:/a").value_or("") == "unsent text for a");
    CHECK(drafts.load("C:/b").value_or("") == "unsent text for b");
    CHECK(drafts.load("C:/never-seen").value_or("").empty());

    REQUIRE(drafts.clear("C:/a").ok());
    CHECK(drafts.load("C:/a").value_or("").empty());
}

TEST_CASE("tool history is recorded and read back newest first", "[storage]") {
    auto db = open_memory_database();
    ToolHistoryRepository history{*db};

    for (int i = 0; i < 3; ++i) {
        ToolHistoryRecord record;
        record.tool_name = "run_command";
        record.arguments_json = R"({"command":["cmake"]})";
        record.succeeded = i % 2 == 0;
        record.summary = "invocation " + std::to_string(i);
        record.duration_ms = 100 + i;
        REQUIRE(history.append(record).ok());
    }

    auto recent = history.recent(10);
    REQUIRE(recent.ok());
    REQUIRE(recent->size() == 3);
    CHECK((*recent)[0].summary == "invocation 2");
}

TEST_CASE("task state is persisted alongside a conversation", "[storage]") {
    auto db = open_memory_database();
    ConversationRepository conversations{*db};
    TaskRepository tasks{*db};

    auto conversation = conversations.create(0, "task", "model");
    REQUIRE(conversation.ok());

    auto id = tasks.create(*conversation, "fix the build", R"({"objective":"fix the build"})");
    REQUIRE(id.ok());
    REQUIRE(tasks.update(*id, R"({"objective":"fix the build","iterations":4})", "active").ok());

    auto latest = tasks.latest_for_conversation(*conversation);
    REQUIRE(latest.ok());
    REQUIRE(latest->has_value());
    CHECK((*latest)->objective == "fix the build");
    CHECK((*latest)->state_json.find("iterations") != std::string::npos);
}

// ---------------------------------------------------------------------------
// Persisting a conversation repeatedly must not duplicate it.
//
// The GUI mirrors the agent's history into storage after every turn so that a
// crash loses nothing. The first implementation appended every message every
// time: after three exchanges a six-message conversation had fourteen rows, the
// growth was quadratic in turns, and a restored conversation showed its opening
// exchange four times over. Found by reading the database after a real GUI
// session, not by any test.
// ---------------------------------------------------------------------------

TEST_CASE("re-persisting a growing conversation appends only what is new",
          "[storage][conversation]") {
    auto db = open_memory_database();
    ConversationRepository conversations{*db};

    auto id = conversations.create(0, "Session", "test-model");
    REQUIRE(id.ok());

    // The same operation the GUI performs: given the whole history, store the
    // part that is not stored yet.
    auto persist = [&](const std::vector<ChatMessage>& history) {
        auto counted = conversations.message_count(*id);
        REQUIRE(counted.ok());
        for (std::size_t i = static_cast<std::size_t>(*counted); i < history.size(); ++i) {
            REQUIRE(conversations.append_message(*id, history[i]).ok());
        }
    };

    std::vector<ChatMessage> history;

    history.push_back(ChatMessage::user("hello"));
    history.push_back(ChatMessage::assistant("hi"));
    persist(history);

    history.push_back(ChatMessage::user("what is 6 times 7"));
    history.push_back(ChatMessage::assistant("42"));
    persist(history);

    history.push_back(ChatMessage::user("thanks"));
    history.push_back(ChatMessage::assistant("welcome"));
    persist(history);

    // Six messages, six rows. The bug produced fourteen.
    auto count = conversations.message_count(*id);
    REQUIRE(count.ok());
    CHECK(*count == 6);

    auto stored = conversations.messages(*id);
    REQUIRE(stored.ok());
    REQUIRE(stored->size() == 6);
    CHECK((*stored)[0].message.content == "hello");
    CHECK((*stored)[2].message.content == "what is 6 times 7");
    CHECK((*stored)[4].message.content == "thanks");

    // Persisting again with nothing new must write nothing at all.
    persist(history);
    count = conversations.message_count(*id);
    REQUIRE(count.ok());
    CHECK(*count == 6);
}
