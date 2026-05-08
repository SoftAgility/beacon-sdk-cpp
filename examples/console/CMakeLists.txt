add_executable(beacon_example main.cpp)

target_link_libraries(beacon_example PRIVATE beacon_sdk)

target_include_directories(beacon_example PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../include
)
