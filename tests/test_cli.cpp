#include "doctest.hpp"

#include "core/cli/commands.hpp"
#include "core/core_api.hpp"
#include "core/runtime/runtime.hpp"

#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>

using namespace ldns;

namespace {

struct TempDir {
    std::filesystem::path path;

    TempDir() {
        path = std::filesystem::temp_directory_path() /
               ("ldns-cli-test-" + std::to_string(::rand()) + "-" +
                std::to_string(reinterpret_cast<std::uintptr_t>(this)));
        std::filesystem::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }

    std::string file(const std::string& name) const { return (path / name).string(); }
};

CoreApi::Options options_for(const TempDir& dir) {
    CoreApi::Options options;
    options.paths.config_file = dir.file("config.toml");
    options.paths.data_file = dir.file("ldns.db");
    return options;
}

}  // namespace

TEST_CASE("CLI add with an explicit target stores a static record") {
    TempDir dir;
    auto api = CoreApi::create(options_for(dir));
    REQUIRE(api.has_value());

    cli::AddOptions options;
    options.name = "app.lan";
    options.targets = {"10.0.0.5"};
    options.type = "A";
    options.ttl = 120;
    options.force = true;

    const auto added = cli::add(*api.value(), options);
    REQUIRE(added.has_value());
    CHECK(added.value().name == "app.lan");
    CHECK(added.value().target_kind == "static");
    CHECK(added.value().ttl == 120);
    REQUIRE(added.value().targets.size() == 1);
    CHECK(added.value().targets[0] == "10.0.0.5");
}

TEST_CASE("CLI add without targets uses the self address") {
    TempDir dir;
    auto api = CoreApi::create(options_for(dir));
    REQUIRE(api.has_value());

    cli::AddOptions options;
    options.name = "me.lan";
    options.targets = {};

    const auto added = cli::add(*api.value(), options);
    // A CI machine with no LAN address has nothing to publish; that is an
    // error from the caller's point of view (602: no LAN address, 708: no matching type),
    // not a crash.
    if (!added) {
        CHECK((added.error().code == 602 || added.error().code == 708));
        return;
    }
    CHECK(added.value().target_kind == "self");
    CHECK_FALSE(added.value().targets.empty());
}

TEST_CASE("CLI add without targets uses the self address (deterministic pinned)") {
    TempDir dir;
    auto opts = options_for(dir);
    opts.config.network.self_ip = "192.168.1.50";
    auto api = CoreApi::create(std::move(opts));
    REQUIRE(api.has_value());

    cli::AddOptions options;
    options.name = "me.lan";
    options.targets = {};

    const auto added = cli::add(*api.value(), options);
    REQUIRE(added.has_value());
    CHECK(added.value().target_kind == "self");
    REQUIRE(added.value().targets.size() == 1);
    CHECK(added.value().targets[0] == "192.168.1.50");
}

TEST_CASE("CLI set replaces targets and rm removes the record") {
    TempDir dir;
    auto api = CoreApi::create(options_for(dir));
    REQUIRE(api.has_value());

    cli::AddOptions add_request;
    add_request.name = "app.lan";
    add_request.targets = {"10.0.0.5"};
    add_request.force = true;
    REQUIRE(cli::add(*api.value(), add_request).has_value());

    cli::AddOptions set_request;
    set_request.name = "app.lan";
    set_request.targets = {"10.0.0.6", "10.0.0.7"};
    set_request.force = true;
    const auto updated = cli::set(*api.value(), set_request);
    REQUIRE(updated.has_value());
    CHECK(updated.value().targets.size() == 2);

    REQUIRE(cli::remove(*api.value(), "app.lan", "A").has_value());
    CHECK_FALSE(cli::remove(*api.value(), "app.lan", "A").has_value());
}

TEST_CASE("CLI exit codes map conflicts and usage errors separately") {
    CHECK(cli::exit_code_for(Error{703, "exists"}) == cli::kExitConflict);
    CHECK(cli::exit_code_for(Error{705, "needs force"}) == cli::kExitConflict);
    CHECK(cli::exit_code_for(Error{700, "bad name"}) == cli::kExitUsage);
    CHECK(cli::exit_code_for(Error{701, "bad type"}) == cli::kExitUsage);
    CHECK(cli::exit_code_for(Error{707, "store"}) == cli::kExitError);
    CHECK(cli::exit_code_for(Error{708, "no self"}) == cli::kExitError);
}

