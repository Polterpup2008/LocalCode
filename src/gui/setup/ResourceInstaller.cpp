#include "ResourceInstaller.h"
#include <miniz.h>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStorageInfo>
#include <QTemporaryDir>
#include <QUrl>
#include <QUuid>
#include <QtEndian>
#include <algorithm>
#include <limits>
#include <set>
#include "model/GgufMetadata.h"

namespace localcode::gui {
namespace {
bool matches(const QString& text, const char* expression) {
    return QRegularExpression(QString::fromLatin1(expression)).match(text).hasMatch();
}

bool digest(const QString& text) {
    return matches(text, "^[a-fA-F0-9]{64}$");
}

bool safePath(const QString& path) {
    if (path.isEmpty() || path.size() > 480 || path.startsWith('/') || path.contains('\\'))
        return false;
    for (const auto& part : path.split('/')) {
        if (!matches(part, "^[A-Za-z0-9_. -]{1,128}$") || part == "." || part == ".." ||
            part.endsWith('.') || part.endsWith(' ') ||
            matches(part, "(?i)^(con|prn|aux|nul|com[1-9]|lpt[1-9])(\\.|$)"))
            return false;
    }
    return true;
}

std::filesystem::path nativePath(const QString& text) {
#ifdef Q_OS_WIN
    return std::filesystem::path(text.toStdWString());
#else
    return std::filesystem::path(text.toStdString());
#endif
}

QString component(const QString& text) {
    return QString::fromLatin1(QUrl::toPercentEncoding(text));
}

QString encodedPath(const QString& text) {
    QStringList parts;
    for (const auto& part : text.split('/'))
        parts << component(part);
    return parts.join('/');
}

Status diskRoom(const QString& root, quint64 needed) {
    if (!QDir().mkpath(root))
        return make_error(ErrorCode::IoError, "Cannot create the resource folder.");
    const QStorageInfo storage(root);
    if (storage.isValid() && storage.isReady() && storage.bytesAvailable() >= 0 &&
        static_cast<quint64>(storage.bytesAvailable()) < needed)
        return make_error(ErrorCode::IoError, "Insufficient free disk space for this download.");
    return ok_status();
}

Status receipt(const QString& directory, const QList<ResourceAsset>& assets) {
    QJsonArray files;
    for (const auto& asset : assets)
        files.append(QJsonObject{{"name", asset.name},
                                 {"url", asset.url},
                                 {"sha256", asset.sha256},
                                 {"bytes", static_cast<qint64>(asset.bytes)},
                                 {"revision", asset.revision},
                                 {"source", asset.source},
                                 {"license", asset.license}});
    QSaveFile file(QDir(directory).filePath("download.json"));
    if (!file.open(QIODevice::WriteOnly) || file.write(QJsonDocument(files).toJson()) < 0 ||
        !file.commit())
        return make_error(ErrorCode::IoError, "Cannot save download provenance.");
    return ok_status();
}

#ifdef Q_OS_WIN
// Read only the bounded PE headers, never load an untrusted downloaded image.
quint16 peMachine(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return 0;
    const auto dos = file.read(64);
    if (dos.size() != 64 || dos.first(2) != "MZ") return 0;
    const auto offset = qFromLittleEndian<quint32>(dos.constData() + 60);
    if (offset < 64 || offset > 1024 * 1024 || !file.seek(offset)) return 0;
    const auto pe = file.read(6);
    if (pe.size() != 6 || pe.first(4) != QByteArray("PE\0\0", 4)) return 0;
    return qFromLittleEndian<quint16>(pe.constData() + 4);
}

Status stageCompilerRuntime(const QString& directory, const QString& supportDirectory,
                            CancellationToken cancel) {
    const auto machine = peMachine(QDir(directory).filePath("llama-server.exe"));
    if (machine == 0 || machine != peMachine(QCoreApplication::applicationFilePath()))
        return ok_status();
    QJsonArray copied;
    // These release DLLs come from LocalCode's compiler redistributable, not PATH.
    const QDir support(supportDirectory);
    for (const auto& entry : support.entryInfoList(
             {"msvcp140*.dll", "vcruntime140*.dll", "concrt140.dll"}, QDir::Files, QDir::Name)) {
        if (cancel.cancelled()) return make_error(ErrorCode::Cancelled, "Installation cancelled.");
        const auto target = QDir(directory).filePath(entry.fileName());
        if (QFileInfo(target).isFile()) continue; // Preserve provider-supplied dependencies.
        if (entry.isSymLink() || peMachine(entry.absoluteFilePath()) != machine) continue;
        if (!QFile::copy(entry.absoluteFilePath(), target))
            return make_error(ErrorCode::IoError, "Cannot stage compiler runtime dependencies.");
        QFile file(target);
        QCryptographicHash hash(QCryptographicHash::Sha256);
        if (!file.open(QIODevice::ReadOnly) || !hash.addData(&file))
            return make_error(ErrorCode::IoError, "Cannot verify compiler runtime dependencies.");
        copied.append(QJsonObject{{"name", entry.fileName()}, {"bytes", file.size()},
                                  {"sha256", QString::fromLatin1(hash.result().toHex())}});
    }
    if (copied.isEmpty()) return ok_status();
    // Keep local additions distinct from the downloaded archive's digest.
    QSaveFile receipt(QDir(directory).filePath("localcode-compiler-runtime.json"));
    if (!receipt.open(QIODevice::WriteOnly) ||
        receipt.write(QJsonDocument(copied).toJson()) < 0 || !receipt.commit())
        return make_error(ErrorCode::IoError, "Cannot save compiler runtime provenance.");
    return ok_status();
}
#endif
}   // namespace

quint64 RuntimePackage::bytes() const {
    quint64 out = 0;
    for (const auto& asset : assets)
        out += asset.bytes;
    return out;
}

ResourceInstaller::ResourceInstaller(std::shared_ptr<HttpClient> http, QString supportDirectory)
    : http_(std::move(http)),
      supportDirectory_(supportDirectory.isEmpty() ? QCoreApplication::applicationDirPath()
                                                  : std::move(supportDirectory)) {}

Result<QByteArray> ResourceInstaller::metadata(const QString& url, CancellationToken cancel) const {
    if (!http_) return make_error(ErrorCode::NotSupported, "No HTTP transport is available.");
    HttpRequest request;
    request.url = url.toStdString();
    request.max_response_bytes = 4 * 1024 * 1024;
    request.timeout = std::chrono::seconds(30);
    request.headers = {{"Accept", "application/json"},
                       {"User-Agent", "LocalCode-setup/" LOCALCODE_VERSION}};
    const auto response = http_->send(request, cancel);
    if (!response) return response.error();
    if (!response->ok() || response->truncated)
        return make_error(ErrorCode::NetworkError,
                          "Cannot read public download metadata (HTTP " +
                              std::to_string(response->status_code) +
                              "). Check the name, network or API rate limit.");
    return QByteArray::fromStdString(response->body);
}

Result<QList<RuntimePackage>> ResourceInstaller::runtimes(const QString& tag,
                                                          CancellationToken cancel) const {
    return runtimesForRelease(tag, cancel, true);
}

Result<QList<RuntimePackage>> ResourceInstaller::runtimesForRelease(
    const QString& tag, CancellationToken cancel, bool followBuildReference) const {
    if (cancel.cancelled()) return make_error(ErrorCode::Cancelled, "Download lookup cancelled.");
    if (!matches(tag, "^[A-Za-z0-9_.-]{1,64}$"))
        return make_error(ErrorCode::InvalidArgument,
                          "Enter a llama.cpp release tag, such as b10655 or v0.6.0.");
    const auto body = metadata(
        "https://api.github.com/repos/ggml-org/llama.cpp/releases/tags/" + component(tag), cancel);
    if (!body) return body.error();
    const auto document = QJsonDocument::fromJson(*body);
    const auto object = document.object();
    if (!document.isObject() || object.value("tag_name").toString() != tag)
        return make_error(ErrorCode::ParseError,
                          "The release metadata does not match the requested build.");
    QList<ResourceAsset> assets;
    const auto prefix = "https://github.com/ggml-org/llama.cpp/releases/download/" + tag + '/';
    for (const auto& value : object.value("assets").toArray()) {
        const auto row = value.toObject();
        const auto name = row.value("name").toString();
        const auto hash = row.value("digest").toString();
        const auto bytes = row.value("size").toInteger();
        const auto url = row.value("browser_download_url").toString();
        if (!safePath(name) || name.contains('/') || !hash.startsWith("sha256:") ||
            !digest(hash.mid(7)) || bytes <= 0 || bytes > 1024ll * 1024 * 1024 ||
            url != prefix + name)
            continue;
        assets.append({name, url, hash.mid(7).toLower(),
                       "https://github.com/ggml-org/llama.cpp/releases/tag/" + tag, tag,
                       "MIT; bundled dependency licenses", static_cast<quint64>(bytes)});
    }
    QList<RuntimePackage> packages;
    for (const auto& asset : assets) {
        const auto match = QRegularExpression("^llama-" + QRegularExpression::escape(tag) +
                                              "-bin-win-(cpu|vulkan|cuda-[0-9.]+)-x64\\.zip$")
                               .match(asset.name);
        if (!match.hasMatch()) continue;
        RuntimePackage package{tag, match.captured(1), {asset}};
        if (package.flavour.startsWith("cuda-")) {
            const auto dependency = "cudart-llama-bin-win-" + package.flavour + "-x64.zip";
            const auto found = std::find_if(assets.begin(), assets.end(), [&](const auto& item) {
                return item.name == dependency;
            });
            if (found == assets.end()) continue;
            package.assets.append(*found);
        }
        packages.append(package);
    }
    std::sort(packages.begin(), packages.end(), [](const auto& a, const auto& b) {
        if ((a.flavour == "cpu") != (b.flavour == "cpu")) return a.flavour == "cpu";
        return a.flavour < b.flavour;
    });
    if (packages.isEmpty() && followBuildReference) {
        const auto reference = std::find_if(assets.begin(), assets.end(), [](const auto& asset) {
            return asset.name == "nightly-tag.txt" && asset.bytes <= 128;
        });
        if (reference != assets.end()) {
            if (cancel.cancelled())
                return make_error(ErrorCode::Cancelled, "Download lookup cancelled.");
            HttpRequest request;
            request.url = reference->url.toStdString();
            request.max_response_bytes = 128;
            request.timeout = std::chrono::seconds(30);
            request.headers = {{"Accept", "application/octet-stream"},
                               {"User-Agent", "LocalCode-setup/" LOCALCODE_VERSION}};
            const auto response = http_->send(request, cancel);
            if (!response) return response.error();
            if (cancel.cancelled())
                return make_error(ErrorCode::Cancelled, "Download lookup cancelled.");
            if (response->status_code != 200 || response->truncated ||
                response->body.size() != reference->bytes)
                return make_error(ErrorCode::NetworkError,
                                  "The release build reference is incomplete or unavailable.");
            const auto bytes = QByteArray::fromStdString(response->body);
            if (QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256)
                                       .toHex()) != reference->sha256)
                return make_error(ErrorCode::ParseError,
                                  "The release build reference does not match its SHA256.");
            const auto build = QString::fromLatin1(bytes.trimmed());
            if (!matches(build, "^b[0-9]{1,9}$") || build == tag)
                return make_error(ErrorCode::ParseError, "The release build reference is invalid.");
            return runtimesForRelease(build, cancel, false);
        }
    }
    if (packages.isEmpty())
        return make_error(ErrorCode::NotSupported,
                          "This release has no supported Windows x64 archives with SHA256 "
                          "metadata. Select a binary build tag or use existing files.");
    return packages;
}

