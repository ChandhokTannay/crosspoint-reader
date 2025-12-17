# WARP.md

This file provides guidance to WARP (warp.dev) when working with code in this repository.

## Key commands

### One-time setup

- Clone with submodules (required; `open-x4-sdk/` is a git submodule):
  ```sh
  git clone --recursive https://github.com/daveallie/crosspoint-reader
  # or
  git submodule update --init --recursive
  ```

- Install PlatformIO Core (provides `pio`):
  ```sh
  python -m pip install --upgrade platformio
  ```

### Build / flash

- Build default firmware (`[env:default]`):
  ```sh
  pio run
  ```

- Build release firmware (`[env:gh_release]`, used by `.github/workflows/release.yml`):
  ```sh
  pio run -e gh_release
  ```

- Flash to a connected device:
  ```sh
  pio run --target upload
  ```

- Serial monitor (115200 baud configured in `platformio.ini`):
  ```sh
  pio device monitor -b 115200
  ```

### Lint / static analysis / formatting

- Run static analysis (cppcheck via PlatformIO; see `platformio.ini` `check_tool=cppcheck`):
  ```sh
  pio check --fail-on-defect medium --fail-on-defect high
  ```

- Apply formatting (repo-standard `clang-format` via `.clang-format`):
  ```sh
  ./bin/clang-format-fix
  ```

  CI runs this and fails if it produces a diff (see `.github/workflows/ci.yml`).

### Tests

This repo currently has no PlatformIO tests checked in (only `test/README`). If/when tests are added under `test/`, they can be run with:

- All tests:
  ```sh
  pio test
  ```

- A single test or suite (filter):
  ```sh
  pio test -f <pattern>
  ```

## High-level architecture

### Firmware entry point and app lifecycle

- `src/main.cpp` is the Arduino entry point (`setup()` / `loop()`). It:
  - Initializes hardware abstractions from the `open-x4-sdk` submodule (`EInkDisplay`, `InputManager`, `BatteryMonitor`).
  - Initializes SD card access (`SD.begin(...)`) and loads persisted state:
    - `SETTINGS` from `src/CrossPointSettings.*`
    - `APP_STATE` from `src/CrossPointState.*`
  - Instantiates fonts and registers them with `GfxRenderer`.
  - Manages the top-level “screen stack” via a single `Activity* currentActivity` and explicit transitions (`exitActivity()`, `enterNewActivity(...)`).
  - Owns power/deep-sleep behavior: verifies long-press on wake, auto-sleeps after inactivity, and enters deep sleep on a power-button long press.

### UI/screens: “Activities”

- UI is organized into “Activities” under `src/activities/`.
  - Base interface: `src/activities/Activity.h` (`onEnter`, `loop`, `onExit`).
  - Nested UI: `src/activities/ActivityWithSubactivity.*` manages an owned `subActivity` for flows like “Reader → file picker → reader”.

- Many activities render from a dedicated FreeRTOS task (`xTaskCreate`) and guard display updates with a semaphore to avoid interrupting EPD transactions:
  - Home screen: `src/activities/home/HomeActivity.*`
  - File picker: `src/activities/reader/FileSelectionActivity.*`
  - Reader: `src/activities/reader/EpubReaderActivity.*`
  - Settings: `src/activities/settings/SettingsActivity.*`

- Navigation flow (top-level):
  - Boot splash: `src/activities/boot_sleep/BootActivity.*`
  - Home: `HomeActivity` → “Read” opens `ReaderActivity`, “Settings” opens `SettingsActivity`.
  - Reader flow: `ReaderActivity` selects an EPUB (via `FileSelectionActivity`) or opens the last book from `APP_STATE.openEpubPath`.

### Rendering: `GfxRenderer` over `EInkDisplay`

- `lib/GfxRenderer/` wraps `EInkDisplay` with:
  - Portrait-oriented coordinates (internally rotates to the display’s orientation).
  - Primitive drawing + text layout (using `EpdFontFamily`).
  - Grayscale rendering support by rendering twice into separate grayscale buffers.
  - Chunked buffering to avoid large contiguous allocations (see `storeBwBuffer()` / `restoreBwBuffer()`).

- Fonts:
  - Built-in font headers live under `lib/EpdFont/builtinFonts/`.
  - `src/config.h` defines stable numeric font IDs (generated via a ruby script comment in that file) that are used throughout rendering.

### EPUB parsing and SD-based caching

- EPUB I/O and parsing lives under `lib/Epub/`:
  - `lib/Epub/Epub.*` opens the `.epub` as a zip (`ZipFile`) and parses `container.xml`, `content.opf`, and NCX TOC using streaming parsers (to minimize heap usage).

- Reader rendering pipeline:
  - `src/activities/reader/EpubReaderActivity.*` drives page/chapter navigation.
  - Pagination and cached page data are handled by `Epub/Section` and `Epub/Page` (used by `EpubReaderActivity`).

- Caching is aggressively SD-based to cope with ESP32-C3 RAM constraints:
  - EPUB cache root: `/.crosspoint/` on the SD card.
  - Each EPUB caches under `/.crosspoint/epub_<hash>/` where `<hash>` is derived from the EPUB filepath (see `Epub` constructor).
  - See `README.md` “Internals → EPUB caching” for cache layout details.

### Persistence (settings + last-opened book)

- Settings: `src/CrossPointSettings.*` → `/sd/.crosspoint/settings.bin`.
- App state: `src/CrossPointState.*` → `/sd/.crosspoint/state.bin`.
- Both use `lib/Serialization/Serialization.h` to serialize PODs/strings.

  Note: `CrossPointSettings::loadFromFile()` checks existence via `SD.exists(SETTINGS_FILE + 3)` (skipping the `/sd` prefix) because Arduino’s `SD` API expects SD paths without the `/sd` mount prefix, while `std::fstream` uses the `/sd/...` path.

### External dependency: `open-x4-sdk/` submodule

- `open-x4-sdk/` is the OpenX4 community SDK (included as a git submodule; see `.gitmodules`).
- PlatformIO uses `symlink://open-x4-sdk/...` entries in `platformio.ini` to include:
  - `BatteryMonitor`
  - `InputManager`
  - `EInkDisplay`
