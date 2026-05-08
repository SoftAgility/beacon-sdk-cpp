#pragma once

#include <string>

namespace beacon {
namespace internal {

// Generate a UUID v7 (time-ordered) per RFC 9562.
std::string new_uuid_v7();

} // namespace internal
} // namespace beacon
