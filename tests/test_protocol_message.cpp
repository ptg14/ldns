#include "doctest.hpp"

#include "core/dns/name.hpp"
#include "core/net/protocol_message.hpp"

using namespace ldns;
using namespace ldns::net;

TEST_CASE("Protocol framing adds 4-byte BE length prefix") {
    std::vector<std::uint8_t> payload = {1, 2, 3, 4, 5};
    auto framed = frame_payload(payload);
    REQUIRE(framed.has_value());
    REQUIRE(framed.value().size() == 9);

    CHECK(framed.value()[0] == 0);
    CHECK(framed.value()[1] == 0);
    CHECK(framed.value()[2] == 0);
    CHECK(framed.value()[3] == 5);
    for (std::size_t i = 0; i < 5; ++i) {
        CHECK(framed.value()[4 + i] == payload[i]);
    }
}

TEST_CASE("Protocol HELLO message encode and parse roundtrip") {
    HelloMessage h;
    h.version = {1, 0};
    for (std::size_t i = 0; i < 16; ++i) h.node_id[i] = static_cast<std::uint8_t>(i + 10);
    h.name = "peer-one";
    h.net = {0xaa, 0xbb, 0xcc, 0xdd, 0x11, 0x22, 0x33, 0x44};
    h.epoch = 999;
    h.seq = 77;
    h.sync_port = 5391;
    h.dns_port = 53;
    h.ips = {{192, 168, 1, 10}};
    h.caps = {"relay", "snapshot-chunk"};
    h.nonce = {0};
    h.psk = false;

    auto frame = encode_hello_frame(h);
    REQUIRE(frame.has_value());
    REQUIRE(frame.value().size() > 4);

    // Extract payload by skipping 4-byte length prefix
    std::span<const std::uint8_t> payload(frame.value().data() + 4, frame.value().size() - 4);
    auto parsed = parse_payload(payload);
    REQUIRE(parsed.has_value());
    CHECK(parsed.value().type == MessageType::Hello);
    REQUIRE(parsed.value().hello.has_value());

    const auto& res = parsed.value().hello.value();
    CHECK(res.name == "peer-one");
    CHECK(res.node_id == h.node_id);
    CHECK(res.net == h.net);
    CHECK(res.epoch == 999);
    CHECK(res.seq == 77);
    CHECK(res.sync_port == 5391);
    CHECK(res.dns_port == 53);
    REQUIRE(res.ips.size() == 1);
    CHECK(res.ips[0] == std::vector<std::uint8_t>{192, 168, 1, 10});
}

TEST_CASE("Protocol PING, PONG, BYE, and ERROR messages") {
    // PING
    auto ping_frame = encode_ping_frame();
    REQUIRE(ping_frame.has_value());
    std::span<const std::uint8_t> ping_pld(ping_frame.value().data() + 4, ping_frame.value().size() - 4);
    auto ping_parsed = parse_payload(ping_pld);
    REQUIRE(ping_parsed.has_value());
    CHECK(ping_parsed.value().type == MessageType::Ping);

    // PONG
    auto pong_frame = encode_pong_frame();
    REQUIRE(pong_frame.has_value());
    std::span<const std::uint8_t> pong_pld(pong_frame.value().data() + 4, pong_frame.value().size() - 4);
    auto pong_parsed = parse_payload(pong_pld);
    REQUIRE(pong_parsed.has_value());
    CHECK(pong_parsed.value().type == MessageType::Pong);

    // BYE
    auto bye_frame = encode_bye_frame();
    REQUIRE(bye_frame.has_value());
    std::span<const std::uint8_t> bye_pld(bye_frame.value().data() + 4, bye_frame.value().size() - 4);
    auto bye_parsed = parse_payload(bye_pld);
    REQUIRE(bye_parsed.has_value());
    CHECK(bye_parsed.value().type == MessageType::Bye);

    // ERROR
    ErrorMessage err{ProtocolErrorCode::Duplicate, "connection already established"};
    auto err_frame = encode_error_frame(err);
    REQUIRE(err_frame.has_value());
    std::span<const std::uint8_t> err_pld(err_frame.value().data() + 4, err_frame.value().size() - 4);
    auto err_parsed = parse_payload(err_pld);
    REQUIRE(err_parsed.has_value());
    CHECK(err_parsed.value().type == MessageType::Error);
    REQUIRE(err_parsed.value().error.has_value());
    CHECK(err_parsed.value().error->code == ProtocolErrorCode::Duplicate);
    CHECK(err_parsed.value().error->msg == "connection already established");
}

