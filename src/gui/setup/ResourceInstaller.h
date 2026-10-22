#pragma once
#include <QList>
#include <QString>
#include "inference/HttpClient.h"

namespace localcode::gui {
struct ResourceAsset {
    QString name, url, sha256, source, revision, license;
    quint64 bytes = 0;
};

struct RuntimePackage {
    QString tag, flavour;
    QList<ResourceAsset> assets;
    quint64 bytes() const;
};

struct HubModels {
    QString repository, revision, license;
    QList<ResourceAsset> files;
};

using ResourceProgress = std::function<void(quint64, quint64)>;

// Explicit desktop setup actions. No bot token, model launch or settings write.
// Network metadata supplies the immutable revision, exact size and digest.
class ResourceInstaller {
public:
    explicit ResourceInstaller(std::shared_ptr<HttpClient> http = HttpClient::create(),
                               QString supportDirectory = {});
    Result<QList<RuntimePackage>> runtimes(const QString& tag, CancellationToken cancel = {}) const;
    Result<HubModels> models(const QString& repository, CancellationToken cancel = {}) const;
    Result<QString> installRuntime(const RuntimePackage& package, const QString& root,
                                   ResourceProgress progress = {},
                                   CancellationToken cancel = {}) const;
    Result<QString> installModel(const ResourceAsset& asset, const QString& root,
                                 ResourceProgress progress = {},
                                 CancellationToken cancel = {}) const;
    bool memoryRuntimeAvailable() const;
    Result<QString> prepareMemoryRuntime(const QString& server, const QString& root,
                                         ResourceProgress progress = {},
                                         CancellationToken cancel = {}) const;
    bool reasoningRuntimeAvailable() const;
    Result<QString> prepareReasoningRuntime(const QString& server, const QString& root,
                                            ResourceProgress progress = {},
                                            CancellationToken cancel = {}) const;
    static Status extractRuntimeZip(const QString& archive, const QString& emptyDirectory,
                                    CancellationToken cancel = {});

private:
    Result<QString> prepareRuntimeCopy(const QString& server, const QString& root,
                                       bool reasoning, ResourceProgress progress,
                                       CancellationToken cancel) const;
    Result<QList<RuntimePackage>> runtimesForRelease(const QString& tag,
                                                    CancellationToken cancel,
                                                    bool followBuildReference) const;
    Result<QByteArray> metadata(const QString& url, CancellationToken cancel) const;
    Status download(const ResourceAsset& asset, const QString& destination,
                    ResourceProgress progress, CancellationToken cancel) const;
    std::shared_ptr<HttpClient> http_;
    QString supportDirectory_;
};
}   // namespace localcode::gui
