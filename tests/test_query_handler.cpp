#include "doctest.hpp"

#include "core/server/query_handler.hpp"

#include <string>
#include <vector>

using namespace ldns;
using namespace ldns::dns;

namespace {

Name nm(const std::string& text) { return Name::from_presentation(text).value(); }

std::vector<std::uint8_t> a(const std::string& ip) { return parse_ipv4(ip).value(); }

void put_a(LocalZone& zone, const std::string& name, const std::string& ip,
           std::uint32_t ttl = 60) {
    REQUIRE(zone.upsert(nm(name), static_cast<std::uint16_t>(RrType::A), ttl, {a(ip)})
                .has_value());
}

void put_name(LocalZone& zone, const std::string& name, RrType type, const std::string& target) {
    auto rdata = encode_rdata_name(nm(target));
    REQUIRE(rdata.has_value());
    REQUIRE(zone.upsert(nm(name), static_cast<std::uint16_t>(type), 60, {rdata.value()})
                .has_value());
}

std::vector<std::uint8_t> query(const std::string& name, RrType type = RrType::A,
                                std::uint16_t req_type = 0) {
    Message msg;
    msg.id = 0xBEEF;
    msg.flags = kRd;
    Question q;
    q.name = nm(name);
    q.type = req_type != 0 ? req_type : static_cast<std::uint16_t>(type);
    msg.questions.push_back(q);
    auto encoded = msg.encode();
    REQUIRE(encoded.has_value());
    return encoded.value();
}

std::optional<HandlerResponse> run(const LocalZone& zone, const std::vector<std::uint8_t>& packet,
                                   QueryContext context = {false, 512}) {
    QueryHandler handler(zone);
    return handler.handle(packet, context);
}

}  // namespace

TEST_CASE("A query for a local name is answered with AA=1") {
    LocalZone zone;
    put_a(zone, "app.lan", "192.168.1.10");

    auto response = run(zone, query("app.lan"));
    REQUIRE(response.has_value());
    CHECK(response->from_local_zone);
    const auto& msg = response->message;
    CHECK(msg.is_response());
    CHECK(msg.id == 0xBEEF);
    CHECK((msg.flags & kAa) != 0);
    CHECK((msg.flags & kRa) != 0);
    CHECK((msg.flags & kRd) != 0);
    CHECK(msg.rcode() == 0);
    REQUIRE(msg.answers.size() == 1);
    CHECK(format_ipv4(msg.answers[0].rdata) == "192.168.1.10");
    REQUIRE(msg.questions.size() == 1);
    CHECK(msg.questions[0].name.to_string() == "app.lan");
}

TEST_CASE("NODATA returns NOERROR with an SOA in authority") {
    LocalZone zone;
    put_a(zone, "app.lan", "10.0.0.1");
    auto soa = encode_rdata_soa(nm("ns.lan"), nm("admin.lan"), 1, 3600, 600, 86400, 60);
    REQUIRE(zone.upsert(nm("lan"), static_cast<std::uint16_t>(RrType::SOA), 60, {soa.value()})
                .has_value());

    auto response = run(zone, query("app.lan", RrType::AAAA));
    REQUIRE(response.has_value());
    CHECK(response->message.rcode() == 0);
    CHECK(response->message.answers.empty());
    REQUIRE(response->message.authority.size() == 1);
    CHECK(response->message.authority[0].type == static_cast<std::uint16_t>(RrType::SOA));
}

TEST_CASE("unknown name returns NXDOMAIN with SOA") {
    LocalZone zone;
    put_a(zone, "app.lan", "10.0.0.1");
    auto soa = encode_rdata_soa(nm("ns.lan"), nm("admin.lan"), 1, 3600, 600, 86400, 60);
    REQUIRE(zone.upsert(nm("lan"), static_cast<std::uint16_t>(RrType::SOA), 60, {soa.value()})
                .has_value());

    auto response = run(zone, query("nope.lan"));
    REQUIRE(response.has_value());
    CHECK(response->message.rcode() == static_cast<std::uint8_t>(Rcode::NxDomain));
    CHECK((response->message.flags & kAa) != 0);
    REQUIRE(response->message.authority.size() == 1);
}

TEST_CASE("empty zone refuses instead of claiming authority") {
    LocalZone zone;
    auto response = run(zone, query("example.com"));
    REQUIRE(response.has_value());
    CHECK(response->message.rcode() == static_cast<std::uint8_t>(Rcode::Refused));
    CHECK((response->message.flags & kAa) == 0);
}

TEST_CASE("wildcards are served for any depth") {
    LocalZone zone;
    REQUIRE(zone.upsert(nm("*.app.lan"), static_cast<std::uint16_t>(RrType::A), 60, {a("10.0.0.9")})
                .has_value());

    for (const char* name : {"x.app.lan", "db.x.app.lan", "a.b.c.app.lan"}) {
        auto response = run(zone, query(name));
        REQUIRE(response.has_value());
        CHECK(response->message.rcode() == 0);
        REQUIRE(response->message.answers.size() == 1);
        CHECK(format_ipv4(response->message.answers[0].rdata) == "10.0.0.9");
    }
}

TEST_CASE("CNAME chain is followed inside the zone") {
    LocalZone zone;
    put_name(zone, "alias.lan", RrType::CNAME, "real.lan");
    put_a(zone, "real.lan", "10.0.0.7");

    auto response = run(zone, query("alias.lan"));
    REQUIRE(response.has_value());
    CHECK(response->message.rcode() == 0);
    REQUIRE(response->message.answers.size() == 2);
    CHECK(response->message.answers[0].type == static_cast<std::uint16_t>(RrType::CNAME));
    CHECK(response->message.answers[1].type == static_cast<std::uint16_t>(RrType::A));
    CHECK(format_ipv4(response->message.answers[1].rdata) == "10.0.0.7");
}