TEST_CASE("Protocol PEERS exchange message") {
    PeersMessage pm;
    PeerItem item;
    for (std::size_t i = 0; i < 16; ++i) item.node_id[i] = static_cast<std::uint8_t>(i + 5);
    item.sync_port = 5391;
    item.ips = {{10, 0, 0, 5}};
    pm.peers.push_back(item);

    auto peers_frame = encode_peers_frame(pm);
    REQUIRE(peers_frame.has_value());
    std::span<const std::uint8_t> peers_pld(peers_frame.value().data() + 4, peers_frame.value().size() - 4);
    auto peers_parsed = parse_payload(peers_pld);
    REQUIRE(peers_parsed.has_value());
    CHECK(peers_parsed.value().type == MessageType::Peers);
    REQUIRE(peers_parsed.value().peers.has_value());
    REQUIRE(peers_parsed.value().peers->peers.size() == 1);
    CHECK(peers_parsed.value().peers->peers[0].sync_port == 5391);
    CHECK(peers_parsed.value().peers->peers[0].node_id == item.node_id);
}

TEST_CASE("Protocol SYNC_REQ and SYNC_END messages") {
    // SYNC_REQ
    SyncReqMessage req;
    req.id = 42;
    SyncReqOwner o1;
    o1.owner.fill(0x11);
    o1.epoch = 100;
    o1.since_seq = 50;
    req.owners.push_back(o1);

    auto frame = encode_sync_req_frame(req);
    REQUIRE(frame.has_value());
    std::span<const std::uint8_t> pld(frame.value().data() + 4, frame.value().size() - 4);
    auto parsed = parse_payload(pld);
    REQUIRE(parsed.has_value());
    CHECK(parsed.value().type == MessageType::SyncReq);
    REQUIRE(parsed.value().sync_req.has_value());
    CHECK(parsed.value().sync_req->id == 42);
    REQUIRE(parsed.value().sync_req->owners.size() == 1);
    CHECK(parsed.value().sync_req->owners[0].owner == o1.owner);
    CHECK(parsed.value().sync_req->owners[0].epoch == 100);
    CHECK(parsed.value().sync_req->owners[0].since_seq == 50);

    // SYNC_END
    SyncEndMessage end_msg;
    end_msg.id = 42;
    auto end_frame = encode_sync_end_frame(end_msg);
    REQUIRE(end_frame.has_value());
    std::span<const std::uint8_t> end_pld(end_frame.value().data() + 4, end_frame.value().size() - 4);
    auto end_parsed = parse_payload(end_pld);
    REQUIRE(end_parsed.has_value());
    CHECK(end_parsed.value().type == MessageType::SyncEnd);
    REQUIRE(end_parsed.value().sync_end.has_value());
    CHECK(end_parsed.value().sync_end->id == 42);
}

