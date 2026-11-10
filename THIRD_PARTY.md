# Third-party code and assets

- `core/src/m114s.c` — SGS-Thomson M114S emulation ported from vpinball/PinMAME
  `src/sound/m114s.c` (Steve Ellenoff, Carsten Waechter). BSD-3-Clause.
- `core/src/acia6850.c` — Motorola 6850 ACIA ported from MAME
  `src/devices/machine/6850acia.cpp` (copyright-holders: smf). BSD-3-Clause.
- `core/src/fake6502_vendor.h` — 6502 core ("MyLittle6502" fork of fake6502). Public domain / CC0.
  Locally patched (marked "ms800-emu patch") to make its state optionally thread-local.
- `assets/fonts/Saira-Variable.ttf` — Saira, © 2020 The Saira Project Authors (Omnibus-Type), SIL Open Font License 1.1
  (`assets/fonts/Saira-OFL.txt`). Used only by `tools/gen_panel_art.py`.

Downloaded at configure time for the plugin build (not in this repository):

- [clap-wrapper](https://github.com/free-audio/clap-wrapper) — MIT.
- [CLAP](https://github.com/free-audio/clap) — MIT.
- [VST3 SDK](https://github.com/steinbergmedia/vst3sdk) — MIT.
- [AudioUnitSDK](https://github.com/apple/AudioUnitSDK) — Apache-2.0.

The panel artwork in `assets/*.bmp` was rendered with Eurostile Extended Bold,
which is a commercial font and is not included. `tools/gen_panel_art.py` uses an
installed copy if present and otherwise falls back to Saira.
