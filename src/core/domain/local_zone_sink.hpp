#pragma once

// DomainSink implementation that mirrors the node's zone into a LocalZone.
// The DNS server reads from the same LocalZone instance, so put/remove take
// effect on the next query (SPEC §6.3 step 1).

#include <cstdint>
#include <vector>

#include "core/dns/local_zone.hpp"
#include "core/dns/name.hpp"
#include "core/domain/domain_service.hpp"
#include "core/util/result.hpp"

namespace ldns::domain {

class LocalZoneSink final : public DomainSink {
public:
    explicit LocalZoneSink(dns::LocalZone& zone) : zone_(zone) {}

    Result<void> apply_put(const dns::Name& name, std::uint16_t type, std::uint32_t ttl,
                           std::vector<std::vector<std::uint8_t>> data) override {
        return zone_.upsert(name, type, ttl, std::move(data));
    }

    void apply_remove(const dns::Name& name, std::uint16_t type) override {
        (void)zone_.remove(name, type);
    }

private:
    dns::LocalZone& zone_;
};

}  // namespace ldns::domain
