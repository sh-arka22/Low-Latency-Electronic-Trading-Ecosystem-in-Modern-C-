set(CMAKE_CXX_STANDARD 20)

file(GLOB SOURCES "*/*.cpp")

include_directories(${PROJECT_SOURCE_DIR})
include_directories(${PROJECT_SOURCE_DIR}/exchange)
include_directories(${PROJECT_SOURCE_DIR}/trading)

list(FILTER SOURCES EXCLUDE REGEX ".*/trading_main\\.cpp$")

add_library(libtrading STATIC
    ${SOURCES}
    strategy/risk_manager.cpp
    strategy/order_manager.cpp
    strategy/market_maker.cpp
    strategy/liquidity_taker.cpp
    strategy/trade_engine.cpp
)
