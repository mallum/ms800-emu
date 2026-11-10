#pragma once
#include <stdint.h>

/*
 * MS800 address bus decode (see MS800_ANALYSIS.md "Memory Map"):
 *
 *   $0000-$1FFF  RAM (8KB)
 *   $2000        M114S data write / front-panel button-scan row select
 *   $4000        M114S status read / front-panel button-matrix readback
 *   $6000        6850 ACIA control (write) / status (read)
 *   $6001        6850 ACIA data (read RX / write TX)
 *   $8000        Front-panel display, packed BCD (write-only)
 *   $A000-$A03F  PCB wave address latch (write-only)
 *   $C000-$C03F  PCB wave address latch (write-only, same data)
 *   $E000-$FFFF  Program ROM (8KB)
 */

void bus_init(const uint8_t *prog_rom, int prog_size,
              const uint8_t *wave_rom, int wave_size);

uint8_t bus_read(uint16_t addr);
void    bus_write(uint16_t addr, uint8_t val);

/* Front-panel state, shared with the UI layer. */
extern volatile uint8_t panel_display;        /* last byte written to $8000: packed BCD */
extern volatile uint8_t bus_button_state[11]; /* [1..10] = buttons held down; [0] unused */

/* Advance chip clocks in step with CPU execution. Call after every batch of
   CPU cycles with the number of CPU cycles just executed. */
void bus_advance_cycles(int cpu_cycles);
