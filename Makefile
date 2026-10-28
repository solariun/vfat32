# =============================================================================
# Makefile — vfat32 (spec §15.1)
# =============================================================================
CXX      ?= c++
WARN      = -Wall -Wextra -Wpedantic -Werror
CXXFLAGS ?= -O2
CXXFLAGS += -std=c++11 $(WARN) -MMD -MP
INCLUDES  = -Isrc -Itests

UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
PTHREAD   =
else
PTHREAD   = -lpthread
endif

BUILDDIR  = build
BINDIR    = bin

LIB_SRC     = $(wildcard src/*.cpp)
TEST_SRC    = $(wildcard tests/*.cpp)
EXAMPLE_SRC = $(wildcard examples/pc/*.cpp)

LIB_OBJ     = $(LIB_SRC:%.cpp=$(BUILDDIR)/%.o)
TEST_OBJ    = $(TEST_SRC:%.cpp=$(BUILDDIR)/%.o)

LIB         = $(BUILDDIR)/libvfat32.a
TEST_BIN    = $(BINDIR)/vfat32_tests
EXAMPLE_BIN = $(EXAMPLE_SRC:examples/pc/%.cpp=$(BINDIR)/%)
FIGURES_BIN = $(BINDIR)/doc_figures

# ── Sanitizer builds (spec §15.1) ──
ASAN_FLAGS  = -std=c++11 $(WARN) -O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined -fno-sanitize-recover=all
TSAN_FLAGS  = -std=c++11 $(WARN) -O1 -g -fsanitize=thread
ASAN_BIN    = $(BINDIR)/vfat32_tests_asan
TSAN_BIN    = $(BINDIR)/vfat32_tests_tsan

# ── Embedded check (spec §15.1, X1): no exceptions, no RTTI, no heap with VFAT32_USE_HEAP=0 ──
ARDUINO_PACKAGES = $(firstword $(wildcard $(HOME)/Library/Arduino15/packages $(HOME)/.arduino15/packages))
AVR_GXX    ?= $(firstword $(wildcard $(ARDUINO_PACKAGES)/arduino/tools/avr-gcc/*/bin/avr-g++) $(shell command -v avr-g++ 2>/dev/null))
XTENSA_GXX ?= $(firstword $(wildcard $(ARDUINO_PACKAGES)/esp32/tools/esp-x32/*/bin/xtensa-esp32-elf-g++) $(shell command -v xtensa-esp32-elf-g++ 2>/dev/null))
EMBED_FLAGS = -std=gnu++11 -Os $(WARN) -fno-exceptions -fno-rtti -ffunction-sections -fdata-sections -Isrc
AVR_FLAGS   = $(EMBED_FLAGS) -mmcu=atmega2560 -fno-threadsafe-statics
XTENSA_FLAGS = $(EMBED_FLAGS) -mlongcalls
# Symbols the library must never reference (heap with VFAT32_USE_HEAP=0, exceptions, RTTI).
FORBIDDEN_SYMBOLS = '^_?(malloc|calloc|realloc|free|_Znwm|_Znwj|_Znam|_Znaj|_ZdlPv|_ZdaPv|_ZdlPvm|_ZdlPvj|__cxa_throw|__cxa_allocate_exception|__gxx_personality_v0|__dynamic_cast|_ZTI.*|_ZTS.*)$$'
# Prints the symbol names of nm output, one per line.
SYMBOLS = awk '{print $$NF}'
EMBED_DIR   = $(BUILDDIR)/embedded

ARDUINO_CLI ?= $(shell command -v arduino-cli 2>/dev/null)

.PHONY: all lib test test_long test_asan test_tsan examples docs check_embedded arduino_check clean help

all: lib $(TEST_BIN) examples $(FIGURES_BIN)

help:
	@echo "make              library, tests, examples, figure tool"
	@echo "make test         unit tests + random stress + threads + external fsck (spec 15.1)"
	@echo "make test_long    the random stress test with many more operations and seeds"
	@echo "make test_asan    tests under AddressSanitizer + UndefinedBehaviorSanitizer"
	@echo "make test_tsan    the threads test under ThreadSanitizer"
	@echo "make check_embedded  AVR + ESP32 builds, forbidden symbols, sizes"
	@echo "make arduino_check   compile the Arduino sketches (arduino-cli)"
	@echo "make docs         regenerate docs/figures from the real code"
	@echo "make examples     build the PC examples into bin/"

lib: $(LIB)

$(LIB): $(LIB_OBJ)
	@mkdir -p $(dir $@)
	$(AR) rcs $@ $^

$(BUILDDIR)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

$(TEST_BIN): $(TEST_OBJ) $(LIB)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $^ -o $@ $(PTHREAD)

test: $(TEST_BIN)
	./$(TEST_BIN)

test_long: $(TEST_BIN)
	VFAT32_LONG=1 ./$(TEST_BIN)

$(ASAN_BIN): $(LIB_SRC) $(TEST_SRC) $(wildcard src/*.hpp tests/*.hpp)
	@mkdir -p $(dir $@)
	$(CXX) $(ASAN_FLAGS) $(INCLUDES) $(LIB_SRC) $(TEST_SRC) -o $@ $(PTHREAD)

test_asan: $(ASAN_BIN)
	ASAN_OPTIONS=detect_leaks=0 ./$(ASAN_BIN)

$(TSAN_BIN): $(LIB_SRC) $(TEST_SRC) $(wildcard src/*.hpp tests/*.hpp)
	@mkdir -p $(dir $@)
	$(CXX) $(TSAN_FLAGS) $(INCLUDES) $(LIB_SRC) $(TEST_SRC) -o $@ $(PTHREAD)

test_tsan: $(TSAN_BIN)
	./$(TSAN_BIN) thread

examples: $(EXAMPLE_BIN)

APP_FLAGS   = -std=c++11 $(WARN) -O2 -Isrc

$(BINDIR)/%: examples/pc/%.cpp $(LIB)
	@mkdir -p $(dir $@)
	$(CXX) $(APP_FLAGS) $< $(LIB) -o $@ $(PTHREAD)

$(FIGURES_BIN): tools/doc_figures.cpp $(LIB)
	@mkdir -p $(dir $@)
	$(CXX) $(APP_FLAGS) $< $(LIB) -o $@

docs: $(FIGURES_BIN)
	./$(FIGURES_BIN) docs/figures

check_embedded:
	@mkdir -p $(EMBED_DIR)
	@echo "== host, -fno-exceptions -fno-rtti, VFAT32_USE_HEAP=0"
	@for f in $(LIB_SRC); do $(CXX) $(EMBED_FLAGS) -DVFAT32_USE_HEAP=0 -c $$f -o $(EMBED_DIR)/host_$$(basename $$f .cpp).o || exit 1; done
	@if nm -u $(EMBED_DIR)/host_*.o | $(SYMBOLS) | grep -E $(FORBIDDEN_SYMBOLS); then echo "FAIL: forbidden symbol (host)"; exit 1; fi
	@echo "   external symbols: $$(nm -u $(EMBED_DIR)/host_*.o | $(SYMBOLS) | grep -v -e ':' -e vfat32 | sort -u | tr '\n' ' ')"
ifneq ($(AVR_GXX),)
	@echo "== AVR (atmega2560), $(AVR_GXX)"
	@for f in $(LIB_SRC); do $(AVR_GXX) $(AVR_FLAGS) -DVFAT32_USE_HEAP=0 -c $$f -o $(EMBED_DIR)/avr_$$(basename $$f .cpp).o || exit 1; done
	@if $(dir $(AVR_GXX))avr-nm -u $(EMBED_DIR)/avr_*.o | $(SYMBOLS) | grep -E $(FORBIDDEN_SYMBOLS); then echo "FAIL: forbidden symbol (AVR)"; exit 1; fi
	$(AVR_GXX) $(AVR_FLAGS) -DVFAT32_USE_HEAP=0 -Wl,--gc-sections tests/embedded/embedded_main.cpp $(LIB_SRC) -o $(EMBED_DIR)/avr_app.elf
	@if $(dir $(AVR_GXX))avr-nm $(EMBED_DIR)/avr_app.elf | $(SYMBOLS) | grep -E $(FORBIDDEN_SYMBOLS); then echo "FAIL: forbidden symbol linked (AVR)"; exit 1; fi
	@$(dir $(AVR_GXX))avr-size $(EMBED_DIR)/avr_app.elf
	@$(dir $(AVR_GXX))avr-nm -S --size-sort -C $(EMBED_DIR)/avr_app.elf | grep -E ' [bBdD] .*(g_fs|g_volume)' || true
else
	@echo "== AVR: avr-g++ not found, skipped"
endif
ifneq ($(XTENSA_GXX),)
	@echo "== ESP32 (xtensa), $(XTENSA_GXX)"
	@for f in $(LIB_SRC); do $(XTENSA_GXX) $(XTENSA_FLAGS) -DVFAT32_USE_HEAP=0 -c $$f -o $(EMBED_DIR)/esp32_$$(basename $$f .cpp).o || exit 1; done
	@if $(dir $(XTENSA_GXX))xtensa-esp32-elf-nm -u $(EMBED_DIR)/esp32_*.o | $(SYMBOLS) | grep -E $(FORBIDDEN_SYMBOLS); then echo "FAIL: forbidden symbol (ESP32)"; exit 1; fi
	@$(dir $(XTENSA_GXX))xtensa-esp32-elf-size -t $(EMBED_DIR)/esp32_*.o | tail -1
else
	@echo "== ESP32: xtensa-esp32-elf-g++ not found, skipped"
endif
	@echo "check_embedded: PASS"

# sketch:board pairs; the retained-RAM sketch needs the ESP32's no-init RAM
ARDUINO_BUILDS = examples/arduino/vfat32_basic:esp32:esp32:esp32 examples/arduino/vfat32_basic:arduino:avr:mega \
                 examples/arduino/vfat32_retained_ram:esp32:esp32:esp32

arduino_check:
ifeq ($(ARDUINO_CLI),)
	@echo "arduino-cli not found, skipped"
else
	@mkdir -p $(BUILDDIR)
	@for pair in $(ARDUINO_BUILDS); do \
	  sketch=$${pair%%:*}; board=$${pair#*:}; \
	  echo "== $$sketch on $$board"; \
	  if ! $(ARDUINO_CLI) compile --warnings all -b $$board --library . $$sketch > $(BUILDDIR)/arduino.log 2>&1; then \
	    cat $(BUILDDIR)/arduino.log; exit 1; fi; \
	  if grep -E 'warning:' $(BUILDDIR)/arduino.log | grep -E 'vfat32|examples/arduino'; then echo "FAIL: warnings"; exit 1; fi; \
	  grep -E 'Sketch uses|Global variables' $(BUILDDIR)/arduino.log; \
	done
	@echo "arduino_check: PASS"
endif

clean:
	rm -rf $(BUILDDIR) $(BINDIR)

-include $(LIB_OBJ:.o=.d) $(TEST_OBJ:.o=.d)
