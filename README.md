# MS800 emulator

![MS800](ms800-emu.png)

An emulator for the **Cheetah MS800** digital wave synthesizer from 1987:
a 6502 running the original firmware, the SGS-Thomson M114S wavetable chip,
a 6850 ACIA for MIDI, and the front panel. Slop coded as a fun adventure with Claude and real hardware (pcb photos, sound A/B'ing etc) as a reference.

- Runs the real v1.0 firmware ROM; MIDI in/out via CoreMIDI (appears as **MS800**
  and **MS800 Out**).
- M114S playback of the 8-bit PCM wave ROM, including the MS800's PCB waveform
  address mapper, dual-table crossfades and stereo outputs.
- Timer rate, attenuation curve and output filtering are fitted to recordings of
  a real unit.
- Clickable front panel; keys `1`–`9`/`0` press the panel buttons, Space toggles power.

Requires copies of the original program and wavetable ROMs to run (not included here!).

## Building (macOS)

```sh
brew install cmake sdl2
# put the ROM dumps in roms/ (see roms/README.md)
cmake -S . -B build
cmake --build build
./build/ms800-emu
```

The panel artwork is compiled into the binary. To regenerate it (if needed):

```sh
python3 -m pip install pillow
python3 tools/gen_panel_art.py   # rewrites assets/*.bmp and src/panel_layout.h
cmake --build build
```

Run with `--trace` to log every M114S channel programme (with the mapped wave-ROM
addresses) to stderr.

## Licence

MIT (see `LICENSE`). Bundled third-party code keeps its own licence — see
`THIRD_PARTY.md` (BSD-3-Clause for the M114S and 6850 ports, CC0 for the 6502 core,
OFL for the Saira font).
