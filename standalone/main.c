/*
 * MS800 Synthesiser Emulator
 *
 * Usage: ./ms800-emu [--trace]
 *
 * Standalone host for the ms800 core library (core/include/ms800.h):
 * SDL2 audio and front panel, CoreMIDI virtual ports "MS800" (in) and
 * "MS800 Out" on macOS.
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include <SDL2/SDL.h>

#ifdef __APPLE__
#  include <CoreMIDI/CoreMIDI.h>
#  include <CoreFoundation/CoreFoundation.h>
#endif

#include "ms800.h"
#include "ui_panel.h"

#define SAMPLE_RATE    48000

#define PROG_ROM_PATH "roms/ms800-10-prog.bin"
#define WAVE_ROM_PATH "roms/ms800-waveform-rom.bin"

static int trace_m114s = 0;
static volatile int powered_on = 1;   /* the panel's power switch */
static ms800_t *machine;

/* ── Audio callback ─────────────────────────────────────────────────────── */
static void audio_callback(void *userdata, Uint8 *stream, int len) {
    (void)userdata;
    int16_t *out = (int16_t *)stream;
    int frames = len / 4;   /* stereo S16 */
    static int was_on = 1;
    if (!powered_on) { memset(out, 0, (size_t)len); was_on = 0; return; }
    if (!was_on) {
        /* Switched back on: everything but the battery-backed RAM starts
           afresh, which gets the firmware out of a lockup as on a real unit. */
        ms800_power_cycle(machine);
        was_on = 1;
    }
    ms800_render_s16(machine, out, frames);
}

/* ── MIDI TX (emulator -> real world) ───────────────────────────────────── */
#define TXQ_SIZE 256
static uint8_t txq[TXQ_SIZE];
static volatile int txq_head = 0, txq_tail = 0;
static void on_midi_out(void *user, uint8_t b) {
    (void)user;
    int next = (txq_tail + 1) % TXQ_SIZE;
    if (next != txq_head) { txq[txq_tail] = b; txq_tail = next; }
}

/* ── CoreMIDI (macOS) ────────────────────────────────────────────────────── */
#ifdef __APPLE__
static MIDIClientRef   midi_client   = 0;
static MIDIPortRef     midi_in_port  = 0;
static MIDIEndpointRef midi_out_src  = 0;
static MIDIEndpointRef midi_in_dest  = 0;

static void midi_read_proc(const MIDIPacketList *pktList, void *refcon, void *srcRefcon) {
    (void)refcon; (void)srcRefcon;
    static uint8_t last_status = 0;
    const MIDIPacket *pkt = &pktList->packet[0];
    for (UInt32 i = 0; i < pktList->numPackets; i++) {
        for (UInt16 j = 0; j < pkt->length; j++) {
            uint8_t b = pkt->data[j];
            if (b & 0x80) {
                last_status = b;
                ms800_midi_in(machine, b);
            } else if ((last_status & 0xF0) == 0xC0) {
                /* Program Change: firmware patch numbers are 1-50 (0 disables
                   the channel); standard MIDI sends 0-127. */
                uint8_t prog = (uint8_t)(b + 1);
                if (prog > 50) prog = 50;
                ms800_midi_in(machine, prog);
            } else {
                ms800_midi_in(machine, b);
            }
        }
        pkt = MIDIPacketNext(pkt);
    }
}

