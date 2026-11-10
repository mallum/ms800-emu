/*
 * MS800 as a CLAP instrument (wrapped to VST3 and AUv2 by clap-wrapper).
 *
 * One stereo output, a note input (MIDI preferred, CLAP notes accepted), a
 * MIDI output carrying whatever the firmware transmits (e.g. SysEx dumps),
 * and on macOS an editor showing the front panel. Each instance owns an ms800_t machine; MIDI is
 * fed in sample-accurately by splitting the render at event times. The
 * plugin state is the machine's battery-backed RAM, so user patches and
 * setup are saved with the host project.
 *
 * ROMs are not built in. They are looked up, first match wins, in
 *   $MS800_ROM_DIR/
 *   ~/Library/Application Support/MS800/roms/   (macOS)
 *   ~/.local/share/ms800/roms/                  (elsewhere)
 *   <plugin bundle>/Contents/Resources/roms/
 * as ms800-10-prog.bin and ms800-waveform-rom.bin. Without them the
 * plugin loads but stays silent.
 */
#include "ms800_clap.h"
#include "ms800.h"
#ifdef __APPLE__
#include "ms800_gui.h"
#endif

#include <clap/clap.h>

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PROG_ROM_NAME "ms800-10-prog.bin"
#define WAVE_ROM_NAME "ms800-waveform-rom.bin"

/* ── ROMs (loaded once, shared read-only by all instances) ───────────────── */

static char    plugin_path[1024];
static uint8_t prog_rom[MS800_PROG_ROM_SIZE];
static uint8_t wave_rom[MS800_WAVE_ROM_SIZE];
static int     roms_tried, roms_ok;

static int read_exact(const char *path, uint8_t *dst, size_t size) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    size_t got = fread(dst, 1, size, f);
    int extra = fgetc(f) != EOF;
    fclose(f);
    return got == size && !extra;
}

static int load_roms_from(const char *dir) {
    char p[1200];
    snprintf(p, sizeof p, "%s/%s", dir, PROG_ROM_NAME);
    if (!read_exact(p, prog_rom, sizeof prog_rom)) return 0;
    snprintf(p, sizeof p, "%s/%s", dir, WAVE_ROM_NAME);
    return read_exact(p, wave_rom, sizeof wave_rom);
}

/* <bundle>/Contents/Resources/roms, from either the bundle path or a path
   to the binary inside it. */
static int bundle_rom_dir(char *out, size_t size) {
    if (!plugin_path[0]) return 0;
    const char *contents = strstr(plugin_path, "/Contents/");
    if (contents)
        snprintf(out, size, "%.*s/Contents/Resources/roms", (int)(contents - plugin_path), plugin_path);
    else
        snprintf(out, size, "%s/Contents/Resources/roms", plugin_path);
    return 1;
}

static void load_roms(void) {
    if (roms_tried) return;
    roms_tried = 1;
    char dir[1200];
    const char *env = getenv("MS800_ROM_DIR");
    const char *home = getenv("HOME");
    if (env && load_roms_from(env)) { roms_ok = 1; return; }
#ifdef __APPLE__
    if (home) {
        snprintf(dir, sizeof dir, "%s/Library/Application Support/MS800/roms", home);
        if (load_roms_from(dir)) { roms_ok = 1; return; }
    }
#else
    if (home) {
        snprintf(dir, sizeof dir, "%s/.local/share/ms800/roms", home);
        if (load_roms_from(dir)) { roms_ok = 1; return; }
    }
#endif
    if (bundle_rom_dir(dir, sizeof dir) && load_roms_from(dir)) { roms_ok = 1; return; }
    fprintf(stderr, "MS800: ROMs not found (%s, %s); the plugin will be silent\n",
            PROG_ROM_NAME, WAVE_ROM_NAME);
}

/* ── Instance ────────────────────────────────────────────────────────────── */

