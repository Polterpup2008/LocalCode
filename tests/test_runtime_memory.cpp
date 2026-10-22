#include <QApplication>
#include <QCheckBox>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPushButton>
#include <QTemporaryDir>
#include <QtEndian>
#include <catch2/catch_test_macros.hpp>
#include <thread>
#include <vector>
#include "dialogs/ResourceSetupDialog.h"
#include "setup/ResourceInstaller.h"
#include "inference/ServerManager.h"
#include "inference/BackendManager.h"
#include "inference/OpenAiCompatBackend.h"
#include "util/Strings.h"

using namespace localcode;
using namespace localcode::gui;

namespace {
void memoryWrite(const QString& path, const QByteArray& data) {
    REQUIRE(QDir().mkpath(QFileInfo(path).absolutePath()));
    QFile file(path); REQUIRE(file.open(QIODevice::WriteOnly));
    REQUIRE(file.write(data) == data.size());
}
QByteArray memoryRead(const QString& path) {
    QFile file(path); REQUIRE(file.open(QIODevice::ReadOnly)); return file.readAll();
}
QString memoryHash(const QByteArray& bytes) {
    return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}
QByteArray memoryPe(const QByteArray& tag) {
    QByteArray bytes(128, '\0');bytes[0]='M';bytes[1]='Z';
    qToLittleEndian<quint32>(64, bytes.data()+60);
    bytes.replace(64, 4, QByteArray("PE\0\0", 4));
    qToLittleEndian<quint16>(0x8664, bytes.data()+68);
    bytes += tag;return bytes;
}
struct MemoryFixture {
    QTemporaryDir temporary;
    QString source, support, root, server;
    QByteArray original = memoryPe("original"), replacement = memoryPe("replacement");
    explicit MemoryFixture(const QByteArray& executable = memoryPe("server")) {
        REQUIRE(temporary.isValid());
        const auto base = temporary.path()+QString::fromUtf8("/équipement");
        source=base+"/source";support=base+"/support";root=base+"/copies";
        server=source+"/llama-server.exe";
        memoryWrite(server, executable);memoryWrite(source+"/llama.dll", original);
        memoryWrite(source+"/ggml-base.dll", memoryPe("base"));
        memoryWrite(source+"/ggml.dll", memoryPe("ggml"));
        memoryWrite(source+"/ggml-cpu-x64.dll", memoryPe("cpu"));
        memoryWrite(source+"/msvcp140.dll", memoryPe("old compiler runtime"));
        memoryWrite(source+"/llama-quantize.exe", memoryPe("quantizer"));
        memoryWrite(source+"/other.exe", "unrelated program");
        memoryWrite(source+"/.env", "test-only secret; must not be copied");
        memoryWrite(source+"/model.gguf", "test-only model; must not be copied");
        const auto package=support+"/runtime-memory";
        memoryWrite(package+"/llama.dll", replacement);
        memoryWrite(package+"/msvcp140.dll", memoryPe("new compiler runtime"));
        QJsonArray files;
        for(const auto* name:{"llama.dll","msvcp140.dll"}) {
            const auto bytes=memoryRead(package+'/'+name);
            files.append(QJsonObject{{"name",name},{"sha256",memoryHash(bytes)},{"bytes",bytes.size()}});
        }
        const QJsonObject meta{{"format",1},{"id","localcode.runtime-memory"},{"architecture","windows-x64"},
            {"original_core_sha256",memoryHash(original)},{"base_sha256",memoryHash(memoryPe("base"))},
            {"ggml_sha256",memoryHash(memoryPe("ggml"))},
            {"server_sha256",memoryHash(executable)},{"files",files}};
        memoryWrite(package+"/manifest.json",QJsonDocument(meta).toJson());
        memoryWrite(package+"/LICENSE","fixture license");memoryWrite(package+"/README.md","fixture source notice");
        memoryWrite(package+"/windows-mmap-working-set.patch","fixture patch");
    }
    std::shared_ptr<ResourceInstaller> installer() const {
        return std::make_shared<ResourceInstaller>(HttpClient::create(),support);
    }
    void reasoningPackage() {
        memoryWrite(source+"/llama-common.dll", memoryPe("original common"));
        const auto folder=support+"/runtime-reasoning";
        QJsonArray rows;
        for(const auto* name:{"llama-common.dll","libssl-3-x64.dll","libcrypto-3-x64.dll"}) {
            const auto data=memoryPe(QByteArray("corrected ")+name);memoryWrite(folder+'/'+name,data);
            rows.append(QJsonObject{{"name",name},{"sha256",memoryHash(data)},{"bytes",data.size()}});
        }
        auto meta=QJsonDocument::fromJson(memoryRead(support+"/runtime-memory/manifest.json")).object();
        meta["id"]="localcode.runtime-reasoning";meta["files"]=rows;
        meta["original_common_sha256"]=memoryHash(memoryRead(source+"/llama-common.dll"));
        QJsonObject sources;
        for(const auto& file:QDir(source).entryInfoList(QDir::Files))
            if(file.suffix()=="dll"||file.suffix()=="exe")sources[file.fileName()]=memoryHash(memoryRead(file.absoluteFilePath()));
        meta["source_files"]=sources;memoryWrite(folder+"/manifest.json",QJsonDocument(meta).toJson());
        for(const auto* name:{"LICENSE","README.md","reasoning-prefill.patch","OPENSSL-LICENSE.txt"})
            memoryWrite(folder+'/'+name,"fixture notice");
    }
};
}