static int midi_init(void) {
    OSStatus err = MIDIClientCreate(CFSTR("MS800"), NULL, NULL, &midi_client);
    if (err) { fprintf(stderr, "MIDIClientCreate failed: %d\n", (int)err); return 0; }

    err = MIDIInputPortCreate(midi_client, CFSTR("MS800 In"), midi_read_proc, NULL, &midi_in_port);
    if (err) { fprintf(stderr, "MIDIInputPortCreate failed: %d\n", (int)err); return 0; }

    err = MIDISourceCreate(midi_client, CFSTR("MS800 Out"), &midi_out_src);
    if (err) fprintf(stderr, "MIDISourceCreate failed: %d (MIDI OUT disabled)\n", (int)err);

    /* Virtual destination: lets other apps/tools (DAWs, sendmidi, etc.)
       address "MS800" directly by name, in addition to the auto-connected
       physical/virtual sources below. */
    err = MIDIDestinationCreate(midi_client, CFSTR("MS800"), midi_read_proc, NULL, &midi_in_dest);
    if (err) fprintf(stderr, "MIDIDestinationCreate failed: %d (no addressable virtual port)\n", (int)err);

    ItemCount n = MIDIGetNumberOfSources();
    printf("MIDI sources available: %lu\n", (unsigned long)n);
    for (ItemCount s = 0; s < n; s++) {
        MIDIEndpointRef src = MIDIGetSource(s);
        CFStringRef name = NULL;
        MIDIObjectGetStringProperty(src, kMIDIPropertyName, &name);
        char buf[128] = {0};
        if (name) { CFStringGetCString(name, buf, sizeof(buf), kCFStringEncodingUTF8); CFRelease(name); }
        printf("  [%lu] %s\n", (unsigned long)s, buf);
        MIDIPortConnectSource(midi_in_port, src, NULL);
    }
    if (n == 0) printf("  (no MIDI sources found - connect a device and restart)\n");
    return 1;
}

static void midi_flush_tx(void) {
    if (!midi_out_src) { txq_head = txq_tail; return; }
    uint8_t buf[64];
    int n = 0;
    while (txq_head != txq_tail && n < (int)sizeof(buf)) {
        buf[n++] = txq[txq_head];
        txq_head = (txq_head + 1) % TXQ_SIZE;
    }
    if (n == 0) return;
    Byte packetBuf[128];
    MIDIPacketList *pl = (MIDIPacketList *)packetBuf;
    MIDIPacket *cur = MIDIPacketListInit(pl);
    cur = MIDIPacketListAdd(pl, sizeof(packetBuf), cur, 0, (ByteCount)n, buf);
    if (cur) MIDIReceived(midi_out_src, pl);
}
#else
static int  midi_init(void) { printf("MIDI: no platform backend compiled in.\n"); return 0; }
static void midi_flush_tx(void) { txq_head = txq_tail; }
#endif

/* ── ROM loading ─────────────────────────────────────────────────────────── */
/* ROMs are looked up in roms/ under the current directory first, then in
   roms/ next to the executable. */
static FILE *open_rom(const char *path, char *found, size_t found_sz) {
    FILE *f = fopen(path, "rb");
    if (f) { snprintf(found, found_sz, "%s", path); return f; }
    char *base = SDL_GetBasePath();
    if (base) {
        snprintf(found, found_sz, "%s%s", base, path);
        SDL_free(base);
        f = fopen(found, "rb");
    }
    return f;
}

