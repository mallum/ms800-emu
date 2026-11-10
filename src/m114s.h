#pragma once
#include <stdint.h>

/*
 * SGS-Thomson M114S Digital Sound Generator (16-channel voice chip; the MS800
 * wave ROM holds plain 8-bit PCM, see m114s.c).
 *
 * Ported from the current vpinball/pinmame src/sound/m114s.c (Steve Ellenoff,
 * heavily extended by Carsten Waechter; BSD-3-Clause), stripped of the MAME
 * device/stream framework and adapted to a standalone single-chip instance
 * driven directly by this emulator's audio callback and CPU cycle clock.
 *
 * MS800-specific adaptation: the chip's 8-bit table-address registers are
 * shifted left 7 (128-byte granules, covering the full 32KB wave ROM) rather
 * than the Mr.Game-pinball-specific left-5-with-A13-toggle scheme (which
 * only reaches an 8KB half of the ROM per table). The MS800 PCB has no A13
 * toggle for table 2, so both tables address the same flat 32KB ROM.
 */

#define M114S_SAMPLE_RATE  48000
#define M114S_CHANNELS     16

void    m114s_init(const uint8_t *wave_rom, int rom_size, int clock_hz);
void    m114s_reset(void);
void    m114s_write(uint8_t data);          /* one byte written to the chip's 6-bit data bus */
void    m114s_read_stereo(int16_t out[2]);   /* pull one L/R output frame at M114S_SAMPLE_RATE */
int16_t m114s_read_sample(void);             /* mono (L+R)/2 of the above */
void    m114s_advance_cycles(uint64_t chip_cycles); /* advance the chip's own clock (for auto-reset timeout) */
void    m114s_debug(int on);                 /* 1 = log channel-program writes to stderr */
void    m114s_map_write(int index, uint8_t value); /* MS800 PCB wave address latch ($A000/$C000 + index) */
