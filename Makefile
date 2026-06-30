CXX = g++
CXXFLAGS = -std=c++11 -Wall -g -pthread
MYSQL_FLAGS = $(shell mysql_config --cflags --libs)

server: main.cpp
	$(CXX) $(CXXFLAGS) main.cpp -o server $(MYSQL_FLAGS)

clean:
	rm -f server
