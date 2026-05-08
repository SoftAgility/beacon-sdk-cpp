#pragma once

#include <string>

namespace beacon {
namespace internal {

// Collects environment data and returns it as a JSON string.
std::string collect_environment_json();

// SHA-256 hash of a string, returned as lowercase hex.
std::string sha256_hex(const std::string& input);

// Returns the RAM bucket string based on total RAM in MB.
std::string ram_bucket(uint64_t total_ram_mb);

} // namespace internal
} // namespace beacon
