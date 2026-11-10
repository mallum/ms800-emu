/*
 * MS800 CLAP plugin: entry points implemented in ms800_clap.c (a static
 * library) and exported by ms800_clap_entry.cpp, which clap-wrapper compiles
 * once per format (CLAP, VST3, AUv2).
 */
#pragma once
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

bool        ms800_clap_entry_init(const char *plugin_path);
void        ms800_clap_entry_deinit(void);
const void *ms800_clap_entry_get_factory(const char *factory_id);

#ifdef __cplusplus
}
#endif
