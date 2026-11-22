# Astralis

A high-performance **C++20** rhythm game engine in the style of Friday Night Funkin', built with **raylib 5.5**, **Lua 5.4**, **HScript**, and **OpenGL shaders**. Designed for full Psych Engine / JS Engine / V-Slice / Kade mod compatibility, massive spam charts, and fast multi-platform builds across Windows, Linux, macOS, and Android.

**Wiki:** [`docs/wiki`](docs/wiki/Home.md) — Lua API (326 functions & callbacks), HScript, shaders, chart formats, editors, mods, and options.

---

## Downloads

| Package | Platform | Notes |
|---|---|---|
| `Astralis-windows-x64-avx2.zip` | Windows 7/8/10/11 (64-bit, AVX2) | Intel 4th gen+ / AMD Ryzen — extract and run `astralis.exe` |
| `Astralis-windows-x64-sse4.zip` | Windows 7/8/10/11 (64-bit, SSE4) | Intel 3rd gen (Ivy Bridge) & older 64-bit CPUs |
| `Astralis-windows-vista-7-x86-sse2.zip` | Windows Vista/7+ (32-bit, pure SSE2) | OpenGL 2.1, static runtime (no extra DLLs required) |
| `Astralis-linux-x64-avx2.zip` | Linux x64 (AVX2) | Extract and run `./astralis` |
| `Astralis-linux-x64-sse4.zip` | Linux x64 (SSE4) | Extract and run `./astralis` |
| `Astralis-macos-arm64.zip` | macOS (Apple Silicon) | First launch: right-click `astralis` → Open |
| `Astralis-android.apk` | Android 7+ (`arm64` / `armv7` / `x86_64`) | Install APK and allow from this source |

---

## Key Features

- **Mod & Format Compatibility**
  - **Psych Engine 0.7.3 & JS Engine:** Sparrow/Packer atlases, characters, 0.7.3 stage objects, cameras, tweens, timers, sandboxed **Lua 5.4** (326 functions), **HScript** (`runHaxeCode`, `.hx` scripts), translated GLSL fragment shaders (per-sprite shader layers & camera filters), and video cutscenes (`.mpg` native, `.mp4`/`.webm` via `ffmpeg`).
  - **Charts & Layouts:** Psych JSON, V-Slice, Kade/base-game builds, PulseForge charts, `.flp`/`.fsc` FL Studio charts, and binary `.ghc` charts. Supports **1–100 keys**, per-side key counts, and the built-in **`Change Key Count`** mid-song event.
  - **Bundled Content:** Includes Psych Engine 0.7.3 shared UI assets, **Tutorial**, and **Week 1** (`Bopeebo`, `Fresh`, `Dad Battle`). Drop mod folders or `.zip` archives into `mods/` (or press `M` in Freeplay for mod packs).

- **Performance & Low Latency**
  - **Streaming Engine:** Multi-core SIMD JSON chart streaming (handles multi-gigabyte / multi-million-note charts with bounded memory), background streaming audio decode (instant song start), and parallel texture decoding.
  - **Binary `.ghc` v3 Charts:** Compact columnar chart format with configurable compression (`None`, `Deflate`, `RLE + Deflate`), strength (`1–9`), and block size.
  - **Note Renderer:** GPU rendering or CPU software rasterizer with hand-written assembly (**x86 SSE2** on 32-bit/64-bit x86, **ARM64 NEON** on Android/Apple Silicon).
  - **Timing & Input:** Audio-clock driven timing, ~1 kHz timestamped input polling, low-latency V-Sync frame pacing, configurable audio buffer latency, and 64-bit score/combo/note counters.

- **Built-in Editors** (Main Menu → `7`)
  - **Chart Editor:** Song, Section, Note, Events, Charting, EZ Spam, and **Save** tabs (Psych JSON with optional minification and separate `events.json`, or `.ghc` v3 compression settings), plus live preview and undo/redo.
  - **Modchart Editor:** Visual beat timeline, live strumline preview, 10 timed modchart properties with easing curves, and `modchart.lua` export/reload.
  - **SNIFF Importer:** Open `.flp`/`.fsc` files in-engine, configure custom Opponent & Player MIDI note ranges for multikey charts, and export to Psych JSON or `.ghc`.
  - **Character, Stage, Event, Script Editors & Chart Converter:** Full in-engine authoring with touch-friendly mobile layouts.

