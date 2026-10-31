#include "core/store/store.hpp"

#include <sqlite3.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <filesystem>
#include <random>
#include <utility>

#include <nlohmann/json.hpp>

#include "core/store/migrations.hpp"
#include "core/util/logger.hpp"

namespace ldns {
namespace {

namespace fs = std::filesystem;
using json = nlohmann::json;

// RAII wrapper for a prepared statement.
class Statement {
public:
    Statement(sqlite3* db, const char* sql) {
        if (sqlite3_prepare_v2(db, sql, -1, &stmt_, nullptr) != SQLITE_OK) stmt_ = nullptr;
    }
    ~Statement() {
        if (stmt_ != nullptr) sqlite3_finalize(stmt_);
    }
    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;

    sqlite3_stmt* get() const noexcept { return stmt_; }
    explicit operator bool() const noexcept { return stmt_ != nullptr; }

private:
    sqlite3_stmt* stmt_{nullptr};
};

// Binds text that may contain embedded NUL bytes (node ids are blobs; names
// and addresses never are, but the helper is shared).
void bind_text(sqlite3_stmt* stmt, int index, std::string_view value) {
    sqlite3_bind_text(stmt, index, value.data(), static_cast<int>(value.size()), SQLITE_TRANSIENT);
}

std::string column_text(sqlite3_stmt* stmt, int index) {
    const auto* text = reinterpret_cast<const char*>(sqlite3_column_text(stmt, index));
    const int bytes = sqlite3_column_bytes(stmt, index);
    return text != nullptr ? std::string(text, static_cast<std::size_t>(bytes)) : std::string();
}

std::uint64_t random_epoch() {
    std::random_device rd;
    std::uint64_t value = (static_cast<std::uint64_t>(rd()) << 32) | rd();
    return value == 0 ? 1 : value;
}

Result<void> exec_sql(sqlite3* db, const char* sql, std::string_view what) {
    char* message = nullptr;
    const int rc = sqlite3_exec(db, sql, nullptr, nullptr, &message);
    if (rc != SQLITE_OK) {
        std::string text = message != nullptr ? message : "sqlite error";
        sqlite3_free(message);
        return Error{403, std::string(what) + ": " + text};
    }
    return {};
}

std::string lower_ascii(std::string_view text) {
    std::string out(text);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return out;
}

}  // namespace

// ---------------------------------------------------------------------------
// Store
// ---------------------------------------------------------------------------
Error Store::db_error(int code, std::string_view what) const {
    return Error{code, std::string(what) + ": " + (db_ != nullptr ? sqlite3_errmsg(db_) : "closed")};
}

Result<Store> Store::open(const StoreOptions& options) {
    Store store;
    store.path_ = options.path;

    if (options.path != ":memory:" && !options.path.starts_with("file:")) {
        std::error_code ec;
        const fs::path parent = fs::path(options.path).parent_path();
        if (!parent.empty()) fs::create_directories(parent, ec);
        if (ec) return Error{404, "cannot create data directory: " + ec.message()};
    }

    const int rc = sqlite3_open_v2(options.path.c_str(), &store.db_,
                                   SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE |
                                       SQLITE_OPEN_FULLMUTEX,
                                   nullptr);
    if (rc != SQLITE_OK) {
        const std::string message = store.db_ != nullptr ? sqlite3_errmsg(store.db_) : "open failed";
        if (store.db_ != nullptr) sqlite3_close(store.db_);
        store.db_ = nullptr;
        return Error{404, "cannot open database " + options.path + ": " + message};
    }

    sqlite3_busy_timeout(store.db_, static_cast<int>(options.busy_timeout_ms));

    // WAL keeps readers running during writes and survives crashes (SPEC §15);
    // WAL + synchronous=FULL keeps the SPEC §15 promise that a record
    // reported as stored survives a crash. The cost is one fsync per commit;
    // bulk imports are a single transaction, so this is negligible.
    for (const char* pragma :
         {"PRAGMA journal_mode = WAL;", "PRAGMA synchronous = FULL;",
          "PRAGMA foreign_keys = ON;", "PRAGMA trusted_schema = OFF;"}) {
        if (auto status = exec_sql(store.db_, pragma, "pragma"); !status) return status.error();
    }

    int version = 0;
    {
        Statement stmt(store.db_, "PRAGMA user_version;");
        if (!stmt) return store.db_error(405, "read schema version");
        if (sqlite3_step(stmt.get()) == SQLITE_ROW) version = sqlite3_column_int(stmt.get(), 0);
    }

    const auto migrated = [&]() -> Result<void> {
        auto tx = store.begin();
        if (!tx) return tx.error();
        if (auto status = store_detail::migrate(store.db_, version); !status) return status.error();
        return tx.value().commit();
    }();
    if (!migrated) return migrated.error();

    return store;
}

Store::~Store() {
    // close_v2 is safe even if a statement is still outstanding: SQLite
    // defers the close until the last statement is finalized, instead of
    // returning SQLITE_BUSY and leaking the connection (plus the WAL/SHM
    // file handles and the file lock on Windows).
    if (put_stmt_ != nullptr) sqlite3_finalize(put_stmt_);
    if (db_ != nullptr) sqlite3_close_v2(db_);
}

Store::Store(Store&& other) noexcept
    : db_(std::exchange(other.db_, nullptr)), path_(std::move(other.path_)),
      in_tx_(std::exchange(other.in_tx_, false)),
      put_stmt_(std::exchange(other.put_stmt_, nullptr)) {}

Result<StoreIdentity> Store::identity() {
    std::lock_guard lock(mutex_);

    auto read_meta = [this](std::string_view key) -> Result<std::optional<std::string>> {
        Statement stmt(db_, "SELECT value FROM meta WHERE key = ?1;");
        if (!stmt) return db_error(406, "read meta");
        bind_text(stmt.get(), 1, key);
        const int rc = sqlite3_step(stmt.get());
        if (rc == SQLITE_ROW) return std::optional<std::string>(column_text(stmt.get(), 0));
        if (rc == SQLITE_DONE) return std::optional<std::string>();
        return db_error(406, "read meta");
    };

    StoreIdentity out;
    auto id_text = read_meta("node_id");
    if (!id_text) return id_text.error();
    auto epoch_text = read_meta("epoch");
    if (!epoch_text) return epoch_text.error();
    auto seq_text = read_meta("seq_self");
    if (!seq_text) return seq_text.error();

    if (id_text.value() && epoch_text.value() && seq_text.value()) {
        if (!from_hex(*id_text.value(), out.node_id)) {
            return Error{407, "stored node_id is corrupt"};
        }
        try {
            out.epoch = std::stoull(*epoch_text.value());
            out.seq = std::stoull(*seq_text.value());
        } catch (const std::exception&) {
            return Error{407, "stored identity is corrupt"};
        }
        return out;
    }

    // First open: create identity in one transaction.
    out.node_id = generate_node_id();
    out.epoch = random_epoch();
    out.seq = 0;

    auto tx = begin();
    if (!tx) return tx.error();
    if (auto status = set_meta("node_id", to_hex(out.node_id)); !status) return status.error();
    if (auto status = set_meta("epoch", std::to_string(out.epoch)); !status) return status.error();
    if (auto status = set_meta("seq_self", "0"); !status) return status.error();
    if (auto status = set_meta("schema_version", std::to_string(store_detail::kSchemaVersion));
        !status) {
        return status.error();
    }
    if (auto status = tx.value().commit(); !status) return status.error();

    logger()->info("created node identity {}", short_id(out.node_id));
    return out;
}

Result<std::uint64_t> Store::bump_seq() {
    std::lock_guard lock(mutex_);
    if (!in_tx_) return Error{408, "bump_seq requires an open transaction"};

    // Read and validate first: a corrupt seq must not silently jump to 1,
    // which peers would treat as stale (protocol.md §7.3).
    Statement read(db_, "SELECT value FROM meta WHERE key = 'seq_self';");
    if (!read) return db_error(409, "read seq");
    if (sqlite3_step(read.get()) != SQLITE_ROW) {
        return Error{410, "identity missing; call identity() first"};
    }
    const std::string stored = column_text(read.get(), 0);
    std::uint64_t current = 0;
    try {
        std::size_t consumed = 0;
        current = std::stoull(stored, &consumed);
        if (consumed != stored.size()) throw std::invalid_argument("trailing");
    } catch (const std::exception&) {
        return Error{407, "stored seq_self is corrupt"};
    }
    if (current == UINT64_MAX) return Error{407, "seq_self overflow"};

    const std::uint64_t next = current + 1;
    Statement stmt(db_, "UPDATE meta SET value = ?1 WHERE key = 'seq_self';");
    if (!stmt) return db_error(409, "bump seq");
    bind_text(stmt.get(), 1, std::to_string(next));
    if (sqlite3_step(stmt.get()) != SQLITE_DONE) return db_error(409, "bump seq");
    if (sqlite3_changes(db_) != 1) return Error{410, "identity missing; call identity() first"};
    return next;
}

Result<std::optional<std::string>> Store::meta(std::string_view key) {
    std::lock_guard lock(mutex_);
    Statement stmt(db_, "SELECT value FROM meta WHERE key = ?1;");
    if (!stmt) return db_error(406, "read meta");
    bind_text(stmt.get(), 1, key);
    const int rc = sqlite3_step(stmt.get());
    if (rc == SQLITE_ROW) return std::optional<std::string>(column_text(stmt.get(), 0));
    if (rc == SQLITE_DONE) return std::optional<std::string>();
    return db_error(406, "read meta");
}

Result<void> Store::set_meta(std::string_view key, std::string_view value) {
    std::lock_guard lock(mutex_);
    Statement stmt(db_,
                   "INSERT INTO meta(key, value) VALUES(?1, ?2) "
                   "ON CONFLICT(key) DO UPDATE SET value = excluded.value;");
    if (!stmt) return db_error(411, "write meta");
    bind_text(stmt.get(), 1, key);
    bind_text(stmt.get(), 2, value);
    if (sqlite3_step(stmt.get()) != SQLITE_DONE) return db_error(411, "write meta");
    return {};
}

Result<std::string> Store::encode_data(const std::vector<std::vector<std::uint8_t>>& data) {
    json array = json::array();
    for (const auto& value : data) array.push_back(to_hex(std::span<const std::uint8_t>(value)));
    return array.dump();
}

Result<std::vector<std::vector<std::uint8_t>>> Store::decode_data(std::string_view text) {
    const json parsed = json::parse(text.begin(), text.end(), nullptr, /*allow_exceptions=*/false);
    if (parsed.is_discarded() || !parsed.is_array()) return Error{412, "corrupt data_json"};
    if (parsed.size() > 256) return Error{412, "corrupt data_json: too many values"};

    std::vector<std::vector<std::uint8_t>> out;
    out.reserve(parsed.size());
    for (const auto& element : parsed) {
        if (!element.is_string()) return Error{412, "corrupt data_json: value is not a string"};
        const std::string hex = element.get<std::string>();
        if (hex.size() % 2 != 0 || hex.size() > 2048) return Error{412, "corrupt data_json: bad hex"};
        std::vector<std::uint8_t> decoded;
        decoded.reserve(hex.size() / 2);
        for (std::size_t i = 0; i < hex.size(); i += 2) {
            const auto nibble = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return -1;
            };
            const int hi = nibble(hex[i]);
            const int lo = nibble(hex[i + 1]);
            if (hi < 0 || lo < 0) return Error{412, "corrupt data_json: bad hex"};
            decoded.push_back(static_cast<std::uint8_t>((hi << 4) | lo));
        }
        out.push_back(std::move(decoded));
    }
    return out;
}

