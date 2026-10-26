#pragma once

// SQLite persistence (SPEC §5.4, §12, PLAN M2).
//
// One connection, WAL mode, migrations tracked with PRAGMA user_version.
// A Store is not copyable; it may be moved. Calls are serialized with an
// internal recursive mutex so a transaction can span several calls on the
// same thread while other threads block until it commits (PLAN §6: the DNS
// hot path never touches SQLite; a small worker pool will own writes later).
//
// Identity (node_id, epoch, seq) lives in the `meta` table and is created on
// first open. `seq` is bumped once per zone mutation transaction
// (protocol.md §7.2).

#include <cstdint>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/util/hlc.hpp"
#include "core/util/id.hpp"
#include "core/util/result.hpp"

struct sqlite3;
struct sqlite3_stmt;

namespace ldns {

// One row of `my_records`. `name` is the normalized presentation form and
// `data` values are already in wire format for `type` (see dns/message.hpp).
struct RecordRow {
    std::string name;
    std::uint16_t type{};
    std::uint32_t ttl{};
    std::string target_kind{"static"};  // "self" | "static" (SPEC §5.2)
    std::vector<std::vector<std::uint8_t>> data;
    HlcTimestamp created{};
    HlcTimestamp updated{};
};

struct StoreIdentity {
    NodeId node_id{};
    std::uint64_t epoch{};
    std::uint64_t seq{};
};

struct StoreOptions {
    // ":memory:" for tests, otherwise a file path. Parent directories are
    // created as needed.
    std::string path{":memory:"};
    std::uint32_t busy_timeout_ms{5000};
};

struct StoredPeer {
    NodeId node_id{};
    std::string name;
    std::uint64_t epoch{0};
    std::uint64_t seq{0};
    std::string last_addr;
    std::int64_t last_seen{0};
};

struct OpLogRow {
    std::uint64_t seq{0};
    std::string op; // "put" | "del"
    std::string name;
    std::uint16_t type{1};
    std::uint32_t ttl{0};
    std::vector<std::vector<std::uint8_t>> data;
    std::uint64_t created_at{0};
    std::uint64_t ts{0};
};

struct PeerRecordRow {
    NodeId owner_id{};
    std::string name;
    std::uint16_t type{1};
    std::uint32_t ttl{0};
    std::vector<std::vector<std::uint8_t>> data;
    std::uint64_t created_at{0};
};

struct UpstreamRow {
    std::string address;
    std::uint16_t port{53};
    std::string proto{"udp"};
};

class Store {
public:
    class Tx;

    static Result<Store> open(const StoreOptions& options);
    ~Store();

    Store(Store&& other) noexcept;
    Store& operator=(Store&&) = delete;
    Store(const Store&) = delete;
    Store& operator=(const Store&) = delete;

    // --- identity ----------------------------------------------------------
    // Creates node_id/epoch/seq on first use; never changes them afterwards.
    Result<StoreIdentity> identity();
    // Increments seq by one and returns the new value. Call inside a Tx
    // together with the record change it accounts for (protocol.md §7.2).
    // Fails with 407 when the stored value is corrupt.
    Result<std::uint64_t> bump_seq();

    // --- meta --------------------------------------------------------------
    Result<std::optional<std::string>> meta(std::string_view key);
    Result<void> set_meta(std::string_view key, std::string_view value);

    // --- my_records --------------------------------------------------------
    Result<std::vector<RecordRow>> records();
    Result<std::vector<RecordRow>> records_for_name(std::string_view name);
    Result<void> put_record(const RecordRow& row);
    Result<bool> delete_record(std::string_view name, std::uint16_t type);

    // --- peers -------------------------------------------------------------
    Result<std::vector<StoredPeer>> peers();
    Result<void> put_peer(const StoredPeer& peer);
    Result<bool> delete_peer(const NodeId& node_id);

    // --- manual_peers ------------------------------------------------------
    Result<std::vector<std::string>> manual_peers();
    Result<void> add_manual_peer(std::string_view endpoint);
    Result<void> remove_manual_peer(std::string_view endpoint);

    // --- op_log ------------------------------------------------------------
    Result<void> log_op(const OpLogRow& row);
    Result<std::vector<OpLogRow>> ops_since(std::uint64_t since_seq, std::uint64_t max_seq = UINT64_MAX);
    Result<std::uint64_t> oldest_op_seq();
    Result<void> purge_op_log_before(std::uint64_t before_seq);

    // --- peer_records ------------------------------------------------------
    Result<std::vector<PeerRecordRow>> peer_records(const NodeId& owner_id);
    Result<std::vector<PeerRecordRow>> all_peer_records();
    Result<void> put_peer_record(const PeerRecordRow& row);
    Result<bool> delete_peer_record(const NodeId& owner_id, std::string_view name, std::uint16_t type);
    Result<void> replace_peer_records(const NodeId& owner_id, const std::vector<PeerRecordRow>& rows);
    Result<void> delete_peer_records_for_owner(const NodeId& owner_id);

    // --- upstreams ---------------------------------------------------------
    Result<std::vector<UpstreamRow>> upstreams();
    Result<void> replace_upstreams(const std::vector<UpstreamRow>& rows);

    // --- transactions ------------------------------------------------------
    // BEGIN IMMEDIATE; commits on commit(), rolls back when destroyed.
    Result<Tx> begin();

    const std::string& path() const noexcept { return path_; }

    // Encodes/decodes the JSON array of hex strings used in data_json.
    static Result<std::string> encode_data(const std::vector<std::vector<std::uint8_t>>& data);
    static Result<std::vector<std::vector<std::uint8_t>>> decode_data(std::string_view json);

private:
    Store() = default;

    Error db_error(int code, std::string_view what) const;

    sqlite3* db_{nullptr};
    std::string path_;
    std::recursive_mutex mutex_;
    bool in_tx_{false};
    // Cached statements for the hot write path; finalized in ~Store. Compiling
    // the upsert once per row dominates a 100 000-row import.
    sqlite3_stmt* put_stmt_{nullptr};
};

// RAII transaction. Destructor rolls back unless commit() succeeded.
class Store::Tx {
public:
    Tx(Tx&& other) noexcept;
    Tx& operator=(Tx&&) = delete;
    Tx(const Tx&) = delete;
    Tx& operator=(const Tx&) = delete;
    ~Tx();

    Result<void> commit();

private:
    friend class Store;
    Tx(Store& store, std::unique_lock<std::recursive_mutex> lock);

    Store* store_{nullptr};
    std::unique_lock<std::recursive_mutex> lock_;
    bool active_{true};
};

}  // namespace ldns
