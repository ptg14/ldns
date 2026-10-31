#pragma once

// CoreApi — the single entry point every frontend uses (SPEC §4, PLAN §0.3).
// Embedded mode calls it directly; the daemon exposes the same methods over
// IPC (M8). Keep this header free of transport and UI dependencies.
//
// Ownership: CoreApi owns the Store, LocalZone and DomainService. The DNS
// server takes a reference to the zone and must not outlive the CoreApi.

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "core/config/config.hpp"
#include "core/config/paths.hpp"
#include "core/dns/local_zone.hpp"
#include "core/registry/registry.hpp"
#include "core/domain/domain_service.hpp"
#include "core/domain/import_export.hpp"
#include "core/domain/local_zone_sink.hpp"
#include "core/domain/self_address.hpp"
#include "core/resolver/cache.hpp"
#include "core/resolver/pipeline.hpp"
#include "core/resolver/peer_resolver.hpp"
#include "core/resolver/query_log.hpp"
#include "core/resolver/upstream_client.hpp"
#include "core/store/store.hpp"
#include "core/util/hlc.hpp"
#include "core/util/result.hpp"

namespace ldns {

struct NodeStatus {
    NodeId node_id{};
    std::string short_id;
    std::uint64_t epoch{};
    std::uint64_t seq{};
    std::string network_name;
    std::string data_file;
    std::string config_file;
    std::size_t domain_count{};
    std::vector<std::string> self_addresses;
    // SPEC D16: /status must always warn when the network has no PSK.
    // v1 (M2) only knows whether a key file is configured; M9 loads and
    // validates it, at which point this becomes "protection active".
    bool psk_configured{false};
    bool psk_key_file_found{false};
};

struct DomainInfo {
    dns::Name name;
    std::uint16_t type{1};
    bool has_local{false};
    std::vector<std::string> local_records;
    bool has_conflict{false};
    bool local_divergence{false};
    resolver::RankingResult ranking;
};

class CoreApi {
public:
    struct Options {
        config::Config config;
        config::Paths paths;
        // Skip the public-TLD confirmation (tests, --force, imports).
        bool force_public_tld{false};
    };

    // Opens the store (creating it if needed) and loads the node's zone into
    // memory so the DNS server can start answering immediately (PLAN M2).
    static Result<std::unique_ptr<CoreApi>> create(Options options);

    CoreApi(const CoreApi&) = delete;
    CoreApi& operator=(const CoreApi&) = delete;
    ~CoreApi();

    // --- zone --------------------------------------------------------------
    // ttl == 0 means "use [domains].default_ttl" (SPEC §11).
    Result<domain::DomainEntry> add_domain(domain::AddRequest request);
    Result<domain::DomainEntry> set_domain(domain::AddRequest request);
    Result<void> remove_domain(std::string_view name, std::string_view type);
    Result<std::vector<domain::DomainEntry>> list_domains(
        std::optional<std::string> type = std::nullopt);

    Result<domain::Validation> validate_name(std::string_view name) const;

    // Adds a record targeting this node's current LAN addresses (SPEC §9.2).
    // Fails with 708 when no address is available; the caller should suggest
    // network.self_ip.
    Result<domain::DomainEntry> add_self_domain(std::string_view name, std::string_view type,
                                                std::uint32_t ttl);

    // Re-enumerates LAN addresses and rewrites every `self` record whose
    // value changed (SPEC §9.4, AC-2.5). Returns the number of records
    // updated; also refreshes the cached self_addresses in NodeStatus.
    Result<std::size_t> refresh_self_addresses();

    // --- peers -------------------------------------------------------------
    Result<std::vector<StoredPeer>> peers();
    Result<void> put_peer(const StoredPeer& peer);
    Result<bool> delete_peer(const NodeId& node_id);

    Result<std::vector<std::string>> manual_peers() const;
    Result<void> add_manual_peer(std::string_view endpoint);
    Result<void> remove_manual_peer(std::string_view endpoint);

