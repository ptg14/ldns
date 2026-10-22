#include "core/version.hpp"

#ifndef LDNS_VERSION
#define LDNS_VERSION "0.0.0-unknown"
#endif

namespace ldns {

std::string version_string() { return LDNS_VERSION; }

}  // namespace ldns
