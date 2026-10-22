#include "FileSystem.h"
#include "PathEncoding.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <deque>
#include <fstream>
#include <optional>
#include <random>
#include <system_error>
#include <utility>

#include "../util/Log.h"
#include "../util/Strings.h"

#if defined(_WIN32)
#include <windows.h>
#endif

namespace localcode::fs {
namespace {

std::int64_t to_unix_ms(std::filesystem::file_time_type tp) {
    using namespace std::chrono;
    // file_clock's epoch differs per platform; clock_cast is the portable route.
    const auto sys = time_point_cast<system_clock::duration>(
        tp - std::filesystem::file_time_type::clock::now() + system_clock::now());
    return duration_cast<milliseconds>(sys.time_since_epoch()).count();
}

std::string utf16_to_utf8(const char* data, std::size_t bytes, bool big_endian) {
    std::string out;
    out.reserve(bytes);
    const auto* raw = reinterpret_cast<const unsigned char*>(data);
    for (std::size_t i = 0; i + 1 < bytes; i += 2) {
        std::uint32_t unit = big_endian
                                 ? (static_cast<std::uint32_t>(raw[i]) << 8 | raw[i + 1])
                                 : (static_cast<std::uint32_t>(raw[i + 1]) << 8 | raw[i]);
        std::uint32_t cp = unit;
        if (unit >= 0xD800 && unit <= 0xDBFF && i + 3 < bytes) {
            const std::uint32_t low =
                big_endian ? (static_cast<std::uint32_t>(raw[i + 2]) << 8 | raw[i + 3])
                           : (static_cast<std::uint32_t>(raw[i + 3]) << 8 | raw[i + 2]);
            if (low >= 0xDC00 && low <= 0xDFFF) {
                cp = 0x10000 + ((unit - 0xD800) << 10) + (low - 0xDC00);
                i += 2;
            }
        }
        if (cp < 0x80) {
            out.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }
    return out;
}

std::string utf8_to_utf16_bytes(std::string_view utf8, bool big_endian) {
    std::string out;
    out.reserve(utf8.size() * 2);
    std::size_t i = 0;
    auto emit = [&](std::uint16_t unit) {
        if (big_endian) {
            out.push_back(static_cast<char>(unit >> 8));
            out.push_back(static_cast<char>(unit & 0xFF));
        } else {
            out.push_back(static_cast<char>(unit & 0xFF));
            out.push_back(static_cast<char>(unit >> 8));
        }
    };
    while (i < utf8.size()) {
        const auto b0 = static_cast<unsigned char>(utf8[i]);
        std::uint32_t cp = 0;
        std::size_t len = 1;
        if (b0 < 0x80) { cp = b0; }
        else if ((b0 & 0xE0) == 0xC0) { cp = b0 & 0x1Fu; len = 2; }
        else if ((b0 & 0xF0) == 0xE0) { cp = b0 & 0x0Fu; len = 3; }
        else if ((b0 & 0xF8) == 0xF0) { cp = b0 & 0x07u; len = 4; }
        else { cp = 0xFFFD; }
        if (i + len > utf8.size()) { cp = 0xFFFD; len = 1; }
        for (std::size_t k = 1; k < len; ++k) {
            cp = (cp << 6) | (static_cast<unsigned char>(utf8[i + k]) & 0x3Fu);
        }
        i += len;
        if (cp < 0x10000) {
            emit(static_cast<std::uint16_t>(cp));
        } else {
            cp -= 0x10000;
            emit(static_cast<std::uint16_t>(0xD800 + (cp >> 10)));
            emit(static_cast<std::uint16_t>(0xDC00 + (cp & 0x3FF)));
        }
    }
    return out;
}

std::string normalise_newlines(std::string_view in, LineEnding& detected) {
    detected = detect_line_ending(in);
    if (detected == LineEnding::Lf || detected == LineEnding::None) return std::string{in};
    std::string out;
    out.reserve(in.size());
    for (std::size_t i = 0; i < in.size(); ++i) {
        if (in[i] == '\r') {
            if (i + 1 < in.size() && in[i + 1] == '\n') continue;  // CRLF -> LF
            out.push_back('\n');                                   // lone CR -> LF
        } else {
            out.push_back(in[i]);
        }
    }
    return out;
}

std::string apply_line_ending(std::string_view content, LineEnding ending) {
    if (ending != LineEnding::CrLf) return std::string{content};
    std::string out;
    out.reserve(content.size() + content.size() / 20);
    for (char c : content) {
        if (c == '\n') out.push_back('\r');
        out.push_back(c);
    }
    return out;
}

std::string random_suffix() {
    static thread_local std::mt19937_64 rng{std::random_device{}()};
    static const char* digits = "0123456789abcdef";
    std::string s(8, '0');
    auto value = rng();
    for (auto& c : s) {
        c = digits[value & 0xF];
        value >>= 4;
    }
    return s;
}

}  // namespace

bool is_link_or_reparse_point(const std::filesystem::path& path) {
#if defined(_WIN32)
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT))
        return true;
#endif
    std::error_code error;
    return std::filesystem::is_symlink(path, error);
}

