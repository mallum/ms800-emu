# ROMs

The emulator needs two ROM dumps from a real Cheetah MS800, which are not
included here (they are Cheetah's copyrighted firmware and sample data):

| File | Size | Contents |
|---|---|---|
| `ms800-10-prog.bin` | 8 KB | v1.0 program ROM (6502 code, mapped at `$E000–$FFFF`) |
| `ms800-waveform-rom.bin` | 32 KB | 27256 wave ROM read by the M114S |

Put them in this folder before building (the build copies `roms/` next to the
executable), or in a `roms/` folder in the directory you run the emulator from.