TEST_CASE("RAM runtime is optional and platform bounded", "[gui][setup][memory]") {
    QTemporaryDir empty;REQUIRE(empty.isValid());ResourceInstaller absent(HttpClient::create(),empty.path());
    CHECK_FALSE(absent.memoryRuntimeAvailable());
    CHECK_FALSE(absent.reasoningRuntimeAvailable());
#ifndef Q_OS_WIN
    CHECK(absent.prepareMemoryRuntime("unused",empty.path()).error().code==ErrorCode::NotSupported);
    CHECK(absent.prepareReasoningRuntime("unused",empty.path()).error().code==ErrorCode::NotSupported);
#endif
}

#ifdef Q_OS_WIN
TEST_CASE("Reasoning copy binds dependencies and preserves the original runtime", "[gui][setup][reasoning]") {
    MemoryFixture f;CHECK_FALSE(f.installer()->reasoningRuntimeAvailable());f.reasoningPackage();
    auto installer=f.installer();REQUIRE(installer->reasoningRuntimeAvailable());
    const auto original=memoryRead(f.source+"/llama-common.dll");
    const auto prepared=installer->prepareReasoningRuntime(f.server,f.root);REQUIRE(prepared);
    const auto directory=QFileInfo(*prepared).absolutePath();
    CHECK(memoryRead(directory+"/llama-common.dll")==memoryPe("corrected llama-common.dll"));
    CHECK(memoryRead(f.source+"/llama-common.dll")==original);
    CHECK(memoryRead(directory+"/llama.dll")==f.original);
    CHECK(QFileInfo::exists(directory+"/localcode-reasoning/OPENSSL-LICENSE.txt"));
    CHECK_FALSE(QFileInfo::exists(directory+"/.env"));CHECK_FALSE(QFileInfo::exists(directory+"/model.gguf"));
    const auto reused=installer->prepareReasoningRuntime(f.server,f.root);REQUIRE(reused);CHECK(*reused==*prepared);
    memoryWrite(directory+"/libssl-3-x64.dll",memoryPe("changed TLS"));
    CHECK_FALSE(installer->prepareReasoningRuntime(f.server,f.root));
}

TEST_CASE("Reasoning copy rejects changed and unknown dependencies before publication", "[gui][setup][reasoning]") {
    for(const auto* changed:{"llama-common.dll","llama.dll","ggml-cpu-x64.dll","ggml-cuda.dll"}) {
        MemoryFixture f;f.reasoningPackage();memoryWrite(f.source+'/'+changed,memoryPe("unqualified dependency"));
        const auto prepared=f.installer()->prepareReasoningRuntime(f.server,f.root);CHECK_FALSE(prepared);
        CHECK_FALSE(QFileInfo::exists(f.root));
    }
    MemoryFixture f;f.reasoningPackage();CancellationSource cancel;cancel.cancel();
    CHECK_FALSE(f.installer()->prepareReasoningRuntime(f.server,f.root,{},cancel.token()));
    CHECK_FALSE(QFileInfo::exists(f.root));
}

TEST_CASE("Setup runtime repairs are explicit independent choices", "[gui][setup][reasoning]") {
    MemoryFixture f;f.reasoningPackage();
    ResourceSetupDialog dialog({},f.root,f.temporary.path(),nullptr,f.installer());
    auto* memory=dialog.findChild<QCheckBox*>("SetupMemoryRuntime");
    auto* reasoning=dialog.findChild<QCheckBox*>("SetupReasoningRuntime");
    REQUIRE(memory);REQUIRE(reasoning);CHECK_FALSE(memory->isChecked());CHECK_FALSE(reasoning->isChecked());
    reasoning->setChecked(true);memory->setChecked(true);CHECK_FALSE(reasoning->isChecked());
    reasoning->setChecked(true);CHECK_FALSE(memory->isChecked());
}

