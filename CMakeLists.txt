cmake_minimum_required(VERSION 3.25)

project(beacon_sdk
    VERSION 1.0.0
    LANGUAGES CXX
    DESCRIPTION "SoftAgility Beacon C++ SDK"
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Option for stack traces (default ON)
option(BEACON_STACK_TRACES "Enable platform-specific stack trace capture" ON)

# Option for building tests (default OFF)
option(BEACON_BUILD_TESTS "Build unit tests" OFF)

# Option for building examples (default OFF)
option(BEACON_BUILD_EXAMPLES "Build example applications" OFF)

# ---------- Dependency Resolution ----------

include(FetchContent)

# --- libcurl ---
find_package(CURL 8.0 QUIET)
if(NOT CURL_FOUND)
    message(STATUS "libcurl not found via find_package, fetching via FetchContent...")
    FetchContent_Declare(
        curl
        URL https://github.com/curl/curl/releases/download/curl-8_9_1/curl-8.9.1.tar.gz
        FIND_PACKAGE_ARGS 8.0
    )
    set(BUILD_CURL_EXE OFF CACHE BOOL "" FORCE)
    set(BUILD_SHARED_LIBS ON CACHE BOOL "" FORCE)
    set(CURL_DISABLE_TESTS ON CACHE BOOL "" FORCE)
    # Enable a TLS backend so HTTPS works. Without this libcurl silently builds
    # with no SSL support and every https:// request fails with
    # CURLE_UNSUPPORTED_PROTOCOL — the SDK transport swallows these as
    # generic network errors, so the symptom is "events never arrive" with
    # zero log noise. Per platform:
    #   Windows: Schannel (built into the OS, no extra deps)
    #   Linux:   OpenSSL (assumed present; standard on every distro)
    #   macOS:   OpenSSL via Homebrew, or system Apple-provided one
    if(WIN32)
        set(CURL_USE_SCHANNEL ON CACHE BOOL "" FORCE)
    else()
        set(CURL_USE_OPENSSL ON CACHE BOOL "" FORCE)
    endif()
    FetchContent_MakeAvailable(curl)
endif()

# --- nlohmann_json ---
find_package(nlohmann_json 3.11 QUIET)
if(NOT nlohmann_json_FOUND)
    message(STATUS "nlohmann_json not found via find_package, fetching via FetchContent...")
    FetchContent_Declare(
        nlohmann_json
        URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
        FIND_PACKAGE_ARGS 3.11
    )
    set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(nlohmann_json)
endif()

# --- SQLite3 ---
find_package(SQLite3 3.40 QUIET)
if(NOT SQLite3_FOUND)
    message(STATUS "SQLite3 not found via find_package, fetching amalgamation via FetchContent...")
    FetchContent_Declare(
        sqlite3
        URL https://www.sqlite.org/2024/sqlite-amalgamation-3450000.zip
    )
    FetchContent_MakeAvailable(sqlite3)

    if(NOT TARGET SQLite::SQLite3)
        add_library(sqlite3_lib STATIC
            ${sqlite3_SOURCE_DIR}/sqlite3.c
        )
        target_include_directories(sqlite3_lib PUBLIC ${sqlite3_SOURCE_DIR})
        if(UNIX)
            target_link_libraries(sqlite3_lib PUBLIC pthread dl)
        endif()
        add_library(SQLite::SQLite3 ALIAS sqlite3_lib)
    endif()
endif()

# --- OpenSSL (Linux/macOS only, for SHA-256) ---
if(NOT WIN32)
    find_package(OpenSSL REQUIRED)
endif()

# ---------- Library Sources ----------

set(BEACON_SOURCES
    src/Tracker.cpp
    src/UuidV7.cpp
    src/Base64.cpp
    src/PropertySanitizer.cpp
    src/StackTrace.cpp
    src/DeviceId.cpp
    src/EnvironmentCollector.cpp
    src/HttpClient.cpp
    src/RetryPolicy.cpp
    src/BreadcrumbBuffer.cpp
    src/DiskQueue.cpp
    src/FlushThread.cpp
)

set(BEACON_PUBLIC_HEADERS
    include/beacon/beacon.hpp
    include/beacon/Tracker.hpp
    include/beacon/Options.hpp
    include/beacon/ILogger.hpp
    include/beacon/ExceptionSeverity.hpp
    include/beacon/FlushStatus.hpp
    include/beacon/EventDefinitionBuilder.hpp
    include/beacon/Export.hpp
)

# ---------- Shared Library Target ----------

add_library(beacon_sdk SHARED ${BEACON_SOURCES})

target_include_directories(beacon_sdk
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_compile_definitions(beacon_sdk PRIVATE BEACON_EXPORTS)

if(WIN32)
    target_compile_definitions(beacon_sdk PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

if(BEACON_STACK_TRACES)
    target_compile_definitions(beacon_sdk PRIVATE BEACON_STACK_TRACES_ENABLED)
endif()

# Link dependencies
target_link_libraries(beacon_sdk
    PRIVATE
        CURL::libcurl
        nlohmann_json::nlohmann_json
        SQLite::SQLite3
)

if(NOT WIN32)
    target_link_libraries(beacon_sdk PRIVATE OpenSSL::Crypto)
endif()

# Platform-specific settings
if(WIN32)
    # Link Windows system libraries
    if(BEACON_STACK_TRACES)
        target_link_libraries(beacon_sdk PRIVATE dbghelp)
    endif()
    target_link_libraries(beacon_sdk PRIVATE bcrypt)
elseif(UNIX)
    set_target_properties(beacon_sdk PROPERTIES
        POSITION_INDEPENDENT_CODE ON
    )
endif()

# Compiler flags
if(MSVC)
    target_compile_options(beacon_sdk PRIVATE /W3)
else()
    target_compile_options(beacon_sdk PRIVATE
        -Wall -Wextra -Wpedantic
        -fvisibility=hidden
    )
endif()

# Shared library versioning (Linux SONAME)
set_target_properties(beacon_sdk PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    VERSION ${PROJECT_VERSION}
    SOVERSION 1
)

# Copy public headers to build directory
foreach(header ${BEACON_PUBLIC_HEADERS})
    get_filename_component(header_name ${header} NAME)
    configure_file(
        ${CMAKE_CURRENT_SOURCE_DIR}/${header}
        ${CMAKE_BINARY_DIR}/include/beacon/${header_name}
        COPYONLY
    )
endforeach()

# ---------- Static Library for Tests ----------
# Tests need access to internal symbols not exported from the DLL.

if(BEACON_BUILD_TESTS)
    add_library(beacon_sdk_static STATIC ${BEACON_SOURCES})

    target_include_directories(beacon_sdk_static
        PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src
    )

    target_compile_definitions(beacon_sdk_static PUBLIC BEACON_STATIC)

    if(WIN32)
        target_compile_definitions(beacon_sdk_static PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    endif()

    if(BEACON_STACK_TRACES)
        target_compile_definitions(beacon_sdk_static PRIVATE BEACON_STACK_TRACES_ENABLED)
    endif()

    target_link_libraries(beacon_sdk_static
        PRIVATE
            CURL::libcurl
            nlohmann_json::nlohmann_json
            SQLite::SQLite3
    )

    if(NOT WIN32)
        target_link_libraries(beacon_sdk_static PRIVATE OpenSSL::Crypto)
    endif()

    if(WIN32)
        if(BEACON_STACK_TRACES)
            target_link_libraries(beacon_sdk_static PRIVATE dbghelp)
        endif()
        target_link_libraries(beacon_sdk_static PRIVATE bcrypt)
    endif()

    if(MSVC)
        target_compile_options(beacon_sdk_static PRIVATE /W3)
    else()
        target_compile_options(beacon_sdk_static PRIVATE -Wall -Wextra -Wpedantic)
    endif()

    enable_testing()
    add_subdirectory(tests)
endif()

# ---------- Examples ----------

if(BEACON_BUILD_EXAMPLES)
    add_subdirectory(examples/console)
endif()

# ---------- Install Rules ----------
# Standard CMake install layout so consumers can `cmake --install <build-dir>`
# and get a clean prefix with headers, lib, runtime DLL, and a CMake config
# package for find_package(beacon_sdk).
#
# After install, customers can integrate via:
#   find_package(beacon_sdk CONFIG REQUIRED)
#   target_link_libraries(my_app PRIVATE beacon_sdk)
#
# Layout produced (relative to CMAKE_INSTALL_PREFIX):
#   bin/        beacon_sdk.dll + transitive runtime DLLs (Windows only)
#   lib/        beacon_sdk.lib (Windows) or libbeacon_sdk.so/.dylib (Unix)
#   include/    public headers under beacon/
#   lib/cmake/beacon_sdk/  CMake config package files

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

install(TARGETS beacon_sdk
    EXPORT beacon_sdk_targets
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}     # .dll on Windows
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}     # .so / .dylib on Unix
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}     # .lib import lib on Windows
)

# Install public headers preserving the beacon/ subdirectory structure.
install(DIRECTORY include/beacon/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/beacon
    FILES_MATCHING PATTERN "*.hpp"
)

# Note on Windows runtime DLL deployment:
# beacon_sdk.dll depends on libcurl.dll. Consumers will hit
# STATUS_DLL_NOT_FOUND (Windows error 53) at launch unless libcurl.dll is
# next to their exe. The standard CMake pattern (3.21+) is for the consumer
# to add this to their own CMakeLists.txt:
#
#   add_custom_command(TARGET my_app POST_BUILD
#       COMMAND ${CMAKE_COMMAND} -E copy -t
#               $<TARGET_FILE_DIR:my_app>
#               $<TARGET_RUNTIME_DLLS:my_app>
#       COMMAND_EXPAND_LISTS)
#
# The README walks consumers through this. We deliberately do NOT bundle
# libcurl.dll inside our install tree — that would force every consumer to
# use our specific libcurl build even when they have a system one.

# Generate and install the CMake config package so find_package() works.
install(EXPORT beacon_sdk_targets
    FILE beacon_sdk-targets.cmake
    NAMESPACE beacon_sdk::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/beacon_sdk
)

write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/beacon_sdk-config-version.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)

# Minimal config file. Re-runs find_dependency for the public-interface deps
# (we have none; libcurl/sqlite3/json are PRIVATE link deps so consumers
# don't need to refind them). If we ever add a PUBLIC dep, add a
# find_dependency(<pkg>) line here.
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/beacon_sdk-config.cmake"
"include(\"\${CMAKE_CURRENT_LIST_DIR}/beacon_sdk-targets.cmake\")\n")

install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/beacon_sdk-config.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/beacon_sdk-config-version.cmake"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/beacon_sdk
)
