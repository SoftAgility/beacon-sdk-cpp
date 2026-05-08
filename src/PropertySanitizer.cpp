#include "PropertySanitizer.hpp"

#include <algorithm>

namespace beacon {
namespace internal {

std::vector<std::pair<std::string, std::string>> sanitize_properties(
    const std::unordered_map<std::string, std::string>& properties) {

    std::vector<std::pair<std::string, std::string>> result;
    result.reserve(std::min(properties.size(), static_cast<size_t>(20)));

    int count = 0;
    for (const auto& [key, value] : properties) {
        if (count >= 20) break;
        if (value.empty()) continue;

        std::string sanitized_key = key.substr(0, 64);
        std::string sanitized_value = value.substr(0, 256);

        result.emplace_back(std::move(sanitized_key), std::move(sanitized_value));
        ++count;
    }

    return result;
}

} // namespace internal
} // namespace beacon
