#pragma once
#include <stdint.h>
#include <stdatomic.h>

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
 * Call acia_clock_edge() once per CPU cycle (2MHz CPU == 2MHz ACIA clock on
 * this board).
 */

typedef void (*acia_tx_byte_fn)(void *user, uint8_t byte);

#define ACIA_RXQ_SIZE 4096   /* power of two; holds a full SysEx bank dump */

typedef struct {
    uint8_t status, tdr, rdr;
    int first_master_reset, dcd_irq_pending, overrun_pending;
    int divide, bits, stopbits, parity, brk;
    int rts, dcd, irq;
    int txc, txd, tx_state, tx_bits, tx_shift, tx_parity, tx_counter, tx_irq_enable;
    int rxc, rxd, rx_state, rx_bits, rx_shift, rx_parity, rx_counter, rx_irq_enable;

    /* RX byte -> bit serialiser */
    int      rx_ser_active;
    uint32_t rx_ser_bits;     /* start+data(+parity)+stop bit sequence, LSB first */
    int      rx_ser_nbits, rx_ser_pos, rx_ser_subcount;

    /* MIDI-in queue: single producer (acia_rx_push), single consumer (the
       thread running the emulation). Lock-free. */
    uint8_t     rxq[ACIA_RXQ_SIZE];
    atomic_uint rxq_head, rxq_tail;

    acia_tx_byte_fn tx_cb;
    void           *tx_user;
} ACIA;

void acia_init(ACIA *a);
void acia_reset(ACIA *a);            /* power-on / master-reset defaults */

void    acia_write_ctrl(ACIA *a, uint8_t v);
void    acia_write_data(ACIA *a, uint8_t v);
uint8_t acia_read_status(ACIA *a);
uint8_t acia_read_data(ACIA *a);

/* Advance the chip's internal UART bit-clock by one CPU cycle. */
void acia_clock_edge(ACIA *a);

/* True while the chip's IRQ output is asserted (wired to the 6502's NMI).
   The caller should edge-detect this and pulse an NMI once per 0->1
   transition; the firmware's own register read clears RDRF and thus
   deasserts the line before the next byte. */
int acia_irq_asserted(const ACIA *a);

/* Queue a byte received from MIDI IN. Safe to call from one producer thread
   while another thread runs the emulation. The byte is serialised onto RXD
   bit-by-bit at whatever baud rate the firmware has programmed. Returns 0 if
   the queue is full and the byte was dropped. */
int acia_rx_push(ACIA *a, uint8_t byte);

/* Callback invoked once per byte, exactly when the chip loads it into the
   transmit shift register (the real start-bit boundary). */
void acia_set_tx_callback(ACIA *a, acia_tx_byte_fn fn, void *user);
