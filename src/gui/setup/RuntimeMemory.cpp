#include "ResourceInstaller.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QStorageInfo>
#include <QTemporaryDir>
#include <QtEndian>
#include <limits>

namespace localcode::gui {
namespace {
#ifdef Q_OS_WIN
struct Binary {
    QString source, digest;
    quint64 bytes = 0;
};
struct Bundle {
    QJsonObject metadata;
    QMap<QString, Binary> files;
    QSet<QString> compatibleCores;
    QString directory;
};

bool digestText(const QString& value) {
    return QRegularExpression("^[a-fA-F0-9]{64}$").match(value).hasMatch();
}

quint16 machine(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return 0;
    const auto dos = file.read(64);
    if (dos.size() != 64 || dos.first(2) != "MZ") return 0;
    const auto offset = qFromLittleEndian<quint32>(dos.constData() + 60);
    if (offset < 64 || offset > 1024 * 1024 || !file.seek(offset)) return 0;
    const auto pe = file.read(6);
    return pe.size() == 6 && pe.first(4) == QByteArray("PE\0\0", 4)
               ? qFromLittleEndian<quint16>(pe.constData() + 4) : 0;
}

Result<Bundle> bundle(const QString& support, bool reasoning = false) {
    Bundle out;
    out.directory = QDir(support).filePath(reasoning ? "runtime-reasoning" : "runtime-memory");
    const QFileInfo manifestInfo(QDir(out.directory).filePath("manifest.json"));
    QFile file(manifestInfo.absoluteFilePath());
    if (!manifestInfo.isFile() || manifestInfo.isSymLink() || !file.open(QIODevice::ReadOnly) || file.size() > 65536)
        return make_error(ErrorCode::NotSupported, "The optional runtime copy package is unavailable.");
    const auto document = QJsonDocument::fromJson(file.readAll());
    out.metadata = document.object();
    if (!document.isObject() || out.metadata.value("format").toInt() != 1 ||
        out.metadata.value("id").toString() !=
            (reasoning ? "localcode.runtime-reasoning" : "localcode.runtime-memory") ||
        out.metadata.value("architecture").toString() != "windows-x64")
        return make_error(ErrorCode::ParseError, "Invalid runtime copy package metadata.");
    for (const auto* key : {"original_core_sha256", "base_sha256", "ggml_sha256", "server_sha256"})
        if (!digestText(out.metadata.value(key).toString()))
            return make_error(ErrorCode::ParseError, "Missing RAM runtime compatibility digest.");
    out.compatibleCores.insert(out.metadata.value("original_core_sha256").toString().toLower());
    if (out.metadata.contains("compatible_core_sha256")) {
        const auto value = out.metadata.value("compatible_core_sha256");
        const auto cores = value.toArray();
        if (!value.isArray() || cores.isEmpty() || cores.size() > 16)
            return make_error(ErrorCode::ParseError, "Invalid RAM runtime compatibility list.");
        for (const auto& core : cores) {
            const auto digest = core.toString().toLower();
            if (!digestText(digest))
                return make_error(ErrorCode::ParseError, "Invalid RAM runtime compatibility digest.");
            out.compatibleCores.insert(digest);
        }
    }
    const auto rows = out.metadata.value("files").toArray();
    if (rows.isEmpty() || rows.size() > 32)
        return make_error(ErrorCode::ParseError, "Invalid runtime copy package file list.");
    for (const auto& value : rows) {
        const auto row = value.toObject();
        const auto name = row.value("name").toString();
        const auto hash = row.value("sha256").toString().toLower();
        const auto bytes = row.value("bytes").toInteger(-1);
        const QFileInfo info(QDir(out.directory).filePath(name));
        if (!QRegularExpression("^[A-Za-z0-9][A-Za-z0-9_.-]{0,127}\\.dll$")
                 .match(name).hasMatch() || !digestText(hash) || bytes <= 0 ||
            !info.isFile() || info.isSymLink() || info.size() != bytes ||
            out.files.contains(name.toLower()))
            return make_error(ErrorCode::ParseError, "Invalid runtime copy package file.");
        out.files.insert(name.toLower(), {info.absoluteFilePath(), hash,
                                          static_cast<quint64>(bytes)});
    }
    const auto primary = reasoning ? "llama-common.dll" : "llama.dll";
    if (!out.files.contains(primary))
        return make_error(ErrorCode::ParseError, "Runtime package lacks its replacement library.");
    if (!reasoning) out.compatibleCores.insert(out.files.value("llama.dll").digest);
    if (reasoning) {
        if (!digestText(out.metadata.value("original_common_sha256").toString()) ||
            !out.metadata.value("source_files").isObject())
            return make_error(ErrorCode::ParseError, "Missing reasoning runtime compatibility data.");
        const auto sources = out.metadata.value("source_files").toObject();
        if (sources.isEmpty() || sources.size() > 128)
            return make_error(ErrorCode::ParseError, "Invalid reasoning runtime source list.");
        for (auto it = sources.begin(); it != sources.end(); ++it) {
            if (!QRegularExpression("^[a-z0-9][a-z0-9_.-]{0,127}\\.(dll|exe)$").match(it.key()).hasMatch() ||
                !digestText(it.value().toString()))
                return make_error(ErrorCode::ParseError, "Invalid reasoning runtime source digest.");
        }
    }
    return out;
}

Result<QString> hashFile(const QString& path, CancellationToken cancel,
                        const std::function<void(quint64)>& progress = {}) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return make_error(ErrorCode::IoError, "Cannot read a runtime file.");
    QCryptographicHash hash(QCryptographicHash::Sha256);
    while (!file.atEnd()) {
        if (cancel.cancelled()) return make_error(ErrorCode::Cancelled, "Runtime preparation cancelled.");
        const auto block = file.read(512 * 1024);
        if (block.isEmpty() && file.error() != QFileDevice::NoError)
            return make_error(ErrorCode::IoError, "Cannot hash a runtime file.");
        hash.addData(block);
        if (progress) progress(static_cast<quint64>(block.size()));
    }
    return QString::fromLatin1(hash.result().toHex());
}

Result<bool> matchesFiles(const QString& directory, const QMap<QString, Binary>& files,
                         CancellationToken cancel) {
    const QFileInfo folder(directory);
    if (!folder.isDir() || folder.isSymLink()) return false;
    for (auto it = files.cbegin(); it != files.cend(); ++it) {
        const QFileInfo info(QDir(directory).filePath(it.key()));
        if (!info.isFile() || info.isSymLink() ||
            static_cast<quint64>(info.size()) != it->bytes) return false;
        const auto hash = hashFile(info.absoluteFilePath(), cancel);
        if (!hash) return hash.error();
        if (*hash != it->digest) return false;
    }
    for (const auto& info : QDir(directory).entryInfoList(QDir::Files | QDir::Hidden)) {
        const auto name = info.fileName().toLower();
        if ((name.endsWith(".dll") || name.endsWith(".exe")) && !files.contains(name))
            return false;
    }
    return true;
}
#endif
} // namespace

