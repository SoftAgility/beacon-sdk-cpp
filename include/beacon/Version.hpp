#pragma once

// SoftAgility Beacon C++ SDK — version metadata.
//
// These macros and the beacon::version() function return the SDK version at
// compile time and run time respectively. They are useful for:
//   - Conditional compilation against an older SDK version (e.g.
//     #if BEACON_VERSION_MAJOR >= 2 ...)
//   - Displaying the SDK version in your application's About dialog
//   - Surfacing the SDK version in customer support tickets
//
// The values are kept in sync with the project(beacon_sdk VERSION ...)
// declaration in CMakeLists.txt by hand. A drift-lock test in the CI matrix
// will fail if they diverge.

#define BEACON_VERSION_MAJOR  1
#define BEACON_VERSION_MINOR  0
#define BEACON_VERSION_PATCH  0
#define BEACON_VERSION_STRING "1.0.0"

namespace beacon {

/// Returns the SDK version as a null-terminated string in MAJOR.MINOR.PATCH form
/// (matching the BEACON_VERSION_STRING macro). Useful for runtime logging.
inline const char* version() noexcept { return BEACON_VERSION_STRING; }

} // namespace beacon
