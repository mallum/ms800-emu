/*
 * MS800 machine: address decode, chip wiring and CPU/interrupt scheduling.
 *
 * Address map (see MS800_ANALYSIS.md "Memory Map"):
 *
 *   $0000-$1FFF  RAM (8KB, battery-backed)
 *   $2000        M114S data write / front-panel button-scan row select
 *   $4000        M114S status read / front-panel button-matrix readback
 *   $6000        6850 ACIA control (write) / status (read)
 *   $6001        6850 ACIA data (read RX / write TX)
 *   $8000        Front-panel display, packed BCD (write-only)
 *   $A000-$A03F  PCB wave address latch (write-only)
 *   $C000-$C03F  PCB wave address latch (write-only, same data)
 *   $E000-$FFFF  Program ROM (8KB)
 */
#include "ms800.h"
#include "acia6850.h"
#include "fake6502.h"
#include "m114s.h"

#include <math.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#define CPU_HZ   2000000
#define M114S_HZ 4000000             /* 4MHz crystal; CPU and ACIA get it /2 */
#define IRQ_HZ   3770                /* firmware timer tick ($E1EE). Measured
                                        against a real unit: P6's crossfade loop
                                        (4 x 128 ticks) repeats every 133.7 ms,
                                        and factory decays (+4 attenuation per
                                        64 ticks) end notes after ~250 ms as on
                                        hardware. */
#define CYCLES_PER_IRQ (CPU_HZ / IRQ_HZ)

struct ms800 {
    cpu6502_regs cpu;
    cpu6502_bus  bus;

    uint8_t ram[MS800_RAM_SIZE];
    uint8_t prog_rom[MS800_PROG_ROM_SIZE];
    uint8_t wave_rom[MS800_WAVE_ROM_SIZE];

    /* $2000 is dual-purpose: an 8-byte-programming write to the M114S data
       bus (triggered by the firmware setting bit 7 on the first byte) or,
       when idle, a front-panel button-matrix row-select strobe (bits[1:0]).
       The 74HCT245 bus transceiver at $4000 then reads back whichever row
       was last selected. This wiring was determined empirically against the
       firmware's panel-scan loop (see MS800_ANALYSIS.md "Open Question:
       Button Read Address"). */
    int in_m114s_seq;            /* bytes forwarded to the M114S so far (0 = idle) */
    int scan_row;

    atomic_uchar display;        /* last byte written to $8000 */
    atomic_uchar buttons[MS800_BTN_COUNT + 1];   /* [1..10] held down */

    M114S chip;
    ACIA  acia;

    double  sample_rate;
    int64_t rate_milli;          /* sample rate in mHz, for exact cycle accounting */
    int64_t cycle_accum;         /* CPU cycles owed, x1000 */
    int64_t irq_countdown;
    int     last_acia_irq;

    ms800_midi_out_fn midi_out;
    void             *midi_out_user;
};

/* ── Bus ─────────────────────────────────────────────────────────────────── */

static uint8_t bus_read(void *ctx, uint16_t addr) {
    ms800_t *m = ctx;
    if (addr >= 0xE000) return m->prog_rom[addr - 0xE000];
    if (addr <  0x2000) return m->ram[addr];
    if (addr == 0x4000) {
        /* Any read aborts a partial M114S sequence, keeping the chip's own
           auto-reset-on-timeout as the sole recovery path for stray bytes. */
        m->in_m114s_seq = 0;

        /* Button matrix readback (active-low, 5 columns per row).
           Row 0 selected (bits[1:0]==0) scans BOTH rows simultaneously —
           used by the firmware's idle/pre-scan check to detect "any button
           held" before doing a full per-row scan. */
        uint8_t r = 0x1F;
        const atomic_uchar *b = m->buttons;
        if (m->scan_row == 1 || m->scan_row == 0) {
            if (b[MS800_BTN_DOWN_DOWN])  r &= ~0x01;
            if (b[MS800_BTN_CHANNEL])    r &= ~0x02;
            if (b[MS800_BTN_PATCH])      r &= ~0x04;
            if (b[MS800_BTN_UP])         r &= ~0x08;
            if (b[MS800_BTN_ENTER])      r &= ~0x10;
        }
        if (m->scan_row == 2 || m->scan_row == 0) {
            if (b[MS800_BTN_DOWN])       r &= ~0x01;
            if (b[MS800_BTN_UP_UP])      r &= ~0x02;
            if (b[MS800_BTN_NEXT_PARAM]) r &= ~0x04;
            if (b[MS800_BTN_FUNCTION])   r &= ~0x08;
            if (b[MS800_BTN_PARAM])      r &= ~0x10;
        }
        return r;
    }
    if (addr == 0x6000) return acia_read_status(&m->acia);
    if (addr == 0x6001) return acia_read_data(&m->acia);
    return 0xFF;
}