TEST_CASE("CLI export writes TOML that imports back into another node") {
    TempDir source;
    auto source_api = CoreApi::create(options_for(source));
    REQUIRE(source_api.has_value());

    cli::AddOptions options;
    options.name = "app.lan";
    options.targets = {"10.0.0.5", "10.0.0.6"};
    options.force = true;
    REQUIRE(cli::add(*source_api.value(), options).has_value());

    cli::ExportOptions export_options;
    export_options.path = source.file("zone.toml");
    REQUIRE(cli::export_zone(*source_api.value(), export_options).has_value());
    REQUIRE(std::filesystem::exists(source.file("zone.toml")));

    TempDir target;
    auto target_api = CoreApi::create(options_for(target));
    REQUIRE(target_api.has_value());

    cli::ImportOptions import_options;
    import_options.path = source.file("zone.toml");
    import_options.force = true;
    const auto report = cli::import_file(*target_api.value(), import_options);
    REQUIRE(report.has_value());
    CHECK(report.value().imported == 1);

    const auto entries = target_api.value()->list_domains();
    REQUIRE(entries.has_value());
    REQUIRE(entries.value().size() == 1);
    CHECK(entries.value()[0].targets.size() == 2);
}

TEST_CASE("listen entries keep an explicit port and default only when absent") {
    // Regression: resolve_listen used to append ":53" unconditionally, which
    // turned "127.0.0.1:0" into "127.0.0.1:0:53" and made the server fail to
    // bind. Any address with a port must pass through untouched.
    TempDir dir;
    RuntimeOptions options;
    options.core = options_for(dir);
    // Port 0 keeps the test unprivileged and avoids clashing with a real
    // resolver; the point is that the entry is not corrupted.
    options.listen = {"127.0.0.1:0", "127.0.0.1:0"};
    auto runtime = Runtime::create(std::move(options));
    REQUIRE(runtime.has_value());
    REQUIRE(runtime.value()->start().has_value());
    const auto& bound = runtime.value()->bound_addresses();
    REQUIRE(bound.size() == 2);
    for (const auto& address : bound) {
        CHECK(address.starts_with("127.0.0.1:"));
        CHECK(address != "127.0.0.1:0:53");
    }
}

TEST_CASE("Runtime binds loopback and answers a query from the zone") {
    TempDir dir;
    RuntimeOptions options;
    options.core = options_for(dir);
    // 0 asks the OS for a free port, which keeps the test unprivileged.
    options.listen = {"127.0.0.1:0"};

    auto runtime = Runtime::create(std::move(options));
    REQUIRE(runtime.has_value());

    cli::AddOptions add_request;
    add_request.name = "app.lan";
    add_request.targets = {"10.0.0.5"};
    add_request.force = true;
    REQUIRE(cli::add(runtime.value()->api(), add_request).has_value());

    REQUIRE(runtime.value()->start().has_value());
    REQUIRE(runtime.value()->bound_addresses().size() == 1);

    // Exercise the zone through the same handler the server uses; the socket
    // path itself is covered by test_dns_server.cpp.
    const auto name = dns::Name::from_presentation("app.lan");
    REQUIRE(name.has_value());
    const auto outcome = runtime.value()->api().zone().lookup(name.value(), 1);
    CHECK(outcome.kind == dns::LookupKind::Answer);
    CHECK(outcome.answers.size() == 1);
    CHECK(outcome.answers[0].ttl == 60);
}

TEST_CASE("CLI resolve answers local domain") {
    TempDir dir;
    auto api = CoreApi::create(options_for(dir));
    REQUIRE(api.has_value());

    cli::AddOptions add_opt;
    add_opt.name = "web.lan";
    add_opt.targets = {"192.168.1.100"};
    add_opt.force = true;
    REQUIRE(cli::add(*api.value(), add_opt).has_value());

    std::ostringstream ss;
    cli::ResolveOptions res_opt;
    res_opt.name = "web.lan";
    res_opt.type = "A";
    auto result = cli::resolve(*api.value(), res_opt, ss);
    REQUIRE(result.has_value());
    CHECK(result.value().resolved_step == resolver::ResolutionStep::Local);
    CHECK(ss.str().find("Resolved via Local") != std::string::npos);
    CHECK(ss.str().find("192.168.1.100") != std::string::npos);
}