std::string_view to_string(TextEncoding encoding) noexcept {
    switch (encoding) {
        case TextEncoding::Utf8:    return "utf-8";
        case TextEncoding::Utf8Bom: return "utf-8-bom";
        case TextEncoding::Utf16Le: return "utf-16le";
        case TextEncoding::Utf16Be: return "utf-16be";
        case TextEncoding::Unknown: return "unknown";
    }
    return "unknown";
}

std::string_view to_string(LineEnding ending) noexcept {
    switch (ending) {
        case LineEnding::Lf:    return "lf";
        case LineEnding::CrLf:  return "crlf";
        case LineEnding::Mixed: return "mixed";
        case LineEnding::None:  return "none";
    }
    return "lf";
}

TextEncoding detect_encoding(std::string_view sample) {
    if (sample.size() >= 3 && static_cast<unsigned char>(sample[0]) == 0xEF &&
        static_cast<unsigned char>(sample[1]) == 0xBB &&
        static_cast<unsigned char>(sample[2]) == 0xBF) {
        return TextEncoding::Utf8Bom;
    }
    if (sample.size() >= 2) {
        const auto b0 = static_cast<unsigned char>(sample[0]);
        const auto b1 = static_cast<unsigned char>(sample[1]);
        if (b0 == 0xFF && b1 == 0xFE) return TextEncoding::Utf16Le;
        if (b0 == 0xFE && b1 == 0xFF) return TextEncoding::Utf16Be;
    }
    // BOM-less UTF-16 shows up as a strong pattern of interleaved NUL bytes.
    if (sample.size() >= 16) {
        std::size_t even_nul = 0, odd_nul = 0, pairs = 0;
        for (std::size_t i = 0; i + 1 < std::min<std::size_t>(sample.size(), 512); i += 2) {
            ++pairs;
            if (sample[i] == '\0') ++even_nul;
            if (sample[i + 1] == '\0') ++odd_nul;
        }
        if (pairs > 4) {
            if (odd_nul * 10 >= pairs * 8 && even_nul * 10 <= pairs * 2) return TextEncoding::Utf16Le;
            if (even_nul * 10 >= pairs * 8 && odd_nul * 10 <= pairs * 2) return TextEncoding::Utf16Be;
        }
    }
    return TextEncoding::Utf8;
}

bool looks_binary(std::string_view sample) {
    if (sample.empty()) return false;
    const auto encoding = detect_encoding(sample);
    if (encoding == TextEncoding::Utf16Le || encoding == TextEncoding::Utf16Be) return false;

    const std::size_t limit = std::min<std::size_t>(sample.size(), 8192);
    std::size_t control = 0;
    for (std::size_t i = 0; i < limit; ++i) {
        const auto c = static_cast<unsigned char>(sample[i]);
        if (c == 0) return true;                       // NUL: definitely binary
        if (c < 0x09 || (c > 0x0D && c < 0x20)) ++control;
    }
    return control * 100 > limit * 2;                  // >2% control characters
}