Result<HubModels> ResourceInstaller::models(const QString& repository,
                                            CancellationToken cancel) const {
    if (!matches(repository, "^[A-Za-z0-9_-]+/[A-Za-z0-9_.-]+$"))
        return make_error(ErrorCode::InvalidArgument,
                          "Enter a public Hugging Face repository as owner/name.");
    const auto body =
        metadata("https://huggingface.co/api/models/" + repository + "?blobs=true", cancel);
    if (!body) return body.error();
    const auto document = QJsonDocument::fromJson(*body);
    const auto object = document.object();
    const auto revision = object.value("sha").toString();
    if (!document.isObject() || !matches(revision, "^[a-fA-F0-9]{40}$") ||
        object.value("id").toString() != repository)
        return make_error(ErrorCode::ParseError,
                          "The model metadata lacks a matching repository and immutable revision.");
    HubModels listing{
        repository,
        revision,
        object.value("cardData").toObject().value("license").toString("Check repository license"),
        {}};
    for (const auto& value : object.value("siblings").toArray()) {
        const auto row = value.toObject();
        const auto name = row.value("rfilename").toString();
        const auto lfs = row.value("lfs").toObject();
        const auto hash = lfs.value("sha256").toString();
        const auto bytes = lfs.value("size").toInteger();
        if (!safePath(name) || !name.endsWith(".gguf", Qt::CaseInsensitive) || !digest(hash) ||
            bytes <= 0 || bytes > 64ll * 1024 * 1024 * 1024 ||
            matches(name, "-[0-9]{5}-of-[0-9]{5}\\.gguf$"))
            continue;
        listing.files.append({name,
                              "https://huggingface.co/" + repository + "/resolve/" + revision +
                                  '/' + encodedPath(name),
                              hash.toLower(), "https://huggingface.co/" + repository, revision,
                              listing.license, static_cast<quint64>(bytes)});
    }
    if (listing.files.isEmpty())
        return make_error(ErrorCode::NotSupported,
                          "No single-file GGUF with size and SHA256 is available. Gated "
                          "repositories and split models require existing local files.");
    return listing;
}

