# neotree

Firmware for a WS2812 ("NeoPixel") Christmas tree driven by a Raspberry Pi Pico 2 W.

The Pico drives multiple WS2812 LED strings in parallel using the RP2040/RP2350
PIO, with color and animation work split across both cores. It's a personal
project with a fair amount of hardcoding (pin assignments, pixel counts) and is
not meant to be a general-purpose library.

## Layout

```
neotree/
+- firmware/
|  +- CMakeLists.txt        # build configuration
|  +- pico_sdk_import.cmake # locates the installed Pico SDK
|  +- memmap_custom.ld      # custom linker script
|  +- include/              # project headers
|  +- src/                  # sources (main.cpp, dcm_rgb, effects, ws2812.pio, ...)
+- README.md
```

Build output (`build/`, `generated/`), the Pico SDK, and editor settings
(`.vscode/`) are intentionally not tracked - see `.gitignore`.

## Building

Requires the [Raspberry Pi Pico SDK](https://github.com/raspberrypi/pico-sdk)
(installed via the Pico VS Code extension at `~/.pico-sdk`, or set `PICO_SDK_PATH`)
and the Arm GNU toolchain. Target board: `pico_w`.

```sh
cd firmware
cmake -B build -G Ninja
cmake --build build
```

Flash the resulting `build/neo_tree.uf2` to the Pico in BOOTSEL mode.