Result<std::vector<RecordRow>> Store::records() {
    std::lock_guard lock(mutex_);
    Statement stmt(db_, "SELECT name, type, ttl, target_kind, data_json, created_at, updated_at "
                        "FROM my_records ORDER BY name, type;");
    if (!stmt) return db_error(413, "list records");

    std::vector<RecordRow> out;
    while (true) {
        const int rc = sqlite3_step(stmt.get());
        if (rc == SQLITE_DONE) break;
        if (rc != SQLITE_ROW) return db_error(413, "list records");

        RecordRow row;
        row.name = column_text(stmt.get(), 0);
        row.type = static_cast<std::uint16_t>(sqlite3_column_int(stmt.get(), 1));
        row.ttl = static_cast<std::uint32_t>(sqlite3_column_int64(stmt.get(), 2));
        row.target_kind = column_text(stmt.get(), 3);
        row.created = static_cast<HlcTimestamp>(sqlite3_column_int64(stmt.get(), 5));
        row.updated = static_cast<HlcTimestamp>(sqlite3_column_int64(stmt.get(), 6));
        auto data = decode_data(column_text(stmt.get(), 4));
        if (!data) return data.error();
        row.data = std::move(data.value());
        out.push_back(std::move(row));
    }
    return out;
}

Result<std::vector<RecordRow>> Store::records_for_name(std::string_view name) {
    std::lock_guard lock(mutex_);
    Statement stmt(db_, "SELECT name, type, ttl, target_kind, data_json, created_at, updated_at "
                        "FROM my_records WHERE name = ?1 ORDER BY type;");
    if (!stmt) return db_error(413, "list records");
    bind_text(stmt.get(), 1, name);

    std::vector<RecordRow> out;
    while (true) {
        const int rc = sqlite3_step(stmt.get());
        if (rc == SQLITE_DONE) break;
        if (rc != SQLITE_ROW) return db_error(413, "list records");

        RecordRow row;
        row.name = column_text(stmt.get(), 0);
        row.type = static_cast<std::uint16_t>(sqlite3_column_int(stmt.get(), 1));
        row.ttl = static_cast<std::uint32_t>(sqlite3_column_int64(stmt.get(), 2));
        row.target_kind = column_text(stmt.get(), 3);
        row.created = static_cast<HlcTimestamp>(sqlite3_column_int64(stmt.get(), 5));
        row.updated = static_cast<HlcTimestamp>(sqlite3_column_int64(stmt.get(), 6));
        auto data = decode_data(column_text(stmt.get(), 4));
        if (!data) return data.error();
        row.data = std::move(data.value());
        out.push_back(std::move(row));
    }
    return out;
}

