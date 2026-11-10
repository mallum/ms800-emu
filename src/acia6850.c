/*
 * Motorola 6850 ACIA — ported from MAME's acia6850_device
 * (src/devices/machine/6850acia.cpp, BSD-3-Clause, copyright-holders: smf).
 * The register/status logic and the RX/TX bit-shift state machines below
 * are that device's control_w/data_w/data_r/status_r/write_rxc/write_txc
 * ported near-verbatim to plain C; only the MAME device/scheduler/devcb
 * plumbing has been replaced. See acia6850.h.
 */
#include "acia6850.h"
#include <SDL2/SDL.h>
#include <string.h>

enum {
    SR_RDRF = 0x01, SR_TDRE = 0x02, SR_DCD = 0x04, SR_CTS = 0x08,
    SR_FE = 0x10, SR_OVRN = 0x20, SR_PE = 0x40, SR_IRQ = 0x80
};
enum { STATE_START, STATE_DATA, STATE_STOP };
enum { PARITY_NONE, PARITY_ODD, PARITY_EVEN };
enum { DCD_IRQ_NONE = 0, DCD_IRQ_READ_DATA, DCD_IRQ_READ_STATUS };

static const int counter_divide_select[4] = { 1, 16, 64, 0 };
static const int word_select[8][3] = {
    { 7, PARITY_EVEN, 2 }, { 7, PARITY_ODD, 2 }, { 7, PARITY_EVEN, 1 }, { 7, PARITY_ODD, 1 },
    { 8, PARITY_NONE, 2 }, { 8, PARITY_NONE, 1 }, { 8, PARITY_EVEN, 1 }, { 8, PARITY_ODD, 1 },
};
static const int transmitter_control[4][3] = {
    { 0, 0, 0 }, { 0, 0, 1 }, { 1, 0, 0 }, { 0, 1, 0 },
};

static struct {
    uint8_t status, tdr, rdr;
    int first_master_reset, dcd_irq_pending, overrun_pending;
    int divide, bits, stopbits, parity, brk;
    int rts, dcd, irq;
    int txc, txd, tx_state, tx_bits, tx_shift, tx_parity, tx_counter, tx_irq_enable;
    int rxc, rxd, rx_state, rx_bits, rx_shift, rx_parity, rx_counter, rx_irq_enable;
} a;

static acia_tx_byte_fn tx_byte_cb = 0;

/* ── RX byte queue (MIDI-in thread -> audio thread) ─────────────────────── */
#define RXQ_SIZE 256
static uint8_t rxq[RXQ_SIZE];
static int     rxq_head = 0, rxq_tail = 0;
static SDL_SpinLock rxq_lock = 0;

void acia_rx_push(uint8_t byte) {
    SDL_AtomicLock(&rxq_lock);
    int next = (rxq_tail + 1) % RXQ_SIZE;
    if (next != rxq_head) { rxq[rxq_tail] = byte; rxq_tail = next; }
    SDL_AtomicUnlock(&rxq_lock);
}
static int rxq_pop(uint8_t *out) {
    int got = 0;
    SDL_AtomicLock(&rxq_lock);
    if (rxq_head != rxq_tail) { *out = rxq[rxq_head]; rxq_head = (rxq_head + 1) % RXQ_SIZE; got = 1; }
    SDL_AtomicUnlock(&rxq_lock);
    return got;
}

void acia_set_tx_byte_callback(acia_tx_byte_fn fn) { tx_byte_cb = fn; }

/* ── Line outputs ────────────────────────────────────────────────────────── */
static void output_txd(int txd) { a.txd = txd; }
static void output_rts(int rts) { a.rts = rts; }
static void output_irq(int irq) {
    if (a.irq != irq) {
        a.irq = irq;
        if (irq) a.status &= ~SR_IRQ; else a.status |= SR_IRQ;
    }
}

static int calculate_txirq(void) { return !(a.tx_irq_enable && a.divide && (a.status & SR_TDRE) && !(a.status & SR_CTS)); }
static int calculate_rxirq(void) { return !(a.rx_irq_enable && a.divide && ((a.status & SR_RDRF) || a.dcd_irq_pending != DCD_IRQ_NONE)); }
static void update_irq(void) { output_irq(calculate_txirq() && calculate_rxirq()); }

int acia_irq_asserted(void) { return !a.irq; }  /* IRQ line is active-low */

