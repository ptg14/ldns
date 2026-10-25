#include "core/core_api.hpp"

#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <utility>

#include "core/domain/import_export.hpp"
#include "core/util/logger.hpp"

namespace ldns {
namespace {

// Loads every persisted record into the in-memory zone. A row that fails
// validation is skipped with a warning: the DB is our own data, but a
// hand-edited or corrupted DB must not stop the server from starting.
Result<void> load_zone(Store& store, dns::LocalZone& zone) {
    auto rows = store.records();
    if (!rows) return Error{730, rows.error().message};

    zone.clear();
    for (const auto& row : rows.value()) {
        auto name = dns::Name::from_presentation(row.name);
        if (!name) {
            logger()->warn("skipping stored record with invalid name: {}", row.name);
            continue;
        }
        auto status = zone.upsert(name.value(), row.type, row.ttl, row.data);
        if (!status) {
            logger()->warn("skipping stored record {} {}: {}", row.name,
                           dns::rr_type_name(row.type), status.error().message);
        }
    }
    logger()->info("loaded {} record(s) into the local zone", zone.record_count());
    return {};
}

domain::SelfAddressOptions self_options(const config::Config& config) {
    domain::SelfAddressOptions options;
    options.pinned_ip = config.network.self_ip;
    options.interface_name = config.network.self_interface;
    options.interface_filter = config.network.interfaces;
    return options;
}

}  // namespace

CoreApi::CoreApi(Options options, Store store)
    : options_(std::move(options)),
      store_(std::move(store)),
      sink_(std::make_unique<domain::LocalZoneSink>(zone_)),
      service_(store_, *sink_, clock_) {
    const std::string& key_file = options_.config.network.key_file;
    psk_configured_ = !key_file.empty();
    if (psk_configured_) {
        std::error_code ec;
        psk_key_file_found_ = std::filesystem::is_regular_file(key_file, ec);
        if (!psk_key_file_found_) {
            logger()->warn("network.key_file is set but {} is not a readable file; "
                           "the network is currently unprotected",
                           key_file);
        }
    }
    service_.set_warn_public_tld(options_.config.domains.warn_public_tld);
}

CoreApi::~CoreApi() = default;

Result<std::unique_ptr<CoreApi>> CoreApi::create(Options options) {
    if (auto status = options.config.validate(); !status) return status.error();
    if (options.paths.data_file.empty()) {
        // SQLite treats "" as a temporary database that disappears on close;
        // that would silently discard every record.
        return Error{730, "data_file is empty; set LDNS_DATA or pass a path"};
    }

    StoreOptions store_options;
    store_options.path = options.paths.data_file;
    auto store = Store::open(store_options);
    if (!store) return store.error();

    auto identity = store.value().identity();
    if (!identity) return identity.error();

    auto api = std::unique_ptr<CoreApi>(new CoreApi(std::move(options), std::move(store.value())));

    if (auto status = load_zone(api->store_, api->zone_); !status) return status.error();

    // Cache the current self addresses for status/add_self. Failure is not
    // fatal: a machine with no LAN interface can still serve static records.
    auto addresses = api->self_addresses();
    if (addresses) {
        for (const auto& address : addresses.value()) {
            const std::string text = domain::DomainService::format_address(address);
            if (!text.empty()) api->cached_self_addresses_.push_back(text);
        }
    } else {
        logger()->warn("{}", addresses.error().message);
    }

    return api;
}

Result<domain::DomainEntry> CoreApi::add_domain(domain::AddRequest request) {
    apply_defaults(request);
    return service_.add(request);
}

Result<domain::DomainEntry> CoreApi::set_domain(domain::AddRequest request) {
    apply_defaults(request);
    return service_.set(request);
}

void CoreApi::apply_defaults(domain::AddRequest& request) const {
    if (request.ttl == 0) request.ttl = options_.config.domains.default_ttl;
    request.force = request.force || options_.force_public_tld;
}

Result<void> CoreApi::remove_domain(std::string_view name, std::string_view type) {
    return service_.remove(name, type);
}

Result<std::vector<domain::DomainEntry>> CoreApi::list_domains(std::optional<std::string> type) {
    return service_.list(std::move(type));
}

Result<domain::Validation> CoreApi::validate_name(std::string_view name) const {
    return service_.validate_name(name);
}

Result<std::vector<domain::SelfAddressBytes>> CoreApi::self_addresses() const {
    return domain::SelfAddress::enumerate(self_options(options_.config));
}

Result<domain::DomainEntry> CoreApi::add_self_domain(std::string_view name, std::string_view type,
                                                     std::uint32_t ttl) {
    auto addresses = self_addresses();
    if (!addresses) return addresses.error();

    domain::AddRequest request;
    request.name = std::string(name);
    request.type = std::string(type);
    request.ttl = ttl;
    request.kind = domain::TargetKind::Self;
    request.force = options_.force_public_tld;

    // Publish only addresses of the requested family: an IPv4 address in an
    // AAAA RRset would be rejected downstream with a confusing 702 (SPEC §5.2).
    std::string normalized_type = request.type;
    for (char& c : normalized_type) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    const bool wants_v4 = normalized_type != "AAAA";
    const bool wants_v6 = normalized_type == "AAAA";

    for (const auto& address : addresses.value()) {
        if (wants_v4 && address.size() != 4) continue;
        if (wants_v6 && address.size() != 16) continue;
        const std::string text = domain::DomainService::format_address(address);
        if (!text.empty()) request.targets.push_back(text);
    }
    if (request.targets.empty()) return Error{708, "no address matches this record type"};

    return service_.add(request);
}

Result<std::size_t> CoreApi::refresh_self_addresses() {
    auto addresses = self_addresses();
    if (!addresses) return addresses.error();

    std::vector<std::string> texts;
    for (const auto& address : addresses.value()) {
        const std::string text = domain::DomainService::format_address(address);
        if (!text.empty()) texts.push_back(text);
    }

    {
        std::lock_guard lock(zone_reload_mutex_);
        cached_self_addresses_ = std::move(texts);
    }

    return service_.refresh_self_records(addresses.value());
}

Result<domain::ImportReport> CoreApi::import_file(const std::string& path, bool force) {
    std::error_code size_ec;
    const auto size = std::filesystem::file_size(path, size_ec);
    if (size_ec) return Error{731, "cannot open import file: " + path};
    if (size > domain::kMaxImportBytes) return Error{731, "import file exceeds 64 MiB: " + path};

    std::ifstream file(path, std::ios::binary);
    if (!file) return Error{731, "cannot open import file: " + path};
    std::ostringstream contents;
    contents << file.rdbuf();

    const bool toml_format = path.ends_with(".toml");
    return import_text(contents.str(), toml_format, force);
}

Result<domain::ImportReport> CoreApi::import_text(std::string_view text, bool toml_format,
                                                  bool force) {
    return domain::import_domains(service_, text, toml_format, force);
}

Result<std::string> CoreApi::export_zone(bool toml_format) {
    auto entries = service_.list();
    if (!entries) return entries.error();
    return domain::export_domains(entries.value(), toml_format);
}

NodeStatus CoreApi::status() {
    NodeStatus out;
    auto identity = store_.identity();
    if (!identity) return out;

    out.node_id = identity.value().node_id;
    out.short_id = short_id(out.node_id);
    out.epoch = identity.value().epoch;
    out.seq = identity.value().seq;
    out.network_name = options_.config.network.name;
    out.data_file = options_.paths.data_file;
    out.config_file = options_.paths.config_file;
    out.domain_count = zone_.record_count();
    out.psk_configured = psk_configured_;
    out.psk_key_file_found = psk_key_file_found_;

    {
        std::lock_guard lock(zone_reload_mutex_);
        out.self_addresses = cached_self_addresses_;
    }
    return out;
}

}  // namespace ldns
