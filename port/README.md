# pokeemerald in the browser

This directory turns the decompiled Pokémon Emerald source into a WebAssembly
build that runs in a web page. It is a **source port, not an emulator**: the
game's C code is compiled to WebAssembly with Emscripten and runs on a small
software implementation of the GBA hardware it talks to.

## Building

You need what the regular *modern* build needs (see [INSTALL.md](../INSTALL.md):
`arm-none-eabi` binutils and gcc, libpng, a C/C++ compiler), plus
[Emscripten](https://emscripten.org/docs/getting_started/downloads.html) and
Python 3.

```sh
# once: install and activate emsdk
git clone https://github.com/emscripten-core/emsdk.git
cd emsdk && ./emsdk install latest && ./emsdk activate latest && cd ..

# every shell: put emcc on PATH
source emsdk/emsdk_env.sh

# from the repository root
make -f port/wasm.mk -j$(nproc)
```

The output goes to `port/dist/` (`index.html`, `app.js`, `style.css`,
`emerald.js`, `emerald.wasm`). Serve that folder with any static web server,
for example:

```sh
make -f port/wasm.mk serve     # http://localhost:8000
```

Opening `index.html` from the file system does not work; browsers only load
WebAssembly over HTTP(S). Sound uses an AudioWorklet, which needs a secure
context (`https://` or `localhost`); on plain `http://` LAN addresses the page
falls back to a ScriptProcessorNode.

`make -f port/wasm.mk DEBUG=1` builds an unoptimized variant with debug info
and Emscripten assertions into `port/dist-debug/`.

## Playing

| GBA | Keyboard | Gamepad |
| --- | --- | --- |
| D-Pad | Arrow keys | D-pad / left stick |
| A | X | East button (B / ○) |
| B | Z | South button (A / ×) |
| L / R | A / S | LB / RB |
| Start | Enter | Start |
| Select | Backspace / Shift | Back / Select |
| Fast-forward | hold Space | RT |

Touch controls appear on touch screens. `F` toggles fullscreen and `M` mutes.

The game saves to the browser (IndexedDB) whenever you save in-game. The
*Save file* menu downloads the save as a 128 KiB `.sav` flash image, imports
one, or deletes it. Saves use the same flash layout as the cartridge, so they
should be interchangeable with saves from emulators.

The real-time clock follows the computer's local time.

## How it works

```
 game C code (src/*.c) ──emcc──┐
                              ├── wasm-ld ──> emerald.wasm + emerald.js
 port layer (port/src/*.c) ───┤
                              │
 data/*.s, sound/*.mid ──arm-none-eabi-as──> ARM ELF objects
                      ──port/tools/elf2wasm.py──> wasm object ┘
```

* **Memory map.** The GBA memory regions (I/O registers, palette, VRAM, OAM,
  and scratch EWRAM/IWRAM) live in the bottom megabyte of WebAssembly memory;
  everything Emscripten allocates starts above it (`-sGLOBAL_BASE`). The
  addresses are set in `include/gba/defines.h` and `include/gba/io_reg.h`
  when `PORT_WASM` is defined, so code like `REG_DISPCNT = ...` and
  `DmaCopy16(3, src, (void *)VRAM, ...)` keeps working unchanged.
* **Data.** Event scripts, battle scripts, map data, and songs are assembly
  files with pointers between them and into the C code. They are assembled
  for ARM by the regular modern build, and `elf2wasm.py` converts the
  resulting relocatable objects into WebAssembly assembly: every section
  becomes a data segment, every label a symbol, and every 32-bit relocation a
  `.int32 symbol+addend` that the wasm linker resolves (to a table index for
  C functions).
* **Frame loop.** Browsers can't block, so `AgbMain` is split into its
  initialization and one main loop iteration (`AgbMainLoop`), which the page
  calls 59.73 times a second. `WaitForVBlank` draws the frame scanline by
  scanline, running H-Blank DMA and the H-Blank, V-Count, and V-Blank
  interrupt handlers at the right lines (`port/src/port_main.c`).
* **Interrupts in loops.** A few places spin until an interrupt handler has
  done some work (for example the map loader waiting for the DMA3 queue that
  V-Blank processes). `Port_BusyWait` notices repeated polling within a frame
  and runs a video frame from inside the loop, like the hardware would.
* **Graphics.** `port/src/ppu.c` renders text and affine backgrounds, bitmap
  modes, regular and affine sprites, windows, mosaic, and blending.
* **Sound.** `port/src/m4a_engine.c` is a C port of the assembly parts of the
  m4a ("Sappy") engine (sequencer, note allocation, and the 8-bit Direct
  Sound mixer) from `src/m4a_1.s`. `port/src/audio.c` emulates the four Game
  Boy sound channels, mixes them with the Direct Sound output, and resamples
  to the browser's rate.
* **BIOS and cartridge.** `bios.c` reimplements the BIOS calls (CpuSet, LZ77
  and RL decompression, ArcTan2, affine setup, ...), `flash.c` the save chip,
  and `rtc.c` the clock. The wireless adapter and GameCube link are stubbed
  out as "not connected".
* **Division.** WebAssembly traps on integer division by zero, where the
  GBA's division routines return 0 (the Spark animation relies on that).
  `port/tools/wasm_safe_div.py` rewrites the division instructions of the
  linked module into calls to the helpers in `port/src/divide.c`.
* **Function pointer types.** WebAssembly traps when a function is called
  through a pointer of a different type, which ARM doesn't mind. The game
  does this in a few places (e.g. `special` calls every special as
  `u16 (*)(void)`, including the `void` ones). `elf2wasm.py` generates
  adapter functions for script-referenced functions (see `CALL_SIGS` in
  `wasm.mk`), and the two C cases have `PORT_WASM` versions.

Changes to the game sources are guarded by `#ifdef PORT_WASM`, so the GBA
builds are unaffected.

## Testing tools

`port/tools/play.mjs` runs the game headlessly in Node.js from a small script
of inputs and takes screenshots:

```sh
node port/tools/play.mjs -e "wait 300; press START; wait 400; press START; wait 200; shot title" shots/
```

It can also save and restore the whole emulator state (`snap`/`load`),
print the game state (`dump`), load a save file (`FLASH=file.sav`), start a
test battle (`battle SPECIES LEVEL [MOVE]`), run every move's battle
animation from an overworld snapshot (`movetest SNAPSHOT FIRST LAST`), and
with `WATCHDOG=seconds` print the call stack if the game stops completing
frames.

## Limitations

* Link cable and wireless features (trading, battles, Union Room, Record
  Mixing, Mystery Gift over link, e-Reader) need a second GBA and aren't
  available.
* Audio emulates the GB channels sample by sample without band-limiting, so
  the square waves alias slightly more than on hardware.

## Legal

Pokémon Emerald is © Nintendo / Creatures Inc. / GAME FREAK inc. This
repository contains a decompilation of the game; a build contains the game's
assets. Only host or share builds if you have the right to do so.