LineEnding detect_line_ending(std::string_view content) {
    std::size_t lf = 0, crlf = 0;
    for (std::size_t i = 0; i < content.size(); ++i) {
        if (content[i] == '\n') {
            if (i > 0 && content[i - 1] == '\r') ++crlf;
            else ++lf;
        }
    }
    if (lf == 0 && crlf == 0) return LineEnding::None;
    if (crlf > 0 && lf > 0) return LineEnding::Mixed;
    return crlf > 0 ? LineEnding::CrLf : LineEnding::Lf;
}

Result<std::string> read_bytes(const std::filesystem::path& path, std::size_t max_bytes) {
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        return make_error(ErrorCode::NotFound, "file does not exist: " + path_to_utf8(path));
    }
    if (std::filesystem::is_directory(path, ec)) {
        return make_error(ErrorCode::InvalidArgument, "path is a directory: " + path_to_utf8(path));
    }

    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        return make_error(ErrorCode::IoError, "cannot open file: " + path_to_utf8(path));
    }

    // Size the buffer to the file, not to the ceiling. Resizing to max_bytes
    // zero-fills two megabytes before reading a two-kilobyte file, and a text
    // search that reads two hundred files was spending four hundred megabytes
    // of memset to do it - more time than the reads themselves.
    //
    // A file that grows between the stat and the read is simply read up to the
    // size we saw, which is the same bound as before. If the size cannot be
    // determined - a pipe, a device - the ceiling is still the right answer.
    const auto on_disk = std::filesystem::file_size(path, ec);
    const std::size_t wanted =
        ec ? max_bytes
           : static_cast<std::size_t>(std::min<std::uintmax_t>(on_disk, max_bytes));

    std::string data;
    data.resize(wanted);
    stream.read(data.data(), static_cast<std::streamsize>(wanted));
    data.resize(static_cast<std::size_t>(stream.gcount()));
    if (stream.bad()) {
        return make_error(ErrorCode::IoError, "read failed: " + path_to_utf8(path));
    }
    return data;
}

Result<TextFile> read_text(const std::filesystem::path& path, std::size_t max_bytes) {
    auto raw = read_bytes(path, max_bytes + 1);
    if (!raw) return raw.error();

    TextFile file;
    std::error_code ec;
    file.size_on_disk = std::filesystem::file_size(path, ec);
    if (ec) file.size_on_disk = raw->size();

    if (raw->size() > max_bytes) {
        file.truncated = true;
        raw->resize(max_bytes);
    }

    file.encoding = detect_encoding(*raw);
    std::string decoded;
    switch (file.encoding) {
        case TextEncoding::Utf8Bom:
            decoded = raw->substr(3);
            break;
        case TextEncoding::Utf16Le:
            decoded = utf16_to_utf8(raw->data() + (raw->size() >= 2 &&
                                                   static_cast<unsigned char>((*raw)[0]) == 0xFF ? 2 : 0),
                                    raw->size() - (raw->size() >= 2 &&
                                                   static_cast<unsigned char>((*raw)[0]) == 0xFF ? 2 : 0),
                                    false);
            break;
        case TextEncoding::Utf16Be:
            decoded = utf16_to_utf8(raw->data() + (raw->size() >= 2 &&
                                                   static_cast<unsigned char>((*raw)[0]) == 0xFE ? 2 : 0),
                                    raw->size() - (raw->size() >= 2 &&
                                                   static_cast<unsigned char>((*raw)[0]) == 0xFE ? 2 : 0),
                                    true);
            break;
        default:
            if (looks_binary(*raw)) {
                return make_error(ErrorCode::NotSupported,
                                  "file appears to be binary: " + path_to_utf8(path),
                                  "size=" + strings::human_bytes(file.size_on_disk));
            }
            decoded = std::move(*raw);
            break;
    }

    file.content = normalise_newlines(decoded, file.line_ending);
    // Malformed bytes become U+FFFD so the content is always safe to put in JSON.
    if (!strings::is_valid_utf8(file.content)) {
        file.content = strings::sanitize_utf8(file.content);
    }
    file.line_count = file.content.empty()
                          ? 0
                          : static_cast<std::size_t>(
                                std::count(file.content.begin(), file.content.end(), '\n')) +
                                (file.content.back() == '\n' ? 0 : 1);
    return file;
}

