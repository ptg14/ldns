#include "doctest.hpp"

#include "core/dns/message.hpp"

#include <vector>

using namespace ldns;
using namespace ldns::dns;

namespace {

std::vector<std::uint8_t> bytes(std::initializer_list<int> values) {
    std::vector<std::uint8_t> out;
    out.reserve(values.size());
    for (const int v : values) out.push_back(static_cast<std::uint8_t>(v));
    return out;
}

Message query_for(const std::string& name, std::uint16_t type = 1) {
    Message msg;
    msg.id = 0x1234;
    msg.flags = kRd;
    Question q;
    q.name = Name::from_presentation(name).value();
    q.type = type;
    msg.questions.push_back(q);
    return msg;
}

}  // namespace

TEST_CASE("parses a golden query packet") {
    // dig example.com A, id 0x1234, RD set.
    const auto packet = bytes({0x12, 0x34, 0x01, 0x00, 0x00, 0x01, 0x00, 0x00,
                               0x00, 0x00, 0x00, 0x00, 0x07, 'e',  'x',  'a',
                               'm',  'p',  'l',  'e',  0x03, 'c',  'o',  'm',
                               0x00, 0x00, 0x01, 0x00, 0x01});
    auto parsed = Message::parse(packet);
    REQUIRE(parsed.has_value());
    CHECK(parsed.value().id == 0x1234);
    CHECK_FALSE(parsed.value().is_response());
    CHECK(parsed.value().opcode() == 0);
    CHECK(parsed.value().rcode() == 0);
    REQUIRE(parsed.value().questions.size() == 1);
    CHECK(parsed.value().questions[0].name.to_string() == "example.com");
    CHECK(parsed.value().questions[0].type == 1);
    CHECK(parsed.value().questions[0].qclass == kClassIn);
}

TEST_CASE("encodes a query and round-trips it") {
    const Message msg = query_for("example.com");
    auto encoded = msg.encode();
    REQUIRE(encoded.has_value());
    CHECK(encoded.value().size() == 29);

    auto parsed = Message::parse(encoded.value());
    REQUIRE(parsed.has_value());
    CHECK(parsed.value().id == msg.id);
    CHECK(parsed.value().questions.size() == 1);
    CHECK(parsed.value().questions[0].name.to_string() == "example.com");
}

TEST_CASE("parses a compressed response with an A record") {
    const auto packet = bytes({
        0x12, 0x34, 0x81, 0x80, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00,
        0x07, 'e',  'x',  'a',  'm',  'p',  'l',  'e',  0x03, 'c',  'o',  'm',  0x00,
        0x00, 0x01, 0x00, 0x01,
        // answer: pointer to name at offset 12
        0xC0, 0x0C, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x3C, 0x00, 0x04,
        93,   184,  216,  34,
    });
    auto parsed = Message::parse(packet);
    REQUIRE(parsed.has_value());
    CHECK(parsed.value().is_response());
    REQUIRE(parsed.value().answers.size() == 1);
    const auto& answer = parsed.value().answers[0];
    CHECK(answer.name.to_string() == "example.com");
    CHECK(answer.type == 1);
    CHECK(answer.ttl == 60);
    REQUIRE(answer.rdata.size() == 4);
    CHECK(format_ipv4(answer.rdata) == "93.184.216.34");
}

TEST_CASE("encoder compresses repeated names") {
    Message msg = query_for("app.lan");
    msg.set_flag(kQr, true);
    for (int i = 0; i < 3; ++i) {
        ResourceRecord rr;
        rr.name = msg.questions[0].name;
        rr.type = 1;
        rr.ttl = 60;
        rr.rdata = {10, 0, 0, static_cast<std::uint8_t>(i + 1)};
        msg.answers.push_back(rr);
    }
    auto encoded = msg.encode();
    REQUIRE(encoded.has_value());
    // Header(12) + qname(9) + qtype/class(4) + 3 * (ptr(2) + type/class/ttl/rdlen(10) + rdata(4))
    const std::size_t compressed = 12 + 9 + 4 + 3 * (2 + 10 + 4);
    const std::size_t uncompressed = 12 + 9 + 4 + 3 * (9 + 10 + 4);
    CHECK(encoded.value().size() == compressed);
    CHECK(encoded.value().size() < uncompressed);

    auto parsed = Message::parse(encoded.value());
    REQUIRE(parsed.has_value());
    REQUIRE(parsed.value().answers.size() == 3);
    for (const auto& rr : parsed.value().answers) {
        CHECK(rr.name.to_string() == "app.lan");
    }
}

