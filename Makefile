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
SENSORS_SRC = tests/test_sensors.c src/sensors/ds18b20.c src/sensors/si7021.c
SENSORS_DEP = src/sensors/ds18b20.h src/sensors/si7021.h src/common/sample.h
READER_SRC  = src/sensors/ds18b20_reader.c src/sensors/ds18b20.c
READER_DEP  = src/sensors/ds18b20.h src/common/sample.h
SI_READER_SRC = src/sensors/si7021_reader.c src/sensors/si7021_i2c.c src/sensors/si7021.c
SI_READER_DEP = src/sensors/si7021.h src/sensors/si7021_i2c.h src/common/sample.h
MOLDDET_SRC = src/app/molddet.c src/store/applog.c src/analysis/mold.c \
              src/sensors/ds18b20.c src/sensors/si7021.c src/sensors/si7021_i2c.c
MOLDDET_DEP = src/store/applog.h src/analysis/mold.h src/sensors/ds18b20.h \
              src/sensors/si7021.h src/sensors/si7021_i2c.h src/common/sample.h

.PHONY: all test asan clean

all: $(BUILD)/test_applog $(BUILD)/test_mold $(BUILD)/test_sensors $(BUILD)/ds18b20_reader

# Build and run all tests. All always run, so a failure in one does not
# hide the others; the target fails if any failed.
test: $(BUILD)/test_applog $(BUILD)/test_mold $(BUILD)/test_sensors
	@status=0; \
	./$(BUILD)/test_applog || status=1; \
	./$(BUILD)/test_mold || status=1; \
	./$(BUILD)/test_sensors || status=1; \
	exit $$status

# Build all tests with AddressSanitizer and UBSan, then run them.
asan: $(ASAN)/test_applog $(ASAN)/test_mold $(ASAN)/test_sensors
	@status=0; \
	./$(ASAN)/test_applog || status=1; \
	./$(ASAN)/test_mold || status=1; \
	./$(ASAN)/test_sensors || status=1; \
	exit $$status

$(BUILD)/test_applog: $(APPLOG_SRC) $(APPLOG_DEP) | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $(filter %.c,$^)

$(BUILD)/test_mold: $(MOLD_SRC) $(MOLD_DEP) | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $(filter %.c,$^) -lm

$(BUILD)/test_sensors: $(SENSORS_SRC) $(SENSORS_DEP) | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $(filter %.c,$^)

# The DS18B20 sensor process.
$(BUILD)/ds18b20_reader: $(READER_SRC) $(READER_DEP) | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $(filter %.c,$^)

# The Si7021 sensor process. It uses Linux i2c-dev, so it is only part of
# `all` on Linux.
$(BUILD)/si7021_reader: $(SI_READER_SRC) $(SI_READER_DEP) | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $(filter %.c,$^)

# The single-process mold detector. Linux only, like the Si7021 reader.
$(BUILD)/molddet: $(MOLDDET_SRC) $(MOLDDET_DEP) | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $(filter %.c,$^) -lm

ifeq ($(shell uname -s),Linux)
all: $(BUILD)/si7021_reader $(BUILD)/molddet
endif

$(ASAN)/test_applog: $(APPLOG_SRC) $(APPLOG_DEP) | $(ASAN)
	$(CC) $(CFLAGS) -fsanitize=address,undefined -g -o $@ $(filter %.c,$^)

$(ASAN)/test_mold: $(MOLD_SRC) $(MOLD_DEP) | $(ASAN)
	$(CC) $(CFLAGS) -fsanitize=address,undefined -g -o $@ $(filter %.c,$^) -lm

$(ASAN)/test_sensors: $(SENSORS_SRC) $(SENSORS_DEP) | $(ASAN)
	$(CC) $(CFLAGS) -fsanitize=address,undefined -g -o $@ $(filter %.c,$^)

$(BUILD) $(ASAN):
	mkdir -p $@

clean:
	rm -rf $(BUILD)