TEST_CASE("Reasoning copy answers through managed LocalCode backend and streaming", "[gui][setup][reasoning][integration]") {
    const auto source=qEnvironmentVariable("LOCALCODE_TEST_REASONING_RUNTIME");
    const auto model=qEnvironmentVariable("LOCALCODE_TEST_REASONING_MODEL");
    if(source.isEmpty()||model.isEmpty())SKIP("requires an explicit isolated CPU runtime and public model");
    QTemporaryDir temporary;REQUIRE(temporary.isValid());ResourceInstaller installer;
    const auto prepared=installer.prepareReasoningRuntime(source,temporary.path()+"/runtimes");
    INFO((prepared?std::string{}:prepared.error().to_string()));REQUIRE(prepared);
    BackendManager manager;manager.set_http_client(HttpClient::create());
    manager.set_configured_binary(BackendPreference::LlamaCpp,std::filesystem::path(prepared->toStdWString()));
    manager.set_configured_binary(BackendPreference::IkLlama,std::filesystem::path(temporary.path().toStdWString())/"absent.exe");
    ModelProfile profile;profile.id="reasoning-copy-acceptance";profile.backend=BackendPreference::LlamaCpp;
    profile.model_path=std::filesystem::path(model.toStdWString());profile.gpu_layers=0;
    profile.context_size=2048;profile.threads=2;profile.threads_batch=2;
    profile.batch_size=128;profile.ubatch_size=64;profile.prompt_cache_mib=0;
    profile.cache_type_k="f16";profile.cache_type_v="f16";profile.flash_attention=true;
    profile.capabilities.supports_native_reasoning=true;profile.reasoning_enabled=false;
    const auto backend=manager.activate(profile);INFO((backend?"":backend.error().to_string()));REQUIRE(backend);
    const auto* compat=dynamic_cast<OpenAiCompatBackend*>((*backend).get());REQUIRE(compat);
    CHECK(compat->endpoint().native_reasoning_budget);
    Json results=Json::array();
    for(const bool stream:{false,true}) {
        ChatRequest request;request.messages={ChatMessage::system("Réponds uniquement dans le format demandé, sans préambule ni explication. Utilise les données fournies."),ChatMessage::user("Réponds uniquement avec le mot banane.")};
        request.thinking_override=false;request.stream=stream;request.sampling.temperature=0;
        request.sampling.max_tokens=16;request.timeout=std::chrono::seconds(60);
        const auto reply=(*backend)->chat(request,{},{});INFO((reply?"":reply.error().to_string()));REQUIRE(reply);
        CHECK(strings::trim(reply->message.content)=="banane");CHECK(reply->finish_reason=="stop");
        CHECK(reply->message.reasoning_content.empty());
        results.push_back({{"stream",stream},{"content",reply->message.content},{"finish",reply->finish_reason},{"tokens",reply->usage.completion_tokens}});
    }
    manager.deactivate();CHECK(manager.server_status().process_id==0);
    const auto receipt=qEnvironmentVariable("LOCALCODE_TEST_REASONING_RECEIPT");
    if(!receipt.isEmpty())memoryWrite(receipt,QByteArray::fromStdString(Json{{"runtime",prepared->toStdString()},{"model",model.toStdString()},{"results",results}}.dump(2)+'\n'));
}