Result<TextFile> read_text_range(const std::filesystem::path& path, std::size_t start_line,
                                 std::size_t end_line, std::size_t max_bytes) {
    auto source = read_source(path, start_line, end_line, max_bytes);
    if (!source) return source.error();
    return std::move(source->file);
}

Result<SourceVersion> source_version(const std::filesystem::path& path) {
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec) return make_error(ErrorCode::IoError, "cannot inspect source size", ec.message());
    const auto modified = std::filesystem::last_write_time(path, ec);
    if (ec) return make_error(ErrorCode::IoError, "cannot inspect source version", ec.message());
    return SourceVersion{size, std::to_string(modified.time_since_epoch().count())};
}

Result<SourceRead> read_source(const std::filesystem::path& path, std::size_t start_line,
                               std::size_t end_line, std::size_t max_bytes) {
    start_line = std::max<std::size_t>(1, start_line);
    if (end_line != 0 && end_line < start_line)
        return make_error(ErrorCode::InvalidArgument, "end_line must be >= start_line");
    std::error_code ec;
    if (!std::filesystem::exists(path, ec))
        return make_error(ErrorCode::NotFound, "file does not exist: " + path_to_utf8(path));
    if (std::filesystem::is_directory(path, ec))
        return make_error(ErrorCode::InvalidArgument, "path is a directory: " + path_to_utf8(path));
    const auto version_before = source_version(path);
    std::ifstream stream(path, std::ios::binary);
    if (!stream) return make_error(ErrorCode::IoError, "cannot open file: " + path_to_utf8(path));

    char probe[4096];
    stream.read(probe, sizeof probe);
    if (stream.bad()) return make_error(ErrorCode::IoError, "read failed: " + path_to_utf8(path));
    const std::string_view head{probe, static_cast<std::size_t>(stream.gcount())};
    const auto encoding = detect_encoding(head);
    if (looks_binary(head))
        return make_error(ErrorCode::NotSupported,
                          "file appears to be binary: " + path_to_utf8(path));
    stream.clear();
    const bool wide = encoding == TextEncoding::Utf16Le || encoding == TextEncoding::Utf16Be;
    const bool bom = head.size() >= 2 && ((static_cast<unsigned char>(head[0]) == 0xff &&
                                           static_cast<unsigned char>(head[1]) == 0xfe) ||
                                          (static_cast<unsigned char>(head[0]) == 0xfe &&
                                           static_cast<unsigned char>(head[1]) == 0xff));
    stream.seekg(encoding == TextEncoding::Utf8Bom ? 3 : wide && bom ? 2 : 0);

    // Decode one complete character before deciding whether it fits. A byte
    // ceiling must not manufacture a replacement character at a UTF-8 cut or
    // allocate an unbounded getline buffer for one generated source line.
    auto character = [&]() -> std::optional<std::string> {
        const auto first = stream.get();
        if (first == std::char_traits<char>::eof()) return std::nullopt;
        std::string raw(1, static_cast<char>(first));
        if (wide) {
            const auto second = stream.get();
            if (second == std::char_traits<char>::eof()) return std::string{"\xef\xbf\xbd"};
            raw.push_back(static_cast<char>(second));
            const auto unit =
                encoding == TextEncoding::Utf16Be
                    ? (static_cast<unsigned char>(raw[0]) << 8) | static_cast<unsigned char>(raw[1])
                    : (static_cast<unsigned char>(raw[1]) << 8) |
                          static_cast<unsigned char>(raw[0]);
            if (unit >= 0xd800 && unit <= 0xdbff) {
                const auto position = stream.tellg();
                char low[2];
                stream.read(low, 2);
                const auto low_unit = stream.gcount() == 2
                                          ? encoding == TextEncoding::Utf16Be
                                                ? (static_cast<unsigned char>(low[0]) << 8) |
                                                      static_cast<unsigned char>(low[1])
                                                : (static_cast<unsigned char>(low[1]) << 8) |
                                                      static_cast<unsigned char>(low[0])
                                          : 0;
                if (low_unit >= 0xdc00 && low_unit <= 0xdfff)
                    raw.append(low, 2);
                else {
                    stream.clear();
                    stream.seekg(position);
                }
            }
            auto decoded = utf16_to_utf8(raw.data(), raw.size(), encoding == TextEncoding::Utf16Be);
            return strings::is_valid_utf8(decoded) ? decoded : strings::sanitize_utf8(decoded);
        }
        const auto byte = static_cast<unsigned char>(first);
        const std::size_t length = byte < 0x80                    ? 1
                                   : byte >= 0xc2 && byte <= 0xdf ? 2
                                   : byte >= 0xe0 && byte <= 0xef ? 3
                                   : byte >= 0xf0 && byte <= 0xf4 ? 4
                                                                  : 0;
        if (length == 0) return std::string{"\xef\xbf\xbd"};
        for (std::size_t index = 1; index < length; ++index) {
            const auto next = stream.peek();
            if (next == std::char_traits<char>::eof() ||
                (static_cast<unsigned char>(next) & 0xc0) != 0x80)
                return std::string{"\xef\xbf\xbd"};
            raw.push_back(static_cast<char>(stream.get()));
        }
        return strings::is_valid_utf8(raw) ? raw : std::string{"\xef\xbf\xbd"};
    };

    SourceRead source;
    source.file.encoding = encoding;
    source.file.size_on_disk = std::filesystem::file_size(path, ec);
    if (ec) source.file.size_on_disk = 0;
    std::optional<std::string> pending;
    std::size_t line = 1;
    bool saw_crlf = false, saw_lf = false;
    bool withheld = false;
    while (true) {
        auto current = pending ? std::exchange(pending, std::nullopt) : character();
        if (!current) break;
        if (*current == "\r") {
            auto after = character();
            if (after && *after == "\n")
                saw_crlf = true;
            else {
                pending = std::move(after);
                saw_lf = true;
            }
            *current = "\n";
        } else if (*current == "\n")
            saw_lf = true;
        if (line >= start_line) {
            if (source.file.content.size() + current->size() > max_bytes) {
                source.file.truncated = true;
                withheld = true;
                break;
            }
            if (source.start_line == 0) source.start_line = line;
            source.end_line = line;
            source.file.content += *current;
            if (*current == "\n") source.complete_end_line = line;
        }
        if (*current == "\n") {
            ++line;
            if (end_line != 0 && line > end_line) break;
        }
    }
    if (stream.bad()) return make_error(ErrorCode::IoError, "read failed: " + path_to_utf8(path));
    source.has_more =
        withheld || pending.has_value() || stream.peek() != std::char_traits<char>::eof();
    source.eof = !source.has_more;
    source.last_line_partial =
        source.has_more && !source.file.content.empty() && source.file.content.back() != '\n';
    if (source.eof && !source.file.content.empty()) source.complete_end_line = source.end_line;
    source.file.line_count =
        source.file.content.empty()
            ? 0
            : static_cast<std::size_t>(
                  std::count(source.file.content.begin(), source.file.content.end(), '\n')) +
                  (source.file.content.back() == '\n' ? 0 : 1);
    source.file.line_ending = saw_crlf && saw_lf ? LineEnding::Mixed
                              : saw_crlf         ? LineEnding::CrLf
                              : saw_lf           ? LineEnding::Lf
                                                 : LineEnding::None;
    const auto version_after = source_version(path);
    if (version_before && version_after && *version_before == *version_after)
        source.version = *version_before;
    return source;
}