static void bus_write(void *ctx, uint16_t addr, uint8_t val) {
    ms800_t *m = ctx;
    if (addr < 0x2000) { m->ram[addr] = val; return; }
    if (addr == 0x2000) {
        if (m->in_m114s_seq > 0) {
            m114s_write(&m->chip, val);
            m->in_m114s_seq++;
            /* Go idle the instant the 8th data byte has been forwarded, so
               the firmware's following (9th) status-probe write is NOT fed
               to the chip as a stray byte 1 of a new program. */
            if (m->in_m114s_seq >= 8) m->in_m114s_seq = 0;
        } else if (val & 0x80) {
            m114s_write(&m->chip, val);
            m->in_m114s_seq = 1;
        } else {
            m->scan_row = val & 0x03;
        }
        return;
    }
    if (addr == 0x6000) { acia_write_ctrl(&m->acia, val); return; }
    if (addr == 0x6001) { acia_write_data(&m->acia, val); return; }
    if (addr == 0x8000) { atomic_store_explicit(&m->display, val, memory_order_relaxed); return; }
    /* The firmware ($E9AB) writes the same value to both ranges five times;
       either one loads the latch. See m114s.c. */
    if ((addr >= 0xA000 && addr <= 0xA03F) || (addr >= 0xC000 && addr <= 0xC03F)) {
        m114s_map_write(&m->chip, addr & 0x3F, val);
        return;
    }
}

static void on_acia_tx(void *user, uint8_t byte) {
    ms800_t *m = user;
    if (m->midi_out) m->midi_out(m->midi_out_user, byte);
}

/* ── Lifecycle ───────────────────────────────────────────────────────────── */

ms800_t *ms800_create(const uint8_t *prog_rom, size_t prog_size,
                      const uint8_t *wave_rom, size_t wave_size,
                      double sample_rate) {
    if (prog_size != MS800_PROG_ROM_SIZE || wave_size != MS800_WAVE_ROM_SIZE || sample_rate <= 0)
        return NULL;
    ms800_t *m = calloc(1, sizeof(*m));
    if (!m) return NULL;
    memcpy(m->prog_rom, prog_rom, prog_size);
    memcpy(m->wave_rom, wave_rom, wave_size);
    m->sample_rate = sample_rate;
    m->rate_milli = llround(sample_rate * 1000.0);
    m->bus = (cpu6502_bus){ m, bus_read, bus_write };
    m114s_init(&m->chip, m->wave_rom, MS800_WAVE_ROM_SIZE, M114S_HZ, sample_rate);
    acia_init(&m->acia);
    acia_set_tx_callback(&m->acia, on_acia_tx, m);
    for (int i = 0; i <= MS800_BTN_COUNT; i++) atomic_init(&m->buttons[i], 0);
    atomic_init(&m->display, 0);
    ms800_reset(m);
    return m;
}

void ms800_destroy(ms800_t *m) { free(m); }

void ms800_reset(ms800_t *m) {
    m114s_reset(&m->chip);   /* silence all channels, as at power-on */
    acia_reset(&m->acia);
    m->in_m114s_seq = 0;
    m->scan_row = 0;
    m->irq_countdown = CYCLES_PER_IRQ;
    m->last_acia_irq = 0;
    m->cycle_accum = 0;
    memset(&m->cpu, 0, sizeof(m->cpu));
    cpu6502_enter(&m->bus, &m->cpu);
    cpu6502_reset();
    cpu6502_leave(&m->cpu);
}

