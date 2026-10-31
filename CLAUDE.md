# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project status

The repository contains a problem memo (`README.md`), a design document (`DESIGN.md`), and the source tree. The storage layer (Mechanism D) is implemented and tested: `src/common/sample.h`, `src/store/applog.{c,h}`, and `tests/test_applog.c`. The mold analysis interface is in `src/analysis/mold.h`; `mold_sat_vapor_hpa`, `mold_dew_point_c`, `mold_surface_rh`, `mold_init`, and `mold_update` are implemented and tested. `mold_update` accumulates wet hours and runs the OK/WATCH/RISK level state machine with enter/exit hysteresis. The sensor readers are implemented, and `src/app/molddet.c` ties the sensors, storage, and mold analysis together in one process. The hub and supervisor are still empty placeholders and are being worked on separately.

## Build and test

```
make test    # build and run all tests (build/test_applog, test_mold, test_sensors)
make asan    # same tests under AddressSanitizer + UBSan (build/asan/); does not run on the Pi kernel
make         # build only (also builds build/ds18b20_reader, and build/si7021_reader and build/molddet on Linux)
make clean   # remove build/
```

The Makefile compiles with `gcc -std=c17 -Wall -Wextra -Werror`, so any warning fails the build. Build output goes to `build/`, which is git-ignored. `make test` runs every test binary even if an earlier one fails, and fails if any did. The storage, mold, and DS18B20 parser tests are wired up so far; add targets for the other components as their code lands. On the Pi, run `valgrind --error-exitcode=1 --leak-check=full -q build/<test>` on the three test binaries instead of `make asan`.

`DESIGN.md` is the source of truth for architecture and requirements; where it differs from `README.md` (e.g. mechanism choices), follow `DESIGN.md`.

## Project overview

CS370 term project by Moises Ramirez and Rishab Karki: an embedded IoT device that monitors a washing machine area for conditions (warmth + humidity) that promote mold/mildew growth, since a phone cannot take these measurements continuously and unattended.

- **Hardware**: Raspberry Pi 4 Model B with an Adafruit Si7021 (humidity) and a waterproof DS18B20 (digital temperature). Humidity indicates mold-favorable conditions; temperature is checked against the mold-growth range to confirm.
- **Language**: C.
- **Primary risk**: unreliable temperature/humidity readings leading to false positives.

## Mechanisms (from DESIGN.md)

- **D — Custom storage layer**: append-only log file of timestamped readings, stored locally on the Pi. Readings every 30 seconds, `fsync()` every 5 minutes, 30-day retention. On restart, scan for incomplete records and recover valid data (power-loss recovery).
- **E — Multi-process system**: a separate process per sensor sends readings to a central hub process that handles analysis and storage. A supervisor monitors the processes and restarts any that stop. Sensors run asynchronously so the slow DS18B20 does not block the Si7021.

The README's original guess of mechanism F (high rate sampling pipeline) was replaced by E.

## Code layout

Implemented:

- `src/common/sample.h` — shared reading type `sample_t` (see below)
- `src/store/applog.{c,h}` — append-only log and recovery (Mechanism D)
- `tests/test_applog.c` — storage tests (append/read-back, record layout, recovery, partial writes, `read_all`)
- `Makefile`, `.gitignore` (ignores `build/` and `*.log`)

Rishab's sessions write these:

- `src/analysis/mold.{c,h}` — logic combining both readings to flag mold-favorable conditions (shared; Moises reviews). Pure computation: inputs must be finite, and the hub only passes readings whose status is `SAMPLE_OK`. Math, `mold_init`, and `mold_update` (wet-hour accumulation plus level state machine) done. With no surface reading, `degraded` is set and condensation is never reported.
- `tests/test_mold.c` — tests for the mold math, wet-hour accumulation, and the level state machine
- `src/sensors/ds18b20.{c,h}` — DS18B20 support. `ds18b20_parse` is a pure parser for the `w1_slave` text: `SAMPLE_OK` with the temperature, or `SAMPLE_ERR_READ` for a bad CRC verdict, missing/malformed `t=`, the 85000 power-on value, out-of-range values, an all-zero scratchpad (stuck-low data line), or short/NULL input. `ds18b20_find` returns the `w1_slave` path of the smallest-named `28-*` entry in a 1-Wire folder (ignoring `00-*` ghosts such as `00-400000000000` and `00-800000000000`). `ds18b20_read` reads that file into a bounded buffer and parses it.
- `src/sensors/ds18b20_reader.c` — the temperature sensor process: every `--interval` seconds (default 30) it finds the device (re-finding after any failure so replugging works), reads it, fills a `sample_t` (`SENSOR_DS18B20_TEMP`, `seq` incremented on every reading including failures), and prints one line per sample to stdout; the hub connection comes later. Flags: `--w1-dir` (default `/sys/bus/w1/devices`), `--interval`, `--once`. Exits cleanly on SIGINT/SIGTERM. With `--once` the exit status is 0 for an OK reading and 1 for an error; 2 is bad arguments.
- `src/sensors/si7021.{c,h}` — Si7021 pure functions, no I2C (that is in `si7021_i2c`). `si7021_crc8` (polynomial 0x31, initial value 0x00). `si7021_parse_rh` takes the 3-byte reply (MSB, LSB, CRC): bad CRC gives `SAMPLE_ERR_READ`, otherwise RH = 125·code/65536 − 6 with the 2 status bits masked, clamped to 0–100. `si7021_parse_temp` takes the 2-byte reply to command 0xE0 (no CRC): T = 175.72·code/65536 − 46.85 with the status bits masked, and anything outside −40..125 °C is `SAMPLE_ERR_READ` (rejected, not clamped). Outputs are left untouched on any error, and NULL arguments give `SAMPLE_ERR_READ`. The CRC is confirmed against live sensor readings on the Pi (every live humidity reply passed it).
- `tests/test_sensors.c` — tests for the DS18B20 parser, `ds18b20_find`, and `ds18b20_read` (the last two against a fake 1-Wire folder in a temp directory), and the Si7021 CRC and parsers
- `src/sensors/si7021_i2c.{c,h}` — Si7021 I2C access through Linux i2c-dev (Linux only, no unit test since it needs hardware; checked on the Pi). `si7021_open(bus)` opens the bus and addresses 0x40; `si7021_close(fd)`; `si7021_read(fd, &rh, &temp)` sends 0xF5 (humidity, no hold), waits 25 ms, then retries the 3-byte read every 5 ms while the sensor NACKs (`EREMOTEIO` or `ENXIO`) up to 100 ms after the command (`SAMPLE_ERR_TIMEOUT`), then sends 0xE0 and reads the 2-byte temperature from the same measurement. Any other failure is `SAMPLE_ERR_READ`. Outputs are only written on `SAMPLE_OK`, both together. EINTR is retried.
- `src/sensors/si7021_reader.c` — the humidity/air-temperature sensor process, same structure as `ds18b20_reader.c`: every `--interval` seconds (default 30) it opens the bus if needed, takes one reading, and prints two lines to stdout (`sensor=si7021_rh`, then `sensor=si7021_temp`); the hub connection comes later. Both samples share one `seq` counter (n and n+1, failures counted too). If the reading fails, both samples get the same error status, because the temperature comes from the humidity measurement, and the bus is closed so the next cycle reopens it. Errors and recovery are logged to stderr once each. Flags: `--bus` (default `/dev/i2c-1`), `--interval`, `--once`. Exit codes as for `ds18b20_reader` (with `--once`, 1 if either sample failed).

- `src/app/molddet.c` — single-process mold detector (Linux only, `build/molddet`; no unit test since it needs hardware; checked on the Pi). It stands in for the hub and the sensor processes until Mechanism E exists. Startup: `applog_open(--log, default ./molddet.log)` and prints `recovery: records_kept=N bytes_truncated=M`; an open failure exits 1. Every `--interval` seconds (default 30) it reads the Si7021 (bus `/dev/i2c-1`, reopened after any failure) and the DS18B20 (`/sys/bus/w1/devices`, re-found after any failure), builds three samples in the order humidity, Si7021 temp, DS18B20 temp with one shared `seq` counter (failures included, so `seq` goes up by 3 per cycle and resets to 0 on restart), and appends each; an append failure is logged to stderr once and does not stop the loop. It then calls `mold_update` (`have_air` only if both Si7021 samples are OK, `have_surface` only if the DS18B20 is OK; `dt` is monotonic seconds since the last successful update, 0 the first time) and prints one status line, or `no air data` if `mold_update` returns -1. `applog_sync` runs every `--sync-every` seconds (default 300, monotonic). SIGINT/SIGTERM stop it cleanly (`applog_close` fsyncs, exit 0). `--dump PATH` prints every record with `applog_read_all`, then `records: N`, and exits 0. **Known limitation:** `wet_hours` and the level are not saved, so they restart at 0 and OK after every restart, even though the log keeps the readings. Its small helpers (clock, `sleep_until`, name functions) are copies of those in the readers.

