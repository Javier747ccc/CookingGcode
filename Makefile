CXX ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -pedantic

TARGET := cookinggcode-serial-scan
SRC := main.cpp

.PHONY: all clean run

all: $(TARGET)

$(TARGET): $(SRC)
	$(CXX) $(CXXFLAGS) -o $@ $<

run: $(TARGET)
	./$(TARGET)

clean:
	rm -f $(TARGET)
