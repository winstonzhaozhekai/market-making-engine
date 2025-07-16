CXX = g++
CXXFLAGS = -std=c++17 -O2 -Wall -Wextra -pthread
BOOST_INCLUDE = -I/opt/homebrew/Cellar/boost/1.88.0/include
BOOST_LIB = -L/opt/homebrew/Cellar/boost/1.88.0/lib
BOOST_LINK = -lboost_system -lboost_thread

TARGETS = market_maker_simulator WebSocketServer

all: $(TARGETS)

market_maker_simulator: market_maker_simulator.cpp MarketSimulator.cpp MarketMaker.cpp PerformanceModule.cpp
	$(CXX) $(CXXFLAGS) -o $@ market_maker_simulator.cpp MarketSimulator.cpp MarketMaker.cpp PerformanceModule.cpp

WebSocketServer: WebSocketServer.cpp MarketSimulator.cpp MarketMaker.cpp PerformanceModule.cpp
	$(CXX) $(CXXFLAGS) $(BOOST_INCLUDE) $(BOOST_LIB) -o $@ WebSocketServer.cpp MarketSimulator.cpp MarketMaker.cpp PerformanceModule.cpp $(BOOST_LINK)

clean:
	rm -f $(TARGETS)

.PHONY: all clean