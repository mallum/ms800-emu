/*
 * fake6502 core instantiation.
 *
 * fake6502_vendor.h is public-domain (CC0) third-party code: "Fake6502 CPU
 * emulator core" originally by Mike Chambers, bug-fixed/maintained as
 * "MyLittle6502" by David MHS Webster (github.com/C-Chads/MyLittle6502).
 * It is vendored here unmodified; this file just instantiates it.
 *
 * FAKE6502_NOT_STATIC makes the register set and instruction-count globals
 * external (rather than file-static) so bus.c/main.c can read them via
 * fake6502.h. FAKE6502_USE_STDINT is deliberately left undefined: the
 * vendored header has a missing-semicolon typo in that branch
 * ("typedef uint32_t uint32" with no trailing ';'). Its default typedefs
 * (unsigned short/char/int) are exactly 16/8/32 bits on every platform this
 * project targets, so they are equivalent to uint16_t/uint8_t/uint32_t
 * without needing to patch the vendor file.
 */
#define FAKE6502_NOT_STATIC
#include "fake6502_vendor.h"
