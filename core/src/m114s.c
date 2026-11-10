/*
 * SGS-Thomson M114S Digital Sound Generator emulation.
 *
 * Ported from vpinball/pinmame src/sound/m114s.c (Steve Ellenoff 2004,
 * heavily extended by Carsten Waechter), BSD-3-Clause. The mixing math,
 * frequency tables, table-length/repetition tables and the 8-byte channel
 * programming protocol below are taken from that source essentially
 * verbatim; only the MAME stream/device/cpu-cycle plumbing has been
 * replaced with a standalone single-chip instance.
 *
 * MS800-specific change: table-address registers are shifted left 7
 * (128-byte granules across the full 32KB wave ROM) instead of pinmame's
 * Mr.Game-specific left-5 + A13-toggle-for-table-2 scheme, which only
 * reaches an 8KB half of the ROM. See m114s.h.
 */

#include "m114s.h"
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <math.h>

#define FRAC_BITS  16
#define FRAC_ONE   (1 << FRAC_BITS)
#define FRAC_MASK  (FRAC_ONE - 1)

/* Table 1/2 repeat count per read-method mode */
static const int mode_to_rep[8][2] = {
    {2,2}, {1,1}, {4,4}, {1,1}, {1,2}, {1,1}, {1,4}, {1,1}
};
/* Table 1 length per [read_meth][table_len] (always >= table 2's length) */
static const int mode_to_len_t1[8][8] = {
    {16,32,64,128,256,512,1024,2048},
    {16,32,64,128,256,512,1024,2048},
    {16,32,64,128,256,512,1024,1024},
    {16,32,64,128,256,512,1024,2048},
    {16,32,64,128,256,512,1024,2048},
    {16,32,64,128,256,512,1024,2048},
    {16,32,64,128,256,512,1024,2048},
    {16,32,64,128,256,512,1024,2048},
};
static const int mode_to_len_t2[8][8] = {
    {16,32,64,128,256,512,1024,2048},
    {16,32,64,128,256,512,1024,2048},
    {16,32,64,128,256,512,1024,1024},
    {16,32,64,128,256,512,1024,2048},
    { 8,16,32, 64,128,256, 512,1024},
    {16,16,16, 32, 64,128, 256, 512},
    { 4, 8,16, 32, 64,128, 256, 512},
    {16,16,16, 16, 32, 64, 128, 256},
};
/* Attenuation code -> linear volume (0..1023). Measured on a real MS800:
   firmware level ramps of +4 codes every 64 timer ticks decay at ~73 dB/s,
   i.e. ~0.30 dB per code (the datasheet's table gives 0.75 dB). Code $3F
   switches the channel off. Filled in by m114s_init(). */
#define ATTEN_DB_PER_STEP 0.30

/* Frequency tables, straight from the M114S datasheet (4MHz = M114A part
   fitted to the MS800; the 6MHz M114AF table is kept for completeness). */
