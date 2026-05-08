#pragma once

#include <string>

namespace beacon {
namespace internal {

// Captures a platform-specific stack trace string.
// Returns empty string if capture fails or is unavailable.
// Result is capped at 32768 characters.
std::string capture_stack_trace();

// Demangles a C++ type name (from typeid().name()).
// On GCC/Clang uses abi::__cxa_demangle; on MSVC returns as-is.
std::string demangle_type_name(const char* mangled_name);

} // namespace internal
} // namespace beacon