typedef struct {
    clap_plugin_t     plugin;
    const clap_host_t *host;
    ms800_t           *m;          /* NULL if the ROMs are missing */
    double             sample_rate;
    atomic_flag        lock;       /* guards m between process and state calls */
    atomic_int         power;      /* the panel's power switch: 0 = off, silent */
#ifdef __APPLE__
    ms800_gui         *gui;
#endif

    /* MIDI OUT: bytes the firmware transmits, reassembled into events. Only
       valid inside process(); bytes sent at other times are dropped. */
    const clap_output_events_t *out_events;
    uint32_t           out_time;
    uint8_t            out_status, out_data[2];
    int                out_need, out_have;
    int                out_sysex_start;   /* -1 when not inside a SysEx */
    uint8_t            sysex_arena[8192]; /* this block's outgoing SysEx bytes */
    int                sysex_len;
} ms800_plugin;

static void lock(ms800_plugin *p)   { while (atomic_flag_test_and_set_explicit(&p->lock, memory_order_acquire)) {} }
static void unlock(ms800_plugin *p) { atomic_flag_clear_explicit(&p->lock, memory_order_release); }

static const char *const features[] = {
    CLAP_PLUGIN_FEATURE_INSTRUMENT,
    CLAP_PLUGIN_FEATURE_SYNTHESIZER,
    CLAP_PLUGIN_FEATURE_STEREO,
    NULL,
};

static const clap_plugin_descriptor_t descriptor = {
    .clap_version = CLAP_VERSION_INIT,
    .id           = "com.mallum.ms800",
    .name         = "MS800",
    .vendor       = "mallum",
    .url          = "https://github.com/mallum/ms800-emu",
    .manual_url   = "",
    .support_url  = "",
    .version      = "0.1.0",
    .description  = "Cheetah MS800 digital wave synthesizer, running the original firmware",
    .features     = features,
};

static void on_midi_out(void *user, uint8_t byte);

/* The firmware ignores MIDI for ~0.2 s while it boots; run that silently up
   front (a few ms of CPU) so a new or restored instance plays immediately. */
#define BOOT_SECONDS 0.25
static void fast_boot(ms800_plugin *p) {
    ms800_render_f32(p->m, NULL, NULL, (int)(BOOT_SECONDS * p->sample_rate));
}

static bool plugin_init(const clap_plugin_t *plugin) {
    ms800_plugin *p = plugin->plugin_data;
    load_roms();
    p->sample_rate = 48000.0;
    if (roms_ok) {
        p->m = ms800_create(prog_rom, sizeof prog_rom, wave_rom, sizeof wave_rom, p->sample_rate);
        if (p->m) {
            ms800_set_midi_out(p->m, on_midi_out, p);
            fast_boot(p);
        }
    }
    return true;   /* load even without ROMs, so the host doesn't blacklist us */
}

static void plugin_destroy(const clap_plugin_t *plugin) {
    ms800_plugin *p = plugin->plugin_data;
#ifdef __APPLE__
    if (p->gui) ms800_gui_destroy(p->gui);
#endif
    if (p->m) ms800_destroy(p->m);
    free(p);
}

static bool plugin_activate(const clap_plugin_t *plugin, double sample_rate,
                            uint32_t min_frames, uint32_t max_frames) {
    (void)min_frames; (void)max_frames;
    ms800_plugin *p = plugin->plugin_data;
    p->sample_rate = sample_rate;
    if (p->m) {
        lock(p);
        ms800_set_sample_rate(p->m, sample_rate);
        unlock(p);
    }
    return true;
}

static void plugin_deactivate(const clap_plugin_t *plugin) { (void)plugin; }
static bool plugin_start_processing(const clap_plugin_t *plugin) { (void)plugin; return true; }
static void plugin_stop_processing(const clap_plugin_t *plugin) { (void)plugin; }
static void plugin_reset(const clap_plugin_t *plugin) { (void)plugin; }