Result<void> Store::put_record(const RecordRow& row) {
    std::lock_guard lock(mutex_);
    auto encoded = encode_data(row.data);
    if (!encoded) return encoded.error();

    if (put_stmt_ == nullptr) {
        const char* sql =
            "INSERT INTO my_records(name, type, ttl, target_kind, data_json, created_at, "
            "updated_at) VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7) "
            "ON CONFLICT(name, type) DO UPDATE SET ttl = excluded.ttl, "
            "target_kind = excluded.target_kind, data_json = excluded.data_json, "
            "updated_at = excluded.updated_at;";
        if (sqlite3_prepare_v2(db_, sql, -1, &put_stmt_, nullptr) != SQLITE_OK) {
            put_stmt_ = nullptr;
            return db_error(414, "write record");
        }
    }

    sqlite3_stmt* stmt = put_stmt_;
    sqlite3_reset(stmt);
    sqlite3_clear_bindings(stmt);
    bind_text(stmt, 1, row.name);
    sqlite3_bind_int(stmt, 2, row.type);
    sqlite3_bind_int64(stmt, 3, row.ttl);
    bind_text(stmt, 4, row.target_kind);
    bind_text(stmt, 5, encoded.value());
    sqlite3_bind_int64(stmt, 6, static_cast<sqlite3_int64>(row.created));
    sqlite3_bind_int64(stmt, 7, static_cast<sqlite3_int64>(row.updated));
    if (sqlite3_step(stmt) != SQLITE_DONE) return db_error(414, "write record");
    return {};
}

