#include "core/domain/domain_service.hpp"

#include <algorithm>
#include <cctype>
#include <map>
#include <utility>

#include "core/dns/message.hpp"
#include "core/domain/public_tlds.hpp"
#include "core/util/logger.hpp"

namespace ldns::domain {
namespace {

using dns::Name;
using dns::RrType;
using dns::rr_type_from_name;
using dns::rr_type_name;

constexpr std::size_t kMaxTargets = dns::kMaxRrsetValues;

bool is_user_type(std::uint16_t type) {
    switch (static_cast<RrType>(type)) {
        case RrType::A:
        case RrType::AAAA:
        case RrType::CNAME:
        case RrType::TXT:
            return true;
        default:
            return false;
    }
}

bool is_address_type(std::uint16_t type) {
    return type == static_cast<std::uint16_t>(RrType::A) ||
           type == static_cast<std::uint16_t>(RrType::AAAA);
}

std::string upper_ascii(std::string_view text) {
    std::string out(text);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
        return static_cast<char>(std::toupper(c));
    });
    return out;
}

}  // namespace

Result<std::uint16_t> user_type_from_name(std::string_view type) {
    auto parsed = rr_type_from_name(upper_ascii(type));
    if (!parsed) return Error{701, "invalid record type: " + std::string(type)};
    if (!is_user_type(parsed.value())) {
        return Error{701, "record type not allowed in v1: " + std::string(type)};
    }
    return parsed.value();
}

std::string user_type_name(std::uint16_t type) { return std::string(rr_type_name(type)); }