/* Bytes in a MIDI message with this status byte (no running status here). */
static int midi_length(uint8_t status) {
    switch (status & 0xF0) {
    case 0x80: case 0x90: case 0xA0: case 0xB0: case 0xE0: return 3;
    case 0xC0: case 0xD0: return 2;
    }
    switch (status) {
    case 0xF1: case 0xF3: return 2;
    case 0xF2: return 3;
    default:   return 1;
    }
}

static void send_midi(ms800_t *m, const uint8_t *msg) {
    int n = midi_length(msg[0]);
    ms800_midi_in(m, msg[0]);
    if (n > 1) {
        uint8_t d = msg[1];
        /* Program change: the firmware selects patch n-1 for program n and
           disables the channel for 0, but hosts send 0 for "program 1".
           Shift by one; the firmware ignores 100+, so cap at 99. */
        if ((msg[0] & 0xF0) == 0xC0) d = (uint8_t)(d >= 98 ? 99 : d + 1);
        ms800_midi_in(m, d);
    }
    if (n > 2) ms800_midi_in(m, msg[2]);
}

static void handle_event(ms800_t *m, const clap_event_header_t *hdr) {
    if (hdr->space_id != CLAP_CORE_EVENT_SPACE_ID) return;
    switch (hdr->type) {
    case CLAP_EVENT_MIDI: {
        const clap_event_midi_t *ev = (const clap_event_midi_t *)hdr;
        send_midi(m, ev->data);
        break;
    }
    case CLAP_EVENT_MIDI_SYSEX: {
        const clap_event_midi_sysex_t *ev = (const clap_event_midi_sysex_t *)hdr;
        for (uint32_t i = 0; i < ev->size; i++) ms800_midi_in(m, ev->buffer[i]);
        break;
    }
    case CLAP_EVENT_NOTE_ON:
    case CLAP_EVENT_NOTE_OFF: {
        const clap_event_note_t *ev = (const clap_event_note_t *)hdr;
        if (ev->key < 0 || ev->key > 127) break;
        int ch = ev->channel >= 0 && ev->channel < 16 ? ev->channel : 0;
        int vel = (int)(ev->velocity * 127.0 + 0.5);
        if (vel > 127) vel = 127;
        uint8_t msg[3];
        if (hdr->type == CLAP_EVENT_NOTE_ON) {
            msg[0] = (uint8_t)(0x90 | ch);
            msg[2] = (uint8_t)(vel < 1 ? 1 : vel);
        } else {
            msg[0] = (uint8_t)(0x80 | ch);
            msg[2] = (uint8_t)vel;
        }
        msg[1] = (uint8_t)ev->key;
        send_midi(m, msg);
        break;
    }
    }
}

/* ── MIDI OUT ────────────────────────────────────────────────────────────── */

static void push_midi_out(ms800_plugin *p, const uint8_t *msg) {
    clap_event_midi_t ev = {
        .header = { sizeof ev, p->out_time, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_MIDI, 0 },
        .port_index = 0,
        .data = { msg[0], msg[1], msg[2] },
    };
    /* Undo the input offset so program numbers round-trip (see send_midi). */
    if ((ev.data[0] & 0xF0) == 0xC0 && ev.data[1] > 0) ev.data[1]--;
    p->out_events->try_push(p->out_events, &ev.header);
}

