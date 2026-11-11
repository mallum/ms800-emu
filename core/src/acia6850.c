/*
 * Motorola 6850 ACIA — ported from MAME's acia6850_device
 * (src/devices/machine/6850acia.cpp, BSD-3-Clause, copyright-holders: smf).
 * The register/status logic and the RX/TX bit-shift state machines below
 * are that device's control_w/data_w/data_r/status_r/write_rxc/write_txc
 * ported near-verbatim to plain C; only the MAME device/scheduler/devcb
 * plumbing has been replaced. See acia6850.h.
 */
#include "acia6850.h"
#include <stddef.h>
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

/* ── RX byte queue (MIDI-in producer -> emulation consumer) ─────────────── */
int acia_rx_push(ACIA *a, uint8_t byte) {
    unsigned tail = atomic_load_explicit(&a->rxq_tail, memory_order_relaxed);
    unsigned head = atomic_load_explicit(&a->rxq_head, memory_order_acquire);
    unsigned next = (tail + 1) & (ACIA_RXQ_SIZE - 1);
    if (next == head) return 0;                       /* full: drop */
    a->rxq[tail] = byte;
    atomic_store_explicit(&a->rxq_tail, next, memory_order_release);
    return 1;
}
static int rxq_pop(ACIA *a, uint8_t *out) {
    unsigned head = atomic_load_explicit(&a->rxq_head, memory_order_relaxed);
    unsigned tail = atomic_load_explicit(&a->rxq_tail, memory_order_acquire);
    if (head == tail) return 0;
    *out = a->rxq[head];
    atomic_store_explicit(&a->rxq_head, (head + 1) & (ACIA_RXQ_SIZE - 1), memory_order_release);
    return 1;
}

void acia_set_tx_callback(ACIA *a, acia_tx_byte_fn fn, void *user) { a->tx_cb = fn; a->tx_user = user; }

/* ── Line outputs ────────────────────────────────────────────────────────── */
static void output_txd(ACIA *a, int txd) { a->txd = txd; }
static void output_rts(ACIA *a, int rts) { a->rts = rts; }
static void output_irq(ACIA *a, int irq) {
    if (a->irq != irq) {
        a->irq = irq;
        if (irq) a->status &= ~SR_IRQ; else a->status |= SR_IRQ;
    }
}

static int calculate_txirq(const ACIA *a) { return !(a->tx_irq_enable && a->divide && (a->status & SR_TDRE) && !(a->status & SR_CTS)); }
static int calculate_rxirq(const ACIA *a) { return !(a->rx_irq_enable && a->divide && ((a->status & SR_RDRF) || a->dcd_irq_pending != DCD_IRQ_NONE)); }
static void update_irq(ACIA *a) { output_irq(a, calculate_txirq(a) && calculate_rxirq(a)); }

int acia_irq_asserted(const ACIA *a) { return !a->irq; }  /* IRQ line is active-low */

/* ── Register access ─────────────────────────────────────────────────────── */
uint8_t acia_read_status(ACIA *a) {
    uint8_t status = a->status;
    if (a->divide == 0 || (status & SR_CTS)) status &= ~SR_TDRE;
    if (a->dcd_irq_pending == DCD_IRQ_READ_STATUS) a->dcd_irq_pending = DCD_IRQ_READ_DATA;
    return status;
}

void acia_write_ctrl(ACIA *a, uint8_t data) {
    int cds = (data >> 0) & 3;
    a->divide = counter_divide_select[cds];

    int ws = (data >> 2) & 7;
    a->bits = word_select[ws][0];
    a->parity = word_select[ws][1];
    a->stopbits = word_select[ws][2];

    int tc = (data >> 5) & 3;
    int rts = transmitter_control[tc][0];
    a->brk = transmitter_control[tc][1];
    a->tx_irq_enable = transmitter_control[tc][2];

    a->rx_irq_enable = (data >> 7) & 1;

    if (a->divide == 0) {
        if (a->first_master_reset) { rts = 1; a->first_master_reset = 0; }
        a->dcd_irq_pending = DCD_IRQ_NONE;
        a->overrun_pending = 0;
        a->rx_state = STATE_START;
        a->rx_counter = 0;
        a->tx_state = STATE_START;
        output_txd(a, 1);
        a->status |= SR_TDRE;
        a->status &= (SR_CTS | SR_TDRE);
        if (a->dcd) a->status |= SR_DCD;
    }
    output_rts(a, rts);
    update_irq(a);
}