static const double freqtable4Mhz[256] = {
 1016.78,1021.45,1026.69,1031.46,1036.27,1041.67,1044.39,1045.48,
 1046.57,1047.67,1048.77,1051.52,1056.52,1061.57,1066.67,1071.81,
 1077.01,1082.25,1087.55,1092.90,1098.30,1103.14,1106.81,1107.42,
 1108.65,1109.88,1111.11,1114.21,1119.19,1124.86,1130.58,1135.72,
 1140.90,1146.79,1152.07,1158.08,1163.47,1168.91,1172.33,1173.71,
 1174.40,1175.78,1177.16,1180.64,1186.24,1191.90,1197.60,1203.37,
 1209.19,1215.07,1221.00,1226.99,1232.29,1238.39,1242.24,1243.78,
 1244.56,1245.33,1246.88,1250.78,1256.28,1262.63,1269.04,1274.70,
 1281.23,1287.00,1293.66,1299.55,1305.48,1312.34,1315.79,1317.52,
 1318.39,1319.26,1321.00,1324.50,1331.56,1337.79,1344.09,1350.44,
 1356.85,1363.33,1369.86,1376.46,1383.13,1389.85,1393.73,1395.67,
 1396.65,1397.62,1398.60,1403.51,1410.44,1417.43,1424.50,1430.62,
 1437.81,1445.09,1451.38,1458.79,1466.28,1472.75,1478.20,1479.29,
 1480.38,1481.48,1482.58,1486.99,1494.77,1501.50,1508.30,1516.30,
 1523.23,1530.22,1538.46,1545.60,1552.80,1560.06,1564.95,1566.17,
 1567.40,1568.63,1569.86,1576.04,1583.53,1591.09,1598.72,1606.43,
 1614.21,1622.06,1629.99,1638.00,1644.74,1652.89,1658.37,1659.75,
 1661.13,1662.51,1663.89,1669.45,1677.85,1684.92,1693.48,1702.13,
 1709.40,1718.21,1727.12,1734.61,1743.68,1751.31,1757.47,1759.01,
 1760.56,1762.11,1763.89,1768.35,1777.78,1785.71,1793.72,1803.43,
 1811.59,1819.84,1829.83,1838.24,1846.72,1855.29,1860.47,1862.20,
 1863.93,1865.67,1867.41,1874.41,1883.24,1892.15,1901.14,1910.22,
 1919.39,1928.64,1937.98,1947.42,1956.95,1966.57,1972.39,1974.33,
 1976.28,1978.24,1980.20,1984.13,1994.02,2004.01,2014.10,2024.29,
 2032.52,2042.90,2053.39,2063.98,2072.54,2083.33,2087.68,2089.86,
 2092.05,2094.24,2096.44,2103.05,2114.16,2123.14,2134.47,2143.62,
 2155.17,2164.50,2176.28,2185.79,2195.39,2207.51,2212.39,2214.84,
 2217.29,2219.76,2222.22,2227.17,2239.64,2249.72,2259.89,2272.73,
 2283.11,2293.58,2304.15,2314.81,2325.58,2339.18,2344.67,2347.42,
 2350.18,2352.94,2355.71,2361.28,2372.48,2383.79,2395.21,2406.74,
 0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,
};
static const double freqtable6Mhz[256] = {
 1523.78,1523.79,1538.64,1545.79,1552.99,1561.08,1565.16,1566.80,
 1568.44,1570.08,1571.73,1575.86,1583.35,1590.91,1598.55,1606.26,
 1614.04,1621.90,1629.84,1637.86,1645.95,1653.22,1658.71,1659.62,
 1661.46,1663.31,1665.16,1669.79,1677.27,1685.76,1694.34,1702.03,
 1709.80,1718.62,1726.54,1735.54,1743.62,1751.77,1758.91,1758.97,
 1760.00,1762.07,1764.14,1769.35,1777.75,1786.22,1794.78,1803.42,
 1812.14,1820.95,1829.84,1838.82,1846.75,1855.90,1861.66,1863.98,
 1865.14,1866.30,1868.63,1874.47,1882.71,1892.22,1901.83,1910.31,
 1920.10,1928.75,1938.73,1947.55,1956.45,1966.72,1971.89,1974.49,
 1975.79,1977.10,1979.71,1984.95,1995.53,2004.87,2014.30,2023.82,
 2033.43,2043.14,2052.93,2062.82,2072.82,2082.89,2088.70,2091.61,
 2093.07,2094.54,2096.00,2103.35,2113.74,2124.22,2134.81,2143.98,
 2154.77,2165.66,2175.09,2186.20,2197.42,2207.13,2215.28,2216.92,
 2218.56,2220.21,2221.85,2228.46,2240.12,2250.21,2260.39,2272.39,
 2282.77,2293.25,2305.60,2316.29,2327.08,2337.97,2345.29,2347.13,
 2348.97,2350.81,2352.65,2361.92,2373.14,2384.47,2395.91,2407.45,
 2419.11,2430.88,2442.77,2454.77,2464.87,2477.09,2485.31,2487.37,
 2489.44,2491.50,2493.58,2501.90,2514.50,2525.09,2537.92,2550.88,
 2561.78,2574.98,2588.32,2599.55,2613.15,2624.59,2633.81,2636.13,
 2638.45,2640.78,2643.10,2650.11,2664.25,2676.14,2688.14,2702.69,
 2714.93,2727.28,2742.25,2754.85,2767.57,2780.41,2788.17,2790.76,
 2793.06,2795.97,2798.58,2809.07,2822.30,2835.65,2849.13,2862.73,
 2876.47,2890.34,2904.34,2918.48,2932.76,2947.18,2955.90,2958.82,
 2961.74,2964.67,2967.60,2973.49,2988.31,3003.29,3018.41,3033.68,
 3046.02,3061.57,3077.29,3093.17,3105.99,3122.17,3128.68,3131.95,
 3135.23,3138.51,3141.80,3151.71,3168.37,3181.83,3198.80,3212.52,
 3229.83,3243.81,3261.46,3275.72,3290.10,3308.26,3315.58,3319.25,
 3322.93,3326.61,3330.31,3337.73,3356.42,3371.52,3386.76,3406.00,
 3421.55,3437.25,3453.09,3469.07,3485.21,3505.59,3513.81,3517.93,
 3522.07,3526.21,3530.37,3538.70,3555.49,3572.44,3589.56,3606.84,
 0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,
};

