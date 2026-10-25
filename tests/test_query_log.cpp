#include "doctest.hpp"

#include "core/resolver/query_log.hpp"

using namespace ldns::resolver;

TEST_CASE("QueryLog ring buffer records and retrieves latest entries") {
    QueryLog log(3);  // max 3 items

    CHECK(log.size() == 0);
    CHECK(log.entries().empty());

    log.record(QueryLogEntry{1000, "127.0.0.1", "one.lan", "A", ResolutionStep::Local, "NOERROR", 100, ""});
    log.record(QueryLogEntry{1001, "127.0.0.1", "two.lan", "A", ResolutionStep::Cache, "NOERROR", 50, ""});
    log.record(QueryLogEntry{1002, "127.0.0.1", "three.lan", "A", ResolutionStep::Upstream, "NOERROR", 15000, "1.1.1.1"});

    CHECK(log.size() == 3);

    auto list = log.entries(10);
    REQUIRE(list.size() == 3);
    // Latest first
    CHECK(list[0].qname == "three.lan");
    CHECK(list[1].qname == "two.lan");
    CHECK(list[2].qname == "one.lan");

    // Add fourth item: evicts oldest ("one.lan")
    log.record(QueryLogEntry{1003, "127.0.0.1", "four.lan", "A", ResolutionStep::Local, "NXDOMAIN", 200, ""});
    CHECK(log.size() == 3);

    list = log.entries(2);
    REQUIRE(list.size() == 2);
    CHECK(list[0].qname == "four.lan");
    CHECK(list[1].qname == "three.lan");
}