Status ResourceInstaller::download(const ResourceAsset& asset, const QString& destination,
                                   ResourceProgress progress, CancellationToken cancel) const {
    if (!http_ || !digest(asset.sha256) || asset.bytes == 0 || !asset.url.startsWith("https://"))
        return make_error(ErrorCode::InvalidArgument,
                          "The download lacks a trusted URL, size or SHA256.");
    QFile file(destination);
    if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly))
        return make_error(ErrorCode::IoError, "Cannot create the partial download.");
    QCryptographicHash hash(QCryptographicHash::Sha256);
    quint64 received = 0;
    bool overflow = false, writeFailed = false;
    HttpRequest request;
    request.url = asset.url.toStdString();
    request.timeout = std::chrono::minutes(30);
    const auto response = http_->stream(
        request,
        [&](std::string_view chunk) {
            if (cancel.cancelled()) return false;
            if (chunk.size() > asset.bytes - received) {
                overflow = true;
                return false;
            }
            if (file.write(chunk.data(), static_cast<qint64>(chunk.size())) !=
                static_cast<qint64>(chunk.size())) {
                writeFailed = true;
                return false;
            }
            hash.addData(QByteArrayView(chunk.data(), static_cast<qsizetype>(chunk.size())));
            received += chunk.size();
            if (progress) progress(received, asset.bytes);
            return true;
        },
        cancel);
    file.close();
    if (cancel.cancelled()) return make_error(ErrorCode::Cancelled, "Download cancelled.");
    if (overflow || writeFailed)
        return make_error(ErrorCode::IoError,
                          "Download exceeds the advertised size or could not be written.");
    if (!response) return response.error();
    if (response->status_code != 200 || received != asset.bytes ||
        QString::fromLatin1(hash.result().toHex()) != asset.sha256.toLower())
        return make_error(
            ErrorCode::ParseError,
            "Downloaded bytes do not match the advertised size and SHA256. Nothing was installed.");
    return ok_status();
}

