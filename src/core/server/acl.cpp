#include "core/server/acl.hpp"

#include <algorithm>
#include <array>
#include <cstring>

#include "core/dns/message.hpp"

namespace ldns {
namespace {

// Built-in private ranges, exactly the SPEC §8 default: RFC1918, loopback,
// link-local (v4) and fc00::/7 (ULA) + fe80::/10 (link-local) + ::1 (v6).
constexpr std::array<const char*, 5> kPrivateV4 = {
    "10.0.0.0/8",
    "172.16.0.0/12",
    "192.168.0.0/16",
    "127.0.0.0/8",
    "169.254.0.0/16",
};
constexpr std::array<const char*, 3> kPrivateV6 = {
    "fc00::/7",
    "fe80::/10",
    "::1/128",
};

std::string key_of(const IpAddress& address) {
    return std::string(reinterpret_cast<const char*>(address.bytes), address.v6 ? 16 : 4);
}

}  // namespace

bool operator==(const IpAddress& a, const IpAddress& b) noexcept {
    return a.v6 == b.v6 && std::memcmp(a.bytes, b.bytes, a.v6 ? 16 : 4) == 0;
}

Result<IpAddress> IpAddress::parse(std::string_view text) {
    IpAddress out;
    if (text.find(':') != std::string_view::npos) {
        auto v6 = dns::parse_ipv6(text);
        if (!v6) return Error{300, "invalid IPv6 address"};
        std::copy(v6.value().begin(), v6.value().end(), out.bytes);
        out.v6 = true;
        return out;
    }
    auto v4 = dns::parse_ipv4(text);
    if (!v4) return Error{301, "invalid IPv4 address"};
    std::copy(v4.value().begin(), v4.value().end(), out.bytes);
    return out;
}

std::string IpAddress::to_string() const {
    if (v6) return dns::format_ipv6(std::span<const std::uint8_t>(bytes, 16));
    return dns::format_ipv4(std::span<const std::uint8_t>(bytes, 4));
}

Result<Cidr> Cidr::parse(std::string_view text) {
    const std::size_t slash = text.rfind('/');
    const std::string_view address_part = slash == std::string_view::npos ? text : text.substr(0, slash);

    auto address = IpAddress::parse(address_part);
    if (!address) return address.error();

    Cidr out;
    out.network = address.value();
    const int max_prefix = out.network.v6 ? 128 : 32;

    if (slash == std::string_view::npos) {
        out.prefix = static_cast<std::uint8_t>(max_prefix);
    } else {
        const std::string_view prefix_part = text.substr(slash + 1);
        if (prefix_part.empty() || prefix_part.size() > 3) return Error{302, "invalid CIDR prefix"};
        int value = 0;
        for (const char c : prefix_part) {
            if (c < '0' || c > '9') return Error{302, "invalid CIDR prefix"};
            value = value * 10 + (c - '0');
        }
        if (value < 0 || value > max_prefix) return Error{302, "CIDR prefix out of range"};
        out.prefix = static_cast<std::uint8_t>(value);
    }

    // Zero the host bits so `contains` can compare by prefix length only.
    const std::size_t length = out.network.v6 ? 16 : 4;
    for (std::size_t i = 0; i < length; ++i) {
        const int bits_left = static_cast<int>(out.prefix) - static_cast<int>(i * 8);
        if (bits_left >= 8) continue;
        if (bits_left <= 0) {
            out.network.bytes[i] = 0;
        } else {
            const auto mask = static_cast<std::uint8_t>(0xFF << (8 - bits_left));
            out.network.bytes[i] = static_cast<std::uint8_t>(out.network.bytes[i] & mask);
        }
    }
    return out;
}

bool Cidr::contains(const IpAddress& address) const noexcept {
    if (address.v6 != network.v6) return false;
    const std::size_t length = address.v6 ? 16 : 4;
    std::size_t remaining = prefix;
    for (std::size_t i = 0; i < length && remaining > 0; ++i) {
        if (remaining >= 8) {
            if (address.bytes[i] != network.bytes[i]) return false;
            remaining -= 8;
            continue;
        }
        const auto mask = static_cast<std::uint8_t>(0xFF << (8 - remaining));
        if ((address.bytes[i] & mask) != (network.bytes[i] & mask)) return false;
        remaining = 0;
    }
    return true;
}

Acl::Acl(bool allow_private) {
    if (!allow_private) return;
    for (const char* range : kPrivateV4) {
        auto parsed = Cidr::parse(range);
        if (parsed) rules_.push_back(Rule{parsed.value(), true});
    }
    for (const char* range : kPrivateV6) {
        auto parsed = Cidr::parse(range);
        if (parsed) rules_.push_back(Rule{parsed.value(), true});
    }
}

Result<void> Acl::allow(std::string_view cidr) {
    auto parsed = Cidr::parse(cidr);
    if (!parsed) return parsed.error();
    std::lock_guard lock(mutex_);
    rules_.push_back(Rule{parsed.value(), true});
    return {};
}

Result<void> Acl::deny(std::string_view cidr) {
    auto parsed = Cidr::parse(cidr);
    if (!parsed) return parsed.error();
    std::lock_guard lock(mutex_);
    rules_.push_back(Rule{parsed.value(), false});
    return {};
}

bool Acl::permits(const IpAddress& source) const {
    std::lock_guard lock(mutex_);
    bool allowed = false;
    for (const auto& rule : rules_) {
        if (!rule.range.contains(source)) continue;
        if (!rule.allow) return false;  // deny wins
        allowed = true;
    }
    return allowed;
}

std::size_t Acl::rule_count() const {
    std::lock_guard lock(mutex_);
    return rules_.size();
}

RateLimiter::RateLimiter(std::uint32_t qps, std::uint32_t burst)
    : qps_(qps == 0 ? 1 : qps), burst_(burst == 0 ? (qps == 0 ? 1 : qps) : burst) {}

void RateLimiter::set_limits(std::uint32_t qps, std::uint32_t burst) {
    std::lock_guard lock(mutex_);
    qps_ = qps == 0 ? 1 : qps;
    burst_ = burst == 0 ? qps_ : burst;
}

void RateLimiter::prune_locked(std::int64_t now_ns) {
    for (auto it = buckets_.begin(); it != buckets_.end();) {
        const double elapsed = static_cast<double>(now_ns - it->second.last_ns) / 1e9;
        if (elapsed < 0 || it->second.tokens + elapsed * qps_ >= burst_) {
            it = buckets_.erase(it);  // full again: no state worth keeping
        } else {
            ++it;
        }
    }
}

bool RateLimiter::allow(const IpAddress& source, std::int64_t now_ns) {
    const std::string key = key_of(source);
    std::lock_guard lock(mutex_);

    auto it = buckets_.find(key);
    if (it == buckets_.end()) {
        // Pruning is O(n), so it runs at most once per 1024 new sources. At
        // the hard cap, new sources are refused until a prune frees space.
        if (++inserts_since_prune_ >= 1024) {
            prune_locked(now_ns);
            inserts_since_prune_ = 0;
        }
        if (buckets_.size() >= kMaxTrackedSources) return false;
        it = buckets_.emplace(key, Bucket{}).first;
    }
    auto& bucket = it->second;
    if (!bucket.initialized) {
        bucket.tokens = burst_;
        bucket.last_ns = now_ns;
        bucket.initialized = true;
    }
    if (now_ns < bucket.last_ns) bucket.last_ns = now_ns;  // clock went backwards

    if (now_ns > bucket.last_ns) {
        const double elapsed_seconds = static_cast<double>(now_ns - bucket.last_ns) / 1e9;
        bucket.tokens = std::min<double>(burst_, bucket.tokens + elapsed_seconds * qps_);
        bucket.last_ns = now_ns;
    }

    if (bucket.tokens < 1.0) return false;
    bucket.tokens -= 1.0;
    return true;
}

void RateLimiter::clear() {
    std::lock_guard lock(mutex_);
    buckets_.clear();
    inserts_since_prune_ = 0;
}

std::size_t RateLimiter::tracked_sources() const {
    std::lock_guard lock(mutex_);
    return buckets_.size();
}

}  // namespace ldns
