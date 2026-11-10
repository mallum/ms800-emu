/*
 * Thin wrapper header around the vendored, public-domain fake6502 core
 * (fake6502_vendor.h — "MyLittle6502" fork by David MHS Webster / gek169,
 * originally by Mike Chambers, CC0/public domain, unmodified).
 *
 * The vendored file is designed to be #included once into a .c file with
 * FAKE6502_NOT_STATIC defined (see fake6502.c). This header just exposes
 * the CPU state and control entry points to the rest of the emulator
 * without pulling in the vendor file's internals a second time.
 */
#pragma once
#include <stdint.h>

/* CPU registers (defined in fake6502.c via the vendored core) */
extern uint16_t pc;
extern uint8_t  sp, a, x, y, status;
extern uint32_t clockticks6502;   /* cycles consumed by the last exec6502() call */
extern uint32_t instructions;     /* running instruction count */

/* Callbacks the caller (bus.c) must implement */
uint8_t read6502(uint16_t address);
void    write6502(uint16_t address, uint8_t value);

/* CPU control */
void     reset6502(void);
uint32_t exec6502(uint32_t tickcount);   /* run until at least tickcount cycles have elapsed */
void     irq6502(void);
void     nmi6502(void);
