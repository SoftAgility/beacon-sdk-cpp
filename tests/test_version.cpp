// Covers: version metadata drift between Version.hpp and CMakeLists.txt.
//
// WHY THIS EXISTS. Version.hpp long carried a comment promising that "a drift-lock test in
// the CI matrix will fail if they diverge". No such test existed. The version therefore lived
// in four hand-edited places - project(... VERSION), SOVERSION, the BEACON_VERSION_* macros,
// and BEACON_VERSION_STRING - with nothing checking that a bump touched all of them.
//
// The failure that guards against is quiet. BEACON_VERSION_STRING is baked into the
// User-Agent of every request (HttpClient.cpp), and beacon::version() is what a customer
// reads out into a support ticket. A stale macro means the telemetry backend and the support
// conversation both name a version that was never shipped, and the people debugging it have
// no reason to doubt either source.

#include <gtest/gtest.h>
#include "beacon/Version.hpp"
#include <string>

#ifndef BEACON_CMAKE_PROJECT_VERSION
#error "BEACON_CMAKE_PROJECT_VERSION not defined - see tests/CMakeLists.txt"
#endif

// The string macro must match what CMake built with, exactly.
TEST(VersionTest, StringMatchesCMakeProjectVersion) {
    EXPECT_EQ(std::string(BEACON_VERSION_STRING),
              std::string(BEACON_CMAKE_PROJECT_VERSION))
        << "Version.hpp and CMakeLists.txt disagree. Both must be bumped together.";
}

// The components must compose back into the string, or a bump that edited one and not the
// other would leave beacon::version() and the #if-based feature checks disagreeing.
TEST(VersionTest, ComponentsComposeIntoTheString) {
    const std::string composed = std::to_string(BEACON_VERSION_MAJOR) + "." +
                                 std::to_string(BEACON_VERSION_MINOR) + "." +
                                 std::to_string(BEACON_VERSION_PATCH);

    EXPECT_EQ(composed, std::string(BEACON_VERSION_STRING))
        << "BEACON_VERSION_MAJOR/MINOR/PATCH do not compose into BEACON_VERSION_STRING.";
}

// The runtime accessor is what ends up in a support ticket; it must not drift from the macro.
TEST(VersionTest, RuntimeAccessorMatchesTheMacro) {
    EXPECT_EQ(std::string(beacon::version()), std::string(BEACON_VERSION_STRING));
}

// SOVERSION gates binary compatibility. This library exposes Tracker and Options directly
// with no pImpl, so adding a member to either is an ABI break - which is what forced 4.0.0
// and again 5.0.0. Tying SOVERSION to the major version makes that rule mechanical: a major
// bump that forgets SOVERSION would let a consumer load an incompatible .so under the old
// soname and corrupt memory far from the cause.
TEST(VersionTest, SoVersionTracksTheMajorVersion) {
    EXPECT_EQ(BEACON_CMAKE_SOVERSION, BEACON_VERSION_MAJOR)
        << "SOVERSION must equal the major version; this library has no pImpl, so every "
           "added data member is an ABI break and the soname must move with it.";
}