static void on_midi_out(void *user, uint8_t b) {
    ms800_plugin *p = user;
    if (!p->out_events) return;
    if (b >= 0xF8) {                                   /* real-time: pass straight through */
        const uint8_t msg[3] = { b, 0, 0 };
        push_midi_out(p, msg);
        return;
    }
    if (p->out_sysex_start >= 0) {
        if (b < 0x80 || b == 0xF7) {
            if (p->sysex_len < (int)sizeof p->sysex_arena) p->sysex_arena[p->sysex_len++] = b;
            if (b == 0xF7) {
                clap_event_midi_sysex_t ev = {
                    .header = { sizeof ev, p->out_time, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_MIDI_SYSEX, 0 },
                    .port_index = 0,
                    .buffer = p->sysex_arena + p->out_sysex_start,
                    .size = (uint32_t)(p->sysex_len - p->out_sysex_start),
                };
                p->out_events->try_push(p->out_events, &ev.header);
                p->out_sysex_start = -1;
            }
            return;
        }
        p->sysex_len = p->out_sysex_start;             /* unterminated: drop it */
        p->out_sysex_start = -1;
    }
    if (b == 0xF0) {
        p->out_sysex_start = p->sysex_len;
        if (p->sysex_len < (int)sizeof p->sysex_arena) p->sysex_arena[p->sysex_len++] = b;
        p->out_status = 0;
        return;
    }
    if (b & 0x80) {
        p->out_status = b;
        p->out_need = midi_length(b) - 1;
        p->out_have = 0;
        if (p->out_need == 0) {
            const uint8_t msg[3] = { b, 0, 0 };
            push_midi_out(p, msg);
            p->out_status = 0;
        }
        return;
    }
    if (!p->out_status) return;                        /* data with no status */
    p->out_data[p->out_have++] = b;
    if (p->out_have == p->out_need) {
        const uint8_t msg[3] = { p->out_status, p->out_data[0], p->out_need > 1 ? p->out_data[1] : 0 };
        push_midi_out(p, msg);
        p->out_have = 0;                               /* running status */
    }
}

static clap_process_status plugin_process(const clap_plugin_t *plugin, const clap_process_t *process) {
    ms800_plugin *p = plugin->plugin_data;
    const uint32_t frames = process->frames_count;
    float *left = NULL, *right = NULL;
    if (process->audio_outputs_count > 0 && process->audio_outputs[0].data32) {
        const clap_audio_buffer_t *out = &process->audio_outputs[0];
        left = out->data32[0];
        right = out->channel_count > 1 ? out->data32[1] : NULL;
    }

    if (!p->m || !atomic_load_explicit(&p->power, memory_order_relaxed)) {
        /* No ROMs, or switched off: silent, and the machine doesn't run. */
        if (left) memset(left, 0, frames * sizeof(float));
        if (right) memset(right, 0, frames * sizeof(float));
        return p->m ? CLAP_PROCESS_CONTINUE : CLAP_PROCESS_SLEEP;
    }

    lock(p);
    p->out_events = process->out_events;
    p->sysex_len = 0;
    if (p->out_sysex_start >= 0) p->out_sysex_start = -1;   /* SysEx can't span blocks */
    const clap_input_events_t *in = process->in_events;
    const uint32_t nev = in ? in->size(in) : 0;
    uint32_t pos = 0, ei = 0;
    while (pos < frames) {
        /* Apply every event due at this frame, then render up to the next. */
        uint32_t next = frames;
        while (ei < nev) {
            const clap_event_header_t *hdr = in->get(in, ei);
            if (hdr->time > pos) { next = hdr->time < frames ? hdr->time : frames; break; }
            handle_event(p->m, hdr);
            ei++;
        }
        uint32_t n = next - pos;
        p->out_time = pos;
        ms800_render_f32(p->m, left ? left + pos : NULL, right ? right + pos : NULL, (int)n);
        pos = next;
    }
    while (ei < nev) handle_event(p->m, in->get(in, ei++));   /* events past the block end */
    p->out_events = NULL;
    unlock(p);
    return CLAP_PROCESS_CONTINUE;
}

/* ── Extensions ──────────────────────────────────────────────────────────── */

static uint32_t audio_ports_count(const clap_plugin_t *plugin, bool is_input) {
    (void)plugin;
    return is_input ? 0 : 1;
}

