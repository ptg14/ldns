#pragma once

// Public dedicated suffix list for the "public TLD" confirmation (SPEC §9.1,
// D15). Only the final labels of public suffixes are included: a small built-in
// list is enough to catch the common mistakes (google.com, foo.io, ...) without
// shipping the full Public Suffix List.

#include <string_view>

namespace ldns::domain {

// True if the last label of `name` is a well-known public TLD. Used to ask for
// confirmation (or require --force) when a user creates such a domain.
bool is_public_tld(std::string_view name);

// True if the name ends in ".local", which conflicts with mDNS (SPEC §9.1).
bool is_mdns_local(std::string_view name);

// Suggestion shown in warnings: ".lan", ".home.arpa" or ".test" (SPEC D15).
std::string_view suggested_tlds();

}  // namespace ldns::domain
