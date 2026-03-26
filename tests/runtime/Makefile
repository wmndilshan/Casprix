# Casperix Runtime Tests Makefile

CC = gcc
CFLAGS = -Wall -Wextra -std=c11 -O2 -I../src -I../include -I..
LDFLAGS = -lpthread

# Directories
SRC_DIR = src
RUNTIME_DIR = runtime
TEST_DIR = tests
BIN_DIR = bin

# Runtime source files
ASYNC_SRCS = ../$(RUNTIME_DIR)/async/task.c ../$(RUNTIME_DIR)/async/future.c
CONCURRENT_SRCS = ../$(RUNTIME_DIR)/concurrent/channel.c
NET_SRCS = ../$(RUNTIME_DIR)/net/socket.c ../$(RUNTIME_DIR)/net/http.c
IO_SRCS = ../$(RUNTIME_DIR)/io/iocp.c

ALL_RUNTIME_SRCS = $(ASYNC_SRCS) $(CONCURRENT_SRCS) $(NET_SRCS)

# Test executables
TEST_RUNTIME = ../$(BIN_DIR)/test_runtime.exe
TEST_NETWORK = ../$(BIN_DIR)/test_network.exe
BENCHMARK = ../$(BIN_DIR)/benchmark.exe
DEMO_SERVER = ../$(BIN_DIR)/demo_server.exe

.PHONY: all tests clean demos

# Build all tests
tests: $(TEST_RUNTIME) $(TEST_NETWORK)
	@echo "All tests built successfully!"

# Build demos
demos: $(DEMO_SERVER)
	@echo "Demo programs built successfully!"

# Build everything
all: tests demos $(BENCHMARK)
	@echo "All programs built successfully!"

# Runtime integration test
$(TEST_RUNTIME): test_casperix_runtime.c $(ALL_RUNTIME_SRCS)
	@echo "Building runtime tests..."
	@mkdir -p ../$(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS) -lws2_32

# Network test
$(TEST_NETWORK): test_networking.c $(NET_SRCS)
	@echo "Building network tests..."
	@mkdir -p ../$(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS) -lws2_32

# Benchmark suite
$(BENCHMARK): benchmark.c $(ALL_RUNTIME_SRCS)
	@echo "Building benchmarks..."
	@mkdir -p ../$(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS) -lws2_32

# HTTP Server demo
HTTP_SERVER_SRCS = ../$(RUNTIME_DIR)/net/http_server.c
$(DEMO_SERVER): demo_http_server.c $(ALL_RUNTIME_SRCS) $(HTTP_SERVER_SRCS)
	@echo "Building HTTP server demo..."
	@mkdir -p ../$(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS) -lws2_32

# Run all tests
run-tests: tests $(BENCHMARK)
	@echo "Running runtime tests..."
	@$(TEST_RUNTIME)
	@echo ""
	@echo "Running network tests..."
	@$(TEST_NETWORK)
	@echo ""
	@echo "Running benchmarks..."
	@$(BENCHMARK)

run-demo: $(DEMO_SERVER)
	@echo "Starting HTTP server demo..."
	@$(DEMO_SERVER)

clean:
	rm -f $(TEST_RUNTIME) $(TEST_NETWORK) $(BENCHMARK) $(DEMO_SERVER)

help:
	@echo "Casperix Test Suite"
	@echo "  make tests      - Build all tests"
	@echo "  make run-tests  - Build and run all tests"
	@echo "  make clean      - Remove test binaries"