void acia_write_data(ACIA *a, uint8_t data) {
    if (a->divide != 0) { a->tdr = data; a->status &= ~SR_TDRE; }
    update_irq(a);
}

uint8_t acia_read_data(ACIA *a) {
    if (a->overrun_pending) { a->status |= SR_OVRN; a->overrun_pending = 0; }
    else { a->status &= ~SR_OVRN; a->status &= ~SR_RDRF; }
    if (a->dcd_irq_pending == DCD_IRQ_READ_DATA) a->dcd_irq_pending = DCD_IRQ_NONE;
    update_irq(a);
    return a->rdr;
}

static void write_dcd(ACIA *a, int state) { a->dcd = state; }
static void write_cts(ACIA *a, int state) { if (state) a->status |= SR_CTS; else a->status &= ~SR_CTS; }
static void write_rxd(ACIA *a, int state) { a->rxd = state; }

static void write_rxc(ACIA *a, int state) {
    if (a->rxc == state) return;
    a->rxc = state;
    if (!(state && a->divide > 0)) return;

    if (a->dcd) {
        if (!(a->status & SR_DCD)) { a->status |= SR_DCD; a->dcd_irq_pending = DCD_IRQ_READ_STATUS; }
        a->rx_state = STATE_START;
        a->rx_counter = 0;
    } else {
        if (a->dcd_irq_pending == DCD_IRQ_NONE) a->status &= ~SR_DCD;
        a->rx_counter++;

        switch (a->rx_state) {
        case STATE_START:
            if (a->rxd == 0) {
                if (a->rx_counter >= a->divide / 2) {
                    a->rx_state = STATE_DATA; a->rx_counter = 0;
                    a->rx_shift = 0; a->rx_parity = 0; a->rx_bits = 0;
                }
            } else {
                a->rx_counter = 0;
            }
            break;
        case STATE_DATA:
            if (a->rx_counter == a->divide) {
                a->rx_counter = 0;
                if (a->rxd) a->rx_shift |= 1 << a->rx_bits;
                a->rx_bits++;
                a->rx_parity ^= a->rxd;
                if ((a->rx_bits == a->bits && a->parity == PARITY_NONE) ||
                    (a->rx_bits == a->bits + 1 && a->parity != PARITY_NONE)) {
                    if (a->status & SR_RDRF) {
                        a->overrun_pending = 1;
                    } else {
                        if (a->parity == PARITY_ODD) a->rx_parity = !a->rx_parity;
                        if (a->parity != PARITY_NONE && a->rx_parity) a->status |= SR_PE; else a->status &= ~SR_PE;
                        a->rdr = (uint8_t)a->rx_shift;
                        if (a->bits == 7 && a->parity != PARITY_NONE) a->rdr &= 0x7F;
                        a->status |= SR_RDRF;
                    }
                    a->rx_state = STATE_STOP;
                }
            }
            break;
        case STATE_STOP:
            if (a->rx_counter == a->divide) {
                a->rx_counter = 0;
                if (!a->rxd) a->status |= SR_FE; else a->status &= ~SR_FE;
                a->rx_state = STATE_START;
            }
            break;
        }
    }
    update_irq(a);
}

