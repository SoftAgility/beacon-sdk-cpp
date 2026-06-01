#pragma once

#include <string>

namespace beacon {
namespace internal {

// Returns a stable device ID for the given product.
// Creates the ID file if it doesn't exist.
std::string get_or_create_device_id(const std::string& product);

// Writes a new device ID to the device_id.txt file for the given product.
// Used by reset() to persist a new anonymous identity.
// Throws on I/O failure (caller should catch and handle).
void write_device_id(const std::string& product, const std::string& uuid);

// Returns the platform-specific data directory path for the given product.
// Returns empty string if the path cannot be determined.
std::string get_data_directory(const std::string& product);

// Sanitizes product for use in file paths (replaces unsafe chars with _).
std::string sanitize_path_component(const std::string& input);

} // namespace internal
} // namespace beacon