TEST_CASE("RAM copy preserves source, stages dependencies and reuses verified cache", "[gui][setup][memory]") {
    MemoryFixture fixture;auto installer=fixture.installer();REQUIRE(installer->memoryRuntimeAvailable());
    const auto prepared=installer->prepareMemoryRuntime(fixture.server,fixture.root);REQUIRE(prepared);
    CHECK(*prepared!=fixture.server);const auto copy=QFileInfo(*prepared).absolutePath();
    CHECK(copy.startsWith(fixture.root+'/'));CHECK(memoryRead(copy+"/llama.dll")==fixture.replacement);
    CHECK(memoryRead(fixture.source+"/llama.dll")==fixture.original);
    CHECK(memoryRead(copy+"/ggml-cpu-x64.dll")==memoryRead(fixture.source+"/ggml-cpu-x64.dll"));
    CHECK(memoryRead(copy+"/msvcp140.dll")==memoryPe("new compiler runtime"));
    CHECK(memoryRead(fixture.source+"/msvcp140.dll")==memoryPe("old compiler runtime"));
    CHECK(QFileInfo::exists(copy+"/llama-quantize.exe"));CHECK_FALSE(QFileInfo::exists(copy+"/other.exe"));
    CHECK_FALSE(QFileInfo::exists(copy+"/.env"));CHECK_FALSE(QFileInfo::exists(copy+"/model.gguf"));
    CHECK(QFileInfo::exists(copy+"/localcode-memory/LICENSE"));
    const auto again=installer->prepareMemoryRuntime(fixture.server,fixture.root);REQUIRE(again);CHECK(*again==*prepared);
    memoryWrite(copy+"/ggml-cpu-x64.dll",memoryPe("modified private copy"));
    CHECK(memoryRead(fixture.source+"/ggml-cpu-x64.dll")==memoryPe("cpu"));
    const auto changed=installer->prepareMemoryRuntime(fixture.server,fixture.root);CHECK_FALSE(changed);
    CHECK(QDir(fixture.root).entryList({".memory-*"},QDir::Dirs|QDir::Hidden|QDir::NoDotAndDotDot).isEmpty());
}

TEST_CASE("RAM copy rejects incompatible and damaged input before adoption", "[gui][setup][memory]") {
    SECTION("incompatible provider") {
        MemoryFixture f;memoryWrite(f.source+"/ggml-base.dll",memoryPe("other ABI"));
        const auto out=f.installer()->prepareMemoryRuntime(f.server,f.root);REQUIRE_FALSE(out);
        CHECK(out.error().code==ErrorCode::NotSupported);CHECK_FALSE(QFileInfo::exists(f.root));
    }
    SECTION("modified package") {
        MemoryFixture f;auto corrupted=f.replacement;corrupted[corrupted.size()-1]='X';
        memoryWrite(f.support+"/runtime-memory/llama.dll",corrupted);
        const auto out=f.installer()->prepareMemoryRuntime(f.server,f.root);REQUIRE_FALSE(out);
        CHECK(out.error().code==ErrorCode::ParseError);CHECK_FALSE(QFileInfo::exists(f.root));
        CHECK(memoryRead(f.source+"/llama.dll")==f.original);
    }
    SECTION("invalid payload leaf name") {
        MemoryFixture f;const auto path=f.support+"/runtime-memory/manifest.json";
        auto meta=QJsonDocument::fromJson(memoryRead(path)).object();auto rows=meta["files"].toArray();
        auto row=rows[0].toObject();row["name"]="../llama.dll";rows[0]=row;meta["files"]=rows;
        memoryWrite(path,QJsonDocument(meta).toJson());CHECK_FALSE(f.installer()->memoryRuntimeAvailable());
        CHECK_FALSE(f.installer()->prepareMemoryRuntime(f.server,f.root));CHECK_FALSE(QFileInfo::exists(f.root));
    }
}

TEST_CASE("RAM copy upgrades only explicitly approved compatible cores", "[gui][setup][memory]") {
    MemoryFixture f;
    const auto older = memoryPe("previous RAM core");
    memoryWrite(f.source+"/llama.dll", older);
    const auto manifest = f.support+"/runtime-memory/manifest.json";
    auto metadata = QJsonDocument::fromJson(memoryRead(manifest)).object();
    CHECK_FALSE(f.installer()->prepareMemoryRuntime(f.server,f.root));
    CHECK_FALSE(QFileInfo::exists(f.root));
    metadata["compatible_core_sha256"] = QJsonArray{memoryHash(older).toUpper()};
    memoryWrite(manifest,QJsonDocument(metadata).toJson());
    SECTION("approved core keeps its source and adopts the verified payload") {
        REQUIRE(f.installer()->memoryRuntimeAvailable());
        const auto prepared=f.installer()->prepareMemoryRuntime(f.server,f.root);REQUIRE(prepared);
        CHECK(memoryRead(QFileInfo(*prepared).dir().filePath("llama.dll"))==f.replacement);
        CHECK(memoryRead(f.source+"/llama.dll")==older);
    }
    SECTION("a listed core cannot bypass another dependency ABI check") {
        memoryWrite(f.source+"/ggml.dll",memoryPe("other ggml ABI"));
        const auto prepared=f.installer()->prepareMemoryRuntime(f.server,f.root);REQUIRE_FALSE(prepared);
        CHECK(prepared.error().code==ErrorCode::NotSupported);
        CHECK_FALSE(QFileInfo::exists(f.root));
    }
}

