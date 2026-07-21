CXX ?= g++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -O2

TARGET = todo
SRC = todo.cpp

all: $(TARGET)

$(TARGET): $(SRC)
	$(CXX) $(CXXFLAGS) -o $(TARGET) $(SRC)

clean:
	rm -f $(TARGET)

run: $(TARGET)
	./$(TARGET)

.PHONY: all clean run