static uint8_t *load_rom(const char *name, int expected_size) {
    char path[1024];
    FILE *f = open_rom(name, path, sizeof path);
    if (!f) { fprintf(stderr, "Cannot open ROM: %s (looked in ./ and next to the executable)\n", name); return NULL; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    if (expected_size > 0 && sz != expected_size) {
        fprintf(stderr, "ROM %s: expected %d bytes, got %ld\n", path, expected_size, sz);
        fclose(f); return NULL;
    }
    uint8_t *buf = malloc((size_t)sz);
    if (!buf || (long)fread(buf, 1, (size_t)sz, f) != sz) {
        fprintf(stderr, "ROM read error: %s\n", path);
        free(buf); fclose(f); return NULL;
    }
    fclose(f);
    printf("Loaded ROM: %-40s %ld bytes\n", path, sz);
    return buf;
}

/* ── Main ────────────────────────────────────────────────────────────────── */
int main(int argc, char **argv) {
    for (int i = 1; i < argc; i++)
        if (strcmp(argv[i], "--trace") == 0) trace_m114s = 1;

    setvbuf(stdout, NULL, _IOLBF, 0);   /* show messages promptly when not on a terminal */
    printf("MS800 Emulator\n");

    uint8_t *prog_rom = load_rom(PROG_ROM_PATH, MS800_PROG_ROM_SIZE);
    uint8_t *wave_rom = load_rom(WAVE_ROM_PATH, MS800_WAVE_ROM_SIZE);
    if (!prog_rom || !wave_rom) return 1;

    machine = ms800_create(prog_rom, MS800_PROG_ROM_SIZE, wave_rom, MS800_WAVE_ROM_SIZE, SAMPLE_RATE);
    free(prog_rom);
    free(wave_rom);
    if (!machine) { fprintf(stderr, "Cannot create the MS800 machine\n"); return 1; }
    ms800_set_trace(machine, trace_m114s);
    ms800_set_midi_out(machine, on_midi_out, NULL);

    midi_init();

    if (SDL_Init(SDL_INIT_AUDIO | SDL_INIT_VIDEO) < 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError()); return 1;
    }
    SDL_AudioSpec want = {0};
    want.freq = SAMPLE_RATE; want.format = AUDIO_S16SYS; want.channels = 2;
    want.samples = 512; want.callback = audio_callback;
    SDL_AudioSpec got;
    SDL_AudioDeviceID dev = SDL_OpenAudioDevice(NULL, 0, &want, &got, 0);
    if (!dev) { fprintf(stderr, "SDL_OpenAudioDevice: %s\n", SDL_GetError()); return 1; }
    printf("Audio: %d Hz, %d-sample buffer\n", got.freq, got.samples);

    SDL_Window *win = SDL_CreateWindow("MS800",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, UI_WIN_W, UI_WIN_H, SDL_WINDOW_ALLOW_HIGHDPI);
    SDL_Renderer *ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED);
    ui_panel_init(ren);

    SDL_PauseAudioDevice(dev, 0);
    printf("Running. Buttons: mouse-click the panel, or keys 1-9/0 for\n"
           "UP/DOWN/PARAM/CHANNEL/FUNCTION/UP UP/DOWN DOWN/NEXT PARAM/PATCH/ENTER\n"
           "(scan-matrix order). Space toggles power.\n");

    int running = 1;
    while (running) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) running = 0;
            if (ev.type == SDL_KEYDOWN || ev.type == SDL_KEYUP) {
                int down = (ev.type == SDL_KEYDOWN);
                SDL_Keycode k = ev.key.keysym.sym;
                if (down && k == SDLK_ESCAPE) running = 0;
                if (down && k == SDLK_SPACE) powered_on = !powered_on;
                int btn = 0;
                if (k >= SDLK_1 && k <= SDLK_9) btn = (int)(k - SDLK_1) + 1;
                else if (k == SDLK_0)           btn = 10;
                if (btn) ms800_set_button(machine, btn, down);
            }
            if (ev.type == SDL_MOUSEBUTTONDOWN || ev.type == SDL_MOUSEBUTTONUP) {
                int down = (ev.type == SDL_MOUSEBUTTONDOWN);
                int mx = ev.button.x, my = ev.button.y;
                if (down && ui_panel_hit_test_power(mx, my)) powered_on = !powered_on;
                int btn = ui_panel_hit_test_button(mx, my);
                if (btn) ms800_set_button(machine, btn, down);
            }
        }

        midi_flush_tx();

        ui_panel_draw(ren, machine, powered_on ? ms800_display(machine) : 0, powered_on);
        SDL_RenderPresent(ren);
        SDL_Delay(16);
    }

    SDL_PauseAudioDevice(dev, 1);
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_CloseAudioDevice(dev);
    SDL_Quit();

    ms800_destroy(machine);
    return 0;
}