/* MS800 waveform address mapper (PCB latch RAM written at $A000/$C000 + n).
   The firmware never puts a waveform address in the M114S: each voice's
   tables point at a fixed per-voice block, and the M114S address lines
   A9-A14 (voice slot, table 1/2, ping-pong side) index this 64-entry latch,
   whose value (waveform * 2 | high-note bit) drives wave-ROM A7-A14. So
   waveform w lives at w * 0x100: low-note version at +0, high at +0x80.
   Starts as identity so the chip addresses the ROM directly until the
   firmware loads it. */
void m114s_map_write(M114S *chip, int index, uint8_t value) { chip->addr_map[index & 63] = value; }

static int map_address(const M114S *chip, int m114_addr) {
    return (chip->addr_map[(m114_addr >> 9) & 63] << 7) | (m114_addr & 0x7F);
}

static void init_output_filter(M114S *chip);

void m114s_debug(M114S *chip, int on) { chip->debug = on; }

static void init_channel(M114SChannel *ch) { memset(ch, 0, sizeof(*ch)); }

static void init_all_channels(M114S *chip) {
    for (int i = 0; i < M114S_CHANNELS; i++) init_channel(&chip->channels[i]);
    memset(&chip->tempch_regs, 0, sizeof(chip->tempch_regs));
    chip->channel    = 0;
    chip->bytes_read = 0;
}

void m114s_init(M114S *chip, const uint8_t *wave_rom, int rom_size, int clock_hz, double sample_rate) {
    memset(chip, 0, sizeof(*chip));
    for (int i = 0; i < 64; i++) chip->addr_map[i] = (uint8_t)(i << 2);
    for (int i = 0; i < 64; i++)
        chip->v_linear[i] = (int)(1023.0 * pow(10.0, -ATTEN_DB_PER_STEP * i / 20.0) + 0.5);
    chip->rom      = (const int8_t *)wave_rom;
    chip->rom_size = rom_size;
    if (clock_hz == 6000000 || clock_hz == 5994560) {
        chip->is_M114A     = 0;
        chip->reset_cycles = clock_hz * 0.000085;  /* M114AF resets after 85us idle */
    } else {
        chip->is_M114A     = 1;
        chip->reset_cycles = 4000000 * 0.000128;   /* M114A resets after 128us idle */
    }
    chip->sample_rate = sample_rate;
    init_all_channels(chip);
    init_output_filter(chip);
}

void m114s_reset(M114S *chip) { init_all_channels(chip); }

void m114s_advance_cycles(M114S *chip, uint64_t chip_cycles) { chip->chip_cycles += chip_cycles; }

/* Phase increment per host sample for a table of the given frequency. */
static uint32_t calc_incr(const M114S *chip, double freq_hz) {
    return (uint32_t)(freq_hz * ((double)(16u << FRAC_BITS) / chip->sample_rate));
}

void m114s_set_sample_rate(M114S *chip, double sample_rate) {
    chip->sample_rate = sample_rate;
    for (int c = 0; c < M114S_CHANNELS; c++)
        chip->channels[c].incr = calc_incr(chip, chip->channels[c].freq_hz);
    init_output_filter(chip);
}

/* Build one full table period of DAC input values.
   The MS800 wave ROM holds plain signed 8-bit samples (checked against
   hardware recordings, 2026-11-09). Each table byte is read n times
   (n = 1, 2 or 4) with linear interpolation between neighbours, kept in
   quarter units so the in-between points are exact. The two tables are
   mixed per the datasheet,  D = D1*(K+1)/16 + D2*(15-K)/16,  and scaled
   by the channel's 10-bit attenuator value. The volume ramp is baked into
   the table, as in pinmame. */
