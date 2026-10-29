# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project status

The repository contains a problem memo (`README.md`), a design document (`DESIGN.md`), and a skeleton of empty source, test, and build files. No code has been written yet, and the `Makefile` is empty, so there are no build/lint/test commands to run at this stage. Update this file once code and build targets are added.

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

All files below currently exist as empty placeholders.

- `src/common/sample.h` — shared reading/sample type
- `src/sensors/si7021_reader.c` — humidity sensor process
- `src/sensors/ds18b20_reader.c` — temperature sensor process
- `src/hub/hub.c` — central process that receives readings, runs analysis, and writes storage
- `src/supervisor/supervisor.c` — watches and restarts the sensor/hub processes (Mechanism E)
- `src/store/applog.{c,h}` — append-only log and recovery (Mechanism D)
- `src/analysis/mold.{c,h}` — logic combining both readings to flag mold-favorable conditions
- `tests/test_applog.c`, `tests/test_mold.c` — tests for storage and detection logic
- `Makefile` — build file

## Failure modes to handle

Per `DESIGN.md`: either sensor failing (keep collecting from the other sensor, retry the failed one, log error and recovery), process crash (supervisor restarts it and logs), power outage (recover valid records on boot and log restart), and full storage (report it, pause writes until space frees up, log when possible).

## Ownership

- **Moises**: Mechanism D (storage, log file, recovery, power-loss testing).
- **Rishab**: Mechanism E (multi-process system, IPC, supervisor).
- **Shared**: sensor bring-up, mold detection logic and its tests, integration, 48-hour run, final documentation.

## AI-use notes

Both team members review and test any AI-generated code. Keep Claude Code transcripts to document AI use.
