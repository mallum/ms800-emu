/*
 * Exported CLAP entry for the MS800 plugin. clap-wrapper compiles this file
 * into each format; the implementation lives in the static library.
 */
#include <clap/clap.h>

#include "ms800_clap.h"

extern "C" {
#ifdef __GNUC__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wattributes"
#endif

CLAP_EXPORT const clap_plugin_entry_t clap_entry = {
    CLAP_VERSION,
    ms800_clap_entry_init,
    ms800_clap_entry_deinit,
    ms800_clap_entry_get_factory,
};

#ifdef __GNUC__
#pragma GCC diagnostic pop
#endif
}
