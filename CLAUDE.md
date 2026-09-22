# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project status

This repository currently contains only a problem memo (`README.md`); no source code, build system, or tests exist yet. There are no build/lint/test commands to run at this stage. Update this file once code is added.

## Project overview

CS370 term project by Moises Ramirez and Rishab Karki: an embedded IoT device that monitors a washing machine area for conditions (warmth + humidity) that promote mold/mildew growth, since a phone cannot take these measurements continuously and unattended.

- **Sensors**: Adafruit Si7021 (humidity) and waterproof DS18B20 (digital temperature). The two sensors' readings are meant to corroborate each other — humidity indicates mold-favorable conditions, temperature is checked against the mold-growth range to confirm.
- **Candidate mechanisms** (first guess, from the Section 3.2 menu, subject to change per the design doc):
  - **F** — High rate sampling pipeline
  - **D** — Custom storage layer
- **Primary risk**: unreliable temperature/humidity readings leading to false positives.
