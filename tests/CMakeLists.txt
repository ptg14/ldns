find_package(doctest CONFIG REQUIRED)

set(TEST_SOURCES
    test_main.cpp
    test_id.cpp
    test_hlc.cpp
    test_result.cpp
    test_name.cpp
    test_message.cpp
    test_local_zone.cpp
    test_acl.cpp
    test_query_handler.cpp
    test_dns_server.cpp
    test_store.cpp
    test_config.cpp
    test_domain_service.cpp
    test_core_api.cpp
    test_cli.cpp
    test_cache.cpp
    test_query_log.cpp
    test_system_dns.cpp
    test_upstream_client.cpp
    test_pipeline.cpp
    test_beacon.cpp
    test_discovery.cpp
    test_protocol_message.cpp
    test_session.cpp
    test_peer_manager.cpp
    test_registry.cpp
    test_sync.cpp
    test_peer_ranking.cpp
    test_peer_resolver.cpp
    test_peer_e2e.cpp
    test_ipc.cpp
    test_os.cpp
    test_crypto.cpp
)

if(LDNS_BUILD_UI)
    list(APPEND TEST_SOURCES test_ui.cpp)
endif()

add_executable(ldns_tests ${TEST_SOURCES})
if(LDNS_BUILD_UI)
    target_link_libraries(ldns_tests PRIVATE ldns_core ldns_ui doctest::doctest)
else()
    target_link_libraries(ldns_tests PRIVATE ldns_core doctest::doctest)
endif()
ldns_set_warnings(ldns_tests)

add_test(NAME ldns_tests COMMAND ldns_tests)

# Manual M2 benchmark (not part of ctest): 100k-row import and reopen time.
add_executable(ldns_bench_import bench_import.cpp)
target_link_libraries(ldns_bench_import PRIVATE ldns_core)
ldns_set_warnings(ldns_bench_import)
