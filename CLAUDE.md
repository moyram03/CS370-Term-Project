# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project status

The repository contains a problem memo (`README.md`), a design document (`DESIGN.md`), and the source tree. The storage layer (Mechanism D) is implemented and tested: `src/common/sample.h`, `src/store/applog.{c,h}`, and `tests/test_applog.c`. The mold analysis interface is in `src/analysis/mold.h`; `mold_sat_vapor_hpa`, `mold_dew_point_c`, `mold_surface_rh`, `mold_init`, and `mold_update` are implemented and tested. `mold_update` accumulates wet hours and runs the OK/WATCH/RISK level state machine with enter/exit hysteresis. The sensor readers, hub, and supervisor are still empty placeholders and are being worked on separately.

## Build and test

```
make test    # build and run all tests (build/test_applog, build/test_mold)
make asan    # same tests under AddressSanitizer + UBSan (build/asan/)
make         # build only
make clean   # remove build/
```

The Makefile compiles with `gcc -std=c17 -Wall -Wextra -Werror`, so any warning fails the build. Build output goes to `build/`, which is git-ignored. `make test` runs both test binaries even if the first fails, and fails if either did. The storage and mold math tests are wired up so far; add targets for the other components as their code lands.

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
- `Makefile`, `.gitignore`

Rishab's sessions write these:

- `src/analysis/mold.{c,h}` — logic combining both readings to flag mold-favorable conditions (shared; Moises reviews). Pure computation: inputs must be finite, and the hub only passes readings whose status is `SAMPLE_OK`. Math, `mold_init`, and `mold_update` (wet-hour accumulation plus level state machine) done. With no surface reading, `degraded` is set and condensation is never reported.
- `tests/test_mold.c` — tests for the mold math, wet-hour accumulation, and the level state machine
- `src/sensors/si7021_reader.c` — humidity sensor process (empty placeholder)
- `src/sensors/ds18b20_reader.c` — temperature sensor process (empty placeholder)

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
