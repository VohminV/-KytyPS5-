# KytyPS5

KytyPS5 is a free and open-source PlayStation 5 translation layer written in C++ for Windows and Linux, with experimental macOS support. It is based on a heavily modified version of Kyty (InoriRus/Kyty). The project is in active development.

Not affiliated with Sony. Does not distribute games or copyrighted system software.

## Key areas of the codebase

- `src/graphics/shader/recompiler` — instruction decoding, IR, control flow, resource tracking, SPIR-V emission
- `src/graphics/guest_gpu` — PS5 (Prospero) GPU formats and command processing
- `src/graphics/host_gpu` — Vulkan host backend and resource management
- `src/common` — shared utilities
- `src/loader` — guest loading, timers, instruction emulation
- `src/kernel` — kernel services
- `src/launcher` — Qt 6 GUI launcher
- `tests` — regression tests (memory, shader, resource tracking)

The renderer targets Vulkan 1.3 and emits SPIR-V. PS5 graphics are based on AMD RDNA 2; use AMD's RDNA 2 ISA Reference Guide (document 70648) as the reference for shader decoding/recompilation.

## Build (Linux)

CMake 3.22.1+, Ninja, Clang, Qt 6 (Concurrent, Network, Widgets) for the launcher, glslang (`glslangValidator`), Python 3.

### First-time setup

```bash
git submodule update --init --recursive
```

### Install dependencies

```bash
sudo apt-get install --no-install-recommends \
  clang lld ninja-build cmake git glslang-tools python3 pkg-config \
  libgl1-mesa-dev libx11-dev libxcursor-dev libxext-dev libxfixes-dev \
  libxi-dev libxrandr-dev libxss-dev libxtst-dev libxkbcommon-dev \
  libasound2-dev libpulse-dev libudev-dev libdbus-1-dev libwayland-dev wayland-protocols
```

Qt 6 (Concurrent, Network, Widgets) is required for the launcher (`qt6-base-dev`) or set `KYTY_BUILD_LAUNCHER=OFF`.

### Configure and build

```bash
cmake -S . -B _Build/linux -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_PREFIX_PATH="$Qt6_DIR"

cmake --build _Build/linux --target launcher --parallel
cmake --install _Build/linux --prefix _Build/linux/install
```

Without Qt (emulator + tests only):

```bash
cmake -S . -B _Build/linux-no-qt -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
  -DKYTY_BUILD_LAUNCHER=OFF

cmake --build _Build/linux-no-qt --target kyty_emulator kyty_tests --parallel
```

### NixOS

```bash
nix-shell # or: nix develop
git submodule update --init --recursive

cmake -S . -B _Build/linux -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++

cmake --build _Build/linux --target launcher --parallel
cmake --install _Build/linux --prefix _Build/linux/install
```

A Vulkan 1.3 driver must be available at runtime (`hardware.graphics.enable = true`).

## Running

```bash
./_Build/linux/install/kyty_launcher
```

Or directly:

```bash
./_Build/linux/install/kyty_emulator --game "/games/ExampleGame"
./_Build/linux/install/kyty_emulator --game "/games/ExampleGame.zar"
```

Run `kyty_emulator --help` for options. To override the Vulkan loader:

```bash
SDL_VULKAN_LIBRARY=/path/to/libMoltenVK.dylib ./kyty_emulator --game "/games/ExampleGame"
```

First launch: add game folders in global settings. The launcher searches recursively for directories containing `eboot.bin` or ZArchive (`.zar`) dumps whose archive root contains `eboot.bin`.

### Diagnostics / known issues

- Screen flicker: try `--present-mode Fifo` (default is `Mailbox`; also `Immediate`). `Fifo` synchronizes with vertical blank and usually eliminates flicker, possibly capping FPS.
- `VK_ERROR_OUT_OF_DEVICE_MEMORY` at image creation: VRAM budget exhausted. `CreateImage` in `src/graphics/host_gpu/vma.cpp` now logs the VkResult, image parameters and per-heap memory budget to `stderr` so the cause can be identified instead of silently failing.

