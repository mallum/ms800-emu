/*
 * Wrapper around the vendored, public-domain fake6502 core
 * (fake6502_vendor.h — "MyLittle6502" fork by David MHS Webster / gek169,
 * originally by Mike Chambers, CC0/public domain).
 *
 * The core keeps its registers in (thread-local) globals. A machine owns a
 * cpu6502_regs and a bus; cpu6502_enter() loads them for the calling
 * thread, the step/interrupt calls run against them, and cpu6502_leave()
 * stores the registers back. Only the six architectural registers survive
 * between instructions, so this swap is exact.
 */
#pragma once
#include <stdint.h>

typedef struct {
    uint16_t pc;
    uint8_t  sp, a, x, y, status;
} cpu6502_regs;

typedef struct {
    void    *ctx;
    uint8_t (*read)(void *ctx, uint16_t addr);
    void    (*write)(void *ctx, uint16_t addr, uint8_t value);
} cpu6502_bus;

void     cpu6502_enter(const cpu6502_bus *bus, const cpu6502_regs *regs);
void     cpu6502_leave(cpu6502_regs *regs);

void     cpu6502_reset(void);   /* reads the reset vector through the bus */
uint32_t cpu6502_step(void);    /* one instruction; returns cycles taken */
void     cpu6502_irq(void);
void     cpu6502_nmi(void);