/* ── Register access ─────────────────────────────────────────────────────── */
uint8_t acia_read_status(void) {
    uint8_t status = a.status;
    if (a.divide == 0 || (status & SR_CTS)) status &= ~SR_TDRE;
    if (a.dcd_irq_pending == DCD_IRQ_READ_STATUS) a.dcd_irq_pending = DCD_IRQ_READ_DATA;
    return status;
}

void acia_write_ctrl(uint8_t data) {
    int cds = (data >> 0) & 3;
    a.divide = counter_divide_select[cds];

    int ws = (data >> 2) & 7;
    a.bits = word_select[ws][0];
    a.parity = word_select[ws][1];
    a.stopbits = word_select[ws][2];

    int tc = (data >> 5) & 3;
    int rts = transmitter_control[tc][0];
    a.brk = transmitter_control[tc][1];
    a.tx_irq_enable = transmitter_control[tc][2];

    a.rx_irq_enable = (data >> 7) & 1;

    if (a.divide == 0) {
        if (a.first_master_reset) { rts = 1; a.first_master_reset = 0; }
        a.dcd_irq_pending = DCD_IRQ_NONE;
        a.overrun_pending = 0;
        a.rx_state = STATE_START;
        a.rx_counter = 0;
        a.tx_state = STATE_START;
        output_txd(1);
        a.status |= SR_TDRE;
        a.status &= (SR_CTS | SR_TDRE);
        if (a.dcd) a.status |= SR_DCD;
    }
    output_rts(rts);
    update_irq();
}

void acia_write_data(uint8_t data) {
    if (a.divide != 0) { a.tdr = data; a.status &= ~SR_TDRE; }
    update_irq();
}

uint8_t acia_read_data(void) {
    if (a.overrun_pending) { a.status |= SR_OVRN; a.overrun_pending = 0; }
    else { a.status &= ~SR_OVRN; a.status &= ~SR_RDRF; }
    if (a.dcd_irq_pending == DCD_IRQ_READ_DATA) a.dcd_irq_pending = DCD_IRQ_NONE;
    update_irq();
    return a.rdr;
}

static void write_dcd(int state) { a.dcd = state; }
static void write_cts(int state) { if (state) a.status |= SR_CTS; else a.status &= ~SR_CTS; }
static void write_rxd(int state) { a.rxd = state; }

static void write_rxc(int state) {
    if (a.rxc == state) return;
    a.rxc = state;
    if (!(state && a.divide > 0)) return;

    if (a.dcd) {
        if (!(a.status & SR_DCD)) { a.status |= SR_DCD; a.dcd_irq_pending = DCD_IRQ_READ_STATUS; }
        a.rx_state = STATE_START;
        a.rx_counter = 0;
    } else {
        if (a.dcd_irq_pending == DCD_IRQ_NONE) a.status &= ~SR_DCD;
        a.rx_counter++;

        switch (a.rx_state) {
        case STATE_START:
            if (a.rxd == 0) {
                if (a.rx_counter >= a.divide / 2) {
                    a.rx_state = STATE_DATA; a.rx_counter = 0;
                    a.rx_shift = 0; a.rx_parity = 0; a.rx_bits = 0;
                }
            } else {
                a.rx_counter = 0;
            }
            break;
        case STATE_DATA:
            if (a.rx_counter == a.divide) {
                a.rx_counter = 0;
                if (a.rxd) a.rx_shift |= 1 << a.rx_bits;
                a.rx_bits++;
                a.rx_parity ^= a.rxd;
                if ((a.rx_bits == a.bits && a.parity == PARITY_NONE) ||
                    (a.rx_bits == a.bits + 1 && a.parity != PARITY_NONE)) {
                    if (a.status & SR_RDRF) {
                        a.overrun_pending = 1;
                    } else {
                        if (a.parity == PARITY_ODD) a.rx_parity = !a.rx_parity;
                        if (a.parity != PARITY_NONE && a.rx_parity) a.status |= SR_PE; else a.status &= ~SR_PE;
                        a.rdr = (uint8_t)a.rx_shift;
                        if (a.bits == 7 && a.parity != PARITY_NONE) a.rdr &= 0x7F;
                        a.status |= SR_RDRF;
                    }
                    a.rx_state = STATE_STOP;
                }
            }
            break;
        case STATE_STOP:
            if (a.rx_counter == a.divide) {
                a.rx_counter = 0;
                if (!a.rxd) a.status |= SR_FE; else a.status &= ~SR_FE;
                a.rx_state = STATE_START;
            }
            break;
        }
    }
    update_irq();
}

