#include <catch2/catch_test_macros.hpp>
#include <algorithm>
#include <iterator>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <thread>
#include "inference/HttpClient.h"
#include "inference/OpenAiCompatBackend.h"
#include "platform/PathEncoding.h"
#include "platform/Paths.h"
#include "support/TempDirectory.h"
#include "inference/ServerManager.h"
#include "model/ModelProfile.h"
#include "platform/CpuPlacement.h"

using namespace localcode;

TEST_CASE("physical placement separates SMT and prefers available faster cores", "[cpu][placement]") {
    const std::vector<CpuLogicalProcessor> cores{
        {8,8,0}, {3,2,1}, {1,0,1}, {2,2,1}, {0,0,1}};
    const auto automatic = physical_cpu_placement(cores,0,0);
    REQUIRE(automatic);
    CHECK(automatic->threads == 2);
    CHECK(automatic->threads_batch == 2);
    CHECK(automatic->mask == "0x5");
    CHECK(automatic->mask_batch == "0x5");
    const auto different = physical_cpu_placement(cores,2,3);
    REQUIRE(different);
    CHECK(different->mask == "0x5");
    CHECK(different->mask_batch == "0x105");
    CHECK_FALSE(physical_cpu_placement(cores,4,0));
    CHECK_FALSE(physical_cpu_placement(cores,0,4));
    CHECK_FALSE(physical_cpu_placement(cores,1,2));
    CHECK_FALSE(physical_cpu_placement(cores,2,1));
    CHECK_FALSE(physical_cpu_placement({{0,0,0}},0,0));
}

TEST_CASE("physical placement handles packages and long masks without truncation", "[cpu][placement]") {
    const auto long_mask = physical_cpu_placement({{65,0x100000000ULL,0},{0,0,0}},2,0);
    REQUIRE(long_mask);
    CHECK(long_mask->mask == "0x20000000000000001");
    const auto unknown_classes = physical_cpu_placement({{5,2,0},{3,1,0},{1,0,0}},0,2);
    REQUIRE(unknown_classes);
    CHECK(unknown_classes->threads == 3);
    CHECK(unknown_classes->mask == "0x2a");
    CHECK(unknown_classes->mask_batch == "0xa");
    CHECK_FALSE(physical_cpu_placement({{1,0,0},{1,2,0}},1,0));
    CHECK_FALSE(physical_cpu_placement({{512,0,0}},1,0));
    CHECK_FALSE(physical_cpu_placement({},0,0));
    std::vector<CpuLogicalProcessor> many;
    for (std::uint32_t i=0;i<512;++i) many.push_back({i,i,0});
    const auto capped = physical_cpu_placement(many,0,0);
    REQUIRE(capped);
    CHECK(capped->threads == 256);
    CHECK(capped->mask == "0x" + std::string(64,'f'));
    CHECK_FALSE(physical_cpu_placement(many,257,0));
}

TEST_CASE("CPU placement profiles preserve legacy defaults and reject malformed choices", "[cpu][placement][model]") {
    const auto legacy = model_profile_from_json(Json::object());
    CHECK(legacy.cpu_placement == CpuPlacement::System);
    CHECK_FALSE(to_json(legacy).contains("cpu_placement"));
    auto profile = legacy;
    profile.cpu_placement = CpuPlacement::PhysicalCores;
    const auto serialized = to_json(profile);
    CHECK(serialized["cpu_placement"] == "physical_cores");
    CHECK(model_profile_from_json(serialized).cpu_placement == CpuPlacement::PhysicalCores);
    profile.cpu_placement = CpuPlacement::PhysicalPool;
    CHECK(to_json(profile)["cpu_placement"] == "physical_pool");
    CHECK(model_profile_from_json(to_json(profile)).cpu_placement == CpuPlacement::PhysicalPool);
    for (const auto& bad : {Json(true),Json(1),Json("unknown"),Json::array()})
        CHECK(model_profile_from_json(Json{{"cpu_placement",bad}}).cpu_placement == CpuPlacement::Invalid);
}

