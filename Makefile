CXX = g++
CXXFLAGS = -std=c++17 -O2 -Wall -Wextra -pthread
INCLUDES = -I. -Iinclude
BOOST_INCLUDE = -I/opt/homebrew/Cellar/boost/1.88.0/include
BOOST_LIB = -L/opt/homebrew/Cellar/boost/1.88.0/lib
BOOST_LINK = -lboost_system -lboost_thread

TARGETS = market_maker_simulator WebSocketServer
TEST_TARGETS = tests/test_determinism tests/test_matching_engine

all: $(TARGETS)

market_maker_simulator: market_maker_simulator.cpp MarketSimulator.cpp MarketMaker.cpp MatchingEngine.cpp PerformanceModule.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) -o $@ market_maker_simulator.cpp MarketSimulator.cpp MarketMaker.cpp MatchingEngine.cpp PerformanceModule.cpp

WebSocketServer: WebSocketServer.cpp MarketSimulator.cpp MarketMaker.cpp MatchingEngine.cpp PerformanceModule.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) $(BOOST_INCLUDE) $(BOOST_LIB) -o $@ WebSocketServer.cpp MarketSimulator.cpp MarketMaker.cpp MatchingEngine.cpp PerformanceModule.cpp $(BOOST_LINK)

tests/test_determinism: tests/test_determinism.cpp MarketSimulator.cpp MatchingEngine.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) -o $@ tests/test_determinism.cpp MarketSimulator.cpp MatchingEngine.cpp

tests/test_matching_engine: tests/test_matching_engine.cpp MatchingEngine.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) -o $@ tests/test_matching_engine.cpp MatchingEngine.cpp

test: $(TEST_TARGETS)
	./tests/test_determinism
	./tests/test_matching_engine

clean:
	rm -f $(TARGETS) $(TEST_TARGETS)

.PHONY: all clean test
