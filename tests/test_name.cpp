#include "doctest.hpp"

#include "core/dns/name.hpp"

#include <string>
#include <vector>

using ldns::dns::Name;
using ldns::dns::punycode_encode;

namespace {

Name must_make(const std::string& text, bool wildcard = true) {
    auto result = Name::from_presentation(text, wildcard);
    REQUIRE(result.has_value());
    return result.value();
}

std::vector<std::uint8_t> wire_of(const std::string& dotted) {
    auto result = Name::from_presentation(dotted);
    REQUIRE(result.has_value());
    const auto span = result.value().wire();
    return {span.begin(), span.end()};
}

}  // namespace

TEST_CASE("presentation form normalizes case and trailing dot") {
    CHECK(must_make("App.LAN").to_string() == "app.lan");
    CHECK(must_make("app.lan.").to_string() == "app.lan");
    CHECK(must_make("app.lan").to_string() == "app.lan");
    CHECK(must_make(".").is_root());
    CHECK(must_make(".").to_string() == ".");
}

TEST_CASE("presentation form rejects malformed names") {
    CHECK_FALSE(Name::from_presentation("").has_value());
    CHECK_FALSE(Name::from_presentation(".app.lan").has_value());
    CHECK_FALSE(Name::from_presentation("app..lan").has_value());
    CHECK_FALSE(Name::from_presentation("app lan").has_value());
    CHECK_FALSE(Name::from_presentation("app/lan").has_value());
    CHECK_FALSE(Name::from_presentation(std::string(64, 'a') + ".lan").has_value());
    CHECK_FALSE(Name::from_presentation(std::string(254, 'a')).has_value());
}

TEST_CASE("wildcard rules: leftmost only, '*' exactly") {
    const Name wildcard = must_make("*.app.lan");
    CHECK(wildcard.is_wildcard());
    CHECK(wildcard.to_string() == "*.app.lan");

    CHECK_FALSE(Name::from_presentation("app.*.lan").has_value());
    CHECK_FALSE(Name::from_presentation("app.lan*").has_value());
    CHECK_FALSE(Name::from_presentation("**.lan").has_value());
    CHECK_FALSE(Name::from_presentation("*.app.lan", /*allow_wildcard=*/false).has_value());
}

TEST_CASE("wildcard_matches only covers descendants with an extra label") {
    const Name wildcard = must_make("*.app.lan");
    CHECK(wildcard.wildcard_matches(must_make("x.app.lan")));
    // RFC 1034 wildcard semantics: matches at any depth below the parent, as
    // long as the parent itself has no more specific record.
    CHECK(wildcard.wildcard_matches(must_make("db.x.app.lan")));
    CHECK(wildcard.wildcard_matches(must_make("app.lan")) == false);
    CHECK(wildcard.wildcard_matches(must_make("x.other.lan")) == false);
}

TEST_CASE("label_count and parent") {
    const Name name = must_make("db.app.lan");
    CHECK(name.label_count() == 3);
    CHECK(name.parent().to_string() == "app.lan");
    CHECK(name.parent().parent().to_string() == "lan");
    CHECK(name.parent().parent().parent().is_root());
    CHECK(must_make(".").parent().is_root());
}

TEST_CASE("is_suffix_of") {
    const Name lan = must_make("lan");
    CHECK(lan.is_suffix_of(must_make("app.lan")));
    CHECK(lan.is_suffix_of(must_make("lan")));
    CHECK_FALSE(lan.is_suffix_of(must_make("app.lanx")));
    CHECK(must_make(".").is_suffix_of(must_make("app.lan")));
}

TEST_CASE("wire form parses uncompressed names") {
    const auto wire = wire_of("app.lan");
    std::size_t offset = 0;
    auto parsed = Name::from_wire(wire, offset);
    REQUIRE(parsed.has_value());
    CHECK(parsed.value().to_string() == "app.lan");
    CHECK(offset == wire.size());
}

TEST_CASE("wire form follows compression pointers") {
    // "app.lan" at 0, then "db" + pointer to 0 at offset 9.
    std::vector<std::uint8_t> message = wire_of("app.lan");
    const std::size_t pointer_at = message.size();
    message.push_back(2);
    message.push_back('d');
    message.push_back('b');
    message.push_back(0xC0);
    message.push_back(0x00);

    std::size_t offset = pointer_at;
    auto parsed = Name::from_wire(message, offset);
    REQUIRE(parsed.has_value());
    CHECK(parsed.value().to_string() == "db.app.lan");
    CHECK(offset == pointer_at + 5);
}

