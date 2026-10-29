CC     = gcc
CFLAGS = -std=c17 -Wall -Wextra -Werror
BUILD  = build

.PHONY: all test clean

all: $(BUILD)/test_applog

# Build and run the storage (Mechanism D) tests.
test: $(BUILD)/test_applog
	./$(BUILD)/test_applog

$(BUILD)/test_applog: tests/test_applog.c src/store/applog.c src/store/applog.h src/common/sample.h | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $(filter %.c,$^)

$(BUILD):
	mkdir -p $@

clean:
	rm -rf $(BUILD)