Status ResourceInstaller::extractRuntimeZip(const QString& archive, const QString& emptyDirectory,
                                            CancellationToken cancel) {
    QFile input(archive);
    const QFileInfo directory(emptyDirectory);
    const QDir root(emptyDirectory);
    if (!directory.isDir() || directory.isSymLink() ||
        !root.entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot).isEmpty())
        return make_error(ErrorCode::InvalidArgument,
                          "Runtime extraction requires a fresh empty directory.");
    if (!input.open(QIODevice::ReadOnly) || input.size() > 1024ll * 1024 * 1024)
        return make_error(ErrorCode::IoError, "Cannot read the runtime archive.");
    // miniz loads the central directory before exposing entry counts. Bound it
    // first so a large download cannot cause an equally large metadata allocation.
    input.seek(qMax<qint64>(0, input.size() - 65557));
    const auto tail = input.read(65557);
    const auto signature = QByteArray::fromHex("504b0506");
    qsizetype footer = tail.lastIndexOf(signature);
    while (footer >= 0) {
        if (footer + 22 <= tail.size() &&
            footer + 22 + qFromLittleEndian<quint16>(tail.constData() + footer + 20) == tail.size())
            break;
        if (footer == 0) {
            footer = -1;
            break;
        }
        footer = tail.lastIndexOf(signature, footer - 1);
    }
    if (footer < 0) return make_error(ErrorCode::ParseError, "Invalid runtime ZIP footer.");
    const auto* fields = tail.constData() + footer;
    const auto count = qFromLittleEndian<quint16>(fields + 10);
    const auto directoryBytes = qFromLittleEndian<quint32>(fields + 12);
    const auto directoryOffset = qFromLittleEndian<quint32>(fields + 16);
    if (qFromLittleEndian<quint16>(fields + 4) != 0 ||
        qFromLittleEndian<quint16>(fields + 6) != 0 ||
        qFromLittleEndian<quint16>(fields + 8) != count || count == 0 || count > 4096 ||
        directoryBytes > 4 * 1024 * 1024 ||
        static_cast<quint64>(directoryOffset) + directoryBytes >
            static_cast<quint64>(input.size() - tail.size() + footer) ||
        (footer >= 20 && tail.mid(footer - 20, 4) == QByteArray::fromHex("504b0607")))
        return make_error(ErrorCode::ParseError,
                          "Runtime ZIP requires a bounded classic central directory; multi-disk "
                          "and ZIP64 archives are unsupported.");
    mz_zip_archive zip{};
    zip.m_pIO_opaque = &input;
    zip.m_pRead = [](void* opaque, mz_uint64 offset, void* buffer, size_t bytes) -> size_t {
        auto& file = *static_cast<QFile*>(opaque);
        if (offset > static_cast<mz_uint64>(std::numeric_limits<qint64>::max()) ||
            !file.seek(static_cast<qint64>(offset)))
            return 0;
        const auto count = file.read(static_cast<char*>(buffer), static_cast<qint64>(bytes));
        return count < 0 ? 0 : static_cast<size_t>(count);
    };
    if (!mz_zip_reader_init(&zip, static_cast<mz_uint64>(input.size()), 0))
        return make_error(ErrorCode::ParseError, "Invalid runtime ZIP.");

    struct End {
        mz_zip_archive* zip;

        ~End() { mz_zip_reader_end(zip); }
    } end{&zip};

    const auto fileCount = mz_zip_reader_get_num_files(&zip);
    quint64 expanded = 0;
    std::set<QString> paths;
    if (fileCount == 0 || fileCount > 4096)
        return make_error(ErrorCode::ParseError, "Runtime ZIP has an unsupported entry count.");
    for (mz_uint i = 0; i < fileCount; ++i) {
        mz_zip_archive_file_stat entry{};
        if (!mz_zip_reader_file_stat(&zip, i, &entry) ||
            mz_zip_reader_get_filename(&zip, i, nullptr, 0) > MZ_ZIP_MAX_ARCHIVE_FILENAME_SIZE)
            return make_error(ErrorCode::ParseError, "Invalid ZIP entry metadata.");
        auto name = QString::fromUtf8(entry.m_filename);
        if (entry.m_is_directory && name.endsWith('/')) name.chop(1);
        const auto mode = (entry.m_external_attr >> 16) & 0170000;
        if (!safePath(name) || !paths.insert(name.toCaseFolded()).second || entry.m_is_encrypted ||
            !entry.m_is_supported || (mode != 0 && mode != 0100000 && mode != 0040000) ||
            (entry.m_external_attr & 0x400) || entry.m_uncomp_size > 2ull * 1024 * 1024 * 1024 ||
            expanded > 4ull * 1024 * 1024 * 1024 - entry.m_uncomp_size)
            return make_error(
                ErrorCode::PermissionDenied,
                "Runtime ZIP contains unsafe paths, links, duplicates or oversized entries.");
        expanded += entry.m_uncomp_size;
    }
    for (mz_uint i = 0; i < fileCount; ++i) {
        if (cancel.cancelled()) return make_error(ErrorCode::Cancelled, "Extraction cancelled.");
        mz_zip_archive_file_stat entry{};
        mz_zip_reader_file_stat(&zip, i, &entry);
        auto name = QString::fromUtf8(entry.m_filename);
        if (entry.m_is_directory && name.endsWith('/')) name.chop(1);
        const auto destination = root.filePath(name);
        if (entry.m_is_directory) {
            if (!QDir().mkpath(destination))
                return make_error(ErrorCode::IoError, "Cannot create a runtime directory.");
            continue;
        }
        if (!QDir().mkpath(QFileInfo(destination).absolutePath()))
            return make_error(ErrorCode::IoError, "Cannot create a runtime directory.");
        QFile output(destination);
        if (!output.open(QIODevice::WriteOnly | QIODevice::NewOnly))
            return make_error(ErrorCode::IoError, "Cannot create a runtime file.");

        struct Write {
            QFile* file;
            CancellationToken cancel;
            quint64 written = 0, expected;
        } write{&output, cancel, 0, entry.m_uncomp_size};

        const auto success = mz_zip_reader_extract_to_callback(
            &zip, i,
            [](void* opaque, mz_uint64 offset, const void* buffer, size_t bytes) -> size_t {
                auto& state = *static_cast<Write*>(opaque);
                if (state.cancel.cancelled() || offset != state.written ||
                    bytes > state.expected - state.written)
                    return 0;
                const auto count =
                    state.file->write(static_cast<const char*>(buffer), static_cast<qint64>(bytes));
                if (count < 0) return 0;
                state.written += static_cast<quint64>(count);
                return static_cast<size_t>(count);
            },
            &write, 0);
        if (!success || write.written != entry.m_uncomp_size)
            return make_error(ErrorCode::ParseError,
                              "Runtime extraction failed or its ZIP checksum is invalid.");
    }
    return ok_status();
}

