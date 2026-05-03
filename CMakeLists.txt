cmake_minimum_required(VERSION 3.21)

project(market_making_engine LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Export compile_commands.json for editor/clangd integration.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ---- Warnings (applied to first-party targets only) ------------------------

add_library(mme_warnings INTERFACE)
target_compile_options(mme_warnings INTERFACE
    -Wall -Wextra -Wpedantic
)

# ---- Dependencies ----------------------------------------------------------

find_package(Boost REQUIRED COMPONENTS system thread)
find_package(GTest CONFIG REQUIRED)
find_package(Threads REQUIRED)

# ---- Core library (matching engine + market sim + accounting + risk + strat)

add_library(mme_core STATIC
    MarketSimulator.cpp
    MarketMaker.cpp
    MatchingEngine.cpp
    PerformanceModule.cpp
    RiskManager.cpp
    strategies/AvellanedaStoikovStrategy.cpp
)
target_include_directories(mme_core PUBLIC
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/include
)
target_link_libraries(mme_core
    PUBLIC  Threads::Threads
    PRIVATE mme_warnings
)

# ---- WebSocket library (depends on Boost.Beast/Asio) -----------------------

add_library(mme_ws STATIC
    WsSession.cpp
)
target_link_libraries(mme_ws
    PUBLIC  mme_core
            Boost::system
            Boost::thread
    PRIVATE mme_warnings
)

# ---- Executables -----------------------------------------------------------

add_executable(market_maker_simulator market_maker_simulator.cpp)
target_link_libraries(market_maker_simulator PRIVATE mme_core mme_warnings)

add_executable(WebSocketServer WebSocketServer.cpp)
target_link_libraries(WebSocketServer PRIVATE mme_ws mme_warnings)

add_executable(bench_engine bench/bench_engine.cpp)
target_link_libraries(bench_engine PRIVATE mme_core mme_warnings)

# ---- Tests -----------------------------------------------------------------

enable_testing()
add_subdirectory(tests)
