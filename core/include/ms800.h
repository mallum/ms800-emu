/*
 * ms800 — Cheetah MS800 emulation core.
 *
 * A complete machine: 6502 running the original firmware, the SGS-Thomson
 * M114S sound chip, the 6850 ACIA (MIDI), the PCB wave address latch and the
 * front-panel buttons and display. No platform dependencies; the host feeds
 * MIDI bytes in, pulls audio out and reads/presses the panel.
 *
 * Threading: each machine is used by one audio thread at a time; any number
 * of machines may run concurrently on different threads. ms800_midi_in() may
 * be called from one other thread (e.g. a MIDI input callback); the panel
 * calls are safe from a UI thread.
 */
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MS800_PROG_ROM_SIZE 0x2000   /* 8 KB program ROM, $E000-$FFFF */
#define MS800_WAVE_ROM_SIZE 0x8000   /* 32 KB 27256 wave ROM */
#define MS800_RAM_SIZE      0x2000   /* 8 KB battery-backed RAM, $0000-$1FFF */

/* Front-panel buttons, numbered in the firmware's scan-matrix order, which
   does NOT match the on-screen 2x5 grid. Verified against the firmware
   disassembly and by pressing each one alone from a clean boot:
     1  UP          increments the displayed value by 1
     2  DOWN        decrements the displayed value by 1
     3  PARAM       $E369: parameter-edit counter against the per-patch
                    max table at $FDF2
     4  CHANNEL     $E307: max value 16 (MIDI channels 1-16)
     5  FUNCTION    $E362: max value 10 (the printed panel functions)
     6  UP UP       increments the displayed value by 10
     7  DOWN DOWN   decrements the displayed value by 10
     8  NEXT PARAM  $E44E: steps the $4C counter, clamped to $FDF2
     9  PATCH       $E334: max value 99
     10 ENTER       $E2F9: clears display/edit state (confirm) */
enum {
    MS800_BTN_UP = 1,
    MS800_BTN_DOWN,
    MS800_BTN_PARAM,
    MS800_BTN_CHANNEL,
    MS800_BTN_FUNCTION,
    MS800_BTN_UP_UP,
    MS800_BTN_DOWN_DOWN,
    MS800_BTN_NEXT_PARAM,
    MS800_BTN_PATCH,
    MS800_BTN_ENTER,
    MS800_BTN_COUNT = 10
};

typedef struct ms800 ms800_t;

/* Called for each byte the firmware transmits on MIDI OUT, at the moment the
   ACIA starts sending it (from inside ms800_render). */
typedef void (*ms800_midi_out_fn)(void *user, uint8_t byte);

/* Creates a machine and resets the CPU. The ROM images are copied. Returns
   NULL if a size is wrong or memory runs out. */
ms800_t *ms800_create(const uint8_t *prog_rom, size_t prog_size,
                      const uint8_t *wave_rom, size_t wave_size,
                      double sample_rate);
void     ms800_destroy(ms800_t *m);

/* CPU reset, as at power-on. RAM is kept (it is battery-backed). */
void     ms800_reset(ms800_t *m);

void     ms800_set_sample_rate(ms800_t *m, double sample_rate);

/* Queue one raw MIDI byte for the ACIA (status and data bytes as on the
   wire, including SysEx). Returns 0 if the input queue was full. */
int      ms800_midi_in(ms800_t *m, uint8_t byte);
void     ms800_set_midi_out(ms800_t *m, ms800_midi_out_fn fn, void *user);

/* Run the machine for `frames` output frames. Either output may be NULL.
   s16: interleaved L/R. f32: separate L and R buffers, full scale = 1.0. */
void     ms800_render_s16(ms800_t *m, int16_t *interleaved, int frames);
void     ms800_render_f32(ms800_t *m, float *left, float *right, int frames);

/* Front panel. button is MS800_BTN_*. The display byte is the last value the
   firmware wrote to $8000: two packed BCD digits. */
void     ms800_set_button(ms800_t *m, int button, int down);
int      ms800_button(const ms800_t *m, int button);
uint8_t  ms800_display(const ms800_t *m);

/* Battery-backed RAM (user patches, tones, channel setup), for saving and
   restoring a machine's state. */
void     ms800_get_ram(const ms800_t *m, uint8_t out[MS800_RAM_SIZE]);
void     ms800_set_ram(ms800_t *m, const uint8_t in[MS800_RAM_SIZE]);

/* 1 = log every M114S channel programme to stderr. */
void     ms800_set_trace(ms800_t *m, int on);

#ifdef __cplusplus
}
#endif
