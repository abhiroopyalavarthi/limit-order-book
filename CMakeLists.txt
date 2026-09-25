cmake_minimum_required(VERSION 3.20)
project(orderbook CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)  # lets VS Code find the headers
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_library(orderbook src/OrderBook.cpp src/FastOrderBook.cpp)
target_include_directories(orderbook PUBLIC include)
target_compile_options(orderbook PRIVATE -Wall -Wextra)

# Day 6 comparison: ON = old splitmix hash for order ids, OFF = identity hash
option(ORDERBOOK_MIXED_HASH "Use a mixing hash for the order id map" OFF)
if(ORDERBOOK_MIXED_HASH)
  target_compile_definitions(orderbook PUBLIC ORDERBOOK_MIXED_HASH)
endif()

add_executable(demo src/main.cpp)
target_link_libraries(demo orderbook)

add_executable(bench bench/bench.cpp)
target_link_libraries(bench orderbook)

add_executable(replay src/replay.cpp)
target_link_libraries(replay orderbook)

# GoogleTest, downloaded automatically at configure time
include(FetchContent)
FetchContent_Declare(googletest
  URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.zip
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(googletest)

enable_testing()
add_executable(tests tests/test_orderbook.cpp)
target_link_libraries(tests orderbook GTest::gtest_main)
include(GoogleTest)
gtest_discover_tests(tests)