Still empty placeholders (owned/worked on separately; don't modify without asking):

- `src/hub/hub.c` — central process that receives readings, runs analysis, and writes storage
- `src/supervisor/supervisor.c` — watches and restarts the sensor/hub processes (Mechanism E)

`src/store/` is Moises's; don't modify it without asking.

## Data format (agreed; don't change without asking)

- **`sample_t`** is exactly 24 bytes (enforced by `_Static_assert`): `int64_t timestamp_ms`, `uint32_t seq`, `float value`, `uint8_t sensor`, `uint8_t status`, `uint8_t reserved[6]` (always 0). It is shared by the sensors, hub, and storage, and written raw to disk.
- **`sensor`** is a `sensor_id_t`: `SENSOR_SI7021_HUMIDITY = 1` (relative humidity, %RH), `SENSOR_DS18B20_TEMP = 2` (surface temperature, °C), `SENSOR_SI7021_TEMP = 3` (air temperature, °C; the mold analysis combines it with the humidity to compute dew point). The struct layout is unchanged.
- **`seq`** is a per-sensor-process counter that starts at 0 when the process starts and increments on every reading, including failed reads. A gap means lost readings; a reset to 0 means the process restarted.
- **Log record** is exactly 32 bytes: `[magic "MOLD" 4B][sample_t 24B][crc32 of bytes 0..27, 4B]`, in native (little-endian) byte order, no file header.

## Storage behavior (`applog`)

- `applog_open()` always runs recovery: it scans records from the start and truncates the file at the first cut-off or bad (magic/CRC) record, then fsyncs. Damage in the middle of the file therefore also drops the good records after it; this is the chosen behavior for now and is locked in by a test.
- `applog_append()` writes with `O_APPEND` and only hands data to the OS. If a write fails partway (e.g. disk full), it truncates the partial record and returns -1 with `errno` set; the caller decides what to do.
- `applog_sync()` is just `fsync()`. The 5-minute fsync timer belongs to the hub, not to `applog`.
- `applog_read_all()` callbacks return 0 to continue or 1 to stop; it returns 0 (read everything), 1 (stopped early), or -1 (I/O error), and never modifies the file.
- Not yet implemented: 30-day retention (likely one log file per day, deleting files older than 30 days).

## Failure modes to handle

Per `DESIGN.md`: either sensor failing (keep collecting from the other sensor, retry the failed one, log error and recovery), process crash (supervisor restarts it and logs), power outage (recover valid records on boot and log restart), and full storage (report it, pause writes until space frees up, log when possible).

## Ownership

- **Moises**: Mechanism D (storage, log file, recovery, power-loss testing).
- **Rishab**: Mechanism E (multi-process system, IPC, supervisor).
- **Shared**: sensor bring-up, mold detection logic and its tests, integration, 48-hour run, final documentation.

In Rishab's sessions, Claude writes `src/sensors/` and `src/analysis/` (plus `tests/test_mold.c`). The mold logic is shared work, so Moises reviews it. `src/store/` stays Moises's.

## Workflow

- Plan first for multi-file or algorithmic changes, and wait for Rishab's approval before writing code.
- A change is only done when `make test` passes.
- Commit messages use the form `M<n>: <what>`.
- Never weaken, skip, or delete a test to make it pass.

## Style

C17; use full `if`/`else` blocks instead of the ternary `?:` operator.

## AI-use notes

Both team members review and test any AI-generated code. Keep Claude Code transcripts to document AI use.
