#pragma once

// Client ACL and per-source rate limiting for the DNS server (SPEC §8, §14).
//
// Default policy: only private, loopback and link-local sources are served.
// Public sources are refused, which also prevents open-resolver amplification.

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "core/util/result.hpp"

namespace ldns {

// Source address, either v4 (bytes used = 4) or v6 (16).
struct IpAddress {
    std::uint8_t bytes[16]{};
    bool v6{false};

    static Result<IpAddress> parse(std::string_view text);
    std::string to_string() const;

    friend bool operator==(const IpAddress& a, const IpAddress& b) noexcept;
};

// CIDR block such as "10.0.0.0/8" or "2001:db8::/32".
struct Cidr {
    IpAddress network;
    std::uint8_t prefix{};

    static Result<Cidr> parse(std::string_view text);
    bool contains(const IpAddress& address) const noexcept;
};

class Acl {
public:
    // Starts with the built-in private ranges (RFC1918, 127.0.0.0/8,
    // 169.254.0.0/16, fc00::/7, fe80::/10, ::1/128) when `allow_private` is
    // true. SPEC §8.
    explicit Acl(bool allow_private = true);

    // Adds an explicit allow rule; sources matching any rule are served even
    // if they are public.
    Result<void> allow(std::string_view cidr);

    // Adds an explicit deny rule; deny wins over allow.
    Result<void> deny(std::string_view cidr);

    bool permits(const IpAddress& source) const;

    std::size_t rule_count() const;

private:
    struct Rule {
        Cidr range;
        bool allow{};
    };
    mutable std::mutex mutex_;
    std::vector<Rule> rules_;
};

// Token bucket per source (SPEC §8: default 200 qps).
class RateLimiter {
public:
    explicit RateLimiter(std::uint32_t qps = 200, std::uint32_t burst = 0);

    // Returns true if the query may be served. Refills lazily based on passed
    // nanoseconds, which keeps the class testable and clock-agnostic.
    bool allow(const IpAddress& source, std::int64_t now_ns);

    void set_limits(std::uint32_t qps, std::uint32_t burst);
    void clear();

    std::size_t tracked_sources() const;

private:
    struct Bucket {
        double tokens{};
        std::int64_t last_ns{};
        bool initialized{false};
    };

    std::uint32_t qps_;
    std::uint32_t burst_;
    mutable std::mutex mutex_;
    std::unordered_map<std::string, Bucket> buckets_;
};

}  // namespace ldns