bool ResourceInstaller::memoryRuntimeAvailable() const {
#ifdef Q_OS_WIN
    return static_cast<bool>(bundle(supportDirectory_));
#else
    return false;
#endif
}

Result<QString> ResourceInstaller::prepareMemoryRuntime(const QString& server,
                                                        const QString& root,
                                                        ResourceProgress progress,
                                                        CancellationToken cancel) const {
    return prepareRuntimeCopy(server, root, false, std::move(progress), cancel);
}

bool ResourceInstaller::reasoningRuntimeAvailable() const {
#ifdef Q_OS_WIN
    return static_cast<bool>(bundle(supportDirectory_, true));
#else
    return false;
#endif
}

Result<QString> ResourceInstaller::prepareReasoningRuntime(const QString& server, const QString& root,
                                                           ResourceProgress progress,
                                                           CancellationToken cancel) const {
    return prepareRuntimeCopy(server, root, true, std::move(progress), cancel);
}

Result<QString> ResourceInstaller::prepareRuntimeCopy(const QString& server, const QString& root,
                                                      bool reasoning, ResourceProgress progress,
                                                      CancellationToken cancel) const {
#ifndef Q_OS_WIN
    (void)server; (void)root; (void)reasoning; (void)progress; (void)cancel;
    return make_error(ErrorCode::NotSupported, "Optional runtime copies support Windows x64.");
#else
    if (cancel.cancelled()) return make_error(ErrorCode::Cancelled, "Runtime preparation cancelled.");
    const auto package = bundle(supportDirectory_, reasoning);
    if (!package) return package.error();
    const QFileInfo executable(server);
    if (!executable.isFile() || executable.isSymLink() || machine(server) != 0x8664)
        return make_error(ErrorCode::NotSupported, "Choose a supported Windows x64 llama-server.");
    const auto source = executable.absoluteDir();
    for (const auto* name : {"llama.dll", "ggml.dll", "ggml-base.dll"}) {
        const QFileInfo info(source.filePath(name));
        if (!info.isFile() || info.isSymLink())
            return make_error(ErrorCode::NotSupported, "Choose a runtime with regular core libraries.");
    }
    const auto core = hashFile(source.filePath("llama.dll"), cancel);
    const auto base = hashFile(source.filePath("ggml-base.dll"), cancel);
    const auto ggml = hashFile(source.filePath("ggml.dll"), cancel);
    const auto binary = hashFile(server, cancel);
    if (!core) return core.error();
    if (!base) return base.error();
    if (!ggml) return ggml.error();
    if (!binary) return binary.error();
    const auto expected = [&](const char* key) { return package->metadata.value(key).toString().toLower(); };
    if (!package->compatibleCores.contains(*core) ||
        *base != expected("base_sha256") || *ggml != expected("ggml_sha256") ||
        *binary != expected("server_sha256"))
        return make_error(ErrorCode::NotSupported,
                          "This runtime does not match the selected runtime copy. "
                          "Keep the copy option unchecked, or choose a compatible build.");
    if (reasoning) {
        const QFileInfo common(source.filePath("llama-common.dll"));
        if (!common.isFile() || common.isSymLink())
            return make_error(ErrorCode::NotSupported, "Choose a runtime with a regular common library.");
        const auto digest = hashFile(common.absoluteFilePath(), cancel);
        if (!digest) return digest.error();
        if (*digest != expected("original_common_sha256") &&
            *digest != package->files.value("llama-common.dll").digest)
            return make_error(ErrorCode::NotSupported, "The common library does not match the reasoning repair.");
    }
    for (const auto& file : package->files) {
        const auto hash = hashFile(file.source, cancel);
        if (!hash) return hash.error();
        if (*hash != file.digest || machine(file.source) != 0x8664)
            return make_error(ErrorCode::ParseError, "runtime copy package verification failed.");
    }
    QMap<QString, Binary> files;
    for (const auto& info : source.entryInfoList(QDir::Files | QDir::Hidden | QDir::System, QDir::Name)) {
        const auto name = info.fileName().toLower();
        const bool selected = info.canonicalFilePath() == executable.canonicalFilePath();
        const bool tool = name.startsWith("llama-") && name.endsWith(".exe") &&
                          !name.startsWith("llama-server");
        if (!selected && !tool && !name.endsWith(".dll")) continue;
        if (info.isSymLink())
            return make_error(ErrorCode::NotSupported, "Runtime copies require regular binary files.");
        const auto output = selected ? QString("llama-server.exe") : name;
        if (files.contains(output))
            return make_error(ErrorCode::ParseError, "Runtime binary names overlap.");
        if (package->files.contains(output)) continue;
        if (files.size() >= 4096)
            return make_error(ErrorCode::NotSupported, "Runtime folder contains too many binary files.");
        const auto hash = hashFile(info.absoluteFilePath(), cancel);
        if (!hash) return hash.error();
        if (reasoning && package->metadata.value("source_files").toObject().value(output)
                             .toString().toLower() != *hash)
            return make_error(ErrorCode::NotSupported,
                              "This runtime dependency is not qualified for the reasoning copy.",
                              output.toStdString());
        files.insert(output, {info.absoluteFilePath(), *hash, static_cast<quint64>(info.size())});
    }
    for (auto it = package->files.cbegin(); it != package->files.cend(); ++it)
        files.insert(it.key(), it.value());
    if (!files.contains("llama-server.exe") || !files.contains("ggml-base.dll") ||
        !files.contains("ggml.dll"))
        return make_error(ErrorCode::ParseError, "Runtime copy lacks required binaries.");
    QJsonObject digests;
    quint64 bytes = 0;
    for (auto it = files.cbegin(); it != files.cend(); ++it) {
        digests.insert(it.key(), it->digest);
        if (it->bytes > std::numeric_limits<quint64>::max() - bytes - 16ull * 1024 * 1024)
            return make_error(ErrorCode::NotSupported, "Runtime copy size is unsupported.");
        bytes += it->bytes;
    }
    const auto fingerprint = QString::fromLatin1(QCryptographicHash::hash(
        QJsonDocument(digests).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256).toHex());
    if (!QDir().mkpath(root)) return make_error(ErrorCode::IoError, "Cannot create runtime resource folder.");
    const auto canonicalRoot = QFileInfo(root).canonicalFilePath();
    if (canonicalRoot.isEmpty()) return make_error(ErrorCode::IoError, "Cannot resolve runtime resource folder.");
    const auto prefix = reasoning ? QString("reasoning-") : QString("memory-");
    const auto destination = QDir(canonicalRoot).filePath(prefix + fingerprint.first(32));
    if (QFileInfo::exists(destination)) {
        const auto valid = matchesFiles(destination, files, cancel);
        if (!valid) return valid.error();
        if (*valid) return QDir(destination).filePath("llama-server.exe");
        return make_error(ErrorCode::IoError, "The cached runtime copy has changed. Choose another resource folder.");
    }
    const QStorageInfo storage(canonicalRoot);
    if (storage.isValid() && storage.isReady() && storage.bytesAvailable() >= 0 &&
        static_cast<quint64>(storage.bytesAvailable()) < bytes + 16ull * 1024 * 1024)
        return make_error(ErrorCode::IoError, "Insufficient disk space for the runtime copy.");
    QTemporaryDir staging(QDir(canonicalRoot).filePath(QStringLiteral(".") + prefix + "XXXXXX"));
    if (!staging.isValid()) return make_error(ErrorCode::IoError, "Cannot stage runtime copy copy.");
    if (QFileInfo(staging.path()).canonicalPath() != canonicalRoot) {
        staging.setAutoRemove(false);
        return make_error(ErrorCode::IoError, "Runtime staging escaped its resource folder.");
    }
    const auto contents = QDir(staging.path()).filePath("runtime");
    if (!QDir().mkdir(contents)) return make_error(ErrorCode::IoError, "Cannot stage runtime binaries.");
    quint64 verified = 0;
    for (auto it = files.cbegin(); it != files.cend(); ++it) {
        if (cancel.cancelled()) return make_error(ErrorCode::Cancelled, "Runtime preparation cancelled.");
        const auto target = QDir(contents).filePath(it.key());
        if (!QFile::copy(it->source, target)) return make_error(ErrorCode::IoError, "Cannot copy runtime binary.");
        const auto hash = hashFile(target, cancel, [&](quint64 count) {
            verified += count;
            if (progress) progress(verified, bytes);
        });
        if (!hash) return hash.error();
        if (*hash != it->digest) return make_error(ErrorCode::IoError, "Runtime binary changed while copying.");
    }
    const auto notices = QDir(contents).filePath(reasoning ? "localcode-reasoning" : "localcode-memory");
    if (!QDir().mkdir(notices)) return make_error(ErrorCode::IoError, "Cannot stage runtime notices.");
    const QStringList noticeFiles = reasoning
        ? QStringList{"manifest.json", "LICENSE", "README.md", "reasoning-prefill.patch", "OPENSSL-LICENSE.txt"}
        : QStringList{"manifest.json", "LICENSE", "README.md", "windows-mmap-working-set.patch"};
    for (const auto& name : noticeFiles) {
        const auto from = QDir(package->directory).filePath(name);
        if (!QFileInfo(from).isFile() || QFileInfo(from).isSymLink() ||
            !QFile::copy(from, QDir(notices).filePath(name)))
            return make_error(ErrorCode::IoError, "Cannot stage runtime copy source and notices.");
    }
    QSaveFile receipt(QDir(contents).filePath(reasoning ? "localcode-reasoning-runtime.json"
                                                      : "localcode-memory-runtime.json"));
    const QJsonObject provenance{{"format", 1}, {"fingerprint", fingerprint},
                                  {"files", digests}, {"package", package->metadata}};
    if (!receipt.open(QIODevice::WriteOnly) ||
        receipt.write(QJsonDocument(provenance).toJson()) < 0 || !receipt.commit())
        return make_error(ErrorCode::IoError, "Cannot save runtime copy provenance.");
    if (cancel.cancelled()) return make_error(ErrorCode::Cancelled, "Runtime preparation cancelled.");
    if (!QDir().rename(contents, destination)) {
        const auto raced = matchesFiles(destination, files, cancel);
        if (!raced) return raced.error();
        if (!*raced) return make_error(ErrorCode::IoError, "Cannot publish runtime copy copy.");
    }
    return QDir(destination).filePath("llama-server.exe");
#endif
}
} // namespace localcode::gui
