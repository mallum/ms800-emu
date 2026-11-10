/*
 * MS800 plugin editor: the front panel (same artwork as the standalone app)
 * in a native view. Clicks press the panel buttons; the LED display follows
 * the machine. Main thread only; the machine is touched only through the
 * thread-safe panel calls in ms800.h.
 */
#pragma once
#include <stdatomic.h>
#include <stdbool.h>

#include "ms800.h"

typedef struct ms800_gui ms800_gui;

/* Logical size of the panel in points. */
void        ms800_gui_size(unsigned *width, unsigned *height);

/* power: shared with the audio thread; 0 silences the plugin like the
   real power switch. */
ms800_gui  *ms800_gui_create(ms800_t *m, atomic_int *power);
void        ms800_gui_destroy(ms800_gui *gui);
bool        ms800_gui_set_parent(ms800_gui *gui, void *native_parent);   /* NSView * on macOS */
void        ms800_gui_show(ms800_gui *gui, bool visible);
