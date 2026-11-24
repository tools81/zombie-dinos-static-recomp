# Zombie Dinos from Planet Zeltoid: static recompilation

A native Windows build of the CD-i game, for personal use with your own disc
image. The 68000 code is translated to C at build time, and the runtime provides
the CD-RTOS, video and audio services. No game data is in the source: `rom/`,
`extracted/` and `gen/` come from the disc and must not be shared.

## Build and play

1. Put the disc image (`.bin` + `.cue`) in `rom/`.
2. Run `build.bat`, then `zombiedinos.exe`.

Needs Windows 10+, Visual Studio 2022 (C), CMake 3.19+ and Python 3.8+.

| Input | Action |
|-------|--------|
| Mouse, left / right button | Pointer, button 1 / 2 |
| Arrow keys (Shift = faster) | Pointer |
| Space, Z or A / X, S or Ctrl | Button 1 / button 2 |
| Enter or C | Both buttons |
| XInput gamepad: stick or d-pad, A / B | Pointer, button 1 / 2 |
| Tab (hold) | Fast forward |
| F2 | Quicksave to the current slot |
| F3 | Next quicksave slot (1-10) |
| F4 | Load the current slot |
| F6 | Run-ahead: 0, 1 (default), 2, 3 fields |
| F7 | Sprite interpolation on (default) / off |
| F9 / F11 | Aspect mode / full screen |
| F10 | Filter: sharp, smooth, CRT, nearest, Cartoon HD, Cartoon HD+ |
| Esc | Quit |

While a character is speaking, the first click stops the speech and the next
one selects.

## Enhancements

* **Cartoon HD / Cartoon HD+** (F10) are Direct3D edge-directed upscalers. HD+
  is crisper, with darker ink lines and a little more colour.
* **Run-ahead** (F6). After each real field the machine is snapshotted, run on
  with the current input, shown, and restored. The game answers input two fields
  later. Run-ahead 1 makes that one, the least possible, so 2 and 3 gain nothing
  for the pointer. Sound and game state are bit-identical to a run without it.
* **Quicksave** (F2/F3/F4). Ten slots in `saves/quicksave_<n>.state`. They survive
  restarts and updates unless the snapshot layout changes (`STATE_VERSION`). Requests made during the Philips logo wait
  until it ends.
* **Sprite interpolation** (F7). The game shows new pictures 10-30 times a second.
  The machine looks ahead for the next picture (no added delay) and objects are
  moved along their path at 60 fps. Portrait, landscape window and full-screen
  scenes are followed separately, because the panel lights change every field.
  What motion does not explain (cuts, new poses, the turning ship, the time
  travel effect) is shown as the game drew it.

`--runahead N`, `--interpolation on|off` and `--filter N` override the settings.
Headless runs default to no run-ahead and no interpolation.
`RECOMP_FRAMETIME=1` logs timing and interpolation counts at exit.
`--capture FIELD WxH FILTERS PREFIX` saves a field through several filters.
Script keys `S`, `N` and `O` quicksave, select the next slot and quickload.

## Distribution and updates

`tools/package.ps1` builds, from the version in `runtime/version.h`:

* `dist/ZombieDinos-<version>-setup.exe`: single-file installer. Asks for the disc
  image and checks it. Standard or Portable type.
  Silent: `/VERYSILENT /DISC="path" [/DIR="..."] [/TASKS=portable]`.
* `dist/ZombieDinos-<version>-portable.zip`: unzip and run. Asks for the disc image
  on first start, or finds it in `rom/`.

The default packages hold the recompiled program, which is a translation of the
game's code: keep them private. `package.ps1 -Public` writes to `dist/public/` a
program without game code that interprets the disc's program (identical output,
about 60% slower emulation).

The program writes only to `saves/` beside it. `--portable` is accepted and is
always the case. It has no dependencies beyond Windows 10 1903.

At start it installs a newer release of `UPDATE_REPO`, if there is one
(`runtime/update.c`). To release: raise `APP_VERSION`, push, run
`tools/release.ps1`, which publishes the `-Public` packages. An update therefore
replaces a recompiled copy with the interpreted one. To keep a recompiled copy,
set `update=0` in `saves/settings.ini` or use `--no-update-check`.

For tests, CMake takes `-DUPDATE_TEST_PORT=n` (plain HTTP server on 127.0.0.1
instead of GitHub) and `-DAPP_VERSION_OVERRIDE=x.y.z`.

## What this game needed beyond the template

* `game.json`: callbacks the analyzer can't see (`extra_entries`) and
  `label_ranges`, for unrolled loops and `Decompress`, which jump to computed
  addresses.
