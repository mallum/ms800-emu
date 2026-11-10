#pragma once
#include <stdint.h>

/*
 * Motorola 6850 ACIA — bit-accurate port of MAME's acia6850_device
 * (src/devices/machine/6850acia.cpp, BSD-3-Clause, copyright-holders: smf),
 * stripped of the MAME device/scheduler framework.
 *
 * The MS800 wires this chip's clock inputs (RXC/TXC) directly to the 2MHz
 * CPU clock and programs a ÷64 baud-rate divider, giving 2,000,000/64 =
 * 31,250 baud — standard MIDI. Because the real chip serialises data one
 * bit at a time (start bit, 8 data bits, stop bit(s)) sampled at that
 * clock, this port reproduces the same bit-level state machine rather than
 * delivering bytes instantly, so RX NMI timing and TX byte pacing match
 * real hardware instead of an idealised instant FIFO.
 *
 * Call acia_clock_edge() once per CPU cycle from the emulator's main loop
 * (2MHz CPU == 2MHz ACIA clock on this board).
 */

void acia_init(void);
void acia_reset(void);            /* power-on / master-reset defaults */

void    acia_write_ctrl(uint8_t v);
void    acia_write_data(uint8_t v);
uint8_t acia_read_status(void);
uint8_t acia_read_data(void);

/* Advance the chip's internal UART bit-clock by one CPU cycle. */
void acia_clock_edge(void);

/* True while the chip's IRQ output is asserted (wired to the 6502's NMI).
   The caller (main.c) should edge-detect this and pulse nmi6502() once per
   0->1 transition, matching how a level output feeding an edge-triggered
   NMI line behaves; the firmware's own register read clears RDRF/the
   pending condition and thus deasserts the line before the next byte. */
int acia_irq_asserted(void);

/* Feed a byte received from the outside world (MIDI IN) into the chip's
   receive shift register queue; thread-safe, callable from a MIDI input
   thread. The byte is serialised onto RXD bit-by-bit as real MIDI bytes
   arrive on the wire, at whatever baud rate the firmware has programmed. */
void acia_rx_push(uint8_t byte);

/* Registered callback: invoked once per byte, exactly when the chip loads
   it into the transmit shift register and starts sending it (i.e. at the
   real start-bit boundary) — the natural point to hand the byte to a real
   MIDI OUT port with correct 31,250-baud pacing. */
typedef void (*acia_tx_byte_fn)(uint8_t byte);
void acia_set_tx_byte_callback(acia_tx_byte_fn fn);
