CC     = gcc
CFLAGS = -std=c17 -Wall -Wextra -Werror
BUILD  = build
ASAN   = $(BUILD)/asan

# macOS hides mkdtemp() when _XOPEN_SOURCE is set; this brings it back.
# Linux and Pi builds are unchanged.
ifeq ($(shell uname -s),Darwin)
CFLAGS += -D_DARWIN_C_SOURCE
endif

APPLOG_SRC = tests/test_applog.c src/store/applog.c
APPLOG_DEP = src/store/applog.h src/common/sample.h
MOLD_SRC   = tests/test_mold.c src/analysis/mold.c
MOLD_DEP   = src/analysis/mold.h

.PHONY: all test asan clean

all: $(BUILD)/test_applog $(BUILD)/test_mold

# Build and run all tests. Both always run, so a failure in one does not
# hide the other; the target fails if either failed.
test: $(BUILD)/test_applog $(BUILD)/test_mold
	@status=0; \
	./$(BUILD)/test_applog || status=1; \
	./$(BUILD)/test_mold || status=1; \
	exit $$status

# Build both tests with AddressSanitizer and UBSan, then run them.
asan: $(ASAN)/test_applog $(ASAN)/test_mold
	@status=0; \
	./$(ASAN)/test_applog || status=1; \
	./$(ASAN)/test_mold || status=1; \
	exit $$status

$(BUILD)/test_applog: $(APPLOG_SRC) $(APPLOG_DEP) | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $(filter %.c,$^)

$(BUILD)/test_mold: $(MOLD_SRC) $(MOLD_DEP) | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $(filter %.c,$^) -lm

$(ASAN)/test_applog: $(APPLOG_SRC) $(APPLOG_DEP) | $(ASAN)
	$(CC) $(CFLAGS) -fsanitize=address,undefined -g -o $@ $(filter %.c,$^)

$(ASAN)/test_mold: $(MOLD_SRC) $(MOLD_DEP) | $(ASAN)
	$(CC) $(CFLAGS) -fsanitize=address,undefined -g -o $@ $(filter %.c,$^) -lm

$(BUILD) $(ASAN):
	mkdir -p $@

clean:
	rm -rf $(BUILD)