static void write_txc(int state) {
    if (a.txc == state) return;
    a.txc = state;
    if (!(!state && a.divide > 0)) return;

    a.tx_counter++;
    switch (a.tx_state) {
    case STATE_START:
        a.tx_counter = 0;
        if (!(a.status & SR_TDRE) && !(a.status & SR_CTS)) {
            a.tx_state = STATE_DATA;
            a.tx_shift = a.tdr;
            a.tx_bits = 0;
            a.tx_parity = 0;
            a.status |= SR_TDRE;
            output_txd(0);
            if (tx_byte_cb) tx_byte_cb((uint8_t)a.tx_shift);
        } else {
            output_txd(!a.brk);
        }
        break;
    case STATE_DATA:
        if (a.tx_counter == a.divide) {
            a.tx_counter = 0;
            if (a.tx_bits < a.bits) {
                output_txd((a.tx_shift >> a.tx_bits) & 1);
                a.tx_bits++;
                a.tx_parity ^= a.txd;
            } else if (a.tx_bits == a.bits && a.parity != PARITY_NONE) {
                a.tx_bits++;
                if (a.parity == PARITY_ODD) a.tx_parity = !a.tx_parity;
                output_txd(a.tx_parity);
            } else {
                a.tx_state = STATE_STOP;
                a.tx_bits = 0;
                output_txd(1);
            }
        }
        break;
    case STATE_STOP:
        if (a.tx_counter == a.divide) {
            a.tx_counter = 0;
            a.tx_bits++;
            if (a.tx_bits == a.stopbits) a.tx_state = STATE_START;
        }
        break;
    }
    update_irq();
}

/* ── RX byte -> bit serialiser ───────────────────────────────────────────── */
/* Feeds queued MIDI-in bytes onto the RXD line one bit at a time, paced by
   the chip's own configured divide (== the real bit period once the
   firmware has programmed 8N1/÷64), so RDRF/NMI timing matches real MIDI
   byte-arrival timing instead of appearing instantly. */
static int      rx_ser_active = 0;
static uint32_t rx_ser_bits = 0;   /* pre-built start+data(+parity)+stop bit sequence, LSB first */
static int      rx_ser_nbits = 0;
static int      rx_ser_pos = 0;
static int      rx_ser_subcount = 0;

static void rx_serializer_step(void) {
    if (!rx_ser_active) {
        uint8_t byte;
        if (a.divide == 0 || !rxq_pop(&byte)) { write_rxd(1); return; }
        uint32_t bits = 0;
        int n = 0;
        bits |= 0u << n; n++;                       /* start bit */
        for (int i = 0; i < a.bits; i++) { bits |= (uint32_t)((byte >> i) & 1) << n; n++; }
        if (a.parity != PARITY_NONE) {
            int p = 0; for (int i = 0; i < a.bits; i++) p ^= (byte >> i) & 1;
            if (a.parity == PARITY_ODD) p = !p;
            bits |= (uint32_t)p << n; n++;
        }
        for (int i = 0; i < a.stopbits; i++) { bits |= 1u << n; n++; }
        rx_ser_bits = bits; rx_ser_nbits = n; rx_ser_pos = 0; rx_ser_subcount = 0;
        rx_ser_active = 1;
        write_rxd((rx_ser_bits >> 0) & 1);
        return;
    }
    rx_ser_subcount++;
    if (rx_ser_subcount >= a.divide) {
        rx_ser_subcount = 0;
        rx_ser_pos++;
        if (rx_ser_pos >= rx_ser_nbits) { rx_ser_active = 0; write_rxd(1); return; }
        write_rxd((rx_ser_bits >> rx_ser_pos) & 1);
    }
}

/* ── Public entry points ─────────────────────────────────────────────────── */
void acia_init(void) {
    memset(&a, 0, sizeof(a));
    a.first_master_reset = 1;
    write_dcd(0);   /* MS800 ties DCD low: no carrier-detect signalling on a MIDI UART */
    write_cts(0);   /* and CTS low: always clear to send */
    acia_reset();
}

void acia_reset(void) {
    output_txd(1);
    output_rts(1);
    a.irq = -1;      /* force the first output_irq(1) call to actually apply */
    output_irq(1);
    a.status = SR_TDRE;
}

void acia_clock_edge(void) {
    rx_serializer_step();
    write_rxc(0);
    write_rxc(1);
    write_txc(1);
    write_txc(0);
}
