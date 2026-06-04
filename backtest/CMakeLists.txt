set(CMAKE_CXX_STANDARD 20)

include_directories(${PROJECT_SOURCE_DIR})
include_directories(${PROJECT_SOURCE_DIR}/exchange)
include_directories(${PROJECT_SOURCE_DIR}/trading)

add_library(libbacktest STATIC
    binance_tape_reader.cpp
    lobster_tape_reader.cpp
    backtest_engine.cpp
)
target_link_libraries(libbacktest PUBLIC libtrading libexchange libcommon)
