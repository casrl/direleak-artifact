# Compiler
CC := gcc

# Architecture: spr | icx | clx | skx  (default icx)
ARCH ?= icx
ifeq ($(ARCH),spr)
  ARCH_FLAGS := -DARCH=4
else ifeq ($(ARCH),icx)
  ARCH_FLAGS := -DARCH=3
else ifeq ($(ARCH),clx)
  ARCH_FLAGS := -DARCH=2
else ifeq ($(ARCH),skx)
  ARCH_FLAGS := -DARCH=2 -DARCH_SKX
else
  $(error Unknown ARCH=$(ARCH). Valid: spr icx clx skx)
endif

# Compiler Flags (using pkg-config for Jansson)
CFLAGS  := -Wall -std=gnu99 -I./include -O0 -pthread -D_GNU_SOURCE -fPIC $(ARCH_FLAGS)
CFLAGS  += $(shell pkg-config --cflags jansson 2>/dev/null)

# Linker Flags (using pkg-config for Jansson)
LDFLAGS := 
LDLIBS  := -lnuma -lm -ldl -lpthread
LDLIBS  += $(shell pkg-config --libs jansson 2>/dev/null)
LDLIBS  += -ljansson

# Directories
SRC_DIR := src
OBJ_DIR := obj
BIN_DIR := bin
BENCHMARK_DIR := $(SRC_DIR)/benchmarks
BENCHMARK_OBJ_DIR := $(OBJ_DIR)/benchmarks

# Source files
SRCS := $(wildcard $(SRC_DIR)/*.c)
BENCHMARK_SRCS := $(wildcard $(BENCHMARK_DIR)/*.c)

# Object files
OBJS := $(patsubst $(SRC_DIR)/%.c,$(OBJ_DIR)/%.o,$(SRCS))
BENCHMARK_OBJS := $(patsubst $(BENCHMARK_DIR)/%.c,$(BENCHMARK_OBJ_DIR)/%.o,$(BENCHMARK_SRCS))
BENCHMARK_SO := $(patsubst $(BENCHMARK_DIR)/%.c,$(BIN_DIR)/%.so,$(BENCHMARK_SRCS))

# Additional Objects for Benchmark Libraries
SOCKET_MEMORY_OBJ := $(OBJ_DIR)/socket_memory.o
UTIL_OBJ := $(OBJ_DIR)/util.o
MSR_UTILS_OBJ := $(OBJ_DIR)/msr_utils.o

# Executable name
EXEC := $(BIN_DIR)/msr_program

# --- Rules ---

# Default target
all: clean $(EXEC) $(BENCHMARK_SO)

# Install dependencies
install-deps:
	@if ! pkg-config --exists jansson; then \
		echo "Jansson library not found. Attempting to install..."; \
		if command -v apt-get &> /dev/null; then \
			sudo apt-get update; \
			sudo apt-get install -y libjansson-dev; \
		elif command -v yum &> /dev/null; then \
			sudo yum install -y jansson-devel; \
		elif command -v dnf &> /dev/null; then \
			sudo dnf install -y jansson-devel; \
		elif command -v zypper &> /dev/null; then \
			sudo zypper install -y jansson-devel; \
		elif command -v pacman &> /dev/null; then \
			sudo pacman -S jansson --noconfirm; \
		else \
			echo "Error: Could not determine package manager to install libjansson-dev."; \
			echo "Please install libjansson-dev manually and then run 'make' again."; \
			exit 1; \
		fi; \
	else \
		echo "Jansson library is already installed."; \
	fi
	@sudo ldconfig # Update library cache after install

# Create object directories and compile .c files into .o files
$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@ -MMD -MP -MF $(@:.o=.d)

# Compile benchmarks into shared objects and link required dependencies
$(BENCHMARK_OBJ_DIR)/%.o: $(BENCHMARK_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

# Convert benchmark object files into shared libraries (.so) and link necessary objects
$(BIN_DIR)/%.so: $(BENCHMARK_OBJ_DIR)/%.o $(SOCKET_MEMORY_OBJ) $(UTIL_OBJ) $(MSR_UTILS_OBJ)
	@mkdir -p $(BIN_DIR)
	$(CC) -shared -o $@ $< $(SOCKET_MEMORY_OBJ) $(UTIL_OBJ) $(MSR_UTILS_OBJ) $(LDFLAGS) $(LDLIBS)

# Link object files to create the executable
$(EXEC): $(OBJS)
	@mkdir -p $(BIN_DIR)
	$(CC) $(OBJS) -o $(EXEC) $(LDFLAGS) $(LDLIBS)

# Recompile benchmark shared libraries only
bench: $(SOCKET_MEMORY_OBJ) $(UTIL_OBJ) $(MSR_UTILS_OBJ) $(BENCHMARK_SO)

# DireLeak counter-based covert channel PoC (standalone Spy + Trojan).
# Run with the msr_program server STOPPED (the Spy owns the uncore CHA counters).
covert: tools/spy tools/trojan
tools/spy: tools/spy.c tools/cc_shared.h
	$(CC) -O2 -std=gnu99 -D_GNU_SOURCE -Wall -o $@ tools/spy.c -lnuma
tools/trojan: tools/trojan.c tools/cc_shared.h
	$(CC) -O2 -std=gnu99 -D_GNU_SOURCE -Wall -o $@ tools/trojan.c -lnuma

# DireLeak ML side-channel (§7.1): HitME counter fingerprint collector.
# Run with the msr_program server STOPPED.
sidechannel: tools/hitme_monitor
tools/hitme_monitor: tools/hitme_monitor.c
	$(CC) -O2 -std=gnu99 -D_GNU_SOURCE -Wall -o $@ tools/hitme_monitor.c

# Clean build artifacts
clean:
	@rm -rf $(OBJ_DIR) $(BIN_DIR)

-include $(OBJS:.o=.d)

.PHONY: all bench clean install-deps covert sidechannel