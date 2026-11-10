#include "bus.h"
#include "m114s.h"
#include "acia6850.h"
#include "fake6502.h"
#include <string.h>

static uint8_t ram[0x2000];
static const uint8_t *prog_rom;

volatile uint8_t panel_display = 0;
volatile uint8_t bus_button_state[11] = {0};

/* $A000-$A03F / $C000-$C03F: the PCB waveform address latch (64 entries,
   indexed by M114S A9-A14). The firmware ($E9AB) writes the same value to
   both ranges five times; either one loads the map. See m114s.c. */

/* $2000 is dual-purpose: an 8-byte-programming write to the M114S data bus
   (triggered by the firmware setting bit 7 on the first byte) or, when
   idle, a front-panel button-matrix row-select strobe (bits[1:0]). The
   74HCT245 bus transceiver at $4000 then reads back whichever row was last
   selected. This wiring was determined empirically against the firmware's
   panel-scan loop, since the ROM disassembly alone leaves it as an open
   question (see MS800_ANALYSIS.md "Open Question: Button Read Address"). */
static int in_m114s_seq = 0;   /* bytes forwarded to the M114S so far (0 = idle) */
static int scan_row = 0;

/* fake6502 callbacks */
uint8_t read6502(uint16_t addr)             { return bus_read(addr); }
void    write6502(uint16_t addr, uint8_t v) { bus_write(addr, v); }

void bus_init(const uint8_t *prog, int psz, const uint8_t *wave, int wsz) {
    memset(ram, 0, sizeof(ram));
    prog_rom = prog;
    (void)psz;
    m114s_init(wave, wsz, 4000000);  /* M114A: 4MHz part */
    acia_init();
}

void bus_advance_cycles(int cpu_cycles) {
    /* M114S runs at 4MHz against a 2MHz CPU. */
    m114s_advance_cycles((uint64_t)cpu_cycles * 2);
}

uint8_t bus_read(uint16_t addr) {
    if (addr >= 0xE000) return prog_rom[addr - 0xE000];
    if (addr <  0x2000) return ram[addr];
    if (addr == 0x4000) {
        /* Any read aborts a partial M114S sequence, keeping the chip's own
           auto-reset-on-timeout as the sole recovery path for stray bytes. */
        in_m114s_seq = 0;

        /* Button matrix readback (active-low, 5 columns per row).
           Row 0 selected (bits[1:0]==0) scans BOTH rows simultaneously —
           used by the firmware's idle/pre-scan check to detect "any button
           held" before doing a full per-row scan. */
        uint8_t r = 0x1F;
        if (scan_row == 1 || scan_row == 0) {
            if (bus_button_state[7])  r &= ~0x01;  /* Down Down */
            if (bus_button_state[4])  r &= ~0x02;  /* Up Up     */
            if (bus_button_state[9])  r &= ~0x04;  /* Next Param*/
            if (bus_button_state[1])  r &= ~0x08;  /* Channel   */
            if (bus_button_state[10]) r &= ~0x10;  /* Enter     */
        }
        if (scan_row == 2 || scan_row == 0) {
            if (bus_button_state[2])  r &= ~0x01;  /* Patch     */
            if (bus_button_state[6])  r &= ~0x02;  /* Down      */
            if (bus_button_state[8])  r &= ~0x04;  /* Up        */
            if (bus_button_state[5])  r &= ~0x08;  /* Function  */
            if (bus_button_state[3])  r &= ~0x10;  /* Param     */
        }
        return r;
    }
    if (addr == 0x6000) return acia_read_status();
    if (addr == 0x6001) return acia_read_data();
    return 0xFF;
}

void bus_write(uint16_t addr, uint8_t val) {
    if (addr < 0x2000) { ram[addr] = val; return; }
    if (addr == 0x2000) {
        if (in_m114s_seq > 0) {
            m114s_write(val);
            in_m114s_seq++;
            /* Go idle the instant the 8th data byte has been forwarded, so
               the firmware's following (9th) status-probe write is NOT fed
               to the chip as a stray byte 1 of a new program. Off-by-one
               here previously let that byte through, which the M114S's own
               8-byte state machine then treated as real channel data —
               leaving 2-3 channels stuck permanently active/audible after
               boot even with no MIDI input. */
            if (in_m114s_seq >= 8) in_m114s_seq = 0;
        } else if (val & 0x80) {
            m114s_write(val);
            in_m114s_seq = 1;
        } else {
            scan_row = val & 0x03;
        }
        return;
    }
    if (addr == 0x6000) { acia_write_ctrl(val); return; }
    if (addr == 0x6001) { acia_write_data(val); return; }
    if (addr == 0x8000) { panel_display = val; return; }
    if ((addr >= 0xA000 && addr <= 0xA03F) || (addr >= 0xC000 && addr <= 0xC03F)) {
        m114s_map_write(addr & 0x3F, val);
        return;
    }
}