Status write_text_atomic(const std::filesystem::path& path, std::string_view content,
                         TextEncoding encoding, LineEnding line_ending) {
    std::error_code ec;
    const auto parent = path.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, ec);
        if (ec && !std::filesystem::is_directory(parent)) {
            return make_error(ErrorCode::IoError,
                              "cannot create parent directory " + path_to_utf8(parent), ec.message());
        }
    }

    std::string payload = apply_line_ending(content, line_ending);
    switch (encoding) {
        case TextEncoding::Utf8Bom: payload.insert(0, "\xEF\xBB\xBF"); break;
        case TextEncoding::Utf16Le: payload = std::string("\xFF\xFE", 2) +
                                              utf8_to_utf16_bytes(payload, false); break;
        case TextEncoding::Utf16Be: payload = std::string("\xFE\xFF", 2) +
                                              utf8_to_utf16_bytes(payload, true); break;
        default: break;
    }

    // Temp file lives beside the target so the rename stays on one volume.
    auto temp = path;
    temp += ".lc-" + random_suffix();
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out) {
            return make_error(ErrorCode::IoError, "cannot create temporary file " + path_to_utf8(temp));
        }
        out.write(payload.data(), static_cast<std::streamsize>(payload.size()));
        out.flush();
        if (!out) {
            out.close();
            std::filesystem::remove(temp, ec);
            return make_error(ErrorCode::IoError, "write failed for " + path_to_utf8(temp));
        }
    }

