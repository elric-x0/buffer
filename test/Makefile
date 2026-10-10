CC      ?= cc
WARN    := -std=gnu11 -Wall -Wextra -Wpedantic -Wshadow -Wno-format-truncation
SAN     := -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer
SRC     := ../src/buffer.c
TEST    := ./test.main.c
HDR     := ../include/buffer.h
BUILD   := build

.PHONY: test test-plain test-valgrind clean

# Default: AddressSanitizer + UndefinedBehaviorSanitizer (also detects leaks)
test: $(BUILD)/test_asan
	ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=print_stacktrace=1 ./$(BUILD)/test_asan

# No sanitizers; additionally runs the allocation-failure test (RLIMIT_AS)
test-plain: $(BUILD)/test_plain
	./$(BUILD)/test_plain

test-valgrind: $(BUILD)/test_plain
	valgrind --leak-check=full --error-exitcode=3 ./$(BUILD)/test_plain

$(BUILD)/test_asan: $(SRC) $(TEST) $(HDR) | $(BUILD)
	$(CC) $(WARN) -g -O1 $(SAN) $(SRC) $(TEST) -o $@

$(BUILD)/test_plain: $(SRC) $(TEST) $(HDR) | $(BUILD)
	$(CC) $(WARN) -g -O1 $(SRC) $(TEST) -o $@

$(BUILD):
	mkdir -p $(BUILD)

clean:
	rm -rf $(BUILD)
