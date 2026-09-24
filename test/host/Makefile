# wM-Buster ADV — host side tests for the portable wM-Bus library.
#   make -C test/host          build and run all tests
#   make -C test/host drivers  run only the upstream driver regression
CXX      ?= g++
CXXFLAGS ?= -std=gnu++17 -O2 -g -Wall -Wextra -Wno-unused-parameter
ROOT     := ../..
INC      := -I$(ROOT)/lib/wmbus/include -I.
SRCS     := $(wildcard $(ROOT)/lib/wmbus/src/*.cpp)
BUILD    := build

.PHONY: all test drivers units clean analyze

all: test

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/libwmbus.a: $(SRCS) $(wildcard $(ROOT)/lib/wmbus/include/wmbus/*.h) | $(BUILD)
	rm -f $@
	for f in $(SRCS); do $(CXX) $(CXXFLAGS) $(INC) -c $$f -o $(BUILD)/$$(basename $$f .cpp).o || exit 1; done
	ar rcs $@ $(BUILD)/*.o

$(BUILD)/test_drivers: test_drivers.cpp json_mini.h ../vectors/driver_tests.h $(BUILD)/libwmbus.a
	$(CXX) $(CXXFLAGS) $(INC) test_drivers.cpp $(BUILD)/libwmbus.a -o $@

$(BUILD)/test_units: test_units.cpp $(BUILD)/libwmbus.a
	$(CXX) $(CXXFLAGS) $(INC) test_units.cpp $(BUILD)/libwmbus.a -o $@

units: $(BUILD)/test_units
	$(BUILD)/test_units

drivers: $(BUILD)/test_drivers
	$(BUILD)/test_drivers

test: units drivers

clean:
	rm -rf $(BUILD)

$(BUILD)/analyze: analyze.cpp $(BUILD)/libwmbus.a
	$(CXX) $(CXXFLAGS) $(INC) analyze.cpp $(BUILD)/libwmbus.a -o $@

analyze: $(BUILD)/analyze