Result<bool> Store::delete_record(std::string_view name, std::uint16_t type) {
    std::lock_guard lock(mutex_);
    Statement stmt(db_, "DELETE FROM my_records WHERE name = ?1 AND type = ?2;");
    if (!stmt) return db_error(415, "delete record");
    bind_text(stmt.get(), 1, name);
    sqlite3_bind_int(stmt.get(), 2, type);
    if (sqlite3_step(stmt.get()) != SQLITE_DONE) return db_error(415, "delete record");
    return sqlite3_changes(db_) > 0;
}

Result<std::vector<StoredPeer>> Store::peers() {
    std::lock_guard lock(mutex_);
    Statement stmt(db_, "SELECT node_id, name, epoch, seq, last_addr, last_seen FROM peers ORDER BY last_seen DESC;");
    if (!stmt) return db_error(418, "list peers");

    std::vector<StoredPeer> out;
    while (true) {
        const int rc = sqlite3_step(stmt.get());
        if (rc == SQLITE_DONE) break;
        if (rc != SQLITE_ROW) return db_error(418, "list peers");

        StoredPeer p;
        const void* blob = sqlite3_column_blob(stmt.get(), 0);
        const int bytes = sqlite3_column_bytes(stmt.get(), 0);
        if (bytes == 16 && blob != nullptr) {
            std::memcpy(p.node_id.data(), blob, 16);
        }
        p.name = column_text(stmt.get(), 1);
        p.epoch = static_cast<std::uint64_t>(sqlite3_column_int64(stmt.get(), 2));
        p.seq = static_cast<std::uint64_t>(sqlite3_column_int64(stmt.get(), 3));
        p.last_addr = column_text(stmt.get(), 4);
        p.last_seen = sqlite3_column_int64(stmt.get(), 5);
        out.push_back(std::move(p));
    }
    return out;
}