Result<QString> ResourceInstaller::installRuntime(const RuntimePackage& package,
                                                  const QString& root, ResourceProgress progress,
                                                  CancellationToken cancel) const {
    if (package.assets.isEmpty() || package.assets.size() > 2 ||
        package.bytes() > 2ull * 1024 * 1024 * 1024)
        return make_error(ErrorCode::InvalidArgument, "Invalid runtime package.");
    if (auto space = diskRoom(root, package.bytes() * 4 + 128ull * 1024 * 1024); !space)
        return space.error();
    QTemporaryDir staging(QDir(root).filePath(".runtime-XXXXXX"));
    if (!staging.isValid()) return make_error(ErrorCode::IoError, "Cannot create runtime staging.");
    const auto contents = QDir(staging.path()).filePath("payload");
    QDir().mkpath(contents);
    quint64 received = 0;
    for (int index = 0; index < package.assets.size(); ++index) {
        const auto& asset = package.assets[index];
        const auto archive = QDir(staging.path()).filePath(QString("%1.zip").arg(index));
        if (auto status = download(
                asset, archive,
                [&](quint64 now, quint64) {
                    if (progress) progress(received + now, package.bytes());
                },
                cancel);
            !status)
            return status.error();
        const auto unpack = QDir(staging.path()).filePath(QString("unpack-%1").arg(index));
        QDir().mkpath(unpack);
        if (auto status = extractRuntimeZip(archive, unpack, cancel); !status)
            return status.error();
        QDirIterator files(unpack, QDir::Files, QDirIterator::Subdirectories);
        while (files.hasNext()) {
            files.next();
            const auto relative = QDir(unpack).relativeFilePath(files.filePath());
            const auto target = QDir(contents).filePath(relative);
            if (!QDir().mkpath(QFileInfo(target).absolutePath()))
                return make_error(ErrorCode::IoError, "Cannot stage runtime files.");
            if (QFile::exists(target))
                return make_error(ErrorCode::ParseError,
                                  "Runtime archives contain overlapping files.");
            if (!QFile::rename(files.filePath(), target))
                return make_error(ErrorCode::IoError, "Cannot stage a runtime file.");
        }
        received += asset.bytes;
    }
    const auto binary = QDir(contents).filePath("llama-server.exe");
    if (!QFileInfo(binary).isFile())
        return make_error(ErrorCode::ParseError, "The archive lacks llama-server.exe at its root.");
#ifdef Q_OS_WIN
    if (auto status = stageCompilerRuntime(contents, supportDirectory_, cancel); !status)
        return status.error();
#endif
    if (auto status = receipt(contents, package.assets); !status) return status.error();
    if (cancel.cancelled()) return make_error(ErrorCode::Cancelled, "Installation cancelled.");
    const auto destination =
        QDir(root).filePath("llama-" + QUuid::createUuid().toString(QUuid::Id128));
    if (!QDir().rename(contents, destination))
        return make_error(ErrorCode::IoError, "Cannot publish the verified runtime.");
    return QDir(destination).filePath("llama-server.exe");
}

