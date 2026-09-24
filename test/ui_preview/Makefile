# wM-Buster ADV — host preview of the device UI (renders PNG screenshots).
#   make -C test/ui_preview           builds and writes out/*.png
# Needs the M5GFX sources (pio run once, or set M5GFX=/path/to/M5GFX/src).
CXX      ?= g++
CC       ?= gcc
ROOT     := ../..
M5GFX    ?= $(ROOT)/.pio/libdeps/m5stack-cardputer-adv/M5GFX/src
BUILD    := build
CXXFLAGS := -std=gnu++17 -O1 -g -DLGFX_LINUX_FB -Wall -Wno-unused-parameter -Wno-unused-function
INC      := -I$(M5GFX) -I$(ROOT)/src -I$(ROOT)/lib/wmbus/include -I$(ROOT)/lib/radio/include -Ishim -I$(ROOT)/include
WMBUS    := $(wildcard $(ROOT)/lib/wmbus/src/*.cpp)
GFXC     := $(wildcard $(M5GFX)/lgfx/utility/*.c) $(wildcard $(M5GFX)/lgfx/Fonts/efont/*.c) $(wildcard $(M5GFX)/lgfx/Fonts/IPA/*.c)

.PHONY: all clean
all: $(BUILD)/preview
	cd $(BUILD)/.. && $(BUILD)/preview

$(BUILD):
	mkdir -p $(BUILD)

# The graphics library is big: build it once.
$(BUILD)/libgfx.a: | $(BUILD)
	for f in $(GFXC); do $(CC) -O0 -c -I$(M5GFX) $$f -o $(BUILD)/gfx_$$(basename $$f .c).o || exit 1; done
	$(CXX) $(CXXFLAGS) -I$(M5GFX) -c $(M5GFX)/lgfx/v1/lgfx_v1.cpp -o $(BUILD)/gfx_lgfx_v1.o
	ar rcs $@ $(BUILD)/gfx_*.o

$(BUILD)/libwmbus.a: $(WMBUS) | $(BUILD)
	for f in $(WMBUS); do $(CXX) $(CXXFLAGS) $(INC) -c $$f -o $(BUILD)/wmb_$$(basename $$f .cpp).o || exit 1; done
	ar rcs $@ $(BUILD)/wmb_*.o

$(BUILD)/preview: preview.cpp stubs.cpp $(ROOT)/src/ui/ui_view.cpp $(ROOT)/src/ui/ui_view.h $(ROOT)/src/app/meters.cpp $(BUILD)/libwmbus.a $(BUILD)/libgfx.a
	$(CXX) $(CXXFLAGS) $(INC) preview.cpp stubs.cpp $(ROOT)/src/ui/ui_view.cpp $(ROOT)/src/app/meters.cpp \
		$(BUILD)/libwmbus.a $(BUILD)/libgfx.a -o $@

clean:
	rm -rf $(BUILD) out
