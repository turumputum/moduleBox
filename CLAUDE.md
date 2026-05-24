# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project

**moduleBox** is ESP32-S3 firmware that turns the device into a 6-slot modular IO/automation hub. Each physical slot can be configured (via SD-card INI) to run a different role — button+LED, distance sensor, RFID, audio-over-LAN, stepper motor, MQTT/OSC/UDP bridge, etc. Built on **ESP-IDF v5.3.x + ESP-ADF**.

The Russian comments and docs are intentional — the project's primary maintainers work in Russian. Don't translate them when editing.

## Build & flash

The build is a standard ESP-IDF/ADF CMake project but with a custom **manifesto** preprocessor step. `IDF_PATH` and `ADF_PATH` must be exported (Windows VS Code extension config in [.vscode/settings.json](.vscode/settings.json) hard-codes `C:\Users\user\esp\v5.3.3\esp-idf` and `C:\Users\user\.espressif\esp-adf`).

Target chip: **esp32s3**. Partition table: [partitions.csv](partitions.csv) — factory app at `0x50000` (3M), SPIFFS storage at `0x450000` (3M).

Common entry points:
- `idf.py set-target esp32s3 && idf.py build` — standard full build
- [sbin/1_create_build.sh](sbin/1_create_build.sh) / [.cmd](sbin/1_create_build.cmd) — fresh `cmake -B build` (sources `script.env` for IDF/ADF — file is gitignored, must exist locally)
- [sbin/2_build.sh](sbin/2_build.sh) — incremental `make` inside `build/`
- [sbin/3_flash.sh](sbin/3_flash.sh) — `esptool` write_flash at `0x50000`, then opens gtkterm (Linux paths)
- [deploy.bat](deploy.bat) — bumps `VERSION` in [components/stateConfig/include/stateConfig.h](components/stateConfig/include/stateConfig.h), copies `build/moduleBox.bin` → `bootldsd/`, runs `bin2fw.exe`, then `git add . && git commit && git push`. Don't run this from Claude unless explicitly asked — it autocommits everything.