#if defined(_WIN32)
    // MoveFileEx replaces atomically; std::filesystem::rename fails if the
    // destination exists on some Windows configurations.
    if (!MoveFileExW(temp.c_str(), path.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const DWORD err = GetLastError();
        std::filesystem::remove(temp, ec);
        return make_error(ErrorCode::IoError, "cannot replace " + path_to_utf8(path),
                          "win32 error " + std::to_string(err));
    }
#else
    std::filesystem::rename(temp, path, ec);
    if (ec) {
        std::error_code ignore;
        std::filesystem::remove(temp, ignore);
        return make_error(ErrorCode::IoError, "cannot replace " + path_to_utf8(path), ec.message());
    }
#endif
    return ok_status();
}

Status append_text(const std::filesystem::path& path, std::string_view content) {
    std::error_code ec;
    const auto parent = path.parent_path();
    if (!parent.empty()) std::filesystem::create_directories(parent, ec);
    std::ofstream out(path, std::ios::binary | std::ios::app);
    if (!out) return make_error(ErrorCode::IoError, "cannot open for append: " + path_to_utf8(path));
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
    if (!out) return make_error(ErrorCode::IoError, "append failed: " + path_to_utf8(path));
    return ok_status();
}

FileInfo stat_file(const std::filesystem::path& path) {
    FileInfo info;
    info.path = path;

    std::error_code ec;
    const auto status = std::filesystem::symlink_status(path, ec);
    if (ec) return info;

    info.exists = std::filesystem::exists(status);
    if (!info.exists) return info;

    info.is_symlink = std::filesystem::is_symlink(status);
    const auto resolved = info.is_symlink ? std::filesystem::status(path, ec) : status;
    info.is_directory = std::filesystem::is_directory(resolved);
    info.is_regular_file = std::filesystem::is_regular_file(resolved);

#if defined(_WIN32)
    // NTFS junctions are reparse points but not "symlinks" to std::filesystem.
    const DWORD attrs = GetFileAttributesW(path.c_str());
    if (attrs != INVALID_FILE_ATTRIBUTES) {
        if (attrs & FILE_ATTRIBUTE_REPARSE_POINT) info.is_symlink = true;
        info.readonly = (attrs & FILE_ATTRIBUTE_READONLY) != 0;
    }
#else
    const auto perms = std::filesystem::status(path, ec).permissions();
    info.readonly = (perms & std::filesystem::perms::owner_write) == std::filesystem::perms::none;
#endif

    if (info.is_regular_file) {
        info.size = std::filesystem::file_size(path, ec);
        if (ec) info.size = 0;
        const auto mtime = std::filesystem::last_write_time(path, ec);
        if (!ec) info.modified_unix_ms = to_unix_ms(mtime);

        auto sample = read_bytes(path, 8192);
        if (sample) {
            info.encoding = detect_encoding(*sample);
            info.is_binary = looks_binary(*sample);
            if (!info.is_binary && info.size <= kDefaultMaxReadBytes) {
                auto text = read_text(path, kDefaultMaxReadBytes);
                if (text) info.line_count = text->line_count;
            }
        }
    } else if (info.is_directory) {
        const auto mtime = std::filesystem::last_write_time(path, ec);
        if (!ec) info.modified_unix_ms = to_unix_ms(mtime);
    }
    return info;
}