Result<std::vector<std::vector<std::uint8_t>>> encode_targets(
    std::uint16_t type, const std::vector<std::string>& targets) {
    if (targets.empty()) return Error{702, "at least one target is required"};
    if (targets.size() > kMaxTargets) return Error{702, "too many targets (max 64)"};

    std::vector<std::vector<std::uint8_t>> out;
    out.reserve(targets.size());
    for (const auto& target : targets) {
        switch (static_cast<RrType>(type)) {
            case RrType::A: {
                auto bytes = dns::parse_ipv4(target);
                if (!bytes) return Error{702, "invalid IPv4 target: " + target};
                out.push_back(std::move(bytes.value()));
                break;
            }
            case RrType::AAAA: {
                auto bytes = dns::parse_ipv6(target);
                if (!bytes) return Error{702, "invalid IPv6 target: " + target};
                out.push_back(std::move(bytes.value()));
                break;
            }
            case RrType::CNAME: {
                if (targets.size() != 1) return Error{702, "CNAME takes exactly one target"};
                auto name = Name::from_presentation(target);
                if (!name) return Error{702, "invalid CNAME target: " + target};
                if (name.value().is_wildcard()) {
                    return Error{702, "CNAME target cannot be a wildcard"};
                }
                auto rdata = dns::encode_rdata_name(name.value());
                if (!rdata) return Error{702, rdata.error().message};
                out.push_back(std::move(rdata.value()));
                break;
            }
            case RrType::TXT: {
                if (target.size() > 1024) return Error{702, "TXT value exceeds 1024 bytes"};
                auto rdata = dns::encode_rdata_txt(target);
                if (!rdata) return Error{702, rdata.error().message};
                out.push_back(std::move(rdata.value()));
                break;
            }
            default:
                return Error{701, "record type not allowed in v1"};
        }
    }

    // Canonical form for voting (protocol.md §2.4): sorted, de-duplicated.
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

std::vector<std::string> decode_targets(std::uint16_t type,
                                        const std::vector<std::vector<std::uint8_t>>& data) {
    std::vector<std::string> out;
    out.reserve(data.size());
    for (const auto& value : data) {
        switch (static_cast<RrType>(type)) {
            case RrType::A:
                out.push_back(dns::format_ipv4(value));
                break;
            case RrType::AAAA:
                out.push_back(dns::format_ipv6(value));
                break;
            case RrType::CNAME: {
                auto name = dns::decode_rdata_name(value);
                out.push_back(name ? name.value().to_string() : "?");
                break;
            }
            case RrType::TXT: {
                auto text = dns::decode_rdata_txt(value);
                out.push_back(text ? text.value() : "?");
                break;
            }
            default:
                out.push_back("?");
                break;
        }
    }
    return out;
}

std::string DomainService::format_address(const SelfAddressBytes& address) {
    if (address.size() == 4) return dns::format_ipv4(address);
    if (address.size() == 16) return dns::format_ipv6(address);
    return {};
}

DomainService::DomainService(Store& store, DomainSink& zone, HlcClock& clock)
    : store_(store), zone_(zone), clock_(clock) {}

Result<Validation> DomainService::validate_name(std::string_view name) const {
    auto parsed = Name::from_presentation(name);
    if (!parsed) {
        return Error{700, "invalid domain name: " + std::string(name) + " (" +
                              parsed.error().message + ")"};
    }
    Validation out;
    const std::string normalized = parsed.value().to_string();
    out.needs_tld_confirmation = is_public_tld(normalized);
    out.mdns_conflict = is_mdns_local(normalized);
    if (out.mdns_conflict) {
        out.warning = "names ending in .local conflict with mDNS";
    } else if (out.needs_tld_confirmation) {
        out.warning = "this name ends in a public TLD; consider " + std::string(suggested_tlds());
    }
    return out;
}

Result<RecordRow> DomainService::prepare_row(const AddRequest& request, HlcTimestamp created,
                                             HlcTimestamp updated) const {
    auto type = user_type_from_name(request.type);
    if (!type) return type.error();

    auto parsed_name = Name::from_presentation(request.name);
    if (!parsed_name) {
        return Error{700, "invalid domain name: " + request.name + " (" +
                              parsed_name.error().message + ")"};
    }
    if (parsed_name.value().is_root()) return Error{700, "the root name cannot be registered"};

    if (request.targets.empty()) {
        return Error{708, "no target address: cannot expand self for this record"};
    }
    if (request.kind == TargetKind::Self && !is_address_type(type.value())) {
        return Error{702, "only A and AAAA records can target self"};
    }

    auto encoded = encode_targets(type.value(), request.targets);
    if (!encoded) return encoded.error();
    if (auto status = dns::LocalZone::validate_rrset(type.value(), request.ttl, encoded.value());
        !status) {
        return Error{702, status.error().message};
    }

    RecordRow row;
    row.name = parsed_name.value().to_string();
    row.type = type.value();
    row.ttl = request.ttl;
    row.target_kind = request.kind == TargetKind::Self ? "self" : "static";
    row.data = std::move(encoded.value());
    row.created = created;
    row.updated = updated;
    return row;
}

Result<DomainEntry> DomainService::upsert(const AddRequest& request, bool must_not_exist) {
    const auto validation = validate_name(request.name);
    if (!validation) return validation.error();
    if (warn_public_tld_ && validation.value().needs_tld_confirmation && !request.force) {
        return Error{705, request.name + " ends in a public TLD (" + validation.value().warning +
                              "); pass --force to register it anyway"};
    }
    if (validation.value().mdns_conflict) {
        logger()->warn("{}", validation.value().warning);
    }

    auto type = user_type_from_name(request.type);
    if (!type) return type.error();
    auto name = Name::from_presentation(request.name);
    if (!name) return Error{700, "invalid domain name: " + request.name};

    // Everything below runs under the store lock held by the transaction, so
    // the existence check cannot race another writer.
    auto tx = store_.begin();
    if (!tx) return Error{707, tx.error().message};
    auto& transaction = tx.value();

    const auto existing = store_.records_for_name(name.value().to_string());
    if (!existing) return Error{707, existing.error().message};

    std::optional<RecordRow> same_type;
    for (const auto& row : existing.value()) {
        if (row.type == type.value()) same_type = row;
    }

    if (must_not_exist && same_type) {
        return Error{703, "domain already exists: " + name.value().to_string() + " " +
                              user_type_name(type.value()) + " (use /set)"};
    }

    // CNAME owns the whole name (RFC 1034 §3.6.2, mirrored by LocalZone).
    if (static_cast<RrType>(type.value()) == RrType::CNAME) {
        const bool only_this =
            existing.value().empty() ||
            (existing.value().size() == 1 && existing.value().front().type == type.value());
        if (!only_this) {
            return Error{706, "CNAME cannot coexist with other records at the same name"};
        }
    } else {
        for (const auto& row : existing.value()) {
            if (row.type == static_cast<std::uint16_t>(RrType::CNAME)) {
                return Error{706, "CNAME cannot coexist with other records at the same name"};
            }
        }
    }

    const HlcTimestamp now = clock_.now();
    const HlcTimestamp created = same_type ? same_type->created : now;

    auto prepared = prepare_row(request, created, now);
    if (!prepared) return prepared.error();
    const RecordRow& row = prepared.value();

    if (auto status = store_.put_record(row); !status) return Error{707, status.error().message};

    const auto seq = store_.bump_seq();
    if (!seq) return Error{707, seq.error().message};

    if (auto status = transaction.commit(); !status) return Error{707, status.error().message};

    // The DB is authoritative at this point; mirror into the live zone. A
    // rejection here means the two models disagree, which must be visible.
    auto applied = zone_.apply_put(name.value(), row.type, row.ttl, row.data);
    if (!applied) {
        logger()->error("zone rejected committed record {} {}: {}", row.name,
                        user_type_name(row.type), applied.error().message);
        return Error{706, applied.error().message};
    }

    DomainEntry entry;
    entry.name = row.name;
    entry.type = user_type_name(row.type);
    entry.ttl = row.ttl;
    entry.target_kind = row.target_kind;
    entry.targets = decode_targets(row.type, row.data);
    entry.created = row.created;
    entry.updated = row.updated;

    if (events_) {
        DomainEvent event;
        event.kind = EventType::RecordPut;
        event.seq = seq.value();
        event.name = entry.name;
        event.type = entry.type;
        events_(event);
    }
    return entry;
}

Result<DomainEntry> DomainService::add(const AddRequest& request) { return upsert(request, true); }

Result<DomainEntry> DomainService::set(const AddRequest& request) { return upsert(request, false); }

Result<void> DomainService::remove(std::string_view name, std::string_view type) {
    const auto parsed_type = user_type_from_name(type);
    if (!parsed_type) return parsed_type.error();
    auto parsed_name = Name::from_presentation(name);
    if (!parsed_name) return Error{700, "invalid domain name: " + std::string(name)};

    const std::string normalized = parsed_name.value().to_string();

    auto tx = store_.begin();
    if (!tx) return Error{707, tx.error().message};
    auto& transaction = tx.value();

    const auto existing = store_.records_for_name(normalized);
    if (!existing) return Error{707, existing.error().message};

    const bool found = std::any_of(existing.value().begin(), existing.value().end(),
                                   [&](const RecordRow& row) {
                                       return row.type == parsed_type.value();
                                   });
    if (!found) {
        return Error{704, "record not found: " + normalized + " " +
                              user_type_name(parsed_type.value())};
    }

    auto deleted = store_.delete_record(normalized, parsed_type.value());
    if (!deleted) return Error{707, deleted.error().message};
    if (!deleted.value()) return Error{704, "record not found"};

    const auto seq = store_.bump_seq();
    if (!seq) return Error{707, seq.error().message};

    if (auto status = transaction.commit(); !status) return Error{707, status.error().message};

    zone_.apply_remove(parsed_name.value(), parsed_type.value());

    if (events_) {
        DomainEvent event;
        event.kind = EventType::RecordDel;
        event.seq = seq.value();
        event.name = normalized;
        event.type = user_type_name(parsed_type.value());
        events_(event);
    }
    return {};
}

Result<DomainService::BulkResult> DomainService::add_many(const std::vector<AddRequest>& requests,
                                                          bool force) {
    // Pass 1: validate everything and group by (name,type); the last row wins.
    // Nothing is written until the whole batch is known to be valid.
    std::map<std::pair<std::string, std::uint16_t>, AddRequest> normalized;
    std::vector<std::pair<std::string, std::uint16_t>> order;

    for (const auto& request : requests) {
        const auto validation = validate_name(request.name);
        if (!validation) return validation.error();
        if (warn_public_tld_ && validation.value().needs_tld_confirmation && !force) {
            return Error{705, request.name + " ends in a public TLD; pass --force to import it"};
        }

        auto type = user_type_from_name(request.type);
        if (!type) return type.error();
        auto name = Name::from_presentation(request.name);
        if (!name) return Error{700, "invalid domain name: " + request.name};
        if (name.value().is_root()) return Error{700, "the root name cannot be registered"};
        if (request.kind == TargetKind::Self && !is_address_type(type.value())) {
            return Error{702, "only A and AAAA records can target self: " + request.name};
        }

        AddRequest item = request;
        item.name = name.value().to_string();

        const auto key = std::make_pair(item.name, type.value());
        if (normalized.find(key) == normalized.end()) order.push_back(key);
        normalized[key] = std::move(item);
    }
    if (order.empty()) return BulkResult{0, 0};

    // Catch CNAME coexistence inside the batch itself: an import may add
    // app.lan A and app.lan CNAME in the same file.
    for (const auto& key : order) {
        if (key.second != static_cast<std::uint16_t>(RrType::CNAME)) continue;
        for (const auto& other : order) {
            if (other.first == key.first && other.second != key.second) {
                return Error{706, "CNAME cannot coexist with other records at the same name: " +
                                      key.first};
            }
        }
    }

    auto tx = store_.begin();
    if (!tx) return Error{707, tx.error().message};
    auto& transaction = tx.value();

    const HlcTimestamp now = clock_.now();

    // Read the existing zone once rather than once per row: a 100 000-row
    // import must not degrade to O(n^2) full-table scans (AC-3.1).
    const auto existing_rows = store_.records();
    if (!existing_rows) return Error{707, existing_rows.error().message};

    std::map<std::pair<std::string, std::uint16_t>, RecordRow> by_key;
    std::map<std::string, std::uint16_t> types_per_name;
    for (const auto& row : existing_rows.value()) {
        by_key.emplace(std::make_pair(row.name, row.type), row);
        ++types_per_name[row.name];
    }

    struct Pending {
        RecordRow row;
    };
    std::vector<Pending> pending;
    pending.reserve(order.size());

    for (const auto& key : order) {
        const AddRequest& request = normalized.at(key);
        const auto prior = by_key.find(key);
        std::optional<RecordRow> same_type;
        if (prior != by_key.end()) same_type = prior->second;

        const bool is_cname = key.second == static_cast<std::uint16_t>(RrType::CNAME);
        const auto name_types = types_per_name.find(key.first);
        const bool name_exists = name_types != types_per_name.end();

        if (is_cname && name_exists) {
            // A CNAME may only own a name that has no other types.
            const bool only_this = name_types->second == 1 && same_type.has_value();
            if (!only_this) {
                return Error{706, "CNAME cannot coexist with other records at the same name: " +
                                      key.first};
            }
        }
        if (!is_cname) {
            const bool has_cname = by_key.count(std::make_pair(
                                       key.first, static_cast<std::uint16_t>(RrType::CNAME))) != 0;
            if (has_cname) {
                return Error{706, "CNAME cannot coexist with other records at the same name: " +
                                      key.first};
            }
        }

        auto prepared = prepare_row(request, same_type ? same_type->created : now, now);
        if (!prepared) return prepared.error();
        pending.push_back(Pending{std::move(prepared.value())});

        // Track the row inside the batch so a later row for the same name
        // sees the CNAME/other-type state of this one.
        by_key[std::make_pair(prepared.value().name, prepared.value().type)] = prepared.value();
        if (!same_type) ++types_per_name[prepared.value().name];
    }

    for (const auto& item : pending) {
        if (auto status = store_.put_record(item.row); !status) {
            return Error{707, status.error().message};
        }
    }

    const auto seq = store_.bump_seq();
    if (!seq) return Error{707, seq.error().message};

    if (auto status = transaction.commit(); !status) return Error{707, status.error().message};

    // Mirror only after the commit: the zone never serves uncommitted rows.
    for (const auto& item : pending) {
        auto name = Name::from_presentation(item.row.name);
        if (!name) return Error{700, "stored name is invalid: " + item.row.name};
        auto status = zone_.apply_put(name.value(), item.row.type, item.row.ttl, item.row.data);
        if (!status) {
            logger()->error("zone rejected committed record {} {}: {}", item.row.name,
                            user_type_name(item.row.type), status.error().message);
            return Error{706, status.error().message};
        }
    }

    if (events_) {
        for (const auto& item : pending) {
            DomainEvent event;
            event.kind = EventType::RecordPut;
            event.seq = seq.value();
            event.name = item.row.name;
            event.type = user_type_name(item.row.type);
            events_(event);
        }
    }
    return BulkResult{pending.size(), seq.value()};
}

Result<std::vector<DomainEntry>> DomainService::list(std::optional<std::string> type) const {
    std::optional<std::uint16_t> wanted;
    if (type) {
        auto parsed = user_type_from_name(*type);
        if (!parsed) return parsed.error();
        wanted = parsed.value();
    }

    auto rows = store_.records();
    if (!rows) return Error{707, rows.error().message};

    std::vector<DomainEntry> out;
    out.reserve(rows.value().size());
    for (const auto& row : rows.value()) {
        if (wanted && row.type != *wanted) continue;
        DomainEntry entry;
        entry.name = row.name;
        entry.type = user_type_name(row.type);
        entry.ttl = row.ttl;
        entry.target_kind = row.target_kind;
        entry.targets = decode_targets(row.type, row.data);
        entry.created = row.created;
        entry.updated = row.updated;
        out.push_back(std::move(entry));
    }
    return out;
}

Result<std::size_t> DomainService::refresh_self_records(
    const std::vector<SelfAddressBytes>& addresses) {
    if (addresses.empty()) return Error{708, "no self address available"};

    auto tx = store_.begin();
    if (!tx) return Error{707, tx.error().message};
    auto& transaction = tx.value();

    auto rows = store_.records();
    if (!rows) return Error{707, rows.error().message};

    std::vector<RecordRow> self_rows;
    for (const auto& row : rows.value()) {
        if (row.target_kind == "self") self_rows.push_back(row);
    }
    if (self_rows.empty()) return std::size_t{0};

    const HlcTimestamp now = clock_.now();
    std::vector<RecordRow> updated_rows;
    for (auto& row : self_rows) {
        std::vector<std::string> targets;
        if (row.type == static_cast<std::uint16_t>(RrType::A)) {
            for (const auto& address : addresses) {
                if (address.size() == 4) targets.push_back(format_address(address));
            }
        } else if (row.type == static_cast<std::uint16_t>(RrType::AAAA)) {
            for (const auto& address : addresses) {
                if (address.size() == 16) targets.push_back(format_address(address));
            }
        } else {
            continue;
        }

        if (targets.empty()) {
            // The address family disappeared (Wi-Fi down, DHCP release). Keep
            // the last value rather than publishing an empty RRset, which
            // would turn the owner's own name into a NODATA answer.
            logger()->warn("no {} address for self record {}; keeping the previous value",
                           user_type_name(row.type), row.name);
            continue;
        }
        auto encoded = encode_targets(row.type, targets);
        if (!encoded) continue;
        if (encoded.value() == row.data) continue;

        row.data = std::move(encoded.value());
        row.updated = now;
        if (auto status = store_.put_record(row); !status) {
            return Error{707, status.error().message};
        }
        updated_rows.push_back(row);
    }

    if (updated_rows.empty()) return std::size_t{0};

    const auto seq = store_.bump_seq();
    if (!seq) return Error{707, seq.error().message};
    if (auto status = transaction.commit(); !status) return Error{707, status.error().message};

    for (const auto& row : updated_rows) {
        auto name = Name::from_presentation(row.name);
        if (!name) {
            logger()->error("stored name is invalid: {}", row.name);
            continue;
        }
        if (auto status = zone_.apply_put(name.value(), row.type, row.ttl, row.data); !status) {
            logger()->error("zone rejected refreshed record {}: {}", row.name,
                            status.error().message);
        }
    }

    if (events_) {
        for (const auto& row : updated_rows) {
            DomainEvent event;
            event.kind = EventType::RecordPut;
            event.seq = seq.value();
            event.name = row.name;
            event.type = user_type_name(row.type);
            events_(event);
        }
    }
    logger()->info("refreshed {} self record(s)", updated_rows.size());
    return updated_rows.size();
}

}  // namespace ldns::domain
