#include "doctest.hpp"

#include "core/util/result.hpp"

#include <string>

using namespace ldns;

TEST_CASE("Result<T> holds a value") {
    Result<int> r{42};
    CHECK(r.has_value());
    CHECK(static_cast<bool>(r));
    CHECK(r.value() == 42);
}

TEST_CASE("Result<T> holds an error") {
    Result<int> r{Error{3, "bad"}};
    CHECK_FALSE(r.has_value());
    CHECK_FALSE(static_cast<bool>(r));
    CHECK(r.error().code == 3);
    CHECK(r.error().message == "bad");
}

TEST_CASE("Result<T>::value_or falls back on error") {
    Result<std::string> ok{std::string("x")};
    Result<std::string> bad{Error{1, "e"}};
    CHECK(ok.value_or("fallback") == "x");
    CHECK(bad.value_or("fallback") == "fallback");
}

TEST_CASE("Result<void> reports success and failure") {
    Result<void> ok;
    Result<void> bad{Error{2, "nope"}};
    CHECK(ok.has_value());
    CHECK_FALSE(bad.has_value());
    CHECK(bad.error().code == 2);
    CHECK_NOTHROW(ok.value());
    CHECK_THROWS(bad.value());
}
