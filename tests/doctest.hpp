#pragma once

// Wrapper around doctest.
//
// MSVC 14.44 + doctest 2.4.x: doctest's StringMaker<std::string_view>
// instantiates std::operator<<(ostream&, string_view), which requires the
// stream headers to be complete. Include them first so every test file gets a
// consistent, working configuration.

#include <ostream>
#include <sstream>
#include <string>
#include <string_view>

#include <doctest/doctest.h>