TEST_CASE("CLI resolve supports --json and --trace") {
    TempDir dir;
    auto api = CoreApi::create(options_for(dir));
    REQUIRE(api.has_value());

    cli::AddOptions add_opt;
    add_opt.name = "db.lan";
    add_opt.targets = {"10.0.0.1"};
    add_opt.force = true;
    REQUIRE(cli::add(*api.value(), add_opt).has_value());

    std::ostringstream ss;
    cli::ResolveOptions res_opt;
    res_opt.name = "db.lan";
    res_opt.type = "A";
    res_opt.trace = true;
    res_opt.json = true;
    auto result = cli::resolve(*api.value(), res_opt, ss);
    REQUIRE(result.has_value());

    auto parsed_json = nlohmann::json::parse(ss.str(), nullptr, false);
    REQUIRE_FALSE(parsed_json.is_discarded());
    CHECK(parsed_json["qname"] == "db.lan");
    CHECK(parsed_json["step"] == "Local");
    CHECK(parsed_json["rcode"] == "NOERROR");
    REQUIRE(parsed_json["answers"].is_array());
    CHECK(parsed_json["answers"].size() == 1);
    CHECK(parsed_json["answers"][0]["data"] == "10.0.0.1");
    REQUIRE(parsed_json["trace"].is_array());
    CHECK_FALSE(parsed_json["trace"].empty());
}

TEST_CASE("CLI peer add, list, rm, forget") {
    TempDir dir;
    auto api = CoreApi::create(options_for(dir));
    REQUIRE(api.has_value());

    std::ostringstream ss;
    cli::PeersOptions p_opt;
    auto list_empty = cli::peers(*api.value(), p_opt, ss);
    REQUIRE(list_empty.has_value());
    CHECK(ss.str().find("no peers") != std::string::npos);

    // 1. Add manual peer
    ss.str("");
    cli::PeerAddOptions add_opt;
    add_opt.endpoint = "10.0.0.2:5391";
    REQUIRE(cli::peer_add(*api.value(), add_opt, ss).has_value());
    CHECK(ss.str().find("added peer 10.0.0.2:5391") != std::string::npos);

    // 2. List peers - text and JSON
    ss.str("");
    REQUIRE(cli::peers(*api.value(), p_opt, ss).has_value());
    CHECK(ss.str().find("10.0.0.2:5391") != std::string::npos);
    CHECK(ss.str().find("manual") != std::string::npos);

    ss.str("");
    p_opt.json = true;
    REQUIRE(cli::peers(*api.value(), p_opt, ss).has_value());
    auto pj = nlohmann::json::parse(ss.str(), nullptr, false);
    REQUIRE_FALSE(pj.is_discarded());
    REQUIRE(pj.is_array());
    CHECK(pj.size() == 1);
    CHECK(pj[0]["address"] == "10.0.0.2:5391");
    CHECK(pj[0]["manual"] == true);

    // 3. Put a discovered peer directly into store
    StoredPeer sp;
    sp.node_id.fill(0xAA);
    sp.name = "node-alpha";
    sp.epoch = 1;
    sp.seq = 42;
    sp.last_addr = "10.0.0.3:5391";
    sp.last_seen = 1000;
    REQUIRE(api.value()->put_peer(sp).has_value());

    p_opt.json = false;
    ss.str("");
    REQUIRE(cli::peers(*api.value(), p_opt, ss).has_value());
    CHECK(ss.str().find("node-alpha") != std::string::npos);
    CHECK(ss.str().find("10.0.0.3:5391") != std::string::npos);

    // 4. Forget peer
    ss.str("");
    cli::PeerForgetOptions fg_opt;
    fg_opt.node = "aaaa";
    REQUIRE(cli::peer_forget(*api.value(), fg_opt, ss).has_value());
    CHECK(ss.str().find("forgot peer") != std::string::npos);

    // 5. Remove manual peer
    ss.str("");
    cli::PeerRmOptions rm_opt;
    rm_opt.endpoint = "10.0.0.2:5391";
    REQUIRE(cli::peer_rm(*api.value(), rm_opt, ss).has_value());
    CHECK(ss.str().find("removed peer 10.0.0.2:5391") != std::string::npos);

    ss.str("");
    REQUIRE(cli::peers(*api.value(), p_opt, ss).has_value());
    CHECK(ss.str().find("no peers") != std::string::npos);
}

