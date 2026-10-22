#pragma once

#include <string>

namespace ldns {

// Semantic version of this build (from CMake project version).
std::string version_string();

// Protocol version spoken with peers (docs/protocol.md §2.2). Bumping the
// major version is a breaking change; minor additions are compatible.
constexpr int kProtocolMajor = 1;
constexpr int kProtocolMinor = 0;

}  // namespace ldns