TEST_CASE("Invalid RAM compatibility lists are rejected before staging", "[gui][setup][memory]") {
    QJsonArray oversized;
    for(int i=0;i<17;++i)oversized.append(QString(64,'a'));
    for(const auto& bad : std::vector<QJsonValue>{QString("not an array"),QJsonArray{},
            QJsonArray{QString("bad digest")},QJsonArray{123},oversized}) {
        MemoryFixture f;const auto manifest=f.support+"/runtime-memory/manifest.json";
        auto metadata=QJsonDocument::fromJson(memoryRead(manifest)).object();
        metadata["compatible_core_sha256"]=bad;memoryWrite(manifest,QJsonDocument(metadata).toJson());
        CHECK_FALSE(f.installer()->memoryRuntimeAvailable());
        const auto prepared=f.installer()->prepareMemoryRuntime(f.server,f.root);REQUIRE_FALSE(prepared);
        CHECK(prepared.error().code==ErrorCode::ParseError);
        CHECK_FALSE(QFileInfo::exists(f.root));
        CHECK(memoryRead(f.source+"/llama.dll")==f.original);
    }
}

TEST_CASE("Cancelling RAM copy cleans only its unfinished staging", "[gui][setup][memory]") {
    MemoryFixture f;memoryWrite(f.source+"/ggml-large.dll",QByteArray(2*1024*1024,'x'));
    CancellationSource cancel;
    const auto out=f.installer()->prepareMemoryRuntime(f.server,f.root,
        [&](quint64,quint64){cancel.cancel();},cancel.token());
    REQUIRE_FALSE(out);CHECK(out.error().code==ErrorCode::Cancelled);
    CHECK(QDir(f.root).entryList(QDir::AllEntries|QDir::Hidden|QDir::NoDotAndDotDot).isEmpty());
    CHECK(memoryRead(f.source+"/llama.dll")==f.original);
}

TEST_CASE("Setup prepares RAM runtime only after an explicit choice", "[gui][setup][memory]") {
    const auto executable=QDir(QCoreApplication::applicationDirPath()).filePath("localcode_benchmark_fixture.exe");
    REQUIRE(QFileInfo::exists(executable));MemoryFixture f(memoryRead(executable));
    // This case executes the fixture. Its CRT dependencies must be genuine;
    // the other copy-only cases deliberately use small non-runnable PE fixtures.
    for(const auto* name:{"msvcp140.dll","vcruntime140.dll","vcruntime140_1.dll"}) {
        const auto bytes=memoryRead(QDir(QCoreApplication::applicationDirPath()).filePath(name));
        memoryWrite(f.source+'/'+name,bytes);
        if(QString(name)=="msvcp140.dll") {
            memoryWrite(f.support+"/runtime-memory/"+name,bytes);
            const auto manifest=f.support+"/runtime-memory/manifest.json";
            auto meta=QJsonDocument::fromJson(memoryRead(manifest)).object();auto rows=meta["files"].toArray();
            auto row=rows[1].toObject();row["sha256"]=memoryHash(bytes);row["bytes"]=bytes.size();rows[1]=row;meta["files"]=rows;
            memoryWrite(manifest,QJsonDocument(meta).toJson());
        }
    }
    const auto model=f.temporary.path()+"/model.gguf";
    memoryWrite(model,QByteArray::fromHex("474755460300000000000000000000000000000000000000"));
    const auto selectedModel=QDir::current().relativeFilePath(model);
    for(const bool choose:{false,true}) {
        ResourceSetupDialog dialog({f.server,selectedModel,{},false},f.root,f.temporary.path(),nullptr,f.installer());
        dialog.show();QApplication::processEvents();auto* check=dialog.findChild<QCheckBox*>("SetupMemoryRuntime");
        REQUIRE(check);CHECK(check->isVisible());CHECK_FALSE(check->isChecked());check->setChecked(choose);
        auto* use=dialog.findChild<QPushButton*>("SetupUseFiles");REQUIRE(use);REQUIRE(use->isEnabled());use->click();
        QElapsedTimer timer;timer.start();
        while(dialog.isVisible()&&timer.elapsed()<10000) {QApplication::processEvents();std::this_thread::sleep_for(std::chrono::milliseconds(10));}
        REQUIRE(dialog.result()==QDialog::Accepted);
        CHECK(dialog.selection().model==(choose?QFileInfo(model).absoluteFilePath():selectedModel));
        CHECK_FALSE(dialog.selection().cpuStartingProfile);
        if(choose)CHECK(dialog.selection().runtime!=f.server);
        else {CHECK(dialog.selection().runtime==f.server);CHECK_FALSE(QFileInfo::exists(f.root));}
    }
}

