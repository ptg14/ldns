#include "core/dns/local_zone.hpp"

#include <algorithm>

namespace ldns::dns {
namespace {

// Types a zone may hold. Users can register A/AAAA/CNAME/TXT (SPEC §5.2);
// SOA/NS exist for the zone apex and negative answers (SPEC §8).
bool is_zone_type(std::uint16_t type) {
    switch (static_cast<RrType>(type)) {
        case RrType::A:
        case RrType::AAAA:
        case RrType::CNAME:
        case RrType::TXT:
        case RrType::NS:
        case RrType::SOA:
            return true;
        default:
            return false;
    }
}

Result<void> validate_value(std::uint16_t type, const std::vector<std::uint8_t>& value) {
    switch (static_cast<RrType>(type)) {
        case RrType::A:
            if (value.size() != 4) return Error{200, "A record must be 4 bytes"};
            return {};
        case RrType::AAAA:
            if (value.size() != 16) return Error{201, "AAAA record must be 16 bytes"};
            return {};
        case RrType::TXT:
            if (value.size() > 1024) return Error{202, "TXT value exceeds 1024 bytes"};
            return {};
        case RrType::CNAME:
        case RrType::NS: {
            std::size_t offset = 0;
            auto target = Name::from_wire(value, offset);
            if (!target) return Error{203, "target is not a valid name"};
            if (offset != value.size()) return Error{203, "trailing bytes in target name"};
            if (target.value().is_wildcard()) return Error{204, "target cannot be a wildcard"};
            return {};
        }
        case RrType::SOA: {
            std::size_t offset = 0;
            auto primary = Name::from_wire(value, offset);
            if (!primary) return Error{211, "SOA primary name is invalid"};
            auto mail = Name::from_wire(value, offset);
            if (!mail) return Error{211, "SOA mail name is invalid"};
            if (offset + 20 != value.size()) return Error{212, "malformed SOA rdata"};
            return {};
        }
        default:
            return Error{205, "unsupported record type in v1"};
    }
}

}  // namespace

Result<void> LocalZone::validate_rrset(std::uint16_t type, std::uint32_t ttl,
                                       const std::vector<std::vector<std::uint8_t>>& data) {
    if (!is_zone_type(type)) return Error{205, "unsupported record type in v1"};
    if (ttl > kMaxTtl) return Error{206, "TTL exceeds 2^31-1"};
    if (data.empty()) return Error{207, "RRset must contain at least one value"};
    if (data.size() > kMaxRrsetValues) return Error{208, "RRset exceeds 64 values"};
    if (static_cast<RrType>(type) == RrType::CNAME && data.size() != 1) {
        return Error{209, "CNAME RRset must have exactly one value"};
    }
    for (const auto& value : data) {
        if (auto status = validate_value(type, value); !status) return status.error();
    }
    return {};
}

Result<void> LocalZone::upsert(const Name& name, std::uint16_t type, std::uint32_t ttl,
                               std::vector<std::vector<std::uint8_t>> data) {
    if (auto status = validate_rrset(type, ttl, data); !status) return status.error();

    std::unique_lock lock(mutex_);
    auto& types = names_[name];

    // CNAME owns the whole name (RFC 1034 §3.6.2): no other types alongside.
    if (static_cast<RrType>(type) == RrType::CNAME) {
        if (types.size() > 1 || (types.size() == 1 && types.begin()->first != type)) {
            return Error{210, "CNAME cannot coexist with other records at the same name"};
        }
    } else if (types.count(static_cast<std::uint16_t>(RrType::CNAME)) != 0) {
        return Error{210, "CNAME cannot coexist with other records at the same name"};
    }

    types[type] = Rrset{ttl, std::move(data)};
    return {};
}

bool LocalZone::remove(const Name& name, std::uint16_t type) {
    std::unique_lock lock(mutex_);
    const auto name_it = names_.find(name);
    if (name_it == names_.end()) return false;
    if (name_it->second.erase(type) == 0) return false;
    if (name_it->second.empty()) names_.erase(name_it);
    return true;
}

bool LocalZone::contains(const Name& name, std::uint16_t type) const {
    std::shared_lock lock(mutex_);
    const auto name_it = names_.find(name);
    if (name_it == names_.end()) return false;
    return name_it->second.count(type) != 0;
}

std::vector<ResourceRecord> LocalZone::make_answers(const Name& owner, const TypeMap& types,
                                                    std::uint16_t type) {
    std::vector<ResourceRecord> out;
    if (type == static_cast<std::uint16_t>(RrType::ANY)) {
        for (const auto& [rr_type, rrset] : types) {
            for (const auto& value : rrset.data) {
                out.push_back(ResourceRecord{owner, rr_type, kClassIn, rrset.ttl, value});
            }
        }
        return out;
    }
    const auto it = types.find(type);
    if (it == types.end()) return out;
    out.reserve(it->second.data.size());
    for (const auto& value : it->second.data) {
        out.push_back(ResourceRecord{owner, type, kClassIn, it->second.ttl, value});
    }
    return out;
}

std::optional<Name> LocalZone::cname_of(const TypeMap& types) {
    const auto it = types.find(static_cast<std::uint16_t>(RrType::CNAME));
    if (it == types.end() || it->second.data.empty()) return std::nullopt;
    std::size_t offset = 0;
    auto target = Name::from_wire(it->second.data.front(), offset);
    if (!target) return std::nullopt;
    return target.value();
}

LookupOutcome LocalZone::lookup(const Name& name, std::uint16_t qtype) const {
    std::shared_lock lock(mutex_);

    // 1. Exact match.
    const auto exact = names_.find(name);
    if (exact != names_.end()) {
        auto answers = make_answers(name, exact->second, qtype);
        if (!answers.empty()) return LookupOutcome{LookupKind::Answer, std::move(answers), {}};
        if (const auto cname = cname_of(exact->second)) {
            return LookupOutcome{LookupKind::Cname, {}, cname};
        }
        return LookupOutcome{LookupKind::NoData, {}, {}};
    }

    // 2. Wildcard: climb ancestors, most specific first ("*.a.b" before "*.b").
    for (Name ancestor = name.parent(); !ancestor.is_root(); ancestor = ancestor.parent()) {
        auto wildcard = Name::make_wildcard(ancestor);
        if (!wildcard) break;
        const auto it = names_.find(wildcard.value());
        if (it == names_.end()) continue;

        auto answers = make_answers(name, it->second, qtype);
        if (!answers.empty()) return LookupOutcome{LookupKind::Answer, std::move(answers), {}};
        if (const auto cname = cname_of(it->second)) {
            return LookupOutcome{LookupKind::Cname, {}, cname};
        }
        return LookupOutcome{LookupKind::NoData, {}, {}};
    }

    return LookupOutcome{LookupKind::NxDomain, {}, {}};
}

std::optional<ResourceRecord> LocalZone::negative_soa(const Name& name) const {
    std::shared_lock lock(mutex_);
    for (Name current = name;; current = current.parent()) {
        const auto it = names_.find(current);
        if (it != names_.end()) {
            const auto soa = it->second.find(static_cast<std::uint16_t>(RrType::SOA));
            if (soa != it->second.end() && !soa->second.data.empty()) {
                return ResourceRecord{current, static_cast<std::uint16_t>(RrType::SOA),
                                      kClassIn, soa->second.ttl, soa->second.data.front()};
            }
        }
        if (current.is_root()) break;
    }
    return std::nullopt;
}

std::vector<LocalZone::Entry> LocalZone::entries() const {
    std::shared_lock lock(mutex_);
    std::vector<Entry> out;
    out.reserve(record_count_unlocked());
    for (const auto& [name, types] : names_) {
        for (const auto& [type, rrset] : types) {
            out.push_back(Entry{name, type, rrset.ttl, rrset.data});
        }
    }
    std::sort(out.begin(), out.end(), [](const Entry& a, const Entry& b) {
        if (a.name != b.name) return a.name < b.name;
        return a.type < b.type;
    });
    return out;
}

std::size_t LocalZone::name_count() const {
    std::shared_lock lock(mutex_);
    return names_.size();
}

std::size_t LocalZone::record_count() const {
    std::shared_lock lock(mutex_);
    return record_count_unlocked();
}

std::size_t LocalZone::record_count_unlocked() const {
    std::size_t count = 0;
    for (const auto& [name, types] : names_) {
        (void)name;
        count += types.size();
    }
    return count;
}

bool LocalZone::empty() const {
    std::shared_lock lock(mutex_);
    return names_.empty();
}

void LocalZone::clear() {
    std::unique_lock lock(mutex_);
    names_.clear();
}

}  // namespace ldns::dns