Result<QString> ResourceInstaller::installModel(const ResourceAsset& asset, const QString& root,
                                                ResourceProgress progress,
                                                CancellationToken cancel) const {
    if (!safePath(asset.name) || !asset.name.endsWith(".gguf", Qt::CaseInsensitive) ||
        asset.bytes > 64ull * 1024 * 1024 * 1024)
        return make_error(ErrorCode::InvalidArgument, "Invalid GGUF download.");
    if (auto space = diskRoom(root, asset.bytes + 16ull * 1024 * 1024); !space)
        return space.error();
    QTemporaryDir staging(QDir(root).filePath(".model-XXXXXX"));
    if (!staging.isValid()) return make_error(ErrorCode::IoError, "Cannot create model staging.");
    const auto file = QDir(staging.path()).filePath(QFileInfo(asset.name).fileName());
    if (auto status = download(asset, file, progress, cancel); !status) return status.error();
    if (auto header = read_gguf_metadata(nativePath(file)); !header) return header.error();
    if (auto status = receipt(staging.path(), {asset}); !status) return status.error();
    if (cancel.cancelled()) return make_error(ErrorCode::Cancelled, "Installation cancelled.");
    const auto destination =
        QDir(root).filePath("gguf-" + QUuid::createUuid().toString(QUuid::Id128));
    if (!QDir().rename(staging.path(), destination))
        return make_error(ErrorCode::IoError, "Cannot publish the verified GGUF.");
    staging.setAutoRemove(false);
    return QDir(destination).filePath(QFileInfo(file).fileName());
}
}   // namespace localcode::gui