TEST_CASE("wire form is case-insensitive") {
    const std::vector<std::uint8_t> message = {3, 'A', 'P', 'P', 3, 'L', 'A', 'N', 0};
    std::size_t offset = 0;
    auto parsed = Name::from_wire(message, offset);
    REQUIRE(parsed.has_value());
    CHECK(parsed.value().to_string() == "app.lan");
}

TEST_CASE("wire form rejects malformed input") {
    SUBCASE("truncated label") {
        const std::vector<std::uint8_t> message = {5, 'a', 'b'};
        std::size_t offset = 0;
        CHECK_FALSE(Name::from_wire(message, offset).has_value());
    }
    SUBCASE("truncated pointer") {
        const std::vector<std::uint8_t> message = {0xC0};
        std::size_t offset = 0;
        CHECK_FALSE(Name::from_wire(message, offset).has_value());
    }
    SUBCASE("forward pointer") {
        const std::vector<std::uint8_t> message = {0xC0, 0x02, 0};
        std::size_t offset = 0;
        CHECK_FALSE(Name::from_wire(message, offset).has_value());
    }
    SUBCASE("self pointer loop") {
        const std::vector<std::uint8_t> message = {0xC0, 0x00};
        std::size_t offset = 0;
        CHECK_FALSE(Name::from_wire(message, offset).has_value());
    }
    SUBCASE("pointer out of bounds") {
        const std::vector<std::uint8_t> message = {0xC0, 0xFF};
        std::size_t offset = 0;
        CHECK_FALSE(Name::from_wire(message, offset).has_value());
    }
    SUBCASE("reserved label type") {
        const std::vector<std::uint8_t> message = {0x40, 0x00};
        std::size_t offset = 0;
        CHECK_FALSE(Name::from_wire(message, offset).has_value());
    }
    SUBCASE("pointer chain too deep") {
        // Root at 0; each even offset points at the previous one, forming a
        // long acyclic chain that must be rejected by the depth limit.
        std::vector<std::uint8_t> message(2 * 41 + 1, 0);
        for (std::size_t i = 1; i <= 40; ++i) {
            const std::size_t at = 2 * i;
            const std::size_t target = 2 * (i - 1);
            message[at] = static_cast<std::uint8_t>(0xC0 | (target >> 8));
            message[at + 1] = static_cast<std::uint8_t>(target & 0xFF);
        }
        std::size_t offset = 80;
        CHECK_FALSE(Name::from_wire(message, offset).has_value());
    }
}

TEST_CASE("punycode matches reference values") {
    // Reference values from Python's punycode codec (RFC 3492).
    CHECK(punycode_encode("m\xC3\xBC" "nchen").value() == "mnchen-3ya");
    CHECK(punycode_encode("b\xC3\xBC" "cher").value() == "bcher-kva");
    CHECK(punycode_encode("\xE4\xBB\x96\xE4\xBB\xAC\xE4\xB8\xBA\xE4\xBB\x80\xE4\xB9\x88"
                          "\xE4\xB8\x8D\xE8\xAF\xB4\xE4\xB8\xAD\xE6\x96\x87")
              .value() == "ihqwcrb4cv8a8dqg056pqjye");
    CHECK(punycode_encode("\xC3\xBC").value() == "tda");
    CHECK(punycode_encode("a\xC3\xBC").value() == "a-eha");
    CHECK(punycode_encode("abc").value() == "abc-");
}

TEST_CASE("punycode rejects invalid UTF-8") {
    CHECK_FALSE(punycode_encode("\xFF").has_value());
    CHECK_FALSE(punycode_encode("\xC3").has_value());
    CHECK_FALSE(punycode_encode("\xC3\x28").has_value());
}

TEST_CASE("IDN presentation labels are xn-- encoded") {
    const Name name = must_make("m\xC3\xBCnchen.lan");
    CHECK(name.to_string() == "xn--mnchen-3ya.lan");
}

TEST_CASE("IDN normalization is idempotent (fuzz regression)") {
    // "A\xC3\xBC" used to encode as "xn--A-eha", and reparsing lowercased it,
    // so from_presentation(to_string(x)) != x.
    const Name name = must_make("A\xC3\xBC.lan");
    CHECK(name.to_string() == "xn--a-eha.lan");
    const Name again = must_make(name.to_string());
    CHECK(again == name);

    // ASCII letters are case-folded before punycode. Non-ASCII case folding
    // (Ü -> ü) needs Unicode tables (utf8proc); without it the uppercase form
    // is a distinct label, which is still stable across round-trips.
    const Name ascii = must_make("MuNCHEN.lan");
    CHECK(ascii.to_string() == "munchen.lan");
    const Name upper_u = must_make("M\xC3\x9CNCHEN.lan");
    CHECK(must_make(upper_u.to_string()) == upper_u);
}