## Testing

```bash
cmake --build _Build/linux --target kyty_tests --parallel
ctest --test-dir _Build/linux --output-on-failure
```

Tests are defined in `CMakeLists.txt` under `BUILD_TESTING` and include shader, memory, page manager, resource tracking, trophy, AVPlayer, audio, and kernel tests.

## Formatting / pre-commit

Clang-format hook via pre-commit. Formats staged `.cpp`, `.h`, `.inc` files in `src`.

```bash
python -m pip install pre-commit
python -m pre_commit install --install-hooks
```

## Contributing

- Search existing issues first; use the **Game Emulation Status Report** template and attach the complete log file.
- PRs for general fixes, architectural changes, or refactoring that do not address a concrete bug or launch problem in a specific game will be automatically closed.
- Keep each PR focused. Name the affected game, describe the exact problem, and provide evidence of what the change fixes.
- Changes to shared code must not introduce regressions on Windows or Linux. Platform-confined changes only need to build on that platform.
- Consider opening an issue before starting a large change.
- Windows and Linux are the primary target platforms.

## AI use policy

AI tools may be used for research, reverse engineering, and development assistance. Contributors must fully understand, review, and test all code they submit and remain responsible for its correctness.

Repository communication — pull-request descriptions, code comments, and issue comments — must come from the human contributor rather than an autonomous AI agent. PRs that include AI-assisted or AI-generated work must disclose the scope of AI involvement and describe the human review and testing performed. Unverified or untested generated changes may be closed without review.

## License

GNU GPL v2 (`GPL-2.0-only`). Based on Kyty (MIT), whose notice is preserved in `LICENSES/Kyty-MIT.txt`.

## Key optimisations applied (from TheCruZ/KytyPS5-GTA fork research)

- **Descriptor heap**: large pool sizes for bindless/ray-tracing descriptor sets (fixes "descriptor set does not fit a pool sized for it" on AMD GPUs), layout registration and size caching (`src/graphics/host_gpu/renderer/pipeline/descriptorHeap.{h,cpp}`).
- **Shader accuracy**: `V_FRACT_F32`/`V_FRACT_F64`/`V_FRACT_F16` clamp to below 1.0 (fixes black particles/smoke); `V_CUBE*` and packed float handling (`src/graphics/shader/recompiler/frontend/translate/Float.cpp`, `Vector.cpp`).
- **Dynamic state**: `vkCmdSetColorWriteEnableEXT` loaded via dynamic dispatcher (`src/graphics/host_gpu/renderer/dynamicState.cpp`).
- **CMake**: `CMakePresets.json` with Release/Release+LTO/Debug/No-Qt presets; `ccache` with compression; Ninja link pool; per-target build stats target (`kyty_build_report`).
- **Shader SPIR-V**: `DEPFILE` generation for incremental shader recompilation; stable output paths.
- **Vulkan**: `Vulkan::Vulkan` imported target linked to system `libvulkan`; bundled Vulkan-Headers from `3rdparty` for API consistency.

## Key bug fixes applied

- **Red zone protection**: `red_zone_protection_enabled` now defaults to `true` (`src/common/emulatorConfig.h`), `--redzone` CLI option available on all platforms (`src/main.cpp`). Fixes zlib streaming crashes in GTA V prologue (SysV red zone overwritten by Windows exception delivery).
- **XInput support**: `src/common/xinput.h` added with minimal XInput definitions for cross-platform compilation; XInput controller detection in `src/libs/controller.cpp::Initialize()` for Windows (Xbox controller support via XInput API).

- **Texture sRGB formats**: `k8Srgb` → `eR8Srgb`, `k8_8Srgb` → `eR8G8Srgb` (`src/graphics/host_gpu/vulkanCommon.cpp`). Fixes dark/washed-out sRGB textures (e.g. black particles, broken lighting).
- **Gamepad type logging**: `SDL_GetGamepadType` logged on controller connect (`src/graphics/presentation/window/window.cpp:305`) for Xbox/DualSense diagnostics.
