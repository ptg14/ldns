#include "core/store/migrations.hpp"

#include <sqlite3.h>

namespace ldns::store_detail {
namespace {

// Schema per SPEC §5.4. `data_json` holds an array of hex strings; names are
// stored in normalized presentation form. `epoch`/`seq` are 64-bit counters.
constexpr const char* kMigration1 = R"sql(
CREATE TABLE meta(
    key   TEXT PRIMARY KEY NOT NULL,
    value TEXT NOT NULL
);

CREATE TABLE my_records(
    name        TEXT    NOT NULL,
    type        INTEGER NOT NULL,
    ttl         INTEGER NOT NULL,
    target_kind TEXT    NOT NULL DEFAULT 'static'
                        CHECK (target_kind IN ('self','static')),
    data_json   TEXT    NOT NULL,
    created_at  INTEGER NOT NULL,
    updated_at  INTEGER NOT NULL,
    PRIMARY KEY (name, type)
) WITHOUT ROWID;

CREATE TABLE peers(
    node_id   BLOB    PRIMARY KEY NOT NULL,
    name      TEXT    NOT NULL DEFAULT '',
    epoch     INTEGER NOT NULL,
    seq       INTEGER NOT NULL,
    last_addr TEXT    NOT NULL DEFAULT '',
    last_seen INTEGER NOT NULL DEFAULT 0
) WITHOUT ROWID;

CREATE TABLE peer_records(
    owner_id   BLOB    NOT NULL,
    name       TEXT    NOT NULL,
    type       INTEGER NOT NULL,
    ttl        INTEGER NOT NULL,
    data_json  TEXT    NOT NULL,
    created_at INTEGER NOT NULL,
    PRIMARY KEY (owner_id, name, type)
) WITHOUT ROWID;

CREATE TABLE upstreams(
    idx     INTEGER PRIMARY KEY,
    address TEXT    NOT NULL,
    port    INTEGER NOT NULL DEFAULT 53,
    proto   TEXT    NOT NULL DEFAULT 'udp'
);

CREATE TABLE query_log(
    ts         INTEGER NOT NULL,
    client     TEXT    NOT NULL,
    qname      TEXT    NOT NULL,
    qtype      INTEGER NOT NULL,
    step       TEXT    NOT NULL,
    rcode      INTEGER NOT NULL,
    latency_us INTEGER NOT NULL
);
CREATE INDEX idx_query_log_ts ON query_log(ts);
)sql";

Result<void> exec(sqlite3* db, const char* sql) {
    char* message = nullptr;
    const int rc = sqlite3_exec(db, sql, nullptr, nullptr, &message);
    if (rc != SQLITE_OK) {
        std::string text = message != nullptr ? message : "sqlite error";
        sqlite3_free(message);
        return Error{400, text};
    }
    return {};
}

}  // namespace

Result<void> migrate(sqlite3* db, int current_version) {
    if (current_version == kSchemaVersion) return {};
    if (current_version > kSchemaVersion) {
        return Error{401, "database schema is newer than this build"};
    }
    if (current_version < 1) {
        if (auto status = exec(db, kMigration1); !status) return status;
    }
    const std::string pragma = "PRAGMA user_version = " + std::to_string(kSchemaVersion);
    return exec(db, pragma.c_str());
}

}  // namespace ldns::store_detail