static void write_txc(ACIA *a, int state) {
    if (a->txc == state) return;
    a->txc = state;
    if (!(!state && a->divide > 0)) return;

    a->tx_counter++;
    switch (a->tx_state) {
    case STATE_START:
        a->tx_counter = 0;
        if (!(a->status & SR_TDRE) && !(a->status & SR_CTS)) {
            a->tx_state = STATE_DATA;
            a->tx_shift = a->tdr;
            a->tx_bits = 0;
            a->tx_parity = 0;
            a->status |= SR_TDRE;
            output_txd(a, 0);
            if (a->tx_cb) a->tx_cb(a->tx_user, (uint8_t)a->tx_shift);
        } else {
            output_txd(a, !a->brk);
        }
        break;
    case STATE_DATA:
        if (a->tx_counter == a->divide) {
            a->tx_counter = 0;
            if (a->tx_bits < a->bits) {
                output_txd(a, (a->tx_shift >> a->tx_bits) & 1);
                a->tx_bits++;
                a->tx_parity ^= a->txd;
            } else if (a->tx_bits == a->bits && a->parity != PARITY_NONE) {
                a->tx_bits++;
                if (a->parity == PARITY_ODD) a->tx_parity = !a->tx_parity;
                output_txd(a, a->tx_parity);
            } else {
                a->tx_state = STATE_STOP;
                a->tx_bits = 0;
                output_txd(a, 1);
            }
        }
        break;
    case STATE_STOP:
        if (a->tx_counter == a->divide) {
            a->tx_counter = 0;
            a->tx_bits++;
            if (a->tx_bits == a->stopbits) a->tx_state = STATE_START;
        }
        break;
    }
    update_irq(a);
}

/* ── RX byte -> bit serialiser ───────────────────────────────────────────── */
/* Feeds queued MIDI-in bytes onto the RXD line one bit at a time, paced by
   the chip's own configured divide (== the real bit period once the
   firmware has programmed 8N1/÷64), so RDRF/NMI timing matches real MIDI
   byte-arrival timing instead of appearing instantly. */
static void rx_serializer_step(ACIA *a) {
    if (!a->rx_ser_active) {
        uint8_t byte;
        if (a->divide == 0 || !rxq_pop(a, &byte)) { write_rxd(a, 1); return; }
        uint32_t bits = 0;
        int n = 0;
        bits |= 0u << n; n++;                       /* start bit */
        for (int i = 0; i < a->bits; i++) { bits |= (uint32_t)((byte >> i) & 1) << n; n++; }
        if (a->parity != PARITY_NONE) {
            int p = 0; for (int i = 0; i < a->bits; i++) p ^= (byte >> i) & 1;
            if (a->parity == PARITY_ODD) p = !p;
            bits |= (uint32_t)p << n; n++;
        }
        for (int i = 0; i < a->stopbits; i++) { bits |= 1u << n; n++; }
        a->rx_ser_bits = bits; a->rx_ser_nbits = n; a->rx_ser_pos = 0; a->rx_ser_subcount = 0;
        a->rx_ser_active = 1;
        write_rxd(a, (a->rx_ser_bits >> 0) & 1);
        return;
    }
    a->rx_ser_subcount++;
    if (a->rx_ser_subcount >= a->divide) {
        a->rx_ser_subcount = 0;
        a->rx_ser_pos++;
        if (a->rx_ser_pos >= a->rx_ser_nbits) { a->rx_ser_active = 0; write_rxd(a, 1); return; }
        write_rxd(a, (a->rx_ser_bits >> a->rx_ser_pos) & 1);
    }
}

/* ── Public entry points ─────────────────────────────────────────────────── */
/* Chip state as at power-on. Leaves the MIDI-in queue and TX callback alone. */
static void init_chip(ACIA *a) {
    memset(a, 0, offsetof(ACIA, rxq));
    a->first_master_reset = 1;
    write_dcd(a, 0);   /* MS800 ties DCD low: no carrier-detect signalling on a MIDI UART */
    write_cts(a, 0);   /* and CTS low: always clear to send */
    acia_reset(a);
}

void acia_init(ACIA *a) {
    memset(a, 0, sizeof(*a));
    atomic_init(&a->rxq_head, 0);
    atomic_init(&a->rxq_tail, 0);
    init_chip(a);
}

void acia_power_on(ACIA *a) {
    /* Drop queued input from the consumer side, so a producer thread can
       keep pushing meanwhile. */
    atomic_store_explicit(&a->rxq_head, atomic_load_explicit(&a->rxq_tail, memory_order_acquire),
                          memory_order_release);
    init_chip(a);
}

void acia_reset(ACIA *a) {
    output_txd(a, 1);
    output_rts(a, 1);
    a->irq = -1;      /* force the first output_irq(1) call to actually apply */
    output_irq(a, 1);
    a->status = SR_TDRE;
}

void acia_clock_edge(ACIA *a) {
    rx_serializer_step(a);
    write_rxc(a, 0);
    write_rxc(a, 1);
    write_txc(a, 1);
    write_txc(a, 0);
}
