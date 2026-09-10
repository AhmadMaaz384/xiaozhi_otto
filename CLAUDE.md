# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project

XiaoZhi is an ESP-IDF C/C++ voice-assistant firmware supporting many chips, boards, displays, audio devices, and network transports. A single build selects exactly one board implementation via `DECLARE_BOARD(...)`; there are 138+ board directories and 171+ release variants.

Use ESP-IDF v6.0.2 when possible (`export.sh` must be sourced before any `idf.py`/build command). IDF 5.5.x is retained only for documented legacy boards.

## Commands

Source the intended ESP-IDF environment first:

```sh
source /path/to/esp-idf/export.sh
idf.py --version
```

```sh
# Discover exact board and variant names
python3 scripts/build.py --list-boards

# Canonical variant build (the only supported way to build a variant)
python3 scripts/build.py <board-directory> --name <variant-name>

# Host-side build-tooling tests (no ESP-IDF/hardware needed)
python3 -m unittest discover -s scripts/tests -v

# Run a single test
python3 -m unittest scripts.tests.test_build.VersionTests.test_parse_and_match

# Format/check touched files only
clang-format -i <files>
clang-format --dry-run -Werror <files>
```

`scripts/build.py` requires Python 3.9+ (uses `list[str]` type hints); it will fail with a `TypeError` on older interpreters. It rewrites the local `sdkconfig` and build state on every invocation — never assume `build/` still represents a previously built target after switching boards.

## Architecture

- `main/application.*` — main event loop, protocol lifecycle, and high-level behavior.
- `main/device_state_machine.*` — legal runtime state transitions; change state only through `Application::SetDeviceState()`.
- `main/boards/common/` — the `Board` interface plus reusable hardware/network helpers (wifi/ethernet/ml307/nt26/rndis board base classes, battery/backlight/button/knob/camera helpers, power/sleep timers).
- `main/boards/<vendor>/<board>/` — board-specific pins, initialization, and build variants. Each variant exports exactly one board factory via `DECLARE_BOARD(...)`.
- `main/audio/` — `AudioService` owns one input `AudioEngine` per build target and keeps codec I/O, Opus encode/decode, and network queues independent of the chip-specific speech pipeline. See `main/audio/README.md` for the full input/output data-flow diagrams. Key split:
  - ESP32-S3/P4/S31 → `AfeAudioEngine` (WakeNet/MultiNet inside a single shared AFE instance, FD AEC + VAD).
  - ESP32/C3/C5/C6 → `LiteAudioEngine` (standalone WakeNet, raw mono PCM, no AEC).
  - Dedicated tasks: `AudioInputTask`, `AudioOutputTask`, `OpusCodecTask` (encode uplink / decode downlink).
- `main/protocols/` — transport-neutral `Protocol` API plus `WebSocketProtocol` and `MqttProtocol` (MQTT + UDP). Shared message semantics live in `Protocol`; a change to the shared contract must be verified against both transports.
- `main/display/` and `main/led/` — reusable UI implementations (LVGL-based displays, emoji/expressions, LED strip effects).
- `main/mcp_server.*` — device-side MCP tool registration and dispatch (device control: speaker, LED, servo, GPIO, etc.).
- `main/ota.*` — OTA update flow; the OTA-reported board type/name is treated as persistent device identity.
- `main/Kconfig.projbuild` — board and feature configuration options.
- `main/CMakeLists.txt` — source, board, locale, font, and asset selection (very large, generated-adjacent — read the relevant section rather than the whole file).
- `scripts/build.py` — canonical board/variant build entry point; also generates the CI build matrix (`--list-boards --json`).

Board selection is a coupled chain that must be updated together when adding a board or variant:

```
config.json -> scripts/build.py -> main/Kconfig.projbuild -> main/CMakeLists.txt -> board source and config.h
```

A new board/variant needs: a unique board identity, correct chip target, flash/partition settings, exactly one `DECLARE_BOARD`, and board documentation — follow `docs/custom-board.md`. Read the closest existing board implementation before adding a new one, and prefer the narrowest owning layer — never put board-specific behavior into core modules; core code depends only on `Board` interfaces, never a concrete board class or board `config.h`.

## Required Rules

- Preserve unrelated worktree changes and keep patches focused.
- Never alter an existing board's pins to support different hardware — add a uniquely named board or release variant instead; board identity affects OTA compatibility.
- Treat camera, backlight, display, LED, battery, and similar capabilities as optional (guard for their absence).
- Callbacks may run outside the main task — schedule application-state mutations with `Application::Schedule()` or event bits, never mutate directly from a callback context.
- Do not block the main event loop or audio tasks. Avoid unbounded queues and repeated large allocations in audio paths.
- Validate network input and preserve `cJSON` ownership. NVS keys are a persistent API — changing one requires a migration.
- Guard target-specific features with Kconfig/component rules; do not assume every target has PSRAM or S3/P4-only resources.
- Do not manually edit generated/vendor output: `build/`, `releases/`, `managed_components/`, `components/`, `sdkconfig*`, `main/assets/lang_config.h`, or generated mmap headers.
- Format only touched C/C++ files with the repository `.clang-format` (Google C++ style base, 4-space indent, 100-col width, attach-style braces); avoid unrelated mass reformatting, and never hand-fix alignment after running clang-format.

## Validation

- Board-only change: build affected variants and smoke-test changed hardware.
- Core, common-board, audio, protocol, display, dependency, Kconfig, or CMake change: run host tests (`python3 -m unittest discover -s scripts/tests -v`) and build representative affected chip/network paths.
- Protocol changes: verify WebSocket and MQTT/UDP when shared behavior changes.
- Audio changes: verify capture, playback, wake/VAD, interruption, reconnect, and applicable AEC modes.
- UI/assets changes: verify applicable no-display/OLED/LVGL paths and partition size.
- Always report what was tested and what still needs physical hardware — a successful build is not hardware validation.

## Authoritative Documentation

- Overview and SDK policy: `README.md`
- SDK compatibility: `docs/esp-idf-6-migration.md`
- Board guide: `docs/custom-board.md`
- Audio design: `main/audio/README.md`
- Code style: `docs/code_style.md`
- Protocols: `docs/websocket.md`, `docs/mqtt-udp.md`, `docs/mcp-protocol.md`, `docs/mcp-usage.md`
- CI matrix: `.github/workflows/build.yml`

Keep detailed or fast-changing information in those files rather than duplicating it here. Add a nested `AGENTS.md`/`CLAUDE.md` only when a subsystem needs specialized instructions.