TEST_CASE("CNAME rdata names survive compression round-trip") {
    Message msg = query_for("alias.lan");
    msg.set_flag(kQr, true);
    ResourceRecord rr;
    rr.name = msg.questions[0].name;
    rr.type = static_cast<std::uint16_t>(RrType::CNAME);
    rr.ttl = 60;
    auto rdata = encode_rdata_name(Name::from_presentation("real.lan").value());
    REQUIRE(rdata.has_value());
    rr.rdata = rdata.value();
    msg.answers.push_back(rr);

    auto encoded = msg.encode();
    REQUIRE(encoded.has_value());
    auto parsed = Message::parse(encoded.value());
    REQUIRE(parsed.has_value());
    REQUIRE(parsed.value().answers.size() == 1);
    auto target = decode_rdata_name(parsed.value().answers[0].rdata);
    REQUIRE(target.has_value());
    CHECK(target.value().to_string() == "real.lan");
}

TEST_CASE("parses EDNS0 OPT record") {
    Message msg = query_for("example.com");
    msg.edns = EdnsInfo{1232, 0, false, 0};
    auto encoded = msg.encode();
    REQUIRE(encoded.has_value());

    auto parsed = Message::parse(encoded.value());
    REQUIRE(parsed.has_value());
    REQUIRE(parsed.value().edns.has_value());
    CHECK(parsed.value().edns->udp_payload == 1232);
    CHECK(parsed.value().edns->version == 0);
    CHECK_FALSE(parsed.value().edns->do_bit);
    CHECK(parsed.value().additional.empty());

    // The OPT RR must appear as the last additional record on the wire.
    const auto& raw = encoded.value();
    CHECK(raw[raw.size() - 1] == 0);
    CHECK(raw[raw.size() - 2] == 0);
}

TEST_CASE("multiple OPT records are rejected") {
    // Regression from fuzzing: a second OPT used to be left in `additional`,
    // so re-encoding emitted it as a new OPT and the round-trip broke.
    const auto packet = bytes({
        0x12, 0x34, 0x01, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02,
        0x07, 'e',  'x',  'a',  'm',  'p',  'l',  'e',  0x03, 'c',  'o',  'm',  0x00,
        0x00, 0x01, 0x00, 0x01,
        // OPT #1: root, type 41, class 1232, ttl 0, rdlen 0
        0x00, 0x00, 0x29, 0x04, 0xD0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        // OPT #2: identical
        0x00, 0x00, 0x29, 0x04, 0xD0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    });
    auto parsed = Message::parse(packet);
    REQUIRE_FALSE(parsed.has_value());
    CHECK(parsed.error().code == 108);
}

TEST_CASE("OPT with a small payload is normalized to 512") {
    const auto packet = bytes({
        0x12, 0x34, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01,
        // OPT with class 0, ttl 0, rdlen 0
        0x00, 0x00, 0x29, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    });
    auto parsed = Message::parse(packet);
    REQUIRE(parsed.has_value());
    REQUIRE(parsed.value().edns.has_value());
    CHECK(parsed.value().edns->udp_payload == 512);
}

TEST_CASE("stray OPT in additional is not emitted twice") {
    Message msg = query_for("example.com");
    msg.edns = EdnsInfo{1232, 0, false, 0};
    ResourceRecord opt;
    opt.name = Name::from_presentation(".").value();
    opt.type = static_cast<std::uint16_t>(RrType::OPT);
    opt.rr_class = 4096;
    opt.ttl = 0;
    msg.additional.push_back(opt);

    auto parsed = Message::parse(msg.encode().value());
    REQUIRE(parsed.has_value());
    CHECK(parsed.value().edns->udp_payload == 1232);
}

TEST_CASE("EDNS DO bit round-trips") {
    Message msg = query_for("example.com");
    msg.edns = EdnsInfo{4096, 0, true, 0};
    auto parsed = Message::parse(msg.encode().value());
    REQUIRE(parsed.has_value());
    REQUIRE(parsed.value().edns.has_value());
    CHECK(parsed.value().edns->do_bit);
    CHECK(parsed.value().edns->udp_payload == 4096);
}

TEST_CASE("encode respects max_size and truncation drops sections") {
    Message msg = query_for("app.lan");
    msg.set_flag(kQr, true);
    for (int i = 0; i < 50; ++i) {
        ResourceRecord rr;
        rr.name = Name::from_presentation("host" + std::to_string(i) + ".app.lan").value();
        rr.type = static_cast<std::uint16_t>(RrType::TXT);
        rr.ttl = 60;
        rr.rdata = encode_rdata_txt("some text payload").value();
        msg.answers.push_back(rr);
    }

    CHECK_FALSE(msg.encode(512).has_value());
    CHECK(msg.encode(4096).has_value());

    const auto truncated = msg.encode_truncated();
    CHECK(truncated.size() <= 512);
    auto parsed = Message::parse(truncated);
    REQUIRE(parsed.has_value());
    CHECK(parsed.value().truncated());
    CHECK(parsed.value().answers.empty());
    REQUIRE(parsed.value().questions.size() == 1);
}