TEST_CASE("CLI conflicts and info commands") {
    TempDir dir;
    auto api = CoreApi::create(options_for(dir));
    REQUIRE(api.has_value());

    std::stringstream ss;

    // 1. Initially no conflicts
    cli::ConflictsOptions c_opt;
    c_opt.json = false;
    REQUIRE(cli::conflicts(*api.value(), c_opt, ss).has_value());
    CHECK(ss.str().find("no active conflicts") != std::string::npos);

    ss.str("");
    c_opt.json = true;
    REQUIRE(cli::conflicts(*api.value(), c_opt, ss).has_value());
    auto cj = nlohmann::json::parse(ss.str(), nullptr, false);
    REQUIRE_FALSE(cj.is_discarded());
    REQUIRE(cj.is_array());
    CHECK(cj.empty());

    // 2. Info on empty domain
    ss.str("");
    cli::InfoOptions i_opt;
    i_opt.name = "demo.lan";
    i_opt.type = "A";
    i_opt.json = false;
    REQUIRE(cli::info(*api.value(), i_opt, ss).has_value());
    CHECK(ss.str().find("Domain: demo.lan (A)") != std::string::npos);
    CHECK(ss.str().find("Local Zone:\n  (none)") != std::string::npos);
    CHECK(ss.str().find("Conflict: None") != std::string::npos);

    // 3. Add local record and check info again
    cli::AddOptions add_opt;
    add_opt.name = "demo.lan";
    add_opt.targets = {"10.0.0.1"};
    add_opt.type = "A";
    add_opt.force = true;
    REQUIRE(cli::add(*api.value(), add_opt).has_value());

    ss.str("");
    REQUIRE(cli::info(*api.value(), i_opt, ss).has_value());
    CHECK(ss.str().find("Local Zone:\n  - 10.0.0.1") != std::string::npos);

    // 4. Add peer claims creating conflict in registry for conflict.lan
    NodeId peer1{};
    peer1.fill(0x11);
    NodeId peer2{};
    peer2.fill(0x22);

    api.value()->registry().set_owner_online(peer1, true, "peer-1", 100, 1, 0);
    api.value()->registry().set_owner_online(peer2, true, "peer-2", 100, 1, 0);

    net::Op op1{"put", "conflict.lan", 1, 60, {{10, 0, 0, 1}}, 100, 100};
    net::Op op2{"put", "conflict.lan", 1, 60, {{10, 0, 0, 2}}, 200, 200};
    REQUIRE(api.value()->registry().apply_ops(peer1, 1, 1, {op1}).has_value());
    REQUIRE(api.value()->registry().apply_ops(peer2, 1, 1, {op2}).has_value());

    // Info shows 2 peer groups and conflict detected
    ss.str("");
    i_opt.name = "conflict.lan";
    i_opt.json = false;
    REQUIRE(cli::info(*api.value(), i_opt, ss).has_value());
    CHECK(ss.str().find("Group 1") != std::string::npos);
    CHECK(ss.str().find("Group 2") != std::string::npos);
    CHECK(ss.str().find("Multiple competing peer groups") != std::string::npos);

    // Info JSON format
    ss.str("");
    i_opt.json = true;
    REQUIRE(cli::info(*api.value(), i_opt, ss).has_value());
    auto ij = nlohmann::json::parse(ss.str(), nullptr, false);
    REQUIRE_FALSE(ij.is_discarded());
    CHECK(ij["name"] == "conflict.lan");
    CHECK(ij["type"] == "A");
    CHECK(ij["has_local"] == false);
    CHECK(ij["has_conflict"] == true);
    REQUIRE(ij["groups"].is_array());
    CHECK(ij["groups"].size() == 2);

    // Trigger conflict recording via resolution
    (void)api.value()->resolve("conflict.lan", "A");

    // Check conflicts command now sees the active conflict
    ss.str("");
    c_opt.json = false;
    auto confs = cli::conflicts(*api.value(), c_opt, ss);
    REQUIRE(confs.has_value());
    CHECK(confs.value().size() == 1);
    CHECK(ss.str().find("Active conflicts (1)") != std::string::npos);
    CHECK(ss.str().find("conflict.lan") != std::string::npos);

    ss.str("");
    c_opt.json = true;
    REQUIRE(cli::conflicts(*api.value(), c_opt, ss).has_value());
    auto cj2 = nlohmann::json::parse(ss.str(), nullptr, false);
    REQUIRE_FALSE(cj2.is_discarded());
    REQUIRE(cj2.size() == 1);
    CHECK(cj2[0]["name"] == "conflict.lan");
    CHECK(cj2[0]["type"] == "A");
}

