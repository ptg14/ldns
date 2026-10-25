#include "core/domain/public_tlds.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <string>

namespace ldns::domain {
namespace {

// Common public TLDs and popular second-level suffixes. Kept intentionally
// short and embedded (no network access, no external list file).
constexpr std::array<std::string_view, 71> kPublicTlds = {
    "com", "net", "org", "edu", "gov", "mil", "int",
    "io",  "dev", "app", "ai",  "co",  "me",  "tv",
    "info", "biz", "xyz", "online", "site", "store", "tech",
    "cloud", "blog", "shop", "pro", "name", "mobi", "asia",
    "us", "uk", "de", "fr", "jp", "cn", "ru", "in", "br",
    "au", "ca", "it", "nl", "se", "no", "fi", "dk", "ch",
    "at", "be", "es", "pt", "pl", "cz", "gr", "tr", "kr",
    "tw", "hk", "sg", "my", "th", "id", "ph", "vn", "nz",
    "za", "mx", "ar", "cl", "ir", "il", "ua",
};

std::string lower_last_label(std::string_view name) {
    while (!name.empty() && name.back() == '.') name.remove_suffix(1);
    const std::size_t dot = name.rfind('.');
    std::string_view label = dot == std::string_view::npos ? name : name.substr(dot + 1);
    std::string out(label);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return out;
}

}  // namespace

bool is_public_tld(std::string_view name) {
    const std::string label = lower_last_label(name);
    return std::find(kPublicTlds.begin(), kPublicTlds.end(), label) != kPublicTlds.end();
}

bool is_mdns_local(std::string_view name) {
    while (!name.empty() && name.back() == '.') name.remove_suffix(1);
    constexpr std::string_view suffix = ".local";
    if (name.size() < suffix.size()) return false;
    std::string tail(name.substr(name.size() - suffix.size()));
    std::transform(tail.begin(), tail.end(), tail.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return tail == suffix;
}

std::string_view suggested_tlds() { return ".lan, .home.arpa or .test"; }

}  // namespace ldns::domain
