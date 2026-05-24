cmake_minimum_required(VERSION 3.16)

project(ElectronicTradingEcosystem CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -Wpedantic -O2")
set(CMAKE_VERBOSE_MAKEFILE on)

add_subdirectory(common)
add_subdirectory(exchange)
add_subdirectory(trading)
add_subdirectory(backtest)

# Link order matters for GNU ld (Linux): consumers must precede providers.
# Apple ld is order-insensitive so the v1.0 ordering worked on macOS but not Linux.
list(APPEND LIBS libbacktest)
list(APPEND LIBS libtrading)
list(APPEND LIBS libexchange)
list(APPEND LIBS libcommon)
list(APPEND LIBS pthread)

include_directories(${PROJECT_SOURCE_DIR})
include_directories(${PROJECT_SOURCE_DIR}/exchange)
include_directories(${PROJECT_SOURCE_DIR}/trading)

add_executable(exchange_main exchange/exchange_main.cpp)
target_link_libraries(exchange_main PUBLIC ${LIBS})

add_executable(trading_main trading/trading_main.cpp)
target_link_libraries(trading_main PUBLIC ${LIBS})

# v1.1 — backtest harness: drives the same MarketMaker against a replayed tape.
add_executable(backtest_main backtest/backtest_main.cpp)
target_link_libraries(backtest_main PUBLIC ${LIBS})

# ── Chapter 12 benchmarks ───────────────────────────────────────────────────
add_executable(logger_benchmark  benchmarks/logger_benchmark.cpp)
target_link_libraries(logger_benchmark  PUBLIC ${LIBS})

add_executable(release_benchmark benchmarks/release_benchmark.cpp)
target_link_libraries(release_benchmark PUBLIC ${LIBS})

add_executable(hash_benchmark    benchmarks/hash_benchmark.cpp)
target_link_libraries(hash_benchmark    PUBLIC ${LIBS})