* `analyze.py`: function names from the disc's symbol module (`cdi_ZD.stb`).
* `gen.py`: the name table is never empty (an empty array crashed MSVC).
* `cpu.c` / `interp.c`: undiscovered code inside a module runs in the interpreter
  and is logged once, instead of being fatal.
* `os9.c`: F$GPrDsc (`clock()`), and word-aligned modules in F$Load (the second
  module in `cdi_bumper`).
* `audio.c`: SM_Close is function 4 in this game's library.
* `video.c` / `host_win32.c`: mouse. The game keeps its own cursor and re-centres
  the pointer, so the mouse is fed as an offset from that cursor. The `--script`
  key `Mx/y` moves the mouse (768x480 units).

"play overrun" in the log near the end of a video clip is harmless: the game has
already shown the last frame and stops taking buffers before the end marker.

## Layout

| Path | Contents |
|------|----------|
| `tools/cdi_extract.py` | Green Book filesystem reader. Writes program files and `_app.txt` (`--all` also dumps the real-time streams). |
| `tools/m68k.py` | MC68000 decoder |
| `tools/os9mod.py` | OS-9 module header, idata and irefs |
| `tools/analyze.py` | Code discovery (recursive descent plus the patterns above) |
| `tools/gen.py` | 68000→C generator, flag liveness, dispatch tables, `gen/game_config.h` |
| `tools/config.py` | `game.json` loader |
| `tools/gen_tests.py` | Per-instruction differential tests |
| `tools/bmp2png.py` | Converts frame dumps to PNG |
| `runtime/frame.c` | Frame loop: run-ahead, look-ahead, quicksave slots, on-screen messages |
| `runtime/state.c` | Snapshots: one function per module both saves and loads |
| `runtime/motion.c` | Sprite interpolation |
| `runtime/cpu.c` | Dispatcher and run loop |
| `runtime/interp.c` | 68000 interpreter, for RAM code and the `--interp` reference mode |
| `runtime/alu.c` | Shifts, division and BCD with exact flags |
| `runtime/os9.c` | OS-9 kernel: memory, processes (fork/wait/exit), signals, alarms, paths, NVRAM, CSD |
| `runtime/cdfm.c` | CD file manager: reads, and real-time SS_Play with PCB/PCL/CIL |
| `runtime/video.c` | UCM display control (FCT/LCT), MCD212-style renderer, cursor, pointer |
| `runtime/audio.c` | ADPCM levels A/B/C, sound maps with gapless SM_Out, attenuation |
| `runtime/render_d3d11.c` | GPU presenter: aspect modes and sharp/smooth/CRT filters |
| `runtime/host_win32.c` | Window, input (keyboard and XInput), waveOut, pacing, scripted and fuzz input |
| `runtime/update.c` | Automatic update |
| `runtime/interp_only/` | Stands in for `gen/` in builds without game code |
| `installer/`, `tools/package.ps1`, `tools/release.ps1` | Packages and release |
| `test/` | Instruction differential tester and ALU reference vectors |
| `docs/CDI_NOTES.md` | CD-i facts and API codes worked out during the first port |

## Debugging workflow

* **Find what fails.** Unimplemented OS calls and SetStat/GetStat codes are
  logged to `saves/recomp.log`. Fatal errors also show a 68k register dump and a
  stack backtrace with function names.
* **Debug options:**
  * `--trace` / `--trace3` log syscalls with callers, signals and disc activity.
  * `--headless --frames N --dump K` runs fast without a window and writes every
    Kth frame as a BMP.
  * `--script "field:key:dur,..."` scripts input (keys `1 2 L R U D`).
  * `--fuzz SEED` drives the game with random input.
  * `--wav out.wav` records the audio.
* **Check the translation.** Run the same headless session twice, once with
  `--interp`, which executes everything in the interpreter, and compare the
  frame dumps and WAVs. Headless runs are deterministic: emulated time is
  counted in instructions, and the clock uses a fixed epoch. Any difference
  points to a translation bug.
* **Unit-level checks.** `python tools/gen_tests.py` and
  `python test/gen_alu_vectors.py` generate test sources. Then build the
  `insn_test` and `alu_test` targets:

  ```
  cmake --build build\cmake --config Release --target insn_test alu_test
  build\cmake\Release\insn_test.exe rom\<image>.bin 200
  build\cmake\Release\alu_test.exe
  ```

* Code that jumps into RAM is not an error. It runs in the interpreter. Jumps to
  *undiscovered* addresses inside a recompiled module are fatal. Add those
  addresses to `extra_entries`, or teach `analyze.py` the pattern.