TEST_CASE("CNAME loop is SERVFAIL, not an infinite loop") {
    LocalZone zone;
    put_name(zone, "a.lan", RrType::CNAME, "b.lan");
    put_name(zone, "b.lan", RrType::CNAME, "a.lan");

    auto response = run(zone, query("a.lan"));
    REQUIRE(response.has_value());
    CHECK(response->message.rcode() == static_cast<std::uint8_t>(Rcode::ServFail));
}

TEST_CASE("CNAME chain longer than 8 is truncated as SERVFAIL") {
    LocalZone zone;
    for (int i = 0; i < 10; ++i) {
        const std::string name = "c" + std::to_string(i) + ".lan";
        const std::string next = "c" + std::to_string(i + 1) + ".lan";
        put_name(zone, name, RrType::CNAME, next);
    }
    put_a(zone, "c10.lan", "10.0.0.1");

    auto response = run(zone, query("c0.lan"));
    REQUIRE(response.has_value());
    CHECK(response->message.rcode() == static_cast<std::uint8_t>(Rcode::ServFail));
}

TEST_CASE("CNAME CNAME chain that ends outside the zone returns the chain") {
    LocalZone zone;
    put_name(zone, "alias.lan", RrType::CNAME, "remote.other");

    auto response = run(zone, query("alias.lan"));
    REQUIRE(response.has_value());
    CHECK(response->message.rcode() == 0);
    REQUIRE(response->message.answers.size() == 1);
    CHECK(response->message.answers[0].type == static_cast<std::uint16_t>(RrType::CNAME));
}

TEST_CASE("malformed packet with a valid header gets FORMERR") {
    const std::vector<std::uint8_t> packet = {0x12, 0x34, 0x01, 0x00, 0x00, 0x01,
                                              0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    LocalZone zone;
    auto response = run(zone, packet);
    REQUIRE(response.has_value());
    CHECK(response->message.id == 0x1234);
    CHECK(response->message.rcode() == static_cast<std::uint8_t>(Rcode::FormErr));
}

TEST_CASE("responses and tiny packets are dropped") {
    LocalZone zone;
    CHECK_FALSE(run(zone, {0x00, 0x01}).has_value());

    Message response;
    response.id = 1;
    response.set_flag(kQr, true);
    auto encoded = response.encode();
    REQUIRE(encoded.has_value());
    CHECK_FALSE(run(zone, encoded.value()).has_value());
}

TEST_CASE("non-QUERY opcode gets NOTIMP, non-IN class gets REFUSED") {
    LocalZone zone;
    put_a(zone, "app.lan", "10.0.0.1");

    Message msg;
    msg.id = 7;
    msg.flags = static_cast<std::uint16_t>(0x2800);  // opcode 5 (UPDATE)
    Question q;
    q.name = nm("app.lan");
    q.type = 1;
    msg.questions.push_back(q);
    auto encoded = msg.encode();
    REQUIRE(encoded.has_value());
    auto response = run(zone, encoded.value());
    REQUIRE(response.has_value());
    CHECK(response->message.rcode() == static_cast<std::uint8_t>(Rcode::NotImp));

    Message class_msg;
    class_msg.id = 8;
    Question chaos;
    chaos.name = nm("version.bind");
    chaos.type = 16;
    chaos.qclass = 3;  // CH
    class_msg.questions.push_back(chaos);
    auto encoded_class = class_msg.encode();
    REQUIRE(encoded_class.has_value());
    auto refused = run(zone, encoded_class.value());
    REQUIRE(refused.has_value());
    CHECK(refused->message.rcode() == static_cast<std::uint8_t>(Rcode::Refused));
}

TEST_CASE("unsupported EDNS version gets BADVERS") {
    LocalZone zone;
    put_a(zone, "app.lan", "10.0.0.1");

    Message msg;
    msg.id = 10;
    msg.flags = kRd;
    Question q;
    q.name = nm("app.lan");
    q.type = 1;
    msg.questions.push_back(q);
    msg.edns = EdnsInfo{1232, 1, false, 0};  // version 1: not supported
    auto encoded = msg.encode();
    REQUIRE(encoded.has_value());

    auto response = run(zone, encoded.value(), QueryContext{false, 1232});
    REQUIRE(response.has_value());
    REQUIRE(response->message.edns.has_value());
    CHECK(response->message.edns->extended_rcode == 1);  // extended rcode 16 = BADVERS
    CHECK(response->message.answers.empty());
}

TEST_CASE("EDNS payload is echoed and capped") {
    LocalZone zone;
    put_a(zone, "app.lan", "10.0.0.1");

    Message msg;
    msg.id = 9;
    msg.flags = kRd;
    Question q;
    q.name = nm("app.lan");
    q.type = 1;
    msg.questions.push_back(q);
    msg.edns = EdnsInfo{4096, 0, true, 0};
    auto encoded = msg.encode();
    REQUIRE(encoded.has_value());

    auto response = run(zone, encoded.value(), QueryContext{false, 4096});
    REQUIRE(response.has_value());
    REQUIRE(response->message.edns.has_value());
    CHECK(response->message.edns->udp_payload == 1232);  // server cap
    CHECK(response->message.edns->do_bit);
}