static bool audio_ports_get(const clap_plugin_t *plugin, uint32_t index, bool is_input,
                            clap_audio_port_info_t *info) {
    (void)plugin;
    if (is_input || index != 0) return false;
    info->id = 0;
    snprintf(info->name, sizeof info->name, "%s", "Output");
    info->flags = CLAP_AUDIO_PORT_IS_MAIN;
    info->channel_count = 2;
    info->port_type = CLAP_PORT_STEREO;
    info->in_place_pair = CLAP_INVALID_ID;
    return true;
}

static const clap_plugin_audio_ports_t audio_ports = { audio_ports_count, audio_ports_get };

static uint32_t note_ports_count(const clap_plugin_t *plugin, bool is_input) {
    (void)plugin;
    (void)is_input;
    return 1;   /* MIDI in, and MIDI out (what the firmware transmits) */
}

static bool note_ports_get(const clap_plugin_t *plugin, uint32_t index, bool is_input,
                           clap_note_port_info_t *info) {
    (void)plugin;
    if (index != 0) return false;
    if (is_input) {
        info->id = 0;
        info->supported_dialects = CLAP_NOTE_DIALECT_MIDI | CLAP_NOTE_DIALECT_CLAP;
        info->preferred_dialect = CLAP_NOTE_DIALECT_MIDI;
        snprintf(info->name, sizeof info->name, "%s", "MIDI In");
    } else {
        info->id = 1;
        info->supported_dialects = CLAP_NOTE_DIALECT_MIDI;
        info->preferred_dialect = CLAP_NOTE_DIALECT_MIDI;
        snprintf(info->name, sizeof info->name, "%s", "MIDI Out");
    }
    return true;
}

static const clap_plugin_note_ports_t note_ports = { note_ports_count, note_ports_get };

/* State: "MS8S" magic, a version byte, then the 8 KB battery-backed RAM. */
static const uint8_t state_magic[4] = { 'M', 'S', '8', 'S' };
#define STATE_VERSION 1

static bool write_all(const clap_ostream_t *s, const void *buf, uint64_t size) {
    const uint8_t *b = buf;
    while (size) {
        int64_t n = s->write(s, b, size);
        if (n <= 0) return false;
        b += n;
        size -= (uint64_t)n;
    }
    return true;
}

static bool read_all(const clap_istream_t *s, void *buf, uint64_t size) {
    uint8_t *b = buf;
    while (size) {
        int64_t n = s->read(s, b, size);
        if (n <= 0) return false;
        b += n;
        size -= (uint64_t)n;
    }
    return true;
}

static bool state_save(const clap_plugin_t *plugin, const clap_ostream_t *stream) {
    ms800_plugin *p = plugin->plugin_data;
    if (!p->m) return false;
    static _Thread_local uint8_t ram[MS800_RAM_SIZE];
    lock(p);
    ms800_get_ram(p->m, ram);
    unlock(p);
    const uint8_t version = STATE_VERSION;
    return write_all(stream, state_magic, sizeof state_magic) &&
           write_all(stream, &version, 1) &&
           write_all(stream, ram, sizeof ram);
}

static bool state_load(const clap_plugin_t *plugin, const clap_istream_t *stream) {
    ms800_plugin *p = plugin->plugin_data;
    if (!p->m) return false;
    uint8_t magic[4], version;
    static _Thread_local uint8_t ram[MS800_RAM_SIZE];
    if (!read_all(stream, magic, sizeof magic) || memcmp(magic, state_magic, sizeof magic) != 0) return false;
    if (!read_all(stream, &version, 1) || version != STATE_VERSION) return false;
    if (!read_all(stream, ram, sizeof ram)) return false;
    lock(p);
    ms800_set_ram(p->m, ram);
    ms800_reset(p->m);   /* warm boot from the restored RAM */
    fast_boot(p);
    unlock(p);
    return true;
}

static const clap_plugin_state_t state_ext = { state_save, state_load };