void ms800_set_sample_rate(ms800_t *m, double sample_rate) {
    if (sample_rate <= 0) return;
    m->sample_rate = sample_rate;
    m->rate_milli = llround(sample_rate * 1000.0);
    m->cycle_accum = 0;
    m114s_set_sample_rate(&m->chip, sample_rate);
}

/* ── MIDI ────────────────────────────────────────────────────────────────── */

int ms800_midi_in(ms800_t *m, uint8_t byte) { return acia_rx_push(&m->acia, byte); }

void ms800_set_midi_out(ms800_t *m, ms800_midi_out_fn fn, void *user) {
    m->midi_out = fn;
    m->midi_out_user = user;
}

/* ── Execution ───────────────────────────────────────────────────────────── */

/* Run whole instructions until at least `cycles` CPU cycles have elapsed,
   clocking the ACIA and M114S alongside and raising the timer IRQ and the
   ACIA's NMI. The CPU state must already be entered for this thread. */
static void run_cpu(ms800_t *m, int cycles) {
    while (cycles > 0) {
        uint32_t done = cpu6502_step();
        for (uint32_t i = 0; i < done; i++) acia_clock_edge(&m->acia);
        m114s_advance_cycles(&m->chip, (uint64_t)done * (M114S_HZ / CPU_HZ));
        cycles -= (int)done;

        m->irq_countdown -= done;
        if (m->irq_countdown <= 0) { m->irq_countdown += CYCLES_PER_IRQ; cpu6502_irq(); }

        int asserted = acia_irq_asserted(&m->acia);
        if (asserted && !m->last_acia_irq) cpu6502_nmi();
        m->last_acia_irq = asserted;
    }
}

/* CPU cycles to run before the next output frame (2,000,000 / 48,000 =
   41.67 at 48 kHz), accumulated in exact integers. */
static int cycles_for_frame(ms800_t *m) {
    m->cycle_accum += (int64_t)CPU_HZ * 1000;
    int c = (int)(m->cycle_accum / m->rate_milli);
    m->cycle_accum -= (int64_t)c * m->rate_milli;
    return c;
}

void ms800_render_s16(ms800_t *m, int16_t *out, int frames) {
    cpu6502_enter(&m->bus, &m->cpu);
    for (int i = 0; i < frames; i++) {
        run_cpu(m, cycles_for_frame(m));
        int16_t s[2];
        m114s_read_stereo(&m->chip, s);
        if (out) { out[2 * i] = s[0]; out[2 * i + 1] = s[1]; }
    }
    cpu6502_leave(&m->cpu);
}

void ms800_render_f32(ms800_t *m, float *left, float *right, int frames) {
    cpu6502_enter(&m->bus, &m->cpu);
    for (int i = 0; i < frames; i++) {
        run_cpu(m, cycles_for_frame(m));
        int16_t s[2];
        m114s_read_stereo(&m->chip, s);
        if (left)  left[i]  = s[0] * (1.0f / 32768.0f);
        if (right) right[i] = s[1] * (1.0f / 32768.0f);
    }
    cpu6502_leave(&m->cpu);
}

/* ── Panel, RAM, debug ───────────────────────────────────────────────────── */

void ms800_set_button(ms800_t *m, int button, int down) {
    if (button >= 1 && button <= MS800_BTN_COUNT)
        atomic_store_explicit(&m->buttons[button], down ? 1 : 0, memory_order_relaxed);
}

int ms800_button(const ms800_t *m, int button) {
    if (button < 1 || button > MS800_BTN_COUNT) return 0;
    return atomic_load_explicit(&((ms800_t *)m)->buttons[button], memory_order_relaxed);
}

uint8_t ms800_display(const ms800_t *m) {
    return atomic_load_explicit(&((ms800_t *)m)->display, memory_order_relaxed);
}

void ms800_get_ram(const ms800_t *m, uint8_t out[MS800_RAM_SIZE]) { memcpy(out, m->ram, MS800_RAM_SIZE); }
void ms800_set_ram(ms800_t *m, const uint8_t in[MS800_RAM_SIZE]) { memcpy(m->ram, in, MS800_RAM_SIZE); }

void ms800_set_trace(ms800_t *m, int on) { m114s_debug(&m->chip, on); }