TEST_CASE("physical process pools support single workers and retain the fast core class", "[cpu][placement][pool]") {
    const std::vector<CpuLogicalProcessor> cores{{8,8,0},{3,2,1},{1,0,1},{2,2,1},{0,0,1}};
    const auto one = physical_cpu_pool(cores,1,0);
    REQUIRE(one);
    CHECK(one->threads == 1);
    CHECK(one->threads_batch == 1);
    CHECK(one->logical_processors == std::vector<std::uint32_t>{0,2});
    const auto automatic = physical_cpu_pool(cores,0,0);
    REQUIRE(automatic);
    CHECK(automatic->threads == 2);
    CHECK(automatic->logical_processors == one->logical_processors);
    const auto wider = physical_cpu_pool(cores,1,3);
    REQUIRE(wider);
    CHECK(wider->logical_processors == std::vector<std::uint32_t>{0,2,8});
    CHECK_FALSE(physical_cpu_pool(cores,4,0));
    CHECK_FALSE(physical_cpu_pool(cores,1,4));
    CHECK_FALSE(physical_cpu_pool({},0,0));
    CHECK_FALSE(physical_cpu_pool({{0,0,0},{0,1,0}},1,0));
    CHECK_FALSE(physical_cpu_pool({{1024,0,0}},1,0));
    const auto single = physical_cpu_pool({{900,1,0}},0,0);
    REQUIRE(single);
    CHECK(single->logical_processors == std::vector<std::uint32_t>{900});
    CHECK(single->threads == 1);
    std::vector<CpuLogicalProcessor> many;
    for (std::uint32_t cpu=0;cpu<512;++cpu) many.push_back({cpu,cpu,0});
    const auto capped = physical_cpu_pool(many,0,0);
    REQUIRE(capped);
    CHECK(capped->threads == 256);
    CHECK(capped->logical_processors.size() == 512);
    CHECK_FALSE(physical_cpu_pool(many,257,0));
}

TEST_CASE("process pool arguments require resolved threads without worker mask support", "[cpu][placement][pool][backend]") {
    ServerConfig config;
    config.profile.cpu_placement = CpuPlacement::PhysicalPool;
    ServerBinaryFeatures features;
    features.threads = features.threads_batch = true;
    CHECK(ModelServer::build_arguments(config,features,1234).empty());
    config.cpu_pool = CpuPoolPlan{1,2,{0,2}};
    const auto args = ModelServer::build_arguments(config,features,1234);
    REQUIRE_FALSE(args.empty());
    for (const auto& pair : {std::pair{"--threads","1"}, std::pair{"--threads-batch","2"}}) {
        const auto found = std::find(args.begin(),args.end(),pair.first);
        REQUIRE(found != args.end());
        REQUIRE(std::next(found) != args.end());
        CHECK(*std::next(found) == pair.second);
    }
    CHECK(std::none_of(args.begin(),args.end(),[](const auto& arg){return arg.starts_with("--cpu-");}));
    CHECK_FALSE(to_json(config.profile).contains("cpu_pool"));
    CHECK_FALSE(to_json(config.profile).contains("cpu_allowance"));
    features.threads_batch = false;
    CHECK(ModelServer::build_arguments(config,features,1234).empty());
}

TEST_CASE("server arguments use resolved masks without storing hardware indices in profiles", "[cpu][placement][backend]") {
    ServerConfig config;
    config.profile.cpu_placement = CpuPlacement::PhysicalCores;
    config.profile.threads = 2;
    ServerBinaryFeatures features;
    features.cpu_placement = true;
    CHECK(ModelServer::build_arguments(config,features,1234).empty());
    config.cpu_placement = CpuPlacementPlan{2,3,"0x5","0x15"};
    const auto args = ModelServer::build_arguments(config,features,1234);
    auto value = [&](const std::string& flag) {
        const auto found = std::find(args.begin(),args.end(),flag);
        REQUIRE(found != args.end());
        REQUIRE(std::next(found) != args.end());
        return *std::next(found);
    };
    CHECK(value("--threads") == "2");
    CHECK(value("--threads-batch") == "3");
    CHECK(value("--cpu-mask") == "0x5");
    CHECK(value("--cpu-mask-batch") == "0x15");
    CHECK(value("--cpu-strict") == "1");
    CHECK(value("--cpu-strict-batch") == "1");
    CHECK_FALSE(to_json(config.profile).contains("cpu_mask"));
    features.cpu_placement = false;
    CHECK(ModelServer::build_arguments(config,features,1234).empty());
    config.profile.cpu_placement = CpuPlacement::System;
    const auto system_args = ModelServer::build_arguments(config,features,1234);
    CHECK(std::find(system_args.begin(),system_args.end(),"--cpu-mask") == system_args.end());
}