/* No parameters of our own. The extension still has to exist: clap-wrapper
   only creates the VST3 MIDI-mapping parameters (how VST3 hosts deliver
   CCs, pitch bend and program changes as MIDI) when a plugin has it. */
static uint32_t params_count(const clap_plugin_t *plugin) { (void)plugin; return 0; }
static bool params_get_info(const clap_plugin_t *plugin, uint32_t index, clap_param_info_t *info) {
    (void)plugin; (void)index; (void)info; return false;
}
static bool params_get_value(const clap_plugin_t *plugin, clap_id id, double *value) {
    (void)plugin; (void)id; (void)value; return false;
}
static bool params_value_to_text(const clap_plugin_t *plugin, clap_id id, double value, char *out, uint32_t size) {
    (void)plugin; (void)id; (void)value; (void)out; (void)size; return false;
}
static bool params_text_to_value(const clap_plugin_t *plugin, clap_id id, const char *text, double *value) {
    (void)plugin; (void)id; (void)text; (void)value; return false;
}
/* Events delivered while not processing: feed them to the machine anyway. */
static void params_flush(const clap_plugin_t *plugin, const clap_input_events_t *in, const clap_output_events_t *out) {
    (void)out;
    ms800_plugin *p = plugin->plugin_data;
    if (!p->m || !in) return;
    lock(p);
    for (uint32_t i = 0, n = in->size(in); i < n; i++) handle_event(p->m, in->get(in, i));
    unlock(p);
}

static const clap_plugin_params_t params_ext = {
    params_count, params_get_info, params_get_value, params_value_to_text, params_text_to_value, params_flush,
};

#ifdef __APPLE__
/* Editor: the front panel as a fixed-size Cocoa view (see ms800_gui_mac.m). */
static bool gui_is_api_supported(const clap_plugin_t *plugin, const char *api, bool is_floating) {
    (void)plugin;
    return !is_floating && strcmp(api, CLAP_WINDOW_API_COCOA) == 0;
}
static bool gui_get_preferred_api(const clap_plugin_t *plugin, const char **api, bool *is_floating) {
    (void)plugin;
    *api = CLAP_WINDOW_API_COCOA;
    *is_floating = false;
    return true;
}
static bool gui_create(const clap_plugin_t *plugin, const char *api, bool is_floating) {
    ms800_plugin *p = plugin->plugin_data;
    if (!gui_is_api_supported(plugin, api, is_floating) || !p->m || p->gui) return false;
    p->gui = ms800_gui_create(p->m, &p->power);
    return p->gui != NULL;
}
static void gui_destroy(const clap_plugin_t *plugin) {
    ms800_plugin *p = plugin->plugin_data;
    if (p->gui) ms800_gui_destroy(p->gui);
    p->gui = NULL;
}
static bool gui_set_scale(const clap_plugin_t *plugin, double scale) { (void)plugin; (void)scale; return false; }
static bool gui_get_size(const clap_plugin_t *plugin, uint32_t *width, uint32_t *height) {
    (void)plugin;
    unsigned w, h;
    ms800_gui_size(&w, &h);
    *width = w;
    *height = h;
    return true;
}
static bool gui_can_resize(const clap_plugin_t *plugin) { (void)plugin; return false; }
static bool gui_get_resize_hints(const clap_plugin_t *plugin, clap_gui_resize_hints_t *hints) {
    (void)plugin; (void)hints; return false;
}
static bool gui_adjust_size(const clap_plugin_t *plugin, uint32_t *width, uint32_t *height) {
    return gui_get_size(plugin, width, height);
}
static bool gui_set_size(const clap_plugin_t *plugin, uint32_t width, uint32_t height) {
    uint32_t w, h;
    gui_get_size(plugin, &w, &h);
    return width == w && height == h;
}
static bool gui_set_parent(const clap_plugin_t *plugin, const clap_window_t *window) {
    ms800_plugin *p = plugin->plugin_data;
    return p->gui && strcmp(window->api, CLAP_WINDOW_API_COCOA) == 0 && ms800_gui_set_parent(p->gui, window->cocoa);
}
static bool gui_set_transient(const clap_plugin_t *plugin, const clap_window_t *window) {
    (void)plugin; (void)window; return false;
}
static void gui_suggest_title(const clap_plugin_t *plugin, const char *title) { (void)plugin; (void)title; }
static bool gui_show(const clap_plugin_t *plugin) {
    ms800_plugin *p = plugin->plugin_data;
    if (!p->gui) return false;
    ms800_gui_show(p->gui, true);
    return true;
}
static bool gui_hide(const clap_plugin_t *plugin) {
    ms800_plugin *p = plugin->plugin_data;
    if (!p->gui) return false;
    ms800_gui_show(p->gui, false);
    return true;
}