static void fill_table(int16_t *tb, const int8_t *src, int len, int rep) {
    for (int i = 0; i < len; i++) {
        int v1 = src[i], v2 = src[i < len - 1 ? i + 1 : i];
        for (int r = 0; r < rep; r++)
            tb[i * rep + r] = (int16_t)((v1 * (rep - r) + v2 * r) * 4 / rep);
    }
}

static void read_table(M114S *chip, M114SChannel *ch) {
    const int8_t *rom = chip->rom;
    int t1s = (int)ch->table1.start_address, t2s = (int)ch->table2.start_address;
    int lent1 = ch->table1.length, lent2 = ch->table2.length;
    int rep1 = ch->table1.reread, rep2 = ch->table2.reread;
    int intp = ch->regs.interp;
    int16_t *tb1 = chip->tb1, *tb2 = chip->tb2;   /* samples x4 */
    memset(chip->tb1, 0, sizeof(chip->tb1));
    memset(chip->tb2, 0, sizeof(chip->tb2));

    if (t1s < 0 || t1s + lent1 > chip->rom_size) return;
    if (t2s < 0 || t2s + lent2 > chip->rom_size) return;

    fill_table(tb1, &rom[t1s], lent1, rep1);
    fill_table(tb2, &rom[t2s], lent2, rep2);

    for (int i = 0; i < lent1 * rep1; i++) {
        if (ch->regs.env_enable && ch->step_rate_volume_env != 0) {
            int rate = abs(ch->step_rate_volume_env);
            if ((i & (rate - 1)) == rate / 2) {
                ch->current_volume += ch->step_rate_volume_env > 0 ? 4 : -4;
                if (ch->step_rate_volume_env > 0) {
                    if (ch->current_volume > ch->target_volume) ch->current_volume = ch->target_volume;
                } else if (ch->current_volume < ch->target_volume) {
                    ch->current_volume = ch->target_volume;
                }
            }
        }
        int d = ((int)tb1[i] * (intp + 1) + (int)tb2[i] * (15 - intp)) >> 4;
        ch->output[i] = d * (ch->current_volume + 1);
    }
}

static void process_freq_codes(M114SChannel *ch) {
    switch (ch->regs.frequency) {
    case 0xFF: ch->outpos = 0; break;  /* FFT: forced table termination */
    default: break;                    /* ROMID/SSG/RSS/RSG/PSF: not modelled, harmless */
    }
}

static void process_channel_data(M114S *chip) {
    M114SChannel *ch = &chip->channels[chip->channel];
    chip->bytes_read = 0;
    memcpy(&ch->regs, &chip->tempch_regs, sizeof(chip->tempch_regs));

    if (chip->debug) {
        fprintf(stderr,
            "M114S ch%02d: atten=%02X freq=%02X t1=%02X t2=%02X len=%d meth=%d interp=%d oct=%d env=%d out=%d\n",
            chip->channel, ch->regs.atten, ch->regs.frequency,
            ch->regs.table1_addr, ch->regs.table2_addr, ch->regs.table_len,
            ch->regs.read_meth, ch->regs.interp, ch->regs.oct_divisor,
            ch->regs.env_enable, ch->regs.outputs);
    }

    if (ch->regs.frequency >= 0xF0) {
        process_freq_codes(ch);
        if (ch->regs.frequency != 0xFF && ch->regs.frequency != 0xFC) return;
    }

    if (ch->regs.atten == 0x3F) { ch->active = 0; return; }

    int rep1  = mode_to_rep[ch->regs.read_meth][0];
    int rep2  = mode_to_rep[ch->regs.read_meth][1];
    int lent1 = mode_to_len_t1[ch->regs.read_meth][ch->regs.table_len];
    int lent2 = mode_to_len_t2[ch->regs.read_meth][ch->regs.table_len];

    /* MS800: 8-bit table-address registers used directly as the upper
       address bits of the flat 32KB wave ROM (128-byte granules), unlike
       pinmame's Mr.Game-specific 32-byte/8KB + A13-toggle-for-T2 scheme. */
    int t1start = ((ch->regs.table1_addr << 7) & (~(lent1 - 1) & 0x7FFF)) | (lent1 == 16  ? 0x10 : 0);
    int t2start = ((ch->regs.table2_addr << 7) & (~(lent2 - 1) & 0x7FFF)) | (lent2 <= 16 ? 0x10 : 0);
    /* The MS800 only uses 128-byte tables (band codes at $EAD3), so the
       mapped 128-byte block holds the whole table. */
    t1start = map_address(chip, t1start);
    t2start = map_address(chip, t2start);
    if (chip->debug) fprintf(stderr, "M114S ch%02d:   -> wave ROM t1=$%04X t2=$%04X\n", chip->channel, t1start, t2start);

    double freq = chip->is_M114A ? freqtable4Mhz[ch->regs.frequency] : freqtable6Mhz[ch->regs.frequency];

    ch->target_volume = chip->v_linear[ch->regs.atten & 0x3F];
    if (ch->regs.oct_divisor) freq /= 2.0;

    ch->active = 1;
    ch->freq_hz = freq;
    ch->incr   = calc_incr(chip, freq);

    ch->table1.start_address = (uint32_t)t1start;
    ch->table2.start_address = (uint32_t)t2start;
    ch->table1.reread = (uint8_t)rep1;
    ch->table2.reread = (uint8_t)rep2;
    ch->table1.length = (uint16_t)lent1;
    ch->table2.length = (uint16_t)lent2;
    ch->table1.total_length = (uint16_t)(lent1 * rep1);
    ch->table2.total_length = (uint16_t)(lent2 * rep2);

    if (ch->regs.env_enable) {
        int diff = ch->target_volume - ch->prev_volume;
        int ad = abs(diff);
        ch->step_rate_volume_env = ad > 128 ? 1 : ad > 64 ? 2 : ad > 32 ? 4 : 8;
        if (ch->target_volume < ch->prev_volume) ch->step_rate_volume_env = -ch->step_rate_volume_env;
        ch->current_volume = ch->prev_volume;
    } else {
        ch->current_volume = ch->target_volume;
    }
    ch->prev_volume = ch->target_volume;

    read_table(chip, ch);
}

