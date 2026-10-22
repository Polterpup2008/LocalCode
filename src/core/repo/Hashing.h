#pragma once
// ---------------------------------------------------------------------------
// A cheap content fingerprint.
//
// FNV-1a, 64-bit. This is not a cryptographic hash and nothing here pretends it
// is: the question it answers is "has this file changed since it was indexed",
// where size and modification time already agree that it might not have. An
// adversary is not part of that question, and a hash that costs as much as
// reading the file would defeat the purpose of having one.
// ---------------------------------------------------------------------------
#include <cstdint>
#include <string>
#include <string_view>

namespace localcode::repo {

[[nodiscard]] std::uint64_t fnv1a(std::string_view data) noexcept;

// The same value as sixteen lowercase hex digits.
[[nodiscard]] std::string fnv1a_hex(std::string_view data);

// A file name safe to put in a directory listing: lowercase, with anything
// outside [a-z0-9._-] collapsed to a single dash.
[[nodiscard]] std::string slug(std::string_view text);

}  // namespace localcode::repo
