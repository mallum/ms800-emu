#pragma once
#include <stdint.h>

/*
 * SGS-Thomson M114S Digital Sound Generator (16-channel voice chip; the MS800
 * wave ROM holds plain 8-bit PCM, see m114s.c).
 *
 * Ported from the current vpinball/pinmame src/sound/m114s.c (Steve Ellenoff,
 * heavily extended by Carsten Waechter; BSD-3-Clause), stripped of the MAME
 * device/stream framework. All state lives in an M114S struct, so any number
 * of chips can run side by side.
 *
 * MS800-specific adaptation: the chip's 8-bit table-address registers are
 * shifted left 7 (128-byte granules) and then translated through the PCB's
 * wave address latch (see m114s_map_write) to reach the 32KB wave ROM.
 */

#define M114S_CHANNELS 16
#define M114S_TABLE_MAX 4096   /* longest table x repeat count */

typedef struct {
    uint8_t  reread;          /* # of times to re-read each byte (interpolated) */
    uint32_t start_address;   /* offset into wave ROM */
    uint16_t length;          /* table length in samples, before repetition */
    uint16_t total_length;    /* length * reread */
} M114STable;

typedef struct {
    uint8_t  atten;
    uint8_t  outputs;
    uint8_t  table1_addr;
    uint8_t  table2_addr;
    uint8_t  table_len;
    uint8_t  read_meth;
    uint8_t  interp;
    int      env_enable;
    int      oct_divisor;
    uint8_t  frequency;
} M114SChannelRegs;

typedef struct {
    M114SChannelRegs regs;
    int         active;
    int32_t     output[M114S_TABLE_MAX];   /* per chip sample: DAC value x4 x (volume+1) */
    uint32_t    outpos;
    int         prev_volume;
    int         current_volume;
    int         target_volume;
    int         step_rate_volume_env;
    M114STable  table1, table2;
    double      freq_hz;      /* table frequency, so incr can follow a sample-rate change */
    uint32_t    incr;
} M114SChannel;

typedef struct {
    float   shelf_x1, shelf_y1;
    float   dc_x1, dc_y1;
    int32_t lpf_state;
} M114SOutputFilter;

typedef struct {
    int              bytes_read;
    int              channel;
    M114SChannelRegs tempch_regs;
    M114SChannel     channels[M114S_CHANNELS];
    const int8_t    *rom;
    int              rom_size;
    int              is_M114A;
    double           reset_cycles;   /* chip-clock cycles of silence before auto-reset */
    uint64_t         chip_cycles;    /* running chip clock, advanced by m114s_advance_cycles() */
    uint64_t         last_write_cycle;
    int              debug;

    uint8_t          addr_map[64];   /* MS800 PCB wave address latch */
    int              v_linear[64];   /* attenuation code -> linear volume */

    double           sample_rate;
    float            shelf_b0, shelf_b1, shelf_a1;
    float            dc_r;
    int32_t          lpf_alpha_q15;
    M114SOutputFilter out_filter[2];

    int16_t          tb1[M114S_TABLE_MAX], tb2[M114S_TABLE_MAX];   /* read_table scratch */
} M114S;

/* rom must stay valid for the chip's lifetime. */
void m114s_init(M114S *chip, const uint8_t *wave_rom, int rom_size, int clock_hz, double sample_rate);
void m114s_reset(M114S *chip);
void m114s_set_sample_rate(M114S *chip, double sample_rate);
void m114s_write(M114S *chip, uint8_t data);                 /* one byte on the chip's 6-bit data bus */
void m114s_map_write(M114S *chip, int index, uint8_t value); /* PCB wave address latch ($A000/$C000 + index) */
void m114s_advance_cycles(M114S *chip, uint64_t chip_cycles); /* chip clock, for the auto-reset timeout */
void m114s_read_stereo(M114S *chip, int16_t out[2]);         /* one L/R output frame at the sample rate */
void m114s_debug(M114S *chip, int on);                       /* 1 = log channel programmes to stderr */
