cmake_minimum_required(VERSION 3.16)
project(MDS_HFT 
    VERSION 1.0.0 
    LANGUAGES CXX
)

# ============================================================================
# C++ Standard & Compiler Flags
# ============================================================================
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# We need aggressive optimization for HFT components.
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -march=native -Wall -Wextra -Wpedantic")
set(CMAKE_CXX_FLAGS_DEBUG "-g -O0 -Wall -Wextra")

# ============================================================================
# Dependencies
# ============================================================================
find_package(Threads REQUIRED)

# Boost for ASIO
find_package(Boost 1.80 REQUIRED)

include(FetchContent)

# fmt for high-performance string formatting
FetchContent_Declare(
    fmt
    GIT_REPOSITORY https://github.com/fmtlib/fmt.git
    GIT_TAG        10.1.1
)
FetchContent_MakeAvailable(fmt)

# nlohmann_json for TCP payload serialization
FetchContent_Declare(
    json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.11.3
)
FetchContent_MakeAvailable(json)

# Catch2 for testing
FetchContent_Declare(
    Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG        v3.4.0
)
FetchContent_MakeAvailable(Catch2)

# ============================================================================
# Includes
# ============================================================================
include_directories(
    ${CMAKE_SOURCE_DIR}/include
    ${Boost_INCLUDE_DIRS}
)

# ============================================================================
# Targets
# ============================================================================

# 1. Publisher (Process A)
add_executable(publisher src/publisher/publisher.cpp)
target_link_libraries(publisher 
    PRIVATE 
        Boost::headers 
        Threads::Threads 
        fmt::fmt 
        nlohmann_json::nlohmann_json
)
if(NOT APPLE)
    target_link_libraries(publisher PRIVATE rt)
endif()

# 2. Shared Memory Subscriber (Process B)
add_executable(shm_subscriber src/shm_subscriber/shm_subscriber.cpp)
target_link_libraries(shm_subscriber 
    PRIVATE 
        Threads::Threads 
        fmt::fmt 
)
if(NOT APPLE)
    target_link_libraries(shm_subscriber PRIVATE rt)
endif()

# 3. TCP Subscriber (Process C)
add_executable(tcp_subscriber src/tcp_subscriber/tcp_subscriber.cpp)
target_link_libraries(tcp_subscriber 
    PRIVATE 
        Boost::headers 
        Threads::Threads 
        fmt::fmt 
        nlohmann_json::nlohmann_json
)

# ============================================================================
# Tests
# ============================================================================
enable_testing()

add_executable(test_tick_data tests/test_tick_data.cpp)
target_link_libraries(test_tick_data 
    PRIVATE 
        Catch2::Catch2WithMain
        nlohmann_json::nlohmann_json
)

add_test(NAME CoreTests COMMAND test_tick_data)

message(STATUS "=========================================================")
message(STATUS "Market Data System (MDS) HFT Configuration Complete")
message(STATUS "Build Type: ${CMAKE_BUILD_TYPE}")
message(STATUS "CXX Flags: ${CMAKE_CXX_FLAGS_${CMAKE_BUILD_TYPE}}")
message(STATUS "=========================================================")
