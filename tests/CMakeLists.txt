include(FetchContent)

# Fetch Google Test
FetchContent_Declare(
    googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.tar.gz
    FIND_PACKAGE_ARGS
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

# Test source files
set(TEST_SOURCES
    test_configure.cpp
    test_identify_track.cpp
    test_sessions.cpp
    test_exceptions.cpp
    test_flush.cpp
    test_environment.cpp
    test_manifest.cpp
    test_lifecycle.cpp
    test_uuid.cpp
    test_device_id.cpp
    test_thread_safety.cpp
    test_breadcrumb_buffer.cpp
    test_property_sanitizer.cpp
    test_retry_policy.cpp
    test_disk_queue.cpp
    test_event_definition_builder.cpp
    test_options_validation.cpp
)

add_executable(beacon_tests ${TEST_SOURCES})

target_link_libraries(beacon_tests
    PRIVATE
        beacon_sdk_static
        GTest::gtest_main
        nlohmann_json::nlohmann_json
)

target_include_directories(beacon_tests
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../src
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
)

include(GoogleTest)
# DISCOVERY_MODE PRE_TEST defers test enumeration until ctest runs the binary.
# The default (POST_BUILD) tries to run the test exe at configure time, which
# fails on Windows because runtime DLLs (libcurl.dll, beacon_sdk.dll) are not
# yet placed next to the exe — gtest_discover_tests crashes with a generic
# "Error running test executable".
gtest_discover_tests(beacon_tests DISCOVERY_MODE PRE_TEST)

# On Windows, place runtime DLLs (libcurl + transitive deps) next to the test
# binary so it can launch when ctest invokes it. CMake 3.21+ resolves the
# import-lib chain automatically via $<TARGET_RUNTIME_DLLS>.
if(WIN32)
    add_custom_command(TARGET beacon_tests POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_RUNTIME_DLLS:beacon_tests>
                $<TARGET_FILE_DIR:beacon_tests>
        COMMAND_EXPAND_LISTS
    )
endif()