TEST_CASE("RAM copy loads and generates through the real model manager", "[gui][setup][memory][integration]") {
    const auto source=qEnvironmentVariable("LOCALCODE_TEST_MEMORY_RUNTIME");
    const auto model=qEnvironmentVariable("LOCALCODE_TEST_MEMORY_MODEL");
    if(source.isEmpty()||model.isEmpty())SKIP("requires an explicitly supplied isolated runtime and public diagnostic model");
    QTemporaryDir temporary;REQUIRE(temporary.isValid());ResourceInstaller installer;
    REQUIRE(installer.memoryRuntimeAvailable());
    const auto prepared=installer.prepareMemoryRuntime(source,temporary.path()+"/runtimes");REQUIRE(prepared);
    const auto originalCore=memoryHash(memoryRead(QFileInfo(source).dir().filePath("llama.dll")));
    Json receipt={{"source",source.toStdString()},{"prepared",prepared->toStdString()},
                   {"gpu_layers",qEnvironmentVariableIntValue("LOCALCODE_TEST_MEMORY_GPU_LAYERS")},{"runs",Json::array()}};
    Json first;
    for(const auto& executable:{source,*prepared}) {
        ModelServer server;ServerConfig config;
        config.binary=std::filesystem::path(executable.toStdWString());
        config.profile.id="memory-copy-acceptance";
        config.profile.model_path=std::filesystem::path(model.toStdWString());
        config.profile.context_size=512;config.profile.threads=2;config.profile.threads_batch=2;
        config.profile.batch_size=128;config.profile.ubatch_size=64;config.profile.prompt_cache_mib=0;
        config.profile.cache_type_k="f16";config.profile.cache_type_v="f16";
        config.profile.gpu_layers=qEnvironmentVariableIntValue("LOCALCODE_TEST_MEMORY_GPU_LAYERS");
        config.startup_timeout=std::chrono::seconds(60);config.auto_restart=false;config.max_restarts=0;
        config.extra_arguments={"--no-warmup"};
        const auto http=HttpClient::create();
        const auto started=server.start(config,[http](const std::string& base,CancellationToken cancel)->Status {
            HttpRequest request;request.url=base+"/health";request.connect_timeout=std::chrono::milliseconds(500);
            request.timeout=std::chrono::seconds(1);request.max_response_bytes=2048;
            const auto response=http->send(request,cancel);if(!response)return response.error();
            if(!response->ok())return make_error(ErrorCode::BackendError,"diagnostic model not ready");
            return ok_status();
        });
        INFO((started?std::string{}:started.error().to_string()));REQUIRE(started);REQUIRE(server.ready());
        const auto pid=server.status().process_id;
        HttpRequest request;request.method=HttpMethod::Post;request.url=server.base_url()+"/completion";
        request.headers={{"Content-Type","application/json"}};request.timeout=std::chrono::seconds(60);
        request.max_response_bytes=65536;
        std::vector<int> tokens;for(int i=1;i<=31;++i)tokens.push_back(i);
        request.body=Json{{"prompt",tokens},{"n_predict",8},{"temperature",0},{"seed",42},
            {"return_tokens",true},{"cache_prompt",false},{"ignore_eos",true},{"n_probs",0}}.dump();
        const auto response=http->send(request);REQUIRE(response);REQUIRE(response->ok());
        const auto reply=Json::parse(response->body);
        const Json signature={{"content",reply.at("content")},{"tokens",reply.at("tokens")},
                               {"count",reply.at("tokens_predicted")}};
        REQUIRE(signature.at("count")==8);REQUIRE(signature.at("tokens").size()==8);
        if(first.is_null())first=signature;else CHECK(signature==first);
        receipt["runs"].push_back({{"executable",executable.toStdString()},{"pid",pid},{"signature",signature}});
        server.stop();CHECK_FALSE(server.ready());
    }
    CHECK(memoryHash(memoryRead(QFileInfo(source).dir().filePath("llama.dll")))==originalCore);
    const auto output=qEnvironmentVariable("LOCALCODE_TEST_MEMORY_RECEIPT");
    if(!output.isEmpty())memoryWrite(output,QByteArray::fromStdString(receipt.dump(2)+'\n'));
}
#endif
