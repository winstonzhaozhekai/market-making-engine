CXX = g++
CXXFLAGS = -std=c++17 -O2 -Wall -Wextra -pthread
RELEASE_CXXFLAGS = -std=c++17 -O3 -march=native -flto -Wall -Wextra -pthread
INCLUDES = -I. -Iinclude
BOOST_INCLUDE = -I/opt/homebrew/Cellar/boost/1.88.0/include
BOOST_LIB = -L/opt/homebrew/Cellar/boost/1.88.0/lib
BOOST_LINK = -lboost_system -lboost_thread

TARGETS = market_maker_simulator WebSocketServer
TEST_TARGETS = tests/test_determinism tests/test_matching_engine tests/test_accounting tests/test_risk_manager tests/test_strategy_behavior
BENCH_TARGETS = bench/bench_engine

CORE_SRCS = MarketSimulator.cpp MarketMaker.cpp MatchingEngine.cpp PerformanceModule.cpp RiskManager.cpp strategies/AvellanedaStoikovStrategy.cpp

all: $(TARGETS)

market_maker_simulator: market_maker_simulator.cpp $(CORE_SRCS)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -o $@ market_maker_simulator.cpp $(CORE_SRCS)

WebSocketServer: WebSocketServer.cpp $(CORE_SRCS)
	$(CXX) $(CXXFLAGS) $(INCLUDES) $(BOOST_INCLUDE) $(BOOST_LIB) -o $@ WebSocketServer.cpp $(CORE_SRCS) $(BOOST_LINK)

bench/bench_engine: bench/bench_engine.cpp $(CORE_SRCS)
	$(CXX) $(RELEASE_CXXFLAGS) $(INCLUDES) -o $@ bench/bench_engine.cpp $(CORE_SRCS)

tests/test_determinism: tests/test_determinism.cpp MarketSimulator.cpp MatchingEngine.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) -o $@ tests/test_determinism.cpp MarketSimulator.cpp MatchingEngine.cpp

tests/test_matching_engine: tests/test_matching_engine.cpp MatchingEngine.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) -o $@ tests/test_matching_engine.cpp MatchingEngine.cpp

tests/test_accounting: tests/test_accounting.cpp include/Accounting.h
	$(CXX) $(CXXFLAGS) $(INCLUDES) -o $@ tests/test_accounting.cpp

tests/test_risk_manager: tests/test_risk_manager.cpp RiskManager.cpp include/RiskManager.h
	$(CXX) $(CXXFLAGS) $(INCLUDES) -o $@ tests/test_risk_manager.cpp RiskManager.cpp

tests/test_strategy_behavior: tests/test_strategy_behavior.cpp strategies/AvellanedaStoikovStrategy.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) -o $@ tests/test_strategy_behavior.cpp strategies/AvellanedaStoikovStrategy.cpp

test: $(TEST_TARGETS)
	./tests/test_determinism
	./tests/test_matching_engine
	./tests/test_accounting
	./tests/test_risk_manager
	./tests/test_strategy_behavior

bench: $(BENCH_TARGETS)

clean:
	rm -f $(TARGETS) $(TEST_TARGETS) $(BENCH_TARGETS)

.PHONY: all clean test bench