Result<void> Store::put_peer(const StoredPeer& peer) {
    std::lock_guard lock(mutex_);
    Statement stmt(db_,
                   "INSERT INTO peers(node_id, name, epoch, seq, last_addr, last_seen) "
                   "VALUES(?1, ?2, ?3, ?4, ?5, ?6) "
                   "ON CONFLICT(node_id) DO UPDATE SET "
                   "name = excluded.name, epoch = excluded.epoch, seq = excluded.seq, "
                   "last_addr = excluded.last_addr, last_seen = excluded.last_seen;");
    if (!stmt) return db_error(419, "put peer");

    sqlite3_bind_blob(stmt.get(), 1, peer.node_id.data(), static_cast<int>(peer.node_id.size()), SQLITE_TRANSIENT);
    bind_text(stmt.get(), 2, peer.name);
    sqlite3_bind_int64(stmt.get(), 3, static_cast<sqlite3_int64>(peer.epoch));
    sqlite3_bind_int64(stmt.get(), 4, static_cast<sqlite3_int64>(peer.seq));
    bind_text(stmt.get(), 5, peer.last_addr);
    sqlite3_bind_int64(stmt.get(), 6, static_cast<sqlite3_int64>(peer.last_seen));

    if (sqlite3_step(stmt.get()) != SQLITE_DONE) return db_error(419, "put peer");
    return {};
}

Result<bool> Store::delete_peer(const NodeId& node_id) {
    std::lock_guard lock(mutex_);
    Statement stmt(db_, "DELETE FROM peers WHERE node_id = ?1;");
    if (!stmt) return db_error(420, "delete peer");
    sqlite3_bind_blob(stmt.get(), 1, node_id.data(), static_cast<int>(node_id.size()), SQLITE_TRANSIENT);
    if (sqlite3_step(stmt.get()) != SQLITE_DONE) return db_error(420, "delete peer");
    return sqlite3_changes(db_) > 0;
}

Result<std::vector<std::string>> Store::manual_peers() {
    auto val = meta("manual_peers");
    if (!val || !val.value().has_value()) return std::vector<std::string>{};
    try {
        auto j = json::parse(val.value().value());
        if (j.is_array()) {
            return j.get<std::vector<std::string>>();
        }
    } catch (...) {}
    return std::vector<std::string>{};
}

Result<void> Store::add_manual_peer(std::string_view endpoint) {
    auto current = manual_peers().value_or(std::vector<std::string>{});
    std::string ep_str(endpoint);
    if (std::find(current.begin(), current.end(), ep_str) == current.end()) {
        current.push_back(ep_str);
        json j = current;
        return set_meta("manual_peers", j.dump());
    }
    return {};
}

Result<void> Store::remove_manual_peer(std::string_view endpoint) {
    auto current = manual_peers().value_or(std::vector<std::string>{});
    std::string ep_str(endpoint);
    auto it = std::remove(current.begin(), current.end(), ep_str);
    if (it != current.end()) {
        current.erase(it, current.end());
        json j = current;
        return set_meta("manual_peers", j.dump());
    }
    return {};
}

Result<void> Store::log_op(const OpLogRow& row) {
    std::lock_guard lock(mutex_);
    Statement stmt(db_,
                   "INSERT INTO op_log(seq, op, name, type, ttl, data_json, created_at, ts) "
                   "VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8) "
                   "ON CONFLICT(seq, name, type) DO UPDATE SET "
                   "op = excluded.op, ttl = excluded.ttl, data_json = excluded.data_json, "
                   "created_at = excluded.created_at, ts = excluded.ts;");
    if (!stmt) return db_error(421, "log op");

    auto encoded = encode_data(row.data);
    if (!encoded) return encoded.error();

    sqlite3_bind_int64(stmt.get(), 1, static_cast<sqlite3_int64>(row.seq));
    bind_text(stmt.get(), 2, row.op);
    bind_text(stmt.get(), 3, row.name);
    sqlite3_bind_int(stmt.get(), 4, row.type);
    sqlite3_bind_int64(stmt.get(), 5, row.ttl);
    bind_text(stmt.get(), 6, encoded.value());
    sqlite3_bind_int64(stmt.get(), 7, static_cast<sqlite3_int64>(row.created_at));
    sqlite3_bind_int64(stmt.get(), 8, static_cast<sqlite3_int64>(row.ts));

    if (sqlite3_step(stmt.get()) != SQLITE_DONE) return db_error(421, "log op");
    return {};
}

