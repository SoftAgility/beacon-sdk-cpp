#pragma once

#include <string>
#include <unordered_map>
#include <vector>

namespace beacon {
namespace internal {

// Sanitizes event properties:
// - Removes entries with empty values
// - Truncates keys to 64 chars, values to 256 chars
// - Keeps at most 20 entries (by insertion order preserved in the returned vector)
std::vector<std::pair<std::string, std::string>> sanitize_properties(
    const std::unordered_map<std::string, std::string>& properties);

} // namespace internal
} // namespace beacon
