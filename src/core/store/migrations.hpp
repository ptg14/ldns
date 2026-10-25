#pragma once

// Internal: schema migrations for the SQLite store (PLAN M2).
// Not part of the public core API.

#include <string>

#include "core/util/result.hpp"

struct sqlite3;

namespace ldns::store_detail {

// Schema version written to PRAGMA user_version.
inline constexpr int kSchemaVersion = 1;

// Brings `db` from `current_version` to kSchemaVersion. Runs inside a
// transaction owned by the caller.
Result<void> migrate(sqlite3* db, int current_version);

}  // namespace ldns::store_detail