Result<std::vector<OpLogRow>> Store::ops_since(std::uint64_t since_seq, std::uint64_t max_seq) {
    std::lock_guard lock(mutex_);
    Statement stmt(db_,
                   "SELECT seq, op, name, type, ttl, data_json, created_at, ts "
                   "FROM op_log WHERE seq > ?1 AND seq <= ?2 ORDER BY seq;");
    if (!stmt) return db_error(422, "query ops");

    const auto bound_since = std::min<std::uint64_t>(since_seq, static_cast<std::uint64_t>(INT64_MAX));
    const auto bound_max = std::min<std::uint64_t>(max_seq, static_cast<std::uint64_t>(INT64_MAX));
    sqlite3_bind_int64(stmt.get(), 1, static_cast<sqlite3_int64>(bound_since));
    sqlite3_bind_int64(stmt.get(), 2, static_cast<sqlite3_int64>(bound_max));

    std::vector<OpLogRow> out;
    while (true) {
        const int rc = sqlite3_step(stmt.get());
        if (rc == SQLITE_DONE) break;
        if (rc != SQLITE_ROW) return db_error(422, "query ops");

        OpLogRow row;
        row.seq = static_cast<std::uint64_t>(sqlite3_column_int64(stmt.get(), 0));
        row.op = column_text(stmt.get(), 1);
        row.name = column_text(stmt.get(), 2);
        row.type = static_cast<std::uint16_t>(sqlite3_column_int(stmt.get(), 3));
        row.ttl = static_cast<std::uint32_t>(sqlite3_column_int64(stmt.get(), 4));
        auto decoded = decode_data(column_text(stmt.get(), 5));
        if (decoded) row.data = std::move(decoded.value());
        row.created_at = static_cast<std::uint64_t>(sqlite3_column_int64(stmt.get(), 6));
        row.ts = static_cast<std::uint64_t>(sqlite3_column_int64(stmt.get(), 7));
        out.push_back(std::move(row));
    }
    return out;
}

Result<std::uint64_t> Store::oldest_op_seq() {
    std::lock_guard lock(mutex_);
    Statement stmt(db_, "SELECT MIN(seq) FROM op_log;");
    if (!stmt) return db_error(423, "query oldest op seq");
    if (sqlite3_step(stmt.get()) == SQLITE_ROW && sqlite3_column_type(stmt.get(), 0) != SQLITE_NULL) {
        return static_cast<std::uint64_t>(sqlite3_column_int64(stmt.get(), 0));
    }
    return 0;
}

Result<void> Store::purge_op_log_before(std::uint64_t before_seq) {
    std::lock_guard lock(mutex_);
    Statement stmt(db_, "DELETE FROM op_log WHERE seq < ?1;");
    if (!stmt) return db_error(424, "purge op log");
    const auto bound_before = std::min<std::uint64_t>(before_seq, static_cast<std::uint64_t>(INT64_MAX));
    sqlite3_bind_int64(stmt.get(), 1, static_cast<sqlite3_int64>(bound_before));
    if (sqlite3_step(stmt.get()) != SQLITE_DONE) return db_error(424, "purge op log");
    return {};
}

Result<std::vector<PeerRecordRow>> Store::peer_records(const NodeId& owner_id) {
    std::lock_guard lock(mutex_);
    Statement stmt(db_, "SELECT owner_id, name, type, ttl, data_json, created_at FROM peer_records WHERE owner_id = ?1;");
    if (!stmt) return db_error(425, "query peer records");
    sqlite3_bind_blob(stmt.get(), 1, owner_id.data(), static_cast<int>(owner_id.size()), SQLITE_TRANSIENT);

    std::vector<PeerRecordRow> out;
    while (true) {
        const int rc = sqlite3_step(stmt.get());
        if (rc == SQLITE_DONE) break;
        if (rc != SQLITE_ROW) return db_error(425, "query peer records");

        PeerRecordRow row;
        row.owner_id = owner_id;
        row.name = column_text(stmt.get(), 1);
        row.type = static_cast<std::uint16_t>(sqlite3_column_int(stmt.get(), 2));
        row.ttl = static_cast<std::uint32_t>(sqlite3_column_int64(stmt.get(), 3));
        auto decoded = decode_data(column_text(stmt.get(), 4));
        if (decoded) row.data = std::move(decoded.value());
        row.created_at = static_cast<std::uint64_t>(sqlite3_column_int64(stmt.get(), 5));
        out.push_back(std::move(row));
    }
    return out;
}