static const clap_plugin_gui_t gui_ext = {
    gui_is_api_supported, gui_get_preferred_api, gui_create, gui_destroy, gui_set_scale,
    gui_get_size, gui_can_resize, gui_get_resize_hints, gui_adjust_size, gui_set_size,
    gui_set_parent, gui_set_transient, gui_suggest_title, gui_show, gui_hide,
};
#endif

static const void *plugin_get_extension(const clap_plugin_t *plugin, const char *id) {
    (void)plugin;
    if (!strcmp(id, CLAP_EXT_AUDIO_PORTS)) return &audio_ports;
    if (!strcmp(id, CLAP_EXT_NOTE_PORTS))  return &note_ports;
    if (!strcmp(id, CLAP_EXT_STATE))       return &state_ext;
    if (!strcmp(id, CLAP_EXT_PARAMS))      return &params_ext;
#ifdef __APPLE__
    if (!strcmp(id, CLAP_EXT_GUI))         return &gui_ext;
#endif
    return NULL;
}

static void plugin_on_main_thread(const clap_plugin_t *plugin) { (void)plugin; }

/* ── Factory and entry ───────────────────────────────────────────────────── */

static uint32_t factory_count(const clap_plugin_factory_t *f) { (void)f; return 1; }

static const clap_plugin_descriptor_t *factory_descriptor(const clap_plugin_factory_t *f, uint32_t index) {
    (void)f;
    return index == 0 ? &descriptor : NULL;
}

static const clap_plugin_t *factory_create(const clap_plugin_factory_t *f, const clap_host_t *host,
                                           const char *plugin_id) {
    (void)f;
    if (!clap_version_is_compatible(host->clap_version) || strcmp(plugin_id, descriptor.id) != 0)
        return NULL;
    ms800_plugin *p = calloc(1, sizeof(*p));
    if (!p) return NULL;
    p->host = host;
    p->out_sysex_start = -1;
    atomic_init(&p->power, 1);
    atomic_flag_clear(&p->lock);
    p->plugin = (clap_plugin_t){
        .desc = &descriptor,
        .plugin_data = p,
        .init = plugin_init,
        .destroy = plugin_destroy,
        .activate = plugin_activate,
        .deactivate = plugin_deactivate,
        .start_processing = plugin_start_processing,
        .stop_processing = plugin_stop_processing,
        .reset = plugin_reset,
        .process = plugin_process,
        .get_extension = plugin_get_extension,
        .on_main_thread = plugin_on_main_thread,
    };
    return &p->plugin;
}

static const clap_plugin_factory_t factory = { factory_count, factory_descriptor, factory_create };

bool ms800_clap_entry_init(const char *path) {
    snprintf(plugin_path, sizeof plugin_path, "%s", path ? path : "");
    return true;
}

void ms800_clap_entry_deinit(void) {}

const void *ms800_clap_entry_get_factory(const char *factory_id) {
    return strcmp(factory_id, CLAP_PLUGIN_FACTORY_ID) == 0 ? &factory : NULL;
}