    // --- import/export -----------------------------------------------------
    Result<domain::ImportReport> import_file(const std::string& path, bool force);
    Result<domain::ImportReport> import_text(std::string_view text, bool toml_format, bool force);
    Result<std::string> export_zone(bool toml_format);

    // --- status ------------------------------------------------------------
    NodeStatus status();
    const config::Config& config() const noexcept { return options_.config; }
    const config::Paths& paths() const noexcept { return options_.paths; }

    // The zone the DNS server reads from. Valid for the lifetime of CoreApi.
    dns::LocalZone& zone() noexcept { return zone_; }
    const dns::LocalZone& zone() const noexcept { return zone_; }

    Store& store() noexcept { return store_; }
    Registry& registry() noexcept { return registry_; }
    const Registry& registry() const noexcept { return registry_; }
    domain::DomainService& domains() noexcept { return service_; }
    HlcClock& clock() noexcept { return clock_; }

    // Resolves the self addresses without side effects (used by add_self).
    Result<std::vector<domain::SelfAddressBytes>> self_addresses() const;

    void set_event_handler(domain::DomainService::EventHandler handler) {
        service_.set_event_handler(std::move(handler));
    }

    resolver::ResolverPipeline& pipeline() noexcept { return *pipeline_; }
    std::shared_ptr<resolver::ResolverPipeline> pipeline_shared() const noexcept { return pipeline_; }
    resolver::PeerResolver& peer_resolver() noexcept { return *peer_resolver_; }
    const resolver::PeerResolver& peer_resolver() const noexcept { return *peer_resolver_; }
    std::shared_ptr<resolver::PeerResolver> peer_resolver_shared() const noexcept { return peer_resolver_; }

    void set_peer_manager(std::shared_ptr<net::PeerManager> pm);
    std::vector<resolver::ConflictRecord> conflicts() const;
    std::optional<resolver::ConflictRecord> get_conflict(std::string_view name,
                                                         std::string_view type = "A") const;
    Result<DomainInfo> domain_info(std::string_view name, std::string_view type = "A") const;
    resolver::DnsCache& cache() noexcept { return *cache_; }
    resolver::UpstreamClient& upstream_client() noexcept { return *upstream_; }
    resolver::QueryLog& query_log() noexcept { return *query_log_; }

    Result<resolver::PipelineResult> resolve(std::string_view name,
                                             std::string_view type = "A",
                                             bool trace = false);

    const std::optional<std::array<std::uint8_t, 32>>& k_root() const noexcept { return k_root_; }
    const std::optional<std::array<std::uint8_t, 32>>& k_beacon() const noexcept { return k_beacon_; }
    const std::optional<std::array<std::uint8_t, 32>>& k_auth() const noexcept { return k_auth_; }

private:
    CoreApi(Options options, Store store);
    void apply_defaults(domain::AddRequest& request) const;

    Options options_;
    Store store_;
    dns::LocalZone zone_;
    Registry registry_;
    // Declared after zone_: the sink holds a reference to it.
    std::unique_ptr<domain::LocalZoneSink> sink_;
    // Declared before service_: DomainService stores a reference to the clock,
    // so the clock must outlive (and be constructed before) the service.
    HlcClock clock_;
    domain::DomainService service_;
    mutable std::vector<std::string> cached_self_addresses_;
    bool psk_configured_{false};
    bool psk_key_file_found_{false};
    std::optional<std::array<std::uint8_t, 32>> k_root_;
    std::optional<std::array<std::uint8_t, 32>> k_beacon_;
    std::optional<std::array<std::uint8_t, 32>> k_auth_;

    // Serializes reloads of the in-memory zone against concurrent writers.
    std::mutex zone_reload_mutex_;

    std::shared_ptr<resolver::DnsCache> cache_;
    std::shared_ptr<resolver::UpstreamClient> upstream_;
    std::shared_ptr<resolver::QueryLog> query_log_;
    std::shared_ptr<resolver::ResolverPipeline> pipeline_;
    std::shared_ptr<resolver::PeerResolver> peer_resolver_;
};

}  // namespace ldns
