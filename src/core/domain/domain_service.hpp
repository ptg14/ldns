#pragma once

// Domain CRUD for the node's own zone (SPEC §9, PLAN M2).
//
// Every mutation is one atomic unit:
//   1. validate the name/type/values (SPEC §5.1, §5.2, protocol.md §2.3);
//   2. write my_records and bump seq inside one SQLite transaction
//      (protocol.md §7.2: one transaction = one seq);
//   3. mirror the change into LocalZone so DNS answers switch immediately;
//   4. emit a RecordPut/RecordDel event with the owner and seq.
//
// The service is the single writer for this node's zone. Peer updates will
// arrive through the Registry (M5) and never through this class.
//
// Thread safety: Store serializes transactions, but DomainService itself is
// not internally synchronized (the HLC clock and the event handler are
// shared). Callers must serialize mutations on one thread or wrap them in an
// external mutex; reads through CoreApi::zone() are safe concurrently.
//
// Errors carry stable codes so the CLI/TUI can map them to exit codes:
//   700 invalid name        701 invalid type      702 invalid value
//   703 name already exists 704 record not found  705 TLD needs confirmation
//   706 CNAME conflict      707 store failure     708 no self address

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/dns/local_zone.hpp"
#include "core/net/protocol_message.hpp"
#include "core/dns/name.hpp"
#include "core/store/store.hpp"
#include "core/util/hlc.hpp"
#include "core/util/result.hpp"

namespace ldns::domain {

// PUBLISHABLE addresses. SelfAddress::enumerate returns these.
using SelfAddressBytes = std::vector<std::uint8_t>;

enum class TargetKind { Self, Static };

inline constexpr std::uint32_t kDefaultTtl = 60;

struct AddRequest {
    std::string name;
    std::string type{"A"};  // A, AAAA, CNAME, TXT (SPEC §5.2)
    // Wire targets. When kind == Self the caller expands the node's current
    // LAN addresses into this list (and leaves kind = Self so an IP change
    // can refresh the record, SPEC §9.2).
    std::vector<std::string> targets;
    std::uint32_t ttl{kDefaultTtl};
    TargetKind kind{TargetKind::Static};
    // Skip the public-TLD confirmation (CLI --force, SPEC §9.1).
    bool force{false};
};

struct Validation {
    // The name ends in a public TLD: the UI must confirm before adding.
    bool needs_tld_confirmation{false};
    // The name ends in ".local": warn about mDNS (SPEC §9.1).
    bool mdns_conflict{false};
    std::string warning;
};

struct DomainEntry {
    std::string name;
    std::string type;
    std::uint32_t ttl{};
    std::string target_kind{"static"};
    std::vector<std::string> targets;  // presentation form
    HlcTimestamp created{};
    HlcTimestamp updated{};
};

enum class EventType { RecordPut, RecordDel };

struct DomainEvent {
    EventType kind{EventType::RecordPut};
    std::uint64_t seq{};
    std::string name;
    std::string type;
};

// Mirrors the domain changes into the in-memory zone. Implemented by
// Application/Resolver wiring; tests can use a no-op sink.
class DomainSink {
public:
    virtual ~DomainSink() = default;
    virtual Result<void> apply_put(const dns::Name& name, std::uint16_t type, std::uint32_t ttl,
                                   std::vector<std::vector<std::uint8_t>> data) = 0;
    virtual void apply_remove(const dns::Name& name, std::uint16_t type) = 0;
};

class DomainService {
public:
    using EventHandler = std::function<void(const DomainEvent&)>;

    DomainService(Store& store, DomainSink& zone, HlcClock& clock);

    // Adds a domain. Fails with 703 when (name,type) already exists: the user
    // must use set() (SPEC §9.1).
    Result<DomainEntry> add(const AddRequest& request);

    // Replaces the targets of an existing record; created_at is preserved
    // (protocol.md §2.5: only rm + add resets it).
    Result<DomainEntry> set(const AddRequest& request);

    // Removes one (name,type) pair. Returns 704 when it does not exist.
    Result<void> remove(std::string_view name, std::string_view type);

    struct BulkResult {
        std::size_t applied{0};
        std::uint64_t seq{0};
    };

    // Adds/replaces many records atomically: one transaction, one seq bump,
    // and no partial state if any row is invalid (SPEC §9.1 import, AC-3.1).
    // Validation covers every row before the first write.
    Result<BulkResult> add_many(const std::vector<AddRequest>& requests, bool force);

    // Lists this node's records, optionally filtered by type.
    Result<std::vector<DomainEntry>> list(std::optional<std::string> type = std::nullopt) const;

    // Validates a name without touching the store, for live UI feedback.
    Result<Validation> validate_name(std::string_view name) const;

    // Recomputes every `self` record from the current interface addresses and
    // bumps seq once for the whole batch (SPEC §9.2, protocol.md §9.4).
    Result<std::size_t> refresh_self_records(const std::vector<SelfAddressBytes>& addresses);

    void set_event_handler(EventHandler handler) { events_ = std::move(handler); }

    using SyncBroadcaster = std::function<void(const std::vector<net::Op>&)>;
    void set_sync_broadcaster(SyncBroadcaster broadcaster) { sync_broadcaster_ = std::move(broadcaster); }

    // [domains].warn_public_tld (SPEC §11). When false, public TLDs are
    // accepted without confirmation.
    void set_warn_public_tld(bool enabled) { warn_public_tld_ = enabled; }
    bool warn_public_tld() const noexcept { return warn_public_tld_; }

    // Presentation form of one address (dotted quad / RFC 5952).
    static std::string format_address(const SelfAddressBytes& address);

private:
    Result<DomainEntry> upsert(const AddRequest& request, bool must_not_exist);
    Result<RecordRow> prepare_row(const AddRequest& request, HlcTimestamp created,
                                  HlcTimestamp updated) const;

    Store& store_;
    DomainSink& zone_;
    HlcClock& clock_;
    EventHandler events_;
    SyncBroadcaster sync_broadcaster_;
    bool warn_public_tld_{true};
};

// Presentation helpers shared with import/export and tests.
Result<std::uint16_t> user_type_from_name(std::string_view type);
std::string user_type_name(std::uint16_t type);
Result<std::vector<std::vector<std::uint8_t>>> encode_targets(std::uint16_t type,
                                                              const std::vector<std::string>& targets);
std::vector<std::string> decode_targets(std::uint16_t type,
                                        const std::vector<std::vector<std::uint8_t>>& data);

}  // namespace ldns::domain
