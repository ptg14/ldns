find_package(doctest CONFIG REQUIRED)

add_executable(ldns_tests
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
    test_ui.cpp
)
target_link_libraries(ldns_tests PRIVATE ldns_core ldns_ui doctest::doctest)
ldns_set_warnings(ldns_tests)

add_test(NAME ldns_tests COMMAND ldns_tests)
