CXX ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -pedantic

SERIAL_SCAN_TARGET := cookinggcode-serial-scan
CONVERT_TARGET := cookinggcode-convert
TARGETS := $(SERIAL_SCAN_TARGET) $(CONVERT_TARGET)

.PHONY: all clean run

all: $(TARGETS)

$(SERIAL_SCAN_TARGET): main.cpp
	$(CXX) $(CXXFLAGS) -o $@ $<

$(CONVERT_TARGET): convert.cpp
	$(CXX) $(CXXFLAGS) -o $@ $<

run: $(SERIAL_SCAN_TARGET)
	./$(SERIAL_SCAN_TARGET)

clean:
	rm -f $(TARGETS)
