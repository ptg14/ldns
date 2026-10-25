#include "doctest.hpp"

#include "core/store/store.hpp"

#include <filesystem>
#include <string>

using namespace ldns;

namespace {

StoreOptions memory_options() {
    StoreOptions options;
    options.path = ":memory:";
    return options;
}

RecordRow make_row(std::string name, std::uint16_t type, std::vector<std::string> values) {
    RecordRow row;
    row.name = std::move(name);
    row.type = type;
    row.ttl = 60;
    row.target_kind = "static";
    auto encoded = Store::encode_data({});
    (void)encoded;
    row.created = 100;
    row.updated = 100;
    for (auto& value : values) {
        row.data.push_back(std::vector<std::uint8_t>(value.begin(), value.end()));
    }
    return row;
}

}  // namespace

TEST_CASE("Store opens in memory and migrates to the current schema") {
    auto store = Store::open(memory_options());
    REQUIRE(store.has_value());

    // A second open of the same connection must not re-run migrations.
    auto meta = store.value().meta("schema_version");
    REQUIRE(meta.has_value());
    CHECK_FALSE(meta.value().has_value());  // identity() has not run yet

    auto identity = store.value().identity();
    REQUIRE(identity.has_value());
    CHECK(identity.value().seq == 0);

    auto stored = store.value().meta("schema_version");
    REQUIRE(stored.has_value());
    CHECK(stored.value().value() == "1");
}

TEST_CASE("identity is stable across reopens of the same file") {
    const auto path = std::filesystem::temp_directory_path() / "ldns-test-identity.db";
    std::filesystem::remove(path);
    std::filesystem::remove(path.string() + "-wal");
    std::filesystem::remove(path.string() + "-shm");

    NodeId first_id{};
    std::uint64_t first_epoch = 0;
    {
        StoreOptions options;
        options.path = path.string();
        auto store = Store::open(options);
        REQUIRE(store.has_value());
        auto identity = store.value().identity();
        REQUIRE(identity.has_value());
        first_id = identity.value().node_id;
        first_epoch = identity.value().epoch;
        CHECK_FALSE(is_zero(first_id));

        auto tx = store.value().begin();
        REQUIRE(tx.has_value());
        auto seq = store.value().bump_seq();
        REQUIRE(seq.has_value());
        CHECK(seq.value() == 1);
        REQUIRE(tx.value().commit().has_value());
    }

    {
        StoreOptions options;
        options.path = path.string();
        auto store = Store::open(options);
        REQUIRE(store.has_value());
        auto identity = store.value().identity();
        REQUIRE(identity.has_value());
        CHECK(identity.value().node_id == first_id);
        CHECK(identity.value().epoch == first_epoch);
        CHECK(identity.value().seq == 1);
    }

    std::filesystem::remove(path);
    std::filesystem::remove(path.string() + "-wal");
    std::filesystem::remove(path.string() + "-shm");
}

TEST_CASE("bump_seq requires a transaction and increments monotonically") {
    auto store = Store::open(memory_options());
    REQUIRE(store.has_value());
    REQUIRE(store.value().identity().has_value());

    CHECK_FALSE(store.value().bump_seq().has_value());

    for (int i = 1; i <= 3; ++i) {
        auto tx = store.value().begin();
        REQUIRE(tx.has_value());
        auto seq = store.value().bump_seq();
        REQUIRE(seq.has_value());
        CHECK(seq.value() == static_cast<std::uint64_t>(i));
        REQUIRE(tx.value().commit().has_value());
    }
}

TEST_CASE("nested transactions are rejected and rollback discards writes") {
    auto store = Store::open(memory_options());
    REQUIRE(store.has_value());
    REQUIRE(store.value().identity().has_value());

    {
        auto tx = store.value().begin();
        REQUIRE(tx.has_value());
        CHECK_FALSE(store.value().begin().has_value());

        RecordRow row = make_row("app.lan", 1, {"192.168.1.10"});
        row.data = {std::vector<std::uint8_t>{192, 168, 1, 10}};
        REQUIRE(store.value().put_record(row).has_value());
        // Tx destructs without commit: rollback.
    }

    auto rows = store.value().records();
    REQUIRE(rows.has_value());
    CHECK(rows.value().empty());
}

TEST_CASE("put_record replaces and delete_record reports whether it removed") {
    auto store = Store::open(memory_options());
    REQUIRE(store.has_value());
    REQUIRE(store.value().identity().has_value());

    RecordRow row;
    row.name = "app.lan";
    row.type = 1;
    row.ttl = 60;
    row.target_kind = "self";
    row.data = {std::vector<std::uint8_t>{192, 168, 1, 10}};
    row.created = 5;
    row.updated = 5;
    REQUIRE(store.value().put_record(row).has_value());

    row.target_kind = "static";
    row.ttl = 120;
    row.data = {std::vector<std::uint8_t>{192, 168, 1, 11},
                std::vector<std::uint8_t>{192, 168, 1, 12}};
    row.updated = 9;
    REQUIRE(store.value().put_record(row).has_value());

    auto rows = store.value().records();
    REQUIRE(rows.has_value());
    REQUIRE(rows.value().size() == 1);
    CHECK(rows.value()[0].ttl == 120);
    CHECK(rows.value()[0].target_kind == "static");
    CHECK(rows.value()[0].created == 5);  // created_at survives updates
    CHECK(rows.value()[0].updated == 9);
    CHECK(rows.value()[0].data.size() == 2);

    CHECK(store.value().delete_record("app.lan", 1).value());
    CHECK_FALSE(store.value().delete_record("app.lan", 1).value());
}

TEST_CASE("data_json round-trips binary values including TXT with NUL bytes") {
    const std::vector<std::vector<std::uint8_t>> values = {
        {0x00, 0x01, 0xFF},
        {},
        {0xDE, 0xAD, 0xBE, 0xEF},
    };
    auto encoded = Store::encode_data(values);
    REQUIRE(encoded.has_value());
    auto decoded = Store::decode_data(encoded.value());
    REQUIRE(decoded.has_value());
    CHECK(decoded.value() == values);
}

TEST_CASE("decode_data rejects corrupt or oversized JSON") {
    CHECK_FALSE(Store::decode_data("not json").has_value());
    CHECK_FALSE(Store::decode_data("[1]").has_value());
    CHECK_FALSE(Store::decode_data("[\"abc\"]").has_value());   // odd hex length
    CHECK_FALSE(Store::decode_data("[\"zz\"]").has_value());    // non-hex
    CHECK_FALSE(Store::decode_data("{\"a\":1}").has_value());   // not an array
}

TEST_CASE("upstreams replace atomically and preserve order") {
    auto store = Store::open(memory_options());
    REQUIRE(store.has_value());

    std::vector<UpstreamRow> rows = {
        {"1.1.1.1", 53, "udp"},
        {"8.8.8.8", 5353, "tcp"},
    };
    REQUIRE(store.value().replace_upstreams(rows).has_value());

    auto loaded = store.value().upstreams();
    REQUIRE(loaded.has_value());
    REQUIRE(loaded.value().size() == 2);
    CHECK(loaded.value()[0].address == "1.1.1.1");
    CHECK(loaded.value()[1].port == 5353);
    CHECK(loaded.value()[1].proto == "tcp");

    REQUIRE(store.value().replace_upstreams({}).has_value());
    CHECK(store.value().upstreams().value().empty());
}

TEST_CASE("opening a directory path fails cleanly") {
    StoreOptions options;
    options.path = std::filesystem::temp_directory_path().string();
    auto store = Store::open(options);
    CHECK_FALSE(store.has_value());
}
