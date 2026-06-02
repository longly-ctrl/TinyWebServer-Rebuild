CXX = g++
CXXFLAGS = -std=c++11 -Wall -g

server: main.cpp
	$(CXX) $(CXXFLAGS) main.cpp -o server

clean:
	rm -f server