TEST_CASE("physical placement launches through the native model manager", "[backend][placement][.runtime]") {
    const auto* runtime = std::getenv("LOCALCODE_PLACEMENT_RUNTIME");
    const auto* model = std::getenv("LOCALCODE_PLACEMENT_MODEL");
    const auto* output = std::getenv("LOCALCODE_PLACEMENT_REPORT");
    if (!runtime || !model || !output) SKIP("Set explicit public runtime/model and diagnostic report paths.");
    test::TempDirectory directory{"lc-placement-runtime"};
    const auto prior = app_paths().data_dir().parent_path();
    struct Restore { std::filesystem::path path; ~Restore(){app_paths().set_root(path);} } restore{prior};
    app_paths().set_root(directory.path());
    REQUIRE(app_paths().ensure_directories());
    const auto http = HttpClient::create();
    Json records = Json::array();
    Json reference, one_reference;
    for (int index=0;index<6;++index) {
        ServerConfig config;
        config.binary = path_from_utf8(runtime);
        config.profile.model_path = path_from_utf8(model);
        config.profile.id = "owned-placement";
        config.profile.gpu_layers = 0;
        config.profile.context_size = 512;
        config.profile.threads = config.profile.threads_batch = index==2 || index==5 ? 0 : index>=3 ? 1 : 2;
        config.profile.cpu_placement = index==0 || index==3 ? CpuPlacement::System
                                      : index>=4 ? CpuPlacement::PhysicalPool : CpuPlacement::PhysicalCores;
        config.profile.batch_size = config.profile.ubatch_size = 64;
        config.profile.prompt_cache_mib = 0;
        config.profile.cache_type_k = config.profile.cache_type_v = "f16";
        config.auto_restart = false;
        config.startup_timeout = std::chrono::seconds(90);
        config.extra_arguments = {"--no-warmup"};
        ModelServer server;
        REQUIRE(server.start(config,[http](const std::string& url,CancellationToken cancel) {
            BackendEndpoint endpoint; endpoint.base_url=url;
            return OpenAiCompatBackend{endpoint,http}.health(cancel);
        }));
        const auto pid=server.status().process_id;
        REQUIRE(pid>0);
        HttpRequest request;
        request.method=HttpMethod::Post;
        request.url=server.base_url()+"/completion";
        request.body=Json{{"prompt","Le serveur local lit le modèle puis répond avec une phrase simple :"},
                          {"n_predict",8},{"temperature",0},{"seed",42},{"ignore_eos",true},
                          {"return_tokens",true},{"cache_prompt",false},{"stream",false}}.dump();
        request.headers={{"Content-Type","application/json"}};
        request.timeout=std::chrono::seconds(30);
        const auto response=http->send(request);
        REQUIRE(response);
        INFO(response->body);
        REQUIRE(response->status_code==200);
        const auto reply=Json::parse(response->body);
        REQUIRE(reply["tokens_predicted"]==8);
        REQUIRE(reply["tokens"].size()==8);
        if (index==0) reference=reply["tokens"];
        if (index==1) CHECK(reply["tokens"]==reference);
        if (index==3) one_reference=reply["tokens"];
        if (index==4) CHECK(reply["tokens"]==one_reference);
        records.push_back(Json{{"mode",std::string(to_string(config.profile.cpu_placement))},
                               {"profileThreads",config.profile.threads},{"pid",pid},
                               {"tokens",reply["tokens"]},{"stopped",false}});
        std::ofstream(path_from_utf8(output))<<records.dump(2);
        // Give the external read-only observer time to inspect this owned child.
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        server.stop();
        CHECK(server.status().state==ServerState::Stopped);
        records.back()["stopped"]=true;
        std::ofstream(path_from_utf8(output))<<records.dump(2);
    }
}