TEST_CASE("rejects malformed packets") {
    SUBCASE("short header") {
        CHECK_FALSE(Message::parse(bytes({1, 2, 3})).has_value());
    }
    SUBCASE("question count with no data") {
        auto packet = bytes({0, 1, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0});
        CHECK_FALSE(Message::parse(packet).has_value());
    }
    SUBCASE("rdlength past end") {
        const auto packet = bytes({
            0x12, 0x34, 0x81, 0x80, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00,
            0x07, 'e',  'x',  'a',  'm',  'p',  'l',  'e',  0x03, 'c',  'o',  'm',  0x00,
            0x00, 0x01, 0x00, 0x01,
            0xC0, 0x0C, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x3C, 0x00, 0x10,
            1,    2,    3,
        });
        CHECK_FALSE(Message::parse(packet).has_value());
    }
    SUBCASE("counts above limit") {
        auto packet = bytes({0, 1, 0, 0, 0xFF, 0xFF, 0, 0, 0, 0, 0, 0});
        CHECK_FALSE(Message::parse(packet).has_value());
    }
    SUBCASE("compression pointer loop in question") {
        auto packet = bytes({0, 1, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0xC0, 0x0C, 0, 1, 0, 1});
        CHECK_FALSE(Message::parse(packet).has_value());
    }
}

TEST_CASE("TXT rdata splits long payloads into 255-byte strings") {
    const std::string text(600, 'x');
    auto rdata = encode_rdata_txt(text);
    REQUIRE(rdata.has_value());
    CHECK(rdata.value().size() == 600 + 3);
    CHECK(decode_rdata_txt(rdata.value()).value() == text);
}

TEST_CASE("SOA rdata round-trips") {
    auto rdata = encode_rdata_soa(Name::from_presentation("ns.lan").value(),
                                  Name::from_presentation("admin.lan").value(), 7, 3600, 600,
                                  86400, 60);
    REQUIRE(rdata.has_value());

    ResourceRecord rr;
    rr.name = Name::from_presentation("lan").value();
    rr.type = static_cast<std::uint16_t>(RrType::SOA);
    rr.ttl = 60;
    rr.rdata = rdata.value();

    Message msg;
    msg.id = 7;
    msg.set_flag(kQr, true);
    msg.authority.push_back(rr);

    auto parsed = Message::parse(msg.encode().value());
    REQUIRE(parsed.has_value());
    REQUIRE(parsed.value().authority.size() == 1);
    CHECK(parsed.value().authority[0].type == static_cast<std::uint16_t>(RrType::SOA));
}

TEST_CASE("IPv4 parse/format") {
    CHECK(parse_ipv4("192.168.1.10").value() == bytes({192, 168, 1, 10}));
    CHECK(format_ipv4(bytes({192, 168, 1, 10})) == "192.168.1.10");
    CHECK_FALSE(parse_ipv4("256.1.1.1").has_value());
    CHECK_FALSE(parse_ipv4("1.2.3").has_value());
    CHECK_FALSE(parse_ipv4("1.2.3.4.5").has_value());
    CHECK_FALSE(parse_ipv4("a.b.c.d").has_value());
    CHECK_FALSE(parse_ipv4("").has_value());
}

TEST_CASE("IPv6 parse/format") {
    struct Case {
        std::string text;
        std::uint8_t first;
        std::uint8_t last;
    };
    const std::vector<Case> cases = {
        {"::1", 0, 1},
        {"::", 0, 0},
        {"fe80::1", 0xFE, 1},
        {"2001:db8::ff00:42:8329", 0x20, 0x29},
        {"2001:0db8:0000:0000:0000:ff00:0042:8329", 0x20, 0x29},
    };
    for (const auto& c : cases) {
        auto parsed = parse_ipv6(c.text);
        REQUIRE(parsed.has_value());
        REQUIRE(parsed.value().size() == 16);
        CHECK(parsed.value()[0] == c.first);
        CHECK(parsed.value()[15] == c.last);
        CHECK(parse_ipv6(format_ipv6(parsed.value())).value() == parsed.value());
    }

    CHECK(format_ipv6(parse_ipv6("::1").value()) == "::1");
    CHECK(format_ipv6(parse_ipv6("2001:0db8::1").value()) == "2001:db8::1");
    CHECK_FALSE(parse_ipv6("2001:db8:::1").has_value());
    CHECK_FALSE(parse_ipv6("gggg::1").has_value());
}

TEST_CASE("rr type names") {
    CHECK(rr_type_name(1) == "A");
    CHECK(rr_type_name(28) == "AAAA");
    CHECK(rr_type_name(5) == "CNAME");
    CHECK(rr_type_name(16) == "TXT");
    CHECK(rr_type_from_name("A").value() == 1);
    CHECK(rr_type_from_name("aaaa").has_value() == false);
}
