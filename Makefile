CC     = gcc
CFLAGS = -std=c17 -Wall -Wextra -Werror
BUILD  = build

# macOS hides mkdtemp() when _XOPEN_SOURCE is set; this brings it back.
# Linux and Pi builds are unchanged.
ifeq ($(shell uname -s),Darwin)
CFLAGS += -D_DARWIN_C_SOURCE
endif

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