TEST_CASE("CLI network key generate, set, status, clear") {
    TempDir dir;
    config::Paths paths;
    paths.config_file = (dir.path / "config.toml").string();
    paths.data_file = (dir.path / "ldns.db").string();

    std::ostringstream ss;

    // 1. Initial status: not configured
    cli::NetworkKeyOptions status_opt;
    status_opt.action = "status";
    status_opt.json = true;
    REQUIRE(cli::network_key_cmd(paths, status_opt, ss).has_value());
    auto j1 = nlohmann::json::parse(ss.str(), nullptr, false);
    REQUIRE_FALSE(j1.is_discarded());
    CHECK(j1["configured"] == false);
    CHECK(j1["found"] == false);

    // 2. Generate key
    ss.str("");
    std::string key_path = (dir.path / "test.key").string();
    cli::NetworkKeyOptions gen_opt;
    gen_opt.action = "generate";
    gen_opt.path = key_path;
    gen_opt.json = true;
    REQUIRE(cli::network_key_cmd(paths, gen_opt, ss).has_value());
    auto j_gen = nlohmann::json::parse(ss.str(), nullptr, false);
    REQUIRE_FALSE(j_gen.is_discarded());
    CHECK(j_gen["status"] == "ok");
    CHECK(std::filesystem::exists(key_path));

    // Verify key file has 64 hex characters + newline
    std::ifstream kf(key_path);
    std::string hex_content;
    std::getline(kf, hex_content);
    CHECK(hex_content.size() == 64);

    // 3. Set key in config
    ss.str("");
    cli::NetworkKeyOptions set_opt;
    set_opt.action = "set";
    set_opt.path = key_path;
    set_opt.json = true;
    REQUIRE(cli::network_key_cmd(paths, set_opt, ss).has_value());
    auto j_set = nlohmann::json::parse(ss.str(), nullptr, false);
    REQUIRE_FALSE(j_set.is_discarded());
    CHECK(j_set["status"] == "ok");

    // 4. Status now reports configured and found
    ss.str("");
    REQUIRE(cli::network_key_cmd(paths, status_opt, ss).has_value());
    auto j2 = nlohmann::json::parse(ss.str(), nullptr, false);
    REQUIRE_FALSE(j2.is_discarded());
    CHECK(j2["configured"] == true);
    CHECK(j2["found"] == true);

    // 5. Clear key
    ss.str("");
    cli::NetworkKeyOptions clear_opt;
    clear_opt.action = "clear";
    clear_opt.json = true;
    REQUIRE(cli::network_key_cmd(paths, clear_opt, ss).has_value());
    auto j_clear = nlohmann::json::parse(ss.str(), nullptr, false);
    REQUIRE_FALSE(j_clear.is_discarded());
    CHECK(j_clear["status"] == "ok");

    // 6. Status after clear
    ss.str("");
    REQUIRE(cli::network_key_cmd(paths, status_opt, ss).has_value());
    auto j3 = nlohmann::json::parse(ss.str(), nullptr, false);
    REQUIRE_FALSE(j3.is_discarded());
    CHECK(j3["configured"] == false);
}
