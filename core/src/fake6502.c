/*
 * fake6502 core instantiation.
 *
 * fake6502_vendor.h is public-domain (CC0) third-party code: "Fake6502 CPU
 * emulator core" originally by Mike Chambers, bug-fixed/maintained as
 * "MyLittle6502" by David MHS Webster (github.com/C-Chads/MyLittle6502).
 * The only local change is the FAKE6502_TLS storage qualifier on its state
 * (marked "ms800-emu patch"), used here to make that state thread-local.
 *
 * FAKE6502_USE_STDINT is deliberately left undefined: the vendored header
 * has a missing-semicolon typo in that branch ("typedef uint32_t uint32"
 * with no trailing ';'). Its default typedefs (unsigned short/char/int) are
 * exactly 16/8/32 bits on every platform this project targets.
 */
#include "fake6502.h"

#define FAKE6502_TLS _Thread_local
#include "fake6502_vendor.h"

static _Thread_local cpu6502_bus cur_bus;

uint8 read6502(ushort address)              { return cur_bus.read(cur_bus.ctx, address); }
void  write6502(ushort address, uint8 value) { cur_bus.write(cur_bus.ctx, address, value); }

void cpu6502_enter(const cpu6502_bus *bus, const cpu6502_regs *regs) {
    cur_bus = *bus;
    pc = regs->pc;
    sp = regs->sp;
    a = regs->a;
    x = regs->x;
    y = regs->y;
    status = regs->status;
}

void cpu6502_leave(cpu6502_regs *regs) {
    regs->pc = pc;
    regs->sp = sp;
    regs->a = a;
    regs->x = x;
    regs->y = y;
    regs->status = status;
}

void     cpu6502_reset(void) { reset6502(); }
uint32_t cpu6502_step(void)  { return exec6502(1); }
void     cpu6502_irq(void)   { irq6502(); }
void     cpu6502_nmi(void)   { nmi6502(); }