- **UI & Extras**
  - Categorized Psych-style Options hub (Note Colors, Controls for 1–18 keys, Delay & Combo, Visuals & UI, Graphics, Gameplay, Audio & Extras), deterministic replays (`R` on results), audio visualizer, `F3` debug overlay, and touch controls on Android.

---

## Controls

| Action | Keys |
|---|---|
| Notes (4K default) | `D F J K` or `← ↓ ↑ →` (rebindable for 1–18 keys in Options → Controls) |
| Pause / Confirm / Back | `Enter` / `Esc` |
| Main Menu | `7` Editors Menu |
| Freeplay | `F` Search · `Tab` Modifiers · `7` Chart Editor · `M` Mod Packs · `R` Rescan |
| In-Song | `7` Chart Editor · `F3` Debug Overlay · `Ctrl` Bot Energy |
| Volume | `-` / `=` Down / Up · `0` Mute |
| Results / Window | `R` Watch Replay · `F11` Fullscreen |

---

## Building from Source

Requires **CMake ≥ 3.20** and a **C++20** compiler. Dependencies (`raylib 5.5`, `Lua 5.4.7`, `miniz 3.0.2`, `pl_mpeg`) are fetched automatically by CMake (or loaded from `third_party/` if present).

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel
./build/astralis          # Windows: build\Release\astralis.exe
```

**Build options:**
- `-DGH_CPU_ISA=SSE4|AVX2|SSE2` — select target x86 ISA (defaults to `SSE4` on x64, pure `SSE2` on 32-bit x86).
- `-DGH_WINDOWS_LEGACY=ON` — target Windows Vista/7+ (OpenGL 2.1, subsystem 6.0).
- `-DGH_BUILD_GAME=OFF` — build only `gh_core` and the headless `gh_tests` suite.

**Linux dependencies:**
`libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev libgl1-mesa-dev libasound2-dev`

**Android APK:**
With Android SDK (`platforms;android-34`, `build-tools;34.0.0`, `ndk;27.2.12479018`), JDK 17, Ninja, and `zip`:
```sh
ANDROID_SDK=/path/to/android-sdk bash android/build_apk.sh
```

**Tests & CLI tools:**
```sh
./build/gh_tests                         # run headless core & raster tests
./build/gh_tests --chart song.json       # benchmark / botplay-check chart files
./build/gh_tests --psych-corpus mods/    # validate atlases, characters, stages & scripts
./build/astralis --list-songs --base .   # list discovered songs
```

---

## Project Layout

```
src/        C++20 engine and game states
src/asm/    Note renderer assembly (x86 SSE2, x86-64 SSE2, ARM64 NEON)
assets/     Bundled Psych Engine 0.7.3 UI, character, and stage assets
songs/      Bundled Tutorial and Week 1 songs
tests/      Headless test suite, chart benchmark, and corpus validator
android/    Android manifest, activity, icons, and APK build script
docs/wiki/  Engine and scripting documentation
licenses/   Third-party license texts
```

---

## License & Credits

See [`LICENSE`](LICENSE) for full license terms (**All Rights Reserved**; forks, redistribution, and rebranding are not permitted without written permission; contributions are accepted via issues only).

Third-party components retain their respective licenses (see `licenses/`):
- **Psych Engine** assets (Apache 2.0, `licenses/PSYCH_ENGINE_LICENSE.txt`)
- **JS Engine** shader GLSL & **FNF** offsets (Apache 2.0)
- **raylib** (zlib), **Lua 5.4** (MIT), **miniz** (MIT), **pl_mpeg** (MIT)
- Mod packs belong to their respective creators and are not covered by the engine's license.
