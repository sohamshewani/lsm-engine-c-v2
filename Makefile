CC = gcc
CFLAGS = -std=c99 -O3 -Wall -Wextra -Iinclude
LDFLAGS = -lpthread

SRC_DIR = src
OBJ_DIR = obj
BIN_DIR = bin
TEST_DIR = tests
BENCH_DIR = benchmarks

TARGET = $(BIN_DIR)/lsm_engine
TEST_TARGET = $(BIN_DIR)/run_tests
BENCH_TARGET = $(BIN_DIR)/run_bench

SRCS = $(wildcard $(SRC_DIR)/*.c)
OBJS = $(patsubst $(SRC_DIR)/%.c, $(OBJ_DIR)/%.o, $(SRCS))

TEST_SRCS = $(wildcard $(TEST_DIR)/*.c)
BENCH_SRCS = $(wildcard $(BENCH_DIR)/*.c)

.PHONY: all build test clean benchmark

all: build

build: $(BIN_DIR) $(OBJ_DIR) $(TARGET)

$(BIN_DIR):
	mkdir -p $(BIN_DIR)

$(OBJ_DIR):
	mkdir -p $(OBJ_DIR)

$(TARGET): $(OBJS)
	@if [ -n "$(SRCS)" ]; then \
		$(CC) $(OBJS) -o $@ $(LDFLAGS); \
	else \
		echo "No source files found in $(SRC_DIR). Creating dummy binary."; \
		touch $@; \
	fi

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c
	$(CC) $(CFLAGS) -c $< -o $@

test: build
	@if [ -d "$(TEST_DIR)" ] && [ -n "$(TEST_SRCS)" ]; then \
		echo "Building and running tests..."; \
		$(CC) $(CFLAGS) $(TEST_SRCS) $(filter-out $(OBJ_DIR)/main.o, $(OBJS)) -o $(TEST_TARGET) $(LDFLAGS); \
		$(TEST_TARGET); \
	else \
		echo "No tests found in $(TEST_DIR). Skipping."; \
	fi

benchmark: build
	@if [ -d "$(BENCH_DIR)" ] && [ -n "$(BENCH_SRCS)" ]; then \
		echo "Building and running benchmarks..."; \
		$(CC) $(CFLAGS) $(BENCH_SRCS) $(filter-out $(OBJ_DIR)/main.o, $(OBJS)) -o $(BENCH_TARGET) $(LDFLAGS); \
		$(BENCH_TARGET); \
	else \
		echo "No benchmarks found in $(BENCH_DIR). Skipping."; \
	fi

clean:
	rm -rf $(OBJ_DIR) $(BIN_DIR) *.log *.db data/