void m114s_write(M114S *chip, uint8_t data) {
    /* Auto-reset if the gap since the previous byte exceeds the chip's
       programming timeout (128us @ 4MHz) — real M114S behaviour. This also
       naturally absorbs the MS800 firmware's extra channel-select byte
       written to $2000 after each 8-byte program (see bus.c): fed here as
       a stray "byte 1" of a new sequence, it is silently discarded by this
       same auto-reset once real traffic resumes. */
    if (chip->bytes_read > 0) {
        uint64_t diff = chip->chip_cycles - chip->last_write_cycle;
        if ((double)diff > chip->reset_cycles) init_all_channels(chip);
    }
    chip->last_write_cycle = chip->chip_cycles;

    data &= 0x3F;
    chip->bytes_read++;
    switch (chip->bytes_read) {
    case 1: chip->tempch_regs.atten = data; break;
    case 2:
        chip->tempch_regs.table2_addr = (uint8_t)((data & 0x03) << 6);
        chip->tempch_regs.table1_addr = (uint8_t)((data & 0x0C) << 4);
        chip->tempch_regs.outputs     = (uint8_t)((data & 0x30) >> 4);
        break;
    case 3: chip->tempch_regs.table2_addr |= data; break;
    case 4: chip->tempch_regs.table1_addr |= data; break;
    case 5:
        chip->tempch_regs.read_meth = data & 0x07;
        chip->tempch_regs.table_len = (uint8_t)((data & 0x38) >> 3);
        break;
    case 6:
        chip->tempch_regs.oct_divisor = (data & 0x01);
        chip->tempch_regs.env_enable  = (data & 0x02);
        chip->tempch_regs.interp      = (uint8_t)((data & 0x3C) >> 2);
        break;
    case 7:
        chip->tempch_regs.frequency = data & 0x03;
        chip->channel               = (data & 0x3C) >> 2;
        break;
    case 8:
        chip->tempch_regs.frequency |= (uint8_t)(data << 2);
        process_channel_data(chip);
        break;
    default:
        chip->bytes_read = 0;  /* stray extra byte: resync, don't corrupt state */
        break;
    }
}

/* Advance a channel by one host sample and return the area under its DAC
   output over that period (DAC value x chip-sample fraction, Q16). Every
   chip sample is accounted for even when the chip runs faster than the host
   rate (a box filter against aliasing). */
static int64_t step_channel(M114SChannel *ch, uint32_t length) {
    if (length == 0) return 0;
    uint32_t end = length << FRAC_BITS;
    uint32_t remaining = ch->incr;
    int64_t area = 0;
    while (remaining) {
        if (ch->outpos >= end) ch->outpos -= end;
        uint32_t left = FRAC_ONE - (ch->outpos & FRAC_MASK);
        uint32_t take = remaining < left ? remaining : left;
        area += (int64_t)ch->output[ch->outpos >> FRAC_BITS] * take;
        ch->outpos += take;
        remaining -= take;
    }
    if (ch->outpos >= end) ch->outpos -= end;
    return area;
}

/* Output stage. The MS800 wires the M114S output pins in stereo: pin 0
   is left and pin 2 right (patch layers alternate between them; single-
   layer patches record on the left only). Pins 1/3 are assumed to follow
   (bit 1 = side). Each side then goes through the MS800's analog output
   network, modelled as
   - a first-order high-frequency shelf (pole ~975 Hz, zero ~2.5 kHz,
     about -8 dB above 4 kHz), fitted to hardware recordings;
   - a ~10 Hz DC blocker (AC-coupled output);
   - a one-pole ~8 kHz lowpass (reconstruction filter). */
#define PCM_GAIN       (27.75f / 4096.0f)   /* pinmame-derived level */
#define SHELF_POLE_HZ  975.0
#define SHELF_ZERO_HZ  2500.0
/* The reconstruction lowpass and DC blocker were tuned as 48 kHz constants
   (alpha 21277/32768 ~ 8 kHz, R 0.9987 ~ 10 Hz). Their corner frequencies
   are kept when the host rate differs; at 48 kHz the constants are exact. */
#define LPF_ALPHA_48K  (21277.0 / 32768.0)
#define DC_BLOCK_R_48K 0.9987

/* Bilinear transform of H(s) = (1 + s/wz) / (1 + s/wp), plus the 1-pole
   corner constants, for the current sample rate. */
static void init_output_filter(M114S *chip) {
    const double sr = chip->sample_rate;
    const double k = 2.0 * sr;
    const double wz = 2.0 * 3.14159265358979 * SHELF_ZERO_HZ;
    const double wp = 2.0 * 3.14159265358979 * SHELF_POLE_HZ;
    const double n0 = 1.0 + k / wz, n1 = 1.0 - k / wz;
    const double d0 = 1.0 + k / wp, d1 = 1.0 - k / wp;
    chip->shelf_b0 = (float)(n0 / d0);
    chip->shelf_b1 = (float)(n1 / d0);
    chip->shelf_a1 = (float)(d1 / d0);
    const double lpf_w = -log(1.0 - LPF_ALPHA_48K) * 48000.0 / sr;
    chip->lpf_alpha_q15 = (int32_t)lround(32768.0 * (1.0 - exp(-lpf_w)));
    chip->dc_r = (float)exp(log(DC_BLOCK_R_48K) * 48000.0 / sr);
    memset(chip->out_filter, 0, sizeof(chip->out_filter));
}

static int16_t run_output_filter(const M114S *chip, M114SOutputFilter *f, float x) {
    float y = chip->shelf_b0 * x + chip->shelf_b1 * f->shelf_x1 - chip->shelf_a1 * f->shelf_y1;
    f->shelf_x1 = x;
    f->shelf_y1 = y;

    float dc = y - f->dc_x1 + chip->dc_r * f->dc_y1;
    f->dc_x1 = y;
    f->dc_y1 = dc;

    int32_t accum = (int32_t)dc;
    if (accum < -32768) accum = -32768;
    if (accum >  32767) accum =  32767;
    f->lpf_state += (chip->lpf_alpha_q15 * (accum - f->lpf_state)) >> 15;
    return (int16_t)f->lpf_state;
}

void m114s_read_stereo(M114S *chip, int16_t out[2]) {
    float side[2] = {0, 0};
    for (int c = 0; c < M114S_CHANNELS; c++) {
        M114SChannel *ch = &chip->channels[c];
        if (!ch->active || ch->incr == 0) continue;
        /* Average DAC value over the host sample period. */
        side[(ch->regs.outputs >> 1) & 1] +=
            (float)step_channel(ch, ch->table1.total_length) / (float)ch->incr;
    }
    out[0] = run_output_filter(chip, &chip->out_filter[0], side[0] * PCM_GAIN);
    out[1] = run_output_filter(chip, &chip->out_filter[1], side[1] * PCM_GAIN);
}