Result<std::vector<DirEntry>> list_directory(const std::filesystem::path& path,
                                             bool include_hidden) {
    std::error_code ec;
    if (!std::filesystem::is_directory(path, ec)) {
        return make_error(ErrorCode::NotFound, "not a directory: " + path_to_utf8(path));
    }

    std::vector<DirEntry> entries;
    std::filesystem::directory_iterator it(
        path, std::filesystem::directory_options::skip_permission_denied, ec);
    if (ec) {
        return make_error(ErrorCode::IoError, "cannot list " + path_to_utf8(path), ec.message());
    }

    for (const auto& item : it) {
        std::error_code item_ec;
        DirEntry entry;
        entry.name = path_to_utf8(item.path().filename());
        if (!include_hidden && !entry.name.empty() && entry.name[0] == '.') continue;
        entry.is_directory = item.is_directory(item_ec);
        entry.is_symlink = is_link_or_reparse_point(item.path());
        if (!entry.is_directory) {
            entry.size = item.file_size(item_ec);
            if (item_ec) entry.size = 0;
        }
        const auto mtime = item.last_write_time(item_ec);
        if (!item_ec) entry.modified_unix_ms = to_unix_ms(mtime);
        entries.push_back(std::move(entry));
    }

    std::sort(entries.begin(), entries.end(), [](const DirEntry& a, const DirEntry& b) {
        if (a.is_directory != b.is_directory) return a.is_directory;
        return strings::to_lower(a.name) < strings::to_lower(b.name);
    });
    return entries;
}

const std::vector<std::string>& default_ignored_directories() {
    static const std::vector<std::string> kDirs = {
        ".git", ".svn", ".hg", "node_modules", "target", "build", "out", "dist",
        ".venv", "venv", "__pycache__", ".mypy_cache", ".pytest_cache", ".tox",
        ".idea", ".vs", ".vscode", "cmake-build-debug", "cmake-build-release",
        "bin", "obj", "Debug", "Release", "x64", ".cache", ".gradle", "vendor",
        "Pods", "DerivedData", ".next", ".nuxt", ".terraform", "bazel-out",
    };
    return kDirs;
}

IgnoreRules::IgnoreRules() : directories_(default_ignored_directories()) {}

IgnoreRules::IgnoreRules(std::vector<std::string> directories, std::vector<std::string> file_globs)
    : directories_(std::move(directories)), globs_(std::move(file_globs)) {}

void IgnoreRules::add_directory(std::string name) { directories_.push_back(std::move(name)); }
void IgnoreRules::add_glob(std::string pattern) { globs_.push_back(std::move(pattern)); }