Result<std::vector<PeerRecordRow>> Store::all_peer_records() {
    std::lock_guard lock(mutex_);
    Statement stmt(db_, "SELECT owner_id, name, type, ttl, data_json, created_at FROM peer_records;");
    if (!stmt) return db_error(426, "query all peer records");

    std::vector<PeerRecordRow> out;
    while (true) {
        const int rc = sqlite3_step(stmt.get());
        if (rc == SQLITE_DONE) break;
        if (rc != SQLITE_ROW) return db_error(426, "query all peer records");

        PeerRecordRow row;
        const void* blob = sqlite3_column_blob(stmt.get(), 0);
        const int bytes = sqlite3_column_bytes(stmt.get(), 0);
        if (bytes == 16 && blob != nullptr) {
            std::memcpy(row.owner_id.data(), blob, 16);
        }
        row.name = column_text(stmt.get(), 1);
        row.type = static_cast<std::uint16_t>(sqlite3_column_int(stmt.get(), 2));
        row.ttl = static_cast<std::uint32_t>(sqlite3_column_int64(stmt.get(), 3));
        auto decoded = decode_data(column_text(stmt.get(), 4));
        if (decoded) row.data = std::move(decoded.value());
        row.created_at = static_cast<std::uint64_t>(sqlite3_column_int64(stmt.get(), 5));
        out.push_back(std::move(row));
    }
    return out;
}

Result<void> Store::put_peer_record(const PeerRecordRow& row) {
    std::lock_guard lock(mutex_);
    Statement stmt(db_,
                   "INSERT INTO peer_records(owner_id, name, type, ttl, data_json, created_at) "
                   "VALUES(?1, ?2, ?3, ?4, ?5, ?6) "
                   "ON CONFLICT(owner_id, name, type) DO UPDATE SET "
                   "ttl = excluded.ttl, data_json = excluded.data_json, created_at = excluded.created_at;");
    if (!stmt) return db_error(427, "put peer record");

    auto encoded = encode_data(row.data);
    if (!encoded) return encoded.error();

    sqlite3_bind_blob(stmt.get(), 1, row.owner_id.data(), static_cast<int>(row.owner_id.size()), SQLITE_TRANSIENT);
    bind_text(stmt.get(), 2, row.name);
    sqlite3_bind_int(stmt.get(), 3, row.type);
    sqlite3_bind_int64(stmt.get(), 4, row.ttl);
    bind_text(stmt.get(), 5, encoded.value());
    sqlite3_bind_int64(stmt.get(), 6, static_cast<sqlite3_int64>(row.created_at));

    if (sqlite3_step(stmt.get()) != SQLITE_DONE) return db_error(427, "put peer record");
    return {};
}

Result<bool> Store::delete_peer_record(const NodeId& owner_id, std::string_view name, std::uint16_t type) {
    std::lock_guard lock(mutex_);
    Statement stmt(db_, "DELETE FROM peer_records WHERE owner_id = ?1 AND name = ?2 AND type = ?3;");
    if (!stmt) return db_error(428, "delete peer record");
    sqlite3_bind_blob(stmt.get(), 1, owner_id.data(), static_cast<int>(owner_id.size()), SQLITE_TRANSIENT);
    bind_text(stmt.get(), 2, name);
    sqlite3_bind_int(stmt.get(), 3, type);
    if (sqlite3_step(stmt.get()) != SQLITE_DONE) return db_error(428, "delete peer record");
    return sqlite3_changes(db_) > 0;
}

Result<void> Store::replace_peer_records(const NodeId& owner_id, const std::vector<PeerRecordRow>& rows) {
    std::lock_guard lock(mutex_);
    auto tx = begin();
    if (!tx) return tx.error();

    if (auto status = delete_peer_records_for_owner(owner_id); !status) return status;
    for (const auto& r : rows) {
        if (auto status = put_peer_record(r); !status) return status;
    }
    return tx.value().commit();
}