**Board pinout**: There are three pin maps (V3/V4/V6) in [me_slot_config.c:72-74](components/me_slot_config/me_slot_config.c#L72-L74). V3/V4 are selected at runtime from `me_config.boardVersion`. V6 requires the compile-time flag `BOARD_PINOUT_V6` in the top [CMakeLists.txt](CMakeLists.txt) (commented out by default).

The `manifesto/` subproject is a separate g++ host tool (`make` from `manifesto/`, output at `manifesto/.build/manifesto`); the prebuilt `manifesto.exe` lives at the repo root and is invoked at build time by [CMakeManifest.txt](CMakeManifest.txt).

## Tests

There's no integrated test harness. What exists:
- `tests/` — host-side C unit tests (`gcc`, simple [tests/Makefile](tests/Makefile)). Currently only builds `mblog`; `stdcommands_unit.c`, `options_parsing.c`, `varfun.c` are present but commented out — uncomment in the Makefile to build.
- `pyTest/` — MQTT/OSC/UDP integration scripts run against a flashed device.
- `micTest/` — Python audio-stream artifact detector for `audioLAN`/`opusLAN` modules; run as `python audio_artifact_detector.py <minutes>` after `pip install -r requirements.txt`.

## Architecture: slot-based dispatch

The whole system pivots on the **slot** abstraction. SD-card config (`config.ini`) declares `mode=` and `options=` per slot; on boot:

1. [main/main.c](main/main.c) initializes peripherals (SD, LAN/WIFI, MQTT, USB-CDC, executor, reporter, scheduler) and calls `init_slots()`.
2. [me_slot_config.c:86](components/me_slot_config/me_slot_config.c#L86) loops over `NUM_OF_SLOTS` (=10) and dispatches each `slot_mode[i]` string to its `start_<mode>_task(i)` — a giant `if/else if` chain at [me_slot_config.c:115-225](components/me_slot_config/me_slot_config.c#L115-L225). **To add a new module mode, you must add a branch here.**
3. Each `start_*_task` spawns a FreeRTOS task that owns the slot's pins, command queue, and reporting state.

Two globals carry shared state across the whole firmware (declared `extern`):
- `configuration me_config` — parsed config (slot modes, options, network, deviceName, version)
- `stateStruct me_state` — runtime queues and state ([stateConfig.h:39-83](components/stateConfig/include/stateConfig.h#L39-L83)). Critical fields: `command_queue[slot]`, `interrupt_queue[slot]`, `action_topic_list[slot]`, `trigger_topic_list[slot]`, `executor_queue`, `reporter_queue`.
- `SLOTS_PIN_MAP[10][4]` — slot → GPIO pins (populated from V3/V4/V6 pin map at boot).

## Module conventions (strict — see [COMPONENT_SKILL.md](COMPONENT_SKILL.md))

Every slot-mode module is a component under `components/<name>/` and **must** follow the same shape. The most common mistakes when adding/editing modules:

1. **Manifest header include**: every variant `.c` file (the per-mode implementation, not `_common.c`) needs `#include <generated_files/gen_<filename>.h>` near the top, AND a `get_manifest_<filename>()` function at the bottom returning `manifesto`. The filename in both must match the `.c` filename exactly. The CMakeLists.txt must list the file in `MODULE_FILES` (typically a `file(GLOB_RECURSE MODULE_FILES "*_variant1.c" "*_variant2.c")` plus `include(${CMAKE_HOME_DIRECTORY}/CMakeManifest.txt)` guarded by `if(NOT CMAKE_BUILD_EARLY_EXPANSION)`).

2. **Manifesto parses `/* ... */` doc comments statically** to build the SD-card-side `manifest-<VERSION>.json`. Constraints:
   - Every `get_option_*_val()` call should have a `/* description */` directly above it.
   - Every `stdcommand_register()` and `stdreport_register()` call must be **unconditional** — they cannot be inside `if`/`else` branches, or manifesto won't see them. To pick between variants at runtime, register all of them and store the selected ID in `currentReport`/etc.
   - **Forbidden characters inside doc comments**: `"`, `\`, `.` (they break the generated JSON string). Use `'` and `-` instead. Slashes, parens, `|` are fine.

3. **Task skeleton** (see COMPONENT_SKILL.md §5 for full template):
   ```c
   int slot_num = *(int*)arg;
   ctx_t ctx = CTX_DEFAULT();
   configure_<mod>(&ctx, slot_num);
   me_state.command_queue[slot_num] = xQueueCreate(15, sizeof(command_message_t));
   waitForWorkPermit(slot_num);
   while (1) {
       int cmd = stdcommand_receive(&ctx.cmds, &params, 0);
       /* handle commands, read inputs, stdreport_*() */
       vTaskDelayUntil(&lastWakeTime, ctx.refreshPeriod);
   }
   ```
   Stack size 4096–5120 bytes, priority `configMAX_PRIORITIES-5` (or `-10` for sensor reads). Task name `task_<mod>_<slot>`.

4. **Topics**: action topic (commands inbound) goes in `me_state.action_topic_list[slot_num]`, trigger topic (events outbound) in `trigger_topic_list[slot_num]`. Default form is `<deviceName>/<mode>_<slot>`; user override via `topic:<value>` option. Full protocol contract (topic structure, `event`/`action` directions, payload format, `enable` lifecycle, canonical name dictionary) is defined in [CONSTITUTION.md](CONSTITUTION.md) — read it before adding or changing any event/action names.

5. **Adding a new component**: after creating the directory, the new top-level `idf_component_register(... REQUIRES ...)` deps usually include `me_slot_config stateConfig reporter` plus whatever else (`rgbHsv arsenal` for LED stuff, `audioPlayer` for audio, etc.). Then add the `start_<mode>_task` branch in [me_slot_config.c init_slots()](components/me_slot_config/me_slot_config.c).

## Manifest pipeline (gotcha)

The manifest system uses a two-step trick to avoid CMake configure-time accumulation bugs across rebuilds:

- [CMakeLists.txt:7-8](CMakeLists.txt#L7-L8) clears `MODULE_FUNCTIONS` and `MODULE_FUNCTIONS_DEFS` cache vars at the start of each configure.
- [CMakeManifest.txt](CMakeManifest.txt) is included from each component's `CMakeLists.txt` and appends `MOD(<filename>),` and `MODDEF(<filename>) ` to those cache vars per variant `.c` it processes.
- At **build time** (not configure time), [components/manifest/gen_manifest_modules.cmake](components/manifest/gen_manifest_modules.cmake) reads the now-complete `CMakeCache.txt` and emits `manifest_modules.h` with the full `MODULE_FUNCTIONS`/`MODULE_FUNCTIONS_DEFS` macros.
- [components/manifest/manifest.c](components/manifest/manifest.c) uses those macros to build a function-pointer table calling every `get_manifest_<name>()` and concatenates results into `/sdcard/manifest-<VERSION>.json` on first boot for the configured VERSION.

If a module's manifest is missing on the SD card, suspect: (a) the variant file isn't in any `MODULE_FILES`, (b) `get_manifest_<name>()` is missing or misnamed, (c) `gen_<name>.h` isn't included so `manifesto` symbol is undeclared, (d) `VERSION` in stateConfig.h changed but old `manifest-*.json` still exists — manifest.c only writes if the versioned file is missing. Bumping VERSION (or deleting the SD file) regenerates.

## Where to look first

- New module mode → [COMPONENT_SKILL.md](COMPONENT_SKILL.md) (full Russian-language template + checklist), then mimic [components/distanceSens/](components/distanceSens/) (sensor-style) or [components/buttonLeds/](components/buttonLeds/) (input+output split). All topic/payload/lifecycle rules → [CONSTITUTION.md](CONSTITUTION.md).
- Slot dispatch / option parsing → [me_slot_config.c](components/me_slot_config/me_slot_config.c) (`get_option_flag_val`, `get_option_int_val`, `get_option_float_val`, `get_option_string_val`, `get_option_enum_val`).
- Reporter / publish path (MQTT/OSC/UDP fan-out) → [components/reporter/](components/reporter/) and `stdreport.c` in `me_slot_config`.
- Boot sequence → [main/main.c](main/main.c).
- Global config struct fields → [components/stateConfig/include/stateConfig.h](components/stateConfig/include/stateConfig.h).
