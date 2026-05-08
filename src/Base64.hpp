#pragma once

#include <string>

namespace beacon {
namespace internal {

std::string base64_encode(const std::string& input);
std::string base64_decode(const std::string& input);

} // namespace internal
} // namespace beacon