TEST_CASE("Protocol SYNC_DATA and UPDATE messages with Op put/del") {
    Op put_op;
    put_op.op = "put";
    put_op.name = "web.lan";
    put_op.type = 1; // A
    put_op.ttl = 300;
    put_op.data = {{192, 168, 1, 100}};
    put_op.created = 1000;
    put_op.ts = 2000;

    Op del_op;
    del_op.op = "del";
    del_op.name = "old.lan";
    del_op.type = 1;
    del_op.ts = 2050;

    // UPDATE
    UpdateMessage upd;
    upd.owner.fill(0x22);
    upd.epoch = 5;
    upd.seq = 10;
    upd.ops = {put_op, del_op};

    auto upd_frame = encode_update_frame(upd);
    REQUIRE(upd_frame.has_value());
    std::span<const std::uint8_t> upd_pld(upd_frame.value().data() + 4, upd_frame.value().size() - 4);
    auto upd_parsed = parse_payload(upd_pld);
    REQUIRE(upd_parsed.has_value());
    CHECK(upd_parsed.value().type == MessageType::Update);
    REQUIRE(upd_parsed.value().update.has_value());
    CHECK(upd_parsed.value().update->owner == upd.owner);
    CHECK(upd_parsed.value().update->epoch == 5);
    CHECK(upd_parsed.value().update->seq == 10);
    REQUIRE(upd_parsed.value().update->ops.size() == 2);
    CHECK(upd_parsed.value().update->ops[0].name == "web.lan");
    CHECK(upd_parsed.value().update->ops[0].op == "put");
    CHECK(upd_parsed.value().update->ops[0].data[0] == std::vector<std::uint8_t>{192, 168, 1, 100});
    CHECK(upd_parsed.value().update->ops[1].name == "old.lan");
    CHECK(upd_parsed.value().update->ops[1].op == "del");

    // SYNC_DATA
    SyncDataMessage sdata;
    sdata.id = 99;
    sdata.owner.fill(0x33);
    sdata.epoch = 1;
    sdata.from_seq = 0;
    sdata.to_seq = 10;
    sdata.snapshot = true;
    sdata.chunk = 0;
    sdata.last = true;
    sdata.ops = {put_op};

    auto sdata_frame = encode_sync_data_frame(sdata);
    REQUIRE(sdata_frame.has_value());
    std::span<const std::uint8_t> sdata_pld(sdata_frame.value().data() + 4, sdata_frame.value().size() - 4);
    auto sdata_parsed = parse_payload(sdata_pld);
    REQUIRE(sdata_parsed.has_value());
    CHECK(sdata_parsed.value().type == MessageType::SyncData);
    REQUIRE(sdata_parsed.value().sync_data.has_value());
    CHECK(sdata_parsed.value().sync_data->id == 99);
    CHECK(sdata_parsed.value().sync_data->owner == sdata.owner);
    CHECK(sdata_parsed.value().sync_data->snapshot == true);
    CHECK(sdata_parsed.value().sync_data->chunk == 0);
    CHECK(sdata_parsed.value().sync_data->last == true);
    REQUIRE(sdata_parsed.value().sync_data->ops.size() == 1);
    CHECK(sdata_parsed.value().sync_data->ops[0].name == "web.lan");

    // RESOLVE
    ResolveMessage rmsg;
    rmsg.id = 42;
    rmsg.name = "app.lan";
    rmsg.type = 1; // A

    auto rmsg_frame = encode_resolve_frame(rmsg);
    REQUIRE(rmsg_frame.has_value());
    std::span<const std::uint8_t> rmsg_pld(rmsg_frame.value().data() + 4, rmsg_frame.value().size() - 4);
    auto rmsg_parsed = parse_payload(rmsg_pld);
    REQUIRE(rmsg_parsed.has_value());
    CHECK(rmsg_parsed.value().type == MessageType::Resolve);
    REQUIRE(rmsg_parsed.value().resolve.has_value());
    CHECK(rmsg_parsed.value().resolve->id == 42);
    CHECK(rmsg_parsed.value().resolve->name == "app.lan");
    CHECK(rmsg_parsed.value().resolve->type == 1);

    // RESOLVE_R
    ResolveResponseMessage rresp;
    rresp.id = 42;
    rresp.rcode = 0;
    rresp.epoch = 100;
    rresp.seq = 200;

    ResolveAnswer a1;
    a1.name = "app.lan";
    a1.type = 1;
    a1.ttl = 300;
    a1.data = {{10, 0, 0, 1}};

    ResolveAnswer a2;
    a2.name = "alias.lan";
    a2.type = 5; // CNAME
    a2.ttl = 60;
    auto cname_target = dns::Name::from_presentation("app.lan").value();
    auto cname_wire = cname_target.wire();
    a2.data = {std::vector<std::uint8_t>(cname_wire.begin(), cname_wire.end())};

    rresp.answers = {a1, a2};

    auto rresp_frame = encode_resolve_response_frame(rresp);
    REQUIRE(rresp_frame.has_value());
    std::span<const std::uint8_t> rresp_pld(rresp_frame.value().data() + 4, rresp_frame.value().size() - 4);
    auto rresp_parsed = parse_payload(rresp_pld);
    REQUIRE(rresp_parsed.has_value());
    CHECK(rresp_parsed.value().type == MessageType::ResolveResponse);
    REQUIRE(rresp_parsed.value().resolve_response.has_value());
    CHECK(rresp_parsed.value().resolve_response->id == 42);
    CHECK(rresp_parsed.value().resolve_response->rcode == 0);
    CHECK(rresp_parsed.value().resolve_response->epoch == 100);
    CHECK(rresp_parsed.value().resolve_response->seq == 200);
    REQUIRE(rresp_parsed.value().resolve_response->answers.size() == 2);
    CHECK(rresp_parsed.value().resolve_response->answers[0].name == "app.lan");
    CHECK(rresp_parsed.value().resolve_response->answers[0].type == 1);
    CHECK(rresp_parsed.value().resolve_response->answers[0].ttl == 300);
    CHECK(rresp_parsed.value().resolve_response->answers[0].data[0] == std::vector<std::uint8_t>{10, 0, 0, 1});
    CHECK(rresp_parsed.value().resolve_response->answers[1].name == "alias.lan");
    CHECK(rresp_parsed.value().resolve_response->answers[1].type == 5);
    CHECK(rresp_parsed.value().resolve_response->answers[1].data[0] == a2.data[0]);
}