Result<void> Store::delete_peer_records_for_owner(const NodeId& owner_id) {
    std::lock_guard lock(mutex_);
    Statement stmt(db_, "DELETE FROM peer_records WHERE owner_id = ?1;");
    if (!stmt) return db_error(429, "delete peer records for owner");
    sqlite3_bind_blob(stmt.get(), 1, owner_id.data(), static_cast<int>(owner_id.size()), SQLITE_TRANSIENT);
    if (sqlite3_step(stmt.get()) != SQLITE_DONE) return db_error(429, "delete peer records for owner");
    return {};
}

Result<std::vector<UpstreamRow>> Store::upstreams() {
    std::lock_guard lock(mutex_);
    Statement stmt(db_, "SELECT address, port, proto FROM upstreams ORDER BY idx;");
    if (!stmt) return db_error(416, "list upstreams");

    std::vector<UpstreamRow> out;
    while (true) {
        const int rc = sqlite3_step(stmt.get());
        if (rc == SQLITE_DONE) break;
        if (rc != SQLITE_ROW) return db_error(416, "list upstreams");
        out.push_back(UpstreamRow{column_text(stmt.get(), 0),
                                  static_cast<std::uint16_t>(sqlite3_column_int(stmt.get(), 1)),
                                  lower_ascii(column_text(stmt.get(), 2))});
    }
    return out;
}

Result<void> Store::replace_upstreams(const std::vector<UpstreamRow>& rows) {
    std::lock_guard lock(mutex_);
    // One transaction: a failure halfway must not leave the table empty.
    auto tx = begin();
    if (!tx) return tx.error();
    if (auto status = exec_sql(db_, "DELETE FROM upstreams;", "clear upstreams"); !status) {
        return status;
    }
    for (std::size_t i = 0; i < rows.size(); ++i) {
        Statement stmt(db_, "INSERT INTO upstreams(idx, address, port, proto) VALUES(?1, ?2, ?3, ?4);");
        if (!stmt) return db_error(417, "write upstream");
        sqlite3_bind_int64(stmt.get(), 1, static_cast<sqlite3_int64>(i));
        bind_text(stmt.get(), 2, rows[i].address);
        sqlite3_bind_int(stmt.get(), 3, rows[i].port);
        bind_text(stmt.get(), 4, rows[i].proto);
        if (sqlite3_step(stmt.get()) != SQLITE_DONE) return db_error(417, "write upstream");
    }
    return tx.value().commit();
}

Result<Store::Tx> Store::begin() {
    auto lock = std::unique_lock<std::recursive_mutex>(mutex_);
    if (in_tx_) return Error{418, "nested transactions are not supported"};
    if (sqlite3_exec(db_, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        return db_error(419, "begin transaction");
    }
    in_tx_ = true;
    return Tx(*this, std::move(lock));
}

// ---------------------------------------------------------------------------
// Tx
// ---------------------------------------------------------------------------
Store::Tx::Tx(Store& store, std::unique_lock<std::recursive_mutex> lock)
    : store_(&store), lock_(std::move(lock)) {}

Store::Tx::Tx(Tx&& other) noexcept
    : store_(std::exchange(other.store_, nullptr)), lock_(std::move(other.lock_)),
      active_(other.active_) {
    other.active_ = false;
}

Store::Tx::~Tx() {
    if (!active_ || store_ == nullptr) return;
    sqlite3_exec(store_->db_, "ROLLBACK;", nullptr, nullptr, nullptr);
    store_->in_tx_ = false;
}

Result<void> Store::Tx::commit() {
    if (!active_ || store_ == nullptr) return Error{420, "transaction is not active"};
    if (sqlite3_exec(store_->db_, "COMMIT;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        const Error error = store_->db_error(421, "commit transaction");
        sqlite3_exec(store_->db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        store_->in_tx_ = false;
        active_ = false;
        return error;
    }
    store_->in_tx_ = false;
    active_ = false;
    // The transaction is over: stop holding the Store mutex. Callers such as
    // DomainService::add_many do substantial work after committing (mirroring
    // the zone) and must not block every other Store user meanwhile. The
    // unique_lock destructor is a no-op once unlocked, so rollback in ~Tx
    // cannot run (active_ is already false).
    if (lock_.owns_lock()) lock_.unlock();
    return {};
}

}  // namespace ldns
