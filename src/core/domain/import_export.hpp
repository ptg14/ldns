#pragma once

// Bulk import/export of this node's zone (SPEC §9.1, PLAN M2).
//
// Formats:
//   TOML: an array of tables, one per record:
//         [[record]]
//         name = "app.lan"
//         type = "A"
//         ttl = 60
//         target = ["192.168.1.10", "192.168.1.11"]
//   CSV:  one record per line, `name,type,ttl,target[;target...]`; the header
//         line is optional. Comments start with '#'.
//
// Import is all-or-nothing: validation runs for every row before the first
// write, and the writes run inside one Store transaction so a failure halfway
// leaves the zone untouched (AC-3.1: 100 000 rows must not leave a partial
// import). The seq is bumped once for the whole import (protocol.md §7.2:
// large changes are published as a snapshot).

#include <cstddef>
#include <istream>
#include <ostream>
#include <string>
#include <vector>

#include "core/domain/domain_service.hpp"
#include "core/util/result.hpp"

namespace ldns::domain {

// Refuse absurd imports early: a multi-GB file or a TOML array with millions
// of tables is a local OOM as much as a network flood (SPEC §14 discipline).
inline constexpr std::size_t kMaxImportBytes = 64u * 1024u * 1024u;
inline constexpr std::size_t kMaxImportRecords = 500'000;

struct ImportReport {
    std::size_t imported{0};
    std::size_t skipped{0};
    std::vector<std::string> warnings;
};

// Parses `text` and adds every record through `service.add`. `force` skips
// the public-TLD confirmation for all rows. Duplicates within the file use the
// last value (import replaces; it does not fail on an existing name).
Result<ImportReport> import_domains(DomainService& service, std::string_view text,
                                    bool toml_format, bool force);

// Serializes `entries` in TOML or CSV form.
std::string export_domains(const std::vector<DomainEntry>& entries, bool toml_format);

}  // namespace ldns::domain