void IgnoreRules::load_gitignore(const std::filesystem::path& file) {
    auto text = read_text(file, 512 * 1024);
    if (!text) return;
    for (const auto& raw : strings::split_lines(text->content)) {
        auto line = strings::trim(raw);
        if (line.empty() || line[0] == '#') continue;
        bool negate = false;
        if (line[0] == '!') {
            negate = true;
            line = strings::trim(line.substr(1));
            if (line.empty()) continue;
        }
        const bool directory_only = line.back() == '/';
        if (directory_only) line.pop_back();
        if (line.empty()) continue;
        if (negate) {
            negations_.push_back(line);
        } else if (directory_only ||
                   (line.find('*') == std::string::npos && line.find('.') == std::string::npos)) {
            directories_.push_back(line);
        } else {
            globs_.push_back(line);
        }
    }
}

bool IgnoreRules::ignores_directory(std::string_view name) const {
    for (const auto& d : directories_) {
        if (strings::iequals(d, name)) return true;
    }
    return false;
}

bool IgnoreRules::ignores_file(std::string_view relative_path) const {
    for (const auto& n : negations_) {
        if (strings::glob_match(n, relative_path)) return false;
    }
    for (const auto& g : globs_) {
        if (strings::glob_match(g, relative_path)) return true;
        // A bare "*.o" style pattern should also match nested paths.
        if (g.find('/') == std::string::npos) {
            const auto slash = relative_path.find_last_of("/\\");
            const auto name = (slash == std::string_view::npos) ? relative_path
                                                                : relative_path.substr(slash + 1);
            if (strings::glob_match(g, name)) return true;
        }
    }
    return false;
}

Status walk(const std::filesystem::path& root, const WalkOptions& options,
            const std::function<bool(const std::filesystem::path&, const DirEntry&)>& callback) {
    std::error_code ec;
    if (!std::filesystem::is_directory(root, ec)) {
        return make_error(ErrorCode::NotFound, "not a directory: " + path_to_utf8(root));
    }

    struct Pending {
        std::filesystem::path absolute;
        std::filesystem::path relative;
        std::size_t depth;
    };

    std::deque<Pending> queue;
    queue.push_back({root, {}, 0});
    std::size_t visited = 0;

    while (!queue.empty()) {
        const Pending current = std::move(queue.front());
        queue.pop_front();

        std::error_code iter_ec;
        std::filesystem::directory_iterator it(
            current.absolute, std::filesystem::directory_options::skip_permission_denied, iter_ec);
        if (iter_ec) continue;  // unreadable directory: skip, do not abort the scan

        for (const auto& item : it) {
            if (++visited > options.max_entries) return ok_status();

            std::error_code item_ec;
            DirEntry entry;
            entry.name = path_to_utf8(item.path().filename());
            entry.is_symlink = is_link_or_reparse_point(item.path());
            entry.is_directory = item.is_directory(item_ec);

            const auto relative = current.relative / entry.name;

            if (entry.is_directory) {
                if (options.ignore && options.ignore->ignores_directory(entry.name)) continue;
                if (entry.is_symlink && !options.follow_symlinks) continue;
                if (options.include_directories && !callback(relative, entry)) return ok_status();
                if (current.depth + 1 < options.max_depth) {
                    queue.push_back({item.path(), relative, current.depth + 1});
                }
                continue;
            }

            if (entry.is_symlink && !options.follow_symlinks) continue;
            const auto relative_str = path_to_utf8(relative);
            if (options.ignore && options.ignore->ignores_file(relative_str)) continue;

            entry.size = item.file_size(item_ec);
            if (item_ec) entry.size = 0;
            const auto mtime = item.last_write_time(item_ec);
            if (!item_ec) entry.modified_unix_ms = to_unix_ms(mtime);

            if (!callback(relative, entry)) return ok_status();
        }
    }
    return ok_status();
}

}  // namespace localcode::fs
