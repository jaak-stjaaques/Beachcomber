/* SPDX-License-Identifier: GPL-3.0-only */
/* BEACHCOMBER: an FM-1 firmware on Dinghy's platform.
 *
 * How the notes flow:   ARP (a loop of 16 dice)  ->  SEQ (4 tracks)  ->  SCALE  ->  PLUCK / BLOW / HIT  ->  FX
 *
 * The ARP and the SEQ only deal in "steps" of a scale. The SCALE stage at the end turns steps into notes, so a
 * new scale or a transposition changes everything at once, the recorded tracks too.
 *
 * The files: engine.h (the three sound engines and their pings), scale.h (the scales), effects.h (delay, hall,
 * chorus in parallel), this file (the ARP, the SEQ, the pages and the panel). platform.c is Dinghy's plumbing. */
#define APP_NAME "BEACHCOMBER"
static void app_idle(void);
#define LCD_IDLE app_idle                       /* while the screen's DMA runs: the clock and MIDI keep going */
#define CV_MAX (240u * 150u)                    /* the pictures (224 x 88) and SAVE's note (224 x 160) */
#include "platform.c"
#if BOARD_SMK37
#include "smk_wheel.h"
#endif

#define TRACKS 4u
#include "engine.h"
#include "scale.h"
#include "effects.h"

/* ------------------------------------------------------------------------------------------------ the sound */

static int32_t output_level(int32_t sample)
{
    /* Raise quiet notes by about 9.5 dB and soften peaks before the master volume (as LOOPHEADS). */
    int32_t magnitude = (sample < 0 ? -sample : sample) * 3;
    if (magnitude > 20000) {
        int32_t excess = magnitude - 20000;
        magnitude = 20000 + 12000 * excess / (12000 + excess);
    }
    return sample < 0 ? -magnitude : magnitude;
}

static void app_audio(int32_t *out, uint32_t frames)
{
    pluck_mix(out, frames);
    effects_process(out, frames);
    for (uint32_t i = 0; i < 2u * frames; i++) out[i] = output_level(out[i]);
}

/* who started a note (so that the same one stops it) */
enum { OWNER_TRACK = 0, OWNER_ARP = TRACKS, OWNER_KEYS = 8, OWNER_MIDI = OWNER_KEYS + 40 };

/* ------------------------------------------------------------------------------------------------ settings */

#define LONG_MS 700u                            /* a press this long is a "hold" */
#define GATE_PERCENT 60u                        /* how long an ARP or SEQ note is held, of its step */
#define NOTE_VELOCITY 100u
#define KEY0 53u                                /* key 0 = F3 */

static const uint16_t div_ticks[] = {192, 96, 64, 48, 32, 24, 16, 12, 8};    /* 96 ticks a beat */
static const char *const div_names[] = {"1/2", "1/4", "1/4 T", "1/8", "1/8 T",
                                        "1/16", "1/16 T", "1/32", "1/32 T"};
#define DIVS (sizeof div_ticks / sizeof div_ticks[0])
#define DIV_SIXTEENTH 5u

static const char *const delay_division_names[] = {"1/16", "1/8", "1/8 D", "1/4", "1/4 D"};
static const uint8_t delay_division_clocks[] = {6, 12, 18, 24, 36};
#define DELAY_DIVS 5u
#define DELAY_FEEDBACK 40u
#define HALL_DECAY 70u
#define HALL_DAMPING 35u

static struct {
    uint8_t bpm, swing, ch, internal_clock;     /* swing 50 (straight) .. 75 */
    int8_t tuning;
} glo = {110, 50, 0, 0, 0};

static struct { uint8_t delay, time, hall, chorus; } fx = {25, 2, 30, 0};

/* ---------------------------------------------------------------------------------------- the ARP: 16 dice
 * A loop of LENGTH slots. Each slot holds two dice: which note, and whether it plays. Every step the loop moves
 * on by one slot; with the chance CHANGE the slot's dice are thrown again. CHANGE 0% = the loop repeats for ever
 * (Turing Machine "locked", Marbles "deja vu" full), 100% = always new. */
#define ARP_SLOTS 16u
#define REST 0xFFu                              /* a step with no note */
static struct {
    uint8_t on, change, length, range, density, speed;
    uint16_t pool;                              /* the white keys lit: bit i = scale step i */
    uint8_t note[ARP_SLOTS], gate[ARP_SLOTS];
    uint8_t pos, shown;                         /* the next slot; the slot that played last */
    uint32_t rnd;
} arp = {1, 20, 8, 1, 75, DIV_SIXTEENTH, 0};

static uint8_t arp_random(void)
{
    arp.rnd = arp.rnd * 1664525u + 1013904223u;
    return (uint8_t)(arp.rnd >> 24);
}

static void arp_throw(uint32_t slot)
{
    arp.note[slot] = arp_random();
    arp.gate[slot] = arp_random();
}

static void arp_reroll(void)
{
    for (uint32_t i = 0; i < ARP_SLOTS; i++) arp_throw(i);
}

/* the steps the ARP may use: the lit white keys, or the whole scale's first octave */
static uint32_t arp_pool(uint8_t list[16])
{
    uint32_t n = 0;
    for (uint32_t i = 0; i < 16; i++)
        if (arp.pool >> i & 1u) list[n++] = (uint8_t)i;
    if (!n)
        for (n = 0; n < scale_now.size && n < 16u; n++) list[n] = (uint8_t)n;
    return n;
}

/* A note in the ARP and the tracks is "packed": (octave + PACK_OCTAVE0) << 4 | step, the step a white key of the
 * ARP's pool (0..15), the octave counted in the scale's own size. PACK_OCTAVE0 lets a note sit below octave 0. */
#define PACK_OCTAVE0 2

static int32_t packed_step(uint8_t packed)       /* -> a step of the scale, 0 = the root */
{
    return (packed & 15) + ((int32_t)(packed >> 4) - PACK_OCTAVE0) * scale_now.size;
}

static uint8_t pack_step(int32_t step)
{
    int32_t n = scale_now.size, octave = step >= 0 ? step / n : -((-step + n - 1) / n);
    int32_t field = octave + PACK_OCTAVE0;
    if (field < 0) { field = 0; step = -PACK_OCTAVE0 * n; octave = -PACK_OCTAVE0; }
    if (field > 15) { field = 15; octave = 15 - PACK_OCTAVE0; step = octave * n + n - 1; }
    return (uint8_t)(field << 4 | (step - octave * n));
}

/* a slot's note die -> a packed note; and its height on screen, 0..255 */
static uint8_t arp_slot_note(uint32_t slot, uint8_t *height)
{
    uint8_t list[16];
    uint32_t n = arp_pool(list), total = n * arp.range;
    uint32_t i = arp.note[slot] * total >> 8;
    if (height) *height = (uint8_t)(total > 1u ? i * 255u / (total - 1u) : 128u);
    return (uint8_t)((i / n + PACK_OCTAVE0) << 4 | list[i % n]);
}

static uint8_t arp_slot_plays(uint32_t slot)
{
    return arp.gate[slot] * 100u / 256u < arp.density;
}

static uint8_t arp_step(uint8_t *velocity)
{
    uint32_t slot = arp.pos % arp.length;
    if (arp_random() * 100u / 256u < arp.change) arp_throw(slot);
    arp.shown = (uint8_t)slot;
    arp.pos = (uint8_t)((slot + 1u) % arp.length);
    *velocity = (uint8_t)(84u + (arp.gate[slot] & 31u));
    return arp_slot_plays(slot) ? arp_slot_note(slot, 0) : REST;
}

/* --------------------------------------------------------------------------------------- the SEQ: 4 tracks
 * A track is up to 16 steps. (A step has room for two notes; recording and PUNCH IN write one.) */
#define SEQ_STEPS 16u
#define LAYERS 2u
typedef struct {
    uint8_t steps[SEQ_STEPS][LAYERS];           /* packed notes (octave << 4 | step) or REST */
    uint8_t vel[SEQ_STEPS][LAYERS];
    uint8_t len, speed, muted;
    int8_t move;                                /* scale steps up or down */
} track_t;
static track_t tracks[TRACKS];
static uint8_t selected, rec_track = 0xFF, punch;
static uint32_t tracks_version;
static uint8_t chance = 100;                    /* ALGORITHM: the chance (%) that a recorded note plays */
static uint32_t chance_rnd = 0x6c8e9cf5u;                 /* + 1 on every change of a track's notes: the screen redraws */

static void track_clear(uint32_t t)
{
    memset(tracks[t].steps, REST, sizeof tracks[t].steps);
    tracks[t].len = 0;
    tracks_version++;
}

static uint8_t step_has_note(const track_t *k, uint32_t i)
{
    return k->steps[i][0] != REST || k->steps[i][1] != REST;
}

/* ------------------------------------------------------------------------------------------- the clock
 * 96 ticks a beat, from the tempo or from a MIDI clock (4 ticks a MIDI clock). Five players read it: the four
 * tracks and the ARP. Each steps on its own grid; SWING pushes every second step late. */
enum { PLAYER_ARP = TRACKS, PLAYERS };
static struct {
    uint32_t start, next, off;                  /* in ticks */
    uint32_t idx;
    uint8_t pitch[LAYERS];                      /* the notes sounding + 1, 0 = none */
} players[PLAYERS];
static struct {
    uint8_t running;
    uint32_t tick, acc, last_ms, ext_ticks, last_midi_clock;
} clk;

static uint32_t player_div(uint32_t p)
{
    return div_ticks[p == PLAYER_ARP ? arp.speed : tracks[p].speed];
}

static uint32_t player_time(uint32_t p, uint32_t idx)
{
    uint32_t div = player_div(p);
    uint32_t t = players[p].start + idx * div;
    if (idx & 1u) t += div * (glo.swing - 50u) / 50u;
    return t;
}

static void player_restart(uint32_t p, uint32_t at)
{
    players[p].start = at;
    players[p].idx = 0;
    players[p].next = at;
}

static uint8_t midi_out_channel(uint32_t owner)
{
    return (uint8_t)(owner < TRACKS ? owner : selected);   /* MIDI out: track 1..4 on channels 1..4 */
}

static void note_off(uint32_t p)
{
    for (uint32_t l = 0; l < LAYERS; l++) {
        if (!players[p].pitch[l]) continue;
        uint8_t pitch = players[p].pitch[l] - 1u;
        players[p].pitch[l] = 0;
        if (l == 1 && players[p].pitch[0] == pitch + 1u) continue;
        pluck_stop((uint8_t)p, pitch);
        midi_send(0x80u | midi_out_channel(p), pitch, 0);
    }
}

static void note_on(uint32_t p, uint32_t layer, uint8_t part, uint8_t packed, int32_t move, uint8_t velocity)
{
    int32_t step = packed_step(packed) + move;
    uint8_t pitch = scale_step_note(step);
    pluck_start((uint8_t)p, part, pitch, velocity, PLUCK_GAIN_ONE);
    midi_send(0x90u | midi_out_channel(p), pitch, velocity);
    players[p].pitch[layer] = pitch + 1u;
    players[p].off = clk.tick + player_div(p) * GATE_PERCENT / 100u;
}

/* OCT- / OCT+ move the ARP (and the keys), not the tracks: what the ARP plays now, in scale steps */
static int32_t arp_octave_steps(void)
{
    return scale_now.octave * (int32_t)scale_now.size;
}

/* a note as the ARP plays it -> as stored in a track SHIFTed by `move`: with the OCT the ARP had, so it plays
 * back exactly as it was heard, whatever OCT says later */
static uint8_t unmove(uint8_t packed, int32_t move)
{
    return pack_step(packed_step(packed) + arp_octave_steps() - move);
}

static void rec_stop(void)
{
    if (rec_track >= TRACKS) return;
    track_t *t = &tracks[rec_track];
    /* a new loop starts with the ARP's next step; a punched-in one keeps running (punch out) */
    if (!punch && t->len) player_restart(rec_track, player_time(PLAYER_ARP, players[PLAYER_ARP].idx));
    rec_track = 0xFF;
    punch = 0;
}

/* a recording is over (REC pressed again, 16 steps full, another track chosen): the ARP stops too, so what is
 * heard now is only the tracks. REC (or ARP, ARP) brings it back. */
static void rec_finish(void)
{
    if (rec_track >= TRACKS) return;
    rec_stop();
    arp.on = 0;
    note_off(PLAYER_ARP);
}

/* REC on an empty track: a new loop, as long as you record. REC on a track with notes: PUNCH IN, as on tape. The
 * track keeps its length and keeps running; every step it passes over is replaced by what the ARP plays there
 * (silences too), and only the new notes are heard. REC again: PUNCH OUT, the old notes play on from there. */
static void rec_start(uint8_t track)
{
    rec_stop();
    track_t *t = &tracks[track];
    punch = t->len != 0;
    if (!punch) {
        note_off(track);
        track_clear(track);
        t->speed = arp.speed;
        t->move = 0;
    }
    rec_track = track;
}

static void punch_note(track_t *t, uint32_t p, uint8_t n, uint8_t velocity)
{
    uint32_t s = players[p].idx ? (players[p].idx - 1u) % t->len : 0;   /* the step the track is on now */
    t->steps[s][0] = unmove(n, t->move);
    t->steps[s][1] = REST;
    t->vel[s][0] = velocity;
    tracks_version++;
}

#define PRESETS_SLOWEST 0u                      /* the PRESETS encoder: 1/2 .. 1/32 T */
static void arp_set_speed(uint8_t speed)
{
    if (speed == arp.speed) return;
    uint32_t at = players[PLAYER_ARP].next;     /* the step already due keeps its time; the new grid starts there */
    arp.speed = speed;
    player_restart(PLAYER_ARP, clk.running ? at : 0);
    /* a new loop being recorded follows the ARP's speed */
    if (rec_track < TRACKS && !punch && !tracks[rec_track].len) tracks[rec_track].speed = speed;
}

static void player_fire(uint32_t p)
{
    if (p == PLAYER_ARP) {
        if (!arp.on) return;                    /* off: the dice wait, the loop is kept */
        uint8_t velocity, n = arp_step(&velocity);
        note_off(p);
        if (n != REST) note_on(p, 0, selected, n, arp_octave_steps(), velocity);
        if (rec_track < TRACKS) {
            track_t *t = &tracks[rec_track];
            if (punch) {
                if (n != REST) punch_note(t, rec_track, n, velocity);   /* (a silence: the step is already empty) */
            } else {
                t->steps[t->len][0] = n == REST ? REST : unmove(n, 0);
                t->vel[t->len][0] = velocity;
                tracks_version++;
                if (++t->len >= SEQ_STEPS) rec_finish();
            }
        }
        return;
    }
    track_t *t = &tracks[p];
    if (!t->len || (p == rec_track && !punch)) return;
    uint32_t s = players[p].idx % t->len;
    note_off(p);
    if (p == rec_track) {                       /* PUNCH IN: the old notes of this step are wiped, not played */
        t->steps[s][0] = t->steps[s][1] = REST;
        tracks_version++;
        return;
    }
    if (t->muted) return;
    for (uint32_t l = 0; l < LAYERS; l++) {
        if (t->steps[s][l] == REST) continue;
        if (chance < 100) {                     /* each note throws its own die; the track itself is kept */
            chance_rnd = chance_rnd * 1664525u + 1013904223u;
            if ((chance_rnd >> 24) * 100u / 256u >= chance) continue;
        }
        note_on(p, l, (uint8_t)p, t->steps[s][l], t->move, t->vel[s][l]);
    }
}

static void clock_tick(void)
{
    clk.tick++;
    for (uint32_t p = 0; p < PLAYERS; p++) {
        if ((players[p].pitch[0] || players[p].pitch[1]) && (int32_t)(clk.tick - players[p].off) >= 0) note_off(p);
        if ((int32_t)(clk.tick - players[p].next) >= 0) {
            player_fire(p);
            players[p].idx++;
            players[p].next = player_time(p, players[p].idx);
        }
    }
}

static uint32_t external_bpm_value;

static int external_clock(void)
{
    return !glo.internal_clock && fm1_ms - clk.last_midi_clock < 500u;
}

static void transport_start(void)
{
    clk.running = 1;
    clk.tick = 0;
    clk.acc = 0;
    clk.ext_ticks = 0;
    clk.last_ms = fm1_ms;
    arp.pos = 0;
    for (uint32_t p = 0; p < PLAYERS; p++) {
        note_off(p);
        player_restart(p, 0);
        players[p].next = 0;
    }
    clk.tick = (uint32_t)-1;                    /* the first tick is tick 0 */
    clock_tick();
}

static void transport_stop(void)
{
    clk.running = 0;
    rec_stop();
    for (uint32_t p = 0; p < PLAYERS; p++) note_off(p);
}

static void clock_service(void)
{
    uint32_t now = fm1_ms, elapsed = now - clk.last_ms;
    clk.last_ms = now;
    if (!clk.running) { clk.ext_ticks = 0; return; }
    if (external_clock()) {
        while (clk.ext_ticks) { clk.ext_ticks--; clock_tick(); }
        return;
    }
    clk.ext_ticks = 0;
    if (elapsed > 100u) elapsed = 100u;         /* after a long pause (a save), do not rush */
    clk.acc += elapsed * glo.bpm * 96u;         /* in 1/60000 ticks */
    while (clk.acc >= 60000u) {
        clk.acc -= 60000u;
        clock_tick();
    }
}

/* ------------------------------------------------------------------------------------------- MIDI in */
static uint32_t idle_ok, clk_n, clk_t0;
static uint8_t midi_held[16][128];

static void live_on(uint8_t owner, uint8_t note, uint8_t velocity)
{
    int32_t raw = note + scale_now.transpose + 12 * scale_now.octave;
    raw = raw < 12 ? 12 : raw > 120 ? 120 : raw;
    pluck_start(owner, selected, scale_snap((uint8_t)raw), velocity, PLUCK_GAIN_ONE);
}

static void live_off(uint8_t owner)
{
    for (uint32_t i = 0; i < PLUCK_VOICES; i++)
        if (pluck_voices[i].owner == owner) pluck_voices[i].released = 1;
}

static void midi_in(void)
{
    uint8_t st, d1, d2;
    int src;
    while ((src = midi_get(&st, &d1, &d2)) != 0) {
        uint32_t ty = st & 0xF0u, ch = st & 15u;
        if (st == 0xF8u) {                      /* clock: 24 a beat */
            clk.last_midi_clock = fm1_ms;
            clk.ext_ticks += 4u;
            if (clk_n % 24u == 0u) {
                if (clk_n) {
                    uint32_t ms = fm1_ms - clk_t0;
                    external_bpm_value = ms ? (60000u + ms / 2u) / ms : 0;
                }
                clk_t0 = fm1_ms;
            }
            clk_n++;
            continue;
        }
        if (st == 0xFAu) { clk_n = 0; if (!glo.internal_clock) transport_start(); continue; }
        if (st == 0xFCu) { if (!glo.internal_clock) transport_stop(); continue; }
        if (st == 0xFBu) { if (!glo.internal_clock) clk.running = 1; continue; }
        if (glo.ch && ch != glo.ch - 1u) continue;
        uint8_t owner = (uint8_t)(OWNER_MIDI + ch);
        if (ty == 0x90u && d2) {
            midi_held[ch][d1 & 127u] = 1;
            live_on(owner, d1, d2);
        } else if (ty == 0x80u || ty == 0x90u) {
            midi_held[ch][d1 & 127u] = 0;
            uint8_t any = 0;
            for (uint32_t i = 0; i < 128; i++) any |= midi_held[ch][i];
            if (!any) live_off(owner);
        }
    }
}

static void app_idle(void)
{
    if (!idle_ok) return;
    midi_in();
    clock_service();
}

/* -------------------------------------------------------------------------------------------- the sounds */
static void sound_default(uint32_t t)
{
    static const uint8_t engine[] = {ENGINE_PLUCK, ENGINE_BLOW, ENGINE_HIT, ENGINE_PLUCK};
    static const uint8_t ping[] = {PING_TRIANGLE, PING_PINK, PING_WHITE, PING_MAJ7};
    static const uint8_t bright[] = {80, 60, 70, 55};
    static const uint16_t decay[] = {900, 1200, 250, 1400};
    static const uint8_t sustain[] = {0, 70, 0, 20};
    pluck_part_t *s = &pluck_parts[t];
    s->engine = engine[t];
    s->ping = ping[t];
    s->macro[PLUCK_BRIGHTNESS] = bright[t];
    s->macro[PLUCK_POSITION] = 40;
    s->adsr.attack_ms = t == 1 ? 60 : 0;
    s->adsr.decay_ms = decay[t];
    s->adsr.sustain = sustain[t];
    s->adsr.release_ms = t == 1 ? 400 : 300;
    s->lfo.target = LFO_PITCH;
    s->lfo.wave = LFO_SINE;
    pluck_lfo_rate(&s->lfo, 50);
    s->lfo.depth = 0;
    pluck_set_level(s, 95);                     /* 75% (95 of 127) on every track at power-on */
    pluck_lfo_state[t].random = 0x9e3779b9u * (t + 1u);
}

#define ENV_MAX_MS 2000u

static uint32_t random_setting(uint32_t *state, uint32_t choices)
{
    *state = *state * 1664525u + 1013904223u;
    return (*state >> 16) % choices;
}

static void randomize_sound(pluck_part_t *s, uint32_t *state)
{
    s->engine = (uint8_t)random_setting(state, ENGINES);
    s->ping = (uint8_t)random_setting(state, engine_last_ping(s->engine) + 1u);
    s->macro[PLUCK_BRIGHTNESS] = (uint8_t)(20u + random_setting(state, 108));
    s->macro[PLUCK_POSITION] = (uint8_t)random_setting(state, 128);
    s->adsr.attack_ms = (uint16_t)(random_setting(state, 4) ? 0 : random_setting(state, 31) * 10u);
    s->adsr.decay_ms = (uint16_t)(100u + random_setting(state, 140) * 10u);
    s->adsr.sustain = (uint8_t)(random_setting(state, 11) * 5u);
    s->adsr.release_ms = (uint16_t)(50u + random_setting(state, 80) * 10u);
    s->lfo.target = (uint8_t)random_setting(state, LFO_TARGETS);
    s->lfo.wave = (uint8_t)random_setting(state, LFO_WAVES);
    pluck_lfo_rate(&s->lfo, (uint8_t)(1u + random_setting(state, 80)));
    s->lfo.depth = (uint8_t)(random_setting(state, 3) ? 0 : random_setting(state, 9) * 5u);
}

/* ------------------------------------------------------------------------------------------- SAVE
 * BEACHCOMBER keeps nothing. SAVE shows a little note about that instead. */
static const char *const calm_notes[][7] = {
    {"NOTHING IS SAVED.", "", "THE TIDE COMES IN,", "THE TIDE GOES OUT.", "", "PLAY. LISTEN.", "LET IT GO."},
    {"NO SAVE BUTTON", "ON THE BEACH.", "", "THIS MOMENT IS", "THE ONLY COPY.", "", "ENJOY IT."},
    {"FOOTPRINTS IN THE", "SAND DON'T STAY.", "", "NEITHER DOES THIS.", "", "KEEP PLAYING."},
    {"THE SEA KEEPS", "NO RECORDINGS.", "", "NO WAVE COMES", "BACK THE SAME.", "", "LISTEN TO THIS ONE."},
    {"SHELLS GO BACK", "TO THE SEA.", "", "PICK UP ANOTHER.", "THERE ARE PLENTY.", "", "PLAY ON."},
};
#define CALM_NOTES (sizeof calm_notes / sizeof calm_notes[0])
#define CALM_MS 6000u

/* -------------------------------------------------------------------------------------------- the screen
 * As LOOPHEADS and Endless Memory: IBM Plex Mono, the title and the battery on top, a picture, the four knobs
 * numbered underneath, a hint at the bottom. */
#include "ui_font.h"
#include "beachcomber_boot_logo.h"

#define C_BG RGB(11, 17, 20)
#define C_ACC RGB(117, 222, 213)
#define C_KEY RGB(230, 240, 239)
#define C_OFF RGB(163, 179, 187)
#define C_RULE RGB(51, 69, 75)
#define C_GRAPH RGB(23, 39, 42)
#define C_SAND RGB(214, 190, 140)
#define C_REC RGB(255, 96, 96)
#define UI_LEFT 15u
#define UI_WIDTH 210u
#define UI_FOOTER_Y 218u
#define UI_BATTERY_WIDTH 27u
#define PIC_Y 48u                               /* the picture: 210 x 80 */
#define PIC_H 80u
#define ROW_Y 134u                              /* the four knob rows */
#define ROW_H 19u
#define BOOT_LOGO_MS 1500u
#define BOX_X 8u                                /* the box around the selected track (as LOOPHEADS' tapes) */
#define BOX_W 224u

static const int16_t sine64[64] = {
    0, 3212, 6393, 9512, 12539, 15446, 18204, 20787, 23170, 25329, 27245, 28898, 30273, 31356, 32137, 32609,
    32767, 32609, 32137, 31356, 30273, 28898, 27245, 25329, 23170, 20787, 18204, 15446, 12539, 9512, 6393, 3212,
    0, -3212, -6393, -9512, -12539, -15446, -18204, -20787, -23170, -25329, -27245, -28898, -30273, -31356, -32137,
    -32609, -32767, -32609, -32137, -31356, -30273, -28898, -27245, -25329, -23170, -20787, -18204, -15446, -12539,
    -9512, -6393, -3212
};

enum { P_HOME, P_ARP, P_SEQ, P_SCALE, P_EDIT, P_ENV, P_LFO, P_FX, P_GLO };

static void ui_text(uint32_t x, uint32_t y, uint32_t width, const felucca_font_t *font,
                    const char *text, uint16_t color, int align)
{
    draw_text_box_bg(x, y, width, font, text, color, align, C_BG);
}

static void ui_divider(uint32_t y, uint16_t color)
{
    lcd_fill(UI_LEFT, y, UI_WIDTH, 1, color);
}

static void ui_title(const char *title)
{
    ui_text(UI_LEFT, 13, UI_WIDTH - UI_BATTERY_WIDTH - 8u, &UI_LARGE, title, C_ACC, 0);
}

static void ui_footer(const char *hint, uint16_t color)
{
    ui_text(UI_LEFT, UI_FOOTER_Y, UI_WIDTH, &UI_SMALL, hint, color, 1);
}

/* a knob row: "1 CHANGE ........ 25%" */
static void ui_row(uint32_t knob, const char *label, const char *value)
{
    char n[3] = {(char)('1' + knob), ' ', 0};
    uint32_t y = ROW_Y + knob * ROW_H;
    cv_begin(UI_WIDTH, UI_BODY.h, C_BG);
    if (*label) {
        int32_t x = cv_text(0, 0, &UI_BODY, n, C_OFF, C_BG);
        cv_text(x, 0, &UI_BODY, label, C_KEY, C_BG);
        cv_text((int32_t)UI_WIDTH - text_w(&UI_BODY, value), 0, &UI_BODY, value, C_ACC, C_BG);
    }
    cv_blit(UI_LEFT, y);
}

#if !BOARD_SMK37
static uint8_t read_battery_level(uint32_t now)
{
    static uint32_t last_read;
    static int32_t average;
    static uint8_t level = UINT8_MAX;
    if (now - last_read >= 250u) {
        last_read = now;
        int32_t raw = fm1_adc_read(FM1_ADC_BATT);
        if (raw > 0) {
            average = average ? average + (raw * 16 - average) / 4 : raw * 16;
            level = average >= 591 * 16 ? 3 : average >= 561 * 16 ? 2 : average >= 531 * 16 ? 1 : 0;
        }
    }
    return level;
}

static void draw_battery(uint8_t level)
{
    uint16_t outline = level == UINT8_MAX ? C_RULE : C_OFF;
    cv_begin(UI_BATTERY_WIDTH, 12, C_BG);
    cv_rect(0, 0, 24, 12, outline);
    cv_rect(1, 1, 22, 10, C_BG);
    cv_rect(24, 4, 3, 4, outline);
    if (level != UINT8_MAX)
        for (uint32_t i = 0; i < level; i++) cv_rect(3 + i * 6, 3, 5, 6, C_ACC);
    cv_blit(UI_LEFT + UI_WIDTH - UI_BATTERY_WIDTH, 17);
}
#endif

static void draw_boot_logo(void)
{
    lcd_fill(0, 0, 240, 240, C_BG);
    uint32_t run = 0, remaining = 0;
    uint16_t color = 0;
    for (uint32_t y = 0; y < BOOT_LOGO_HEIGHT; y += cv_h) {
        cv_begin(BOOT_LOGO_WIDTH, BOOT_LOGO_HEIGHT - y, C_BG);
        uint16_t *pixel = cv_px;
        uint32_t pixels = cv_w * cv_h;
        while (pixels) {
            if (!remaining) {
                remaining = boot_logo_runs[run++];
                color = swap16(boot_logo_runs[run++]);
            }
            uint32_t count = remaining < pixels ? remaining : pixels;
            remaining -= count;
            pixels -= count;
            while (count--) *pixel++ = color;
        }
        cv_blit(BOOT_LOGO_X, BOOT_LOGO_Y + y);
    }
    ui_text(UI_LEFT, 203, UI_WIDTH, &UI_SMALL, "v" APP_VER, C_OFF, 1);
}

/* small text helpers (no printf on the FM-1) */
static char *put_num(char *p, int32_t v, int sign)
{
    char tmp[12];
    uint32_t n = 0, u = (uint32_t)(v < 0 ? -v : v);
    if (v < 0) *p++ = '-';
    else if (sign && v > 0) *p++ = '+';
    do { tmp[n++] = (char)('0' + u % 10u); u /= 10u; } while (u);
    while (n) *p++ = tmp[--n];
    *p = 0;
    return p;
}

static void fmt(char *b, int32_t v, const char *unit)
{
    char *p = put_num(b, v, 0);
    while (*unit) *p++ = *unit++;
    *p = 0;
}

static const char *const note_names[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
static const char *const engine_short[ENGINES] = {"PLK", "BLW", "HIT", "TUB"};

static void track_title(char *b, const char *page)
{
    char *p = b;
    const char *t = "TRACK ";
    while (*t) *p++ = *t++;
    *p++ = (char)('1' + selected);
    *p++ = ' '; *p++ = ' ';
    while (*page) *p++ = *page++;
    *p = 0;
}

/* --- HOME: four tide pools, one for each track: a ring of its steps, the one playing lit */
static uint32_t pool_state(uint32_t t)
{
    const track_t *k = &tracks[t];
    uint32_t pos = k->len ? players[t].idx % k->len : 0;
    if (players[t].idx) pos = (players[t].idx - 1u) % (k->len ? k->len : 1u);
    return (pos | (uint32_t)k->len << 6 | (uint32_t)k->muted << 13 | (uint32_t)(t == selected) << 14 |
           (uint32_t)(rec_track == t) << 15 | (uint32_t)clk.running << 16 | (uint32_t)pluck_parts[t].engine << 17) ^ tracks_version * 2654435761u;
}

/* a 2-pixel frame, as LOOPHEADS draws around the selected tape */
static void cv_box(int32_t x, int32_t y, int32_t w, int32_t h, uint16_t c)
{
    cv_rect(x, y, w, 2, c);
    cv_rect(x, y + h - 2, w, 2, c);
    cv_rect(x, y + 2, 2, h - 4, c);
    cv_rect(x + w - 2, y + 2, 2, h - 4, c);
}

static void draw_pools(void)
{
    cv_begin(BOX_W, PIC_H, C_BG);
    for (uint32_t t = 0; t < TRACKS; t++) {
        const track_t *k = &tracks[t];
        int32_t cx = 28 + (int32_t)t * 56, cy = 30, r = 21;
        if (t == selected) cv_box((int32_t)t * 56 + 1, 0, 54, (int32_t)PIC_H, rec_track == t ? C_REC : C_ACC);
        uint16_t ring = t == selected ? C_ACC : C_OFF;
        if (k->muted) ring = C_RULE;
        if (rec_track == t) ring = C_REC;
        uint32_t n = k->len, playing = n ? (players[t].idx ? (players[t].idx - 1u) % n : 0) : 0;
        if (!n) {
            for (uint32_t a = 0; a < 64; a += 4)
                cv_pset(cx + sine64[(a + 16u) & 63u] * r / 32768, cy + sine64[a] * r / 32768, C_RULE);
        }
        for (uint32_t i = 0; i < n; i++) {
            uint32_t a = (i * 64u / n + 48u) & 63u;              /* step 0 at the top */
            int32_t x = cx + sine64[(a + 16u) & 63u] * r / 32768, y = cy + sine64[a] * r / 32768;
            uint8_t note = step_has_note(k, i);
            uint16_t c = note ? ring : C_RULE;
            if (clk.running && i == playing && rec_track != t) {
                cv_rect(x - 2, y - 2, 5, 5, note && !k->muted ? C_SAND : C_OFF);
            } else if (note) {
                cv_rect(x - 1, y - 1, 3, 3, c);
            } else {
                cv_pset(x, y, c);
            }
        }
        /* the middle: a little inner ripple for the selected track */
        if (t == selected)
            for (uint32_t a = 0; a < 64; a += 2)
                cv_pset(cx + sine64[(a + 16u) & 63u] * 9 / 32768, cy + sine64[a] * 6 / 32768, C_ACC);
        char label[6] = {(char)('1' + t), ' ', 0};
        const char *e = engine_short[pluck_parts[t].engine];
        label[2] = e[0]; label[3] = e[1]; label[4] = e[2]; label[5] = 0;
        cv_text(cx - text_w(&UI_SMALL, label) / 2, 59, &UI_SMALL, label, t == selected ? C_ACC : C_OFF, C_BG);
    }
    cv_blit(BOX_X, PIC_Y);
}

/* "C", "L40", "R100" */
static void fmt_pan(char *b, int32_t pan)
{
    if (!pan) { b[0] = 'C'; b[1] = 0; return; }
    b[0] = pan < 0 ? 'L' : 'R';
    put_num(b + 1, pan < 0 ? -pan : pan, 0);
}

/* HOME's mixer, under each tide pool: a fader (the volume, its % under it) and a balance (the pan, its value
 * under it: C, L40, R100) */
static void draw_home_rows(void)
{
    cv_begin(BOX_W, 74, C_BG);
    for (uint32_t t = 0; t < TRACKS; t++) {
        const pluck_part_t *part = &pluck_parts[t];
        int32_t cx = 28 + (int32_t)t * 56;
        uint16_t c = tracks[t].muted ? C_RULE : t == selected ? C_ACC : C_KEY;
        int32_t h = part->level * 32 / (int32_t)PLUCK_MAX_LEVEL;
        cv_rect(cx - 6, 1, 12, 35, C_RULE);             /* the fader */
        cv_rect(cx - 5, 2, 10, 33, C_GRAPH);
        if (h) cv_rect(cx - 5, 35 - h, 10, h, c);
        char v[8];
        fmt(v, (part->level * 100 + 63) / PLUCK_MAX_LEVEL, "%");
        cv_text(cx - text_w(&UI_SMALL, v) / 2, 38, &UI_SMALL, v, c, C_BG);
        cv_rect(cx - 20, 57, 41, 1, C_RULE);           /* the balance: left .. centre .. right */
        cv_rect(cx - 20, 54, 1, 7, C_RULE);
        cv_rect(cx + 20, 54, 1, 7, C_RULE);
        cv_rect(cx, 55, 1, 5, C_RULE);
        cv_rect(cx - 1 + part->pan * 19 / 100, 52, 3, 11, t == selected ? C_SAND : C_OFF);
        fmt_pan(v, part->pan);
        cv_text(cx - text_w(&UI_SMALL, v) / 2, 62, &UI_SMALL, v, C_OFF, C_BG);
    }
    cv_blit(BOX_X, ROW_Y);
}

/* --- ARP: the 16 slots as columns (height: the note, filled: it plays), the lit pool keys underneath */
static void draw_arp_pic(void)
{
    cv_begin(UI_WIDTH, PIC_H, C_BG);
    for (uint32_t i = 0; i < ARP_SLOTS; i++) {
        int32_t x = (int32_t)i * 13 + 2;
        uint8_t height;
        arp_slot_note(i, &height);
        int32_t h = 6 + height * 46 / 255;
        uint8_t inside = i < arp.length, plays = arp_slot_plays(i);
        uint16_t c = !inside ? C_RULE : plays ? C_ACC : C_OFF;
        if (inside && plays) cv_rect(x, 56 - h, 10, h, c == C_ACC ? C_GRAPH : c);
        cv_rect(x, 56 - h, 10, 1, c);
        cv_rect(x, 56 - h, 1, h, c);
        cv_rect(x + 9, 56 - h, 1, h, c);
        if (inside && plays) cv_rect(x + 2, 56 - h + 2, 6, 3, C_ACC);
        if (clk.running && arp.on && i == arp.shown) cv_rect(x, 59, 10, 3, C_SAND);
    }
    /* the pool: 16 white keys */
    for (uint32_t i = 0; i < 16; i++) {
        int32_t x = (int32_t)i * 13 + 2;
        uint16_t c = arp.pool >> i & 1u ? C_ACC : C_GRAPH;
        cv_rect(x, 68, 10, 10, C_RULE);
        cv_rect(x + 1, 69, 8, 8, c);
    }
    cv_blit(UI_LEFT, PIC_Y);
}

static void draw_arp_rows(void)
{
    char v[10];
    fmt(v, arp.change, "%");
    ui_row(0, "CHANGE", v);
    fmt(v, arp.length, arp.length == 1 ? " STEP" : " STEPS");
    ui_row(1, "LENGTH", v);
    fmt(v, arp.range, " OCT");
    ui_row(2, "RANGE", v);
    fmt(v, arp.density, "%");
    ui_row(3, "DENSITY", v);
}

/* --- SEQ: the four tracks as lanes, the selected one in a box */
#define LANE_Y 44u
#define LANE_H 22
static void draw_lanes(void)
{
    cv_begin(BOX_W, 4u * LANE_H, C_BG);
    for (uint32_t t = 0; t < TRACKS; t++) {
        const track_t *k = &tracks[t];
        int32_t y = (int32_t)t * LANE_H;
        uint16_t c = rec_track == t ? C_REC : t == selected ? C_ACC : k->muted ? C_RULE : C_OFF;
        if (t == selected) cv_box(0, y, (int32_t)BOX_W, LANE_H - 1, c);
        char n[2] = {(char)('1' + t), 0};
        cv_text(8, y + 4, &UI_SMALL, n, c, C_BG);
        cv_rect(24, y + 16, 192, 1, C_GRAPH);
        uint32_t len = k->len;
        if (!len) continue;
        uint32_t playing = players[t].idx ? (players[t].idx - 1u) % len : 0;
        int32_t lo = 999, hi = -999;                /* the lane's lowest and highest note: bars from 2 to 11 px */
        for (uint32_t i = 0; i < len * LAYERS; i++) {
            uint8_t s = k->steps[i / LAYERS][i % LAYERS];
            if (s == REST) continue;
            int32_t v = (s >> 4) * (int32_t)scale_now.size + (s & 15);
            if (v < lo) lo = v;
            if (v > hi) hi = v;
        }
        for (uint32_t i = 0; i < len; i++) {
            int32_t x0 = 24 + (int32_t)(i * 192u / SEQ_STEPS), w = (int32_t)(192u / SEQ_STEPS);
            for (uint32_t l = 0; l < LAYERS; l++) {   /* a second note on the step: a dot at its height */
                uint8_t s = k->steps[i][l];
                if (s == REST) continue;
                int32_t v = (s >> 4) * (int32_t)scale_now.size + (s & 15);
                int32_t h = hi > lo ? 2 + (v - lo) * 9 / (hi - lo) : 6;
                if (l == 0) cv_rect(x0, y + 16 - h, w - 2, h, k->muted ? C_RULE : c);
                else cv_rect(x0, y + 15 - h, w - 2, 2, k->muted ? C_RULE : C_SAND);
            }
            if (clk.running && i == playing && rec_track != t) cv_rect(x0, y + 17, w, 2, C_SAND);
        }
    }
    cv_blit(BOX_X, LANE_Y);
}

static void draw_seq_rows(void)
{
    for (uint32_t t = 0; t < TRACKS; t++) {
        char v[8];
        put_num(v, tracks[t].move, 1);
        ui_row(t, "SHIFT", tracks[t].muted ? "MUTED" : v);
    }
}

/* --- SCALE: a piano octave with the scale's notes lit */
static void draw_scale_pic(void)
{
    static const uint8_t white[] = {0, 2, 4, 5, 7, 9, 11};
    static const uint8_t black[] = {1, 3, 6, 8, 10};
    static const uint8_t black_x[] = {30, 60, 120, 150, 180};
    uint8_t root = scale_root_pc();
    cv_begin(UI_WIDTH, PIC_H, C_BG);
    cv_text(0, 0, &UI_LARGE, scales[scale_now.scale].name, C_ACC, C_BG);
    for (uint32_t i = 0; i < 7; i++) {
        uint16_t c = scale_contains((uint8_t)white[i]) ? (white[i] == root ? C_SAND : C_ACC) : C_GRAPH;
        cv_rect(i * 30u, 28, 28, 52, C_RULE);
        cv_rect(i * 30u + 1, 29, 26, 50, c);
    }
    for (uint32_t i = 0; i < 5; i++) {
        int32_t x = black_x[i] - 10;
        cv_rect(x, 28, 20, 32, C_RULE);
        cv_rect(x + 1, 28, 18, 31, C_BG);
        uint16_t c = scale_contains(black[i]) ? (black[i] == root ? C_SAND : C_ACC) : C_BG;
        cv_rect(x + 2, 29, 16, 29, c);
    }
    cv_blit(UI_LEFT, PIC_Y);
}

static void draw_scale_rows(void)
{
    char v[16];
    char *p = put_num(v, scale_now.transpose, 1);
    *p++ = ' '; *p++ = '(';
    const char *n = note_names[scale_root_pc()];
    while (*n) *p++ = *n++;
    *p++ = ')'; *p = 0;
    char s[12];
    fmt(s, scale_now.size, " NOTES");
    ui_row(0, "SCALE", s);
    ui_row(1, "TRANSPOSE", scale_now.transpose ? v : "0 (C)");
    ui_row(2, "", "");
    ui_row(3, "", "");
}

/* --- EDIT: the ping's shape (what hits the string, the skin or the tube) */
static int32_t preview_ping(uint8_t ping, uint32_t i, uint32_t *noise_state, int32_t *smooth)
{
    uint32_t phase = (i * 65536u / 70u) & 65535u;
    if (ping >= PING_MAJOR) {
        return chord_wave(chord_modes[ping][0], i * 61356676u);   /* one root cycle every 70 pixels */
    }
    if (ping >= PING_SINE) return ping_wave(ping, phase);
    *noise_state = *noise_state * 1664525u + 1013904223u;
    int32_t x = (int32_t)(*noise_state >> 16) - 32768;
    if (ping == PING_PINK) { *smooth += (x - *smooth) / 3; return *smooth * 2; }
    if (ping == PING_BROWN) { *smooth += (x - *smooth) / 10; return *smooth * 3; }
    return x;
}

static void draw_edit_pic(const pluck_part_t *s)
{
    cv_begin(UI_WIDTH, PIC_H, C_BG);
    cv_text(0, 0, &UI_LARGE, engine_names[s->engine], C_ACC, C_BG);
    cv_text((int32_t)UI_WIDTH - text_w(&UI_SMALL, ping_names[s->ping]), 5, &UI_SMALL, ping_names[s->ping], C_OFF, C_BG);
    /* the ping */
    uint32_t ns = 0x1234567u;
    int32_t sm = 0, py = 50;
    cv_rect(0, 50, UI_WIDTH, 1, C_GRAPH);
    for (uint32_t x = 0; x < 140; x++) {
        int32_t v = preview_ping(s->ping, x, &ns, &sm);
        v = v < -32768 ? -32768 : v > 32767 ? 32767 : v;
        int32_t y = 50 - v * 22 / 32768;
        if (x) cv_line((int32_t)x - 1, py, (int32_t)x, y, C_ACC);
        py = y;
    }
    /* the engine: a string, a tube, a skin */
    int32_t ex = 152;
    if (s->engine == ENGINE_PLUCK) {
        cv_line(ex, 50, ex + 56, 50, C_OFF);
        int32_t px = ex + 4 + s->macro[PLUCK_POSITION] * 26 / 127;
        cv_line(ex, 50, px, 40, C_SAND);
        cv_line(px, 40, ex + 56, 50, C_SAND);
    } else if (s->engine == ENGINE_BLOW) {
        cv_rect(ex, 40, 56, 1, C_OFF);
        cv_rect(ex, 60, 56, 1, C_OFF);
        cv_rect(ex + 55, 40, 1, 21, C_OFF);
        for (int32_t i = 0; i < 4; i++) cv_rect(ex + 8 + i * 12, 49, 4, 3, C_SAND);
    } else if (s->engine == ENGINE_TUBE) {      /* an open tube with the wave standing in it */
        cv_rect(ex, 40, 56, 1, C_OFF);
        cv_rect(ex, 60, 56, 1, C_OFF);
        for (int32_t x = 0; x < 56; x++)
            cv_pset(ex + x, 50 - sine64[(uint32_t)x * 32u / 56u] * 7 / 32768, C_SAND);
    } else {
        for (uint32_t a = 0; a < 64; a++)
            cv_pset(ex + 28 + sine64[(a + 16u) & 63u] * 26 / 32768, 50 + sine64[a] * 10 / 32768, C_OFF);
        cv_rect(ex + 26, 46, 5, 5, C_SAND);
    }
    cv_blit(UI_LEFT, PIC_Y);
}

static void draw_edit_rows(const pluck_part_t *s)
{
    char v[8];
    ui_row(0, "ENGINE", engine_names[s->engine]);
    ui_row(1, "PING", ping_names[s->ping]);
    fmt(v, s->macro[PLUCK_BRIGHTNESS] * 100 / 127, "%");
    ui_row(2, "BRIGHTNESS", v);
    fmt(v, s->macro[PLUCK_POSITION] * 100 / 127, "%");
    ui_row(3, "POSITION", v);
}

/* --- ENV */
static void draw_env_pic(const pluck_adsr_t *adsr)
{
    int32_t times[] = {adsr->attack_ms + 20, adsr->decay_ms + 20, 600, adsr->release_ms + 20};
    int32_t total = times[0] + times[1] + times[2] + times[3];
    int32_t x[5] = {0}, elapsed = 0;
    for (uint32_t i = 0; i < 4; i++) {
        elapsed += times[i];
        x[i + 1] = elapsed * (int32_t)(UI_WIDTH - 1u) / total;
    }
    int32_t sustain = 76 - (int32_t)adsr->sustain * 66 / 100;
    int32_t y[] = {76, 8, sustain, sustain, 76};
    cv_begin(UI_WIDTH, PIC_H, C_BG);
    for (uint32_t i = 0; i < 4; i++) {
        int32_t width = x[i + 1] - x[i];
        for (int32_t col = 0; col < width; col++) {
            int32_t top = y[i] + (y[i + 1] - y[i]) * col / width;
            cv_rect(x[i] + col, top, 1, 77 - top, C_GRAPH);
        }
    }
    for (uint32_t i = 1; i < 4; i++)
        for (int32_t row = y[i] + 3; row < 76; row += 4) cv_pset(x[i], row, C_RULE);
    for (uint32_t i = 0; i < 4; i++) cv_line(x[i], y[i], x[i + 1], y[i + 1], C_ACC);
    cv_blit(UI_LEFT, PIC_Y);
}

static void draw_env_rows(const pluck_adsr_t *adsr)
{
    char v[10];
    fmt(v, adsr->attack_ms, " MS");
    ui_row(0, "ATTACK", v);
    fmt(v, adsr->decay_ms, " MS");
    ui_row(1, "DECAY", v);
    fmt(v, adsr->sustain, "%");
    ui_row(2, "SUSTAIN", v);
    fmt(v, adsr->release_ms, " MS");
    ui_row(3, "RELEASE", v);
}

/* --- LFO */
static const char *const lfo_targets[] = {"PITCH", "VOLUME", "BRIGHTNESS", "POSITION"};
static const char *const lfo_waves[] = {"SINE", "TRIANGLE", "SQUARE", "SAW", "RANDOM"};

static void draw_lfo_pic(const pluck_lfo_t *lfo)
{
    static const int16_t held[] = {18000, -11000, 29000, 7000, -28000, 13000};
    cv_begin(UI_WIDTH, PIC_H, C_BG);
    cv_rect(0, 40, UI_WIDTH, 1, C_RULE);
    int32_t previous_y = 40;
    for (uint32_t x = 0; x < UI_WIDTH; x++) {
        int32_t sample = pluck_lfo_wave(lfo->wave, x * 131072u / UI_WIDTH % 65536u, held[x * 6u / UI_WIDTH]);
        int32_t y = 40 - sample * lfo->depth * 34 / (32768 * 100);
        if (x) cv_line((int32_t)x - 1, previous_y, (int32_t)x, y, C_ACC);
        previous_y = y;
    }
    cv_blit(UI_LEFT, PIC_Y);
}

static void draw_lfo_rows(const pluck_lfo_t *lfo)
{
    char v[12];
    ui_row(0, "TARGET", lfo_targets[lfo->target]);
    ui_row(1, "WAVE", lfo_waves[lfo->wave]);
    char *p = put_num(v, lfo->rate_tenths / 10, 0);
    *p++ = '.'; *p++ = (char)('0' + lfo->rate_tenths % 10);
    *p++ = ' '; *p++ = 'H'; *p++ = 'Z'; *p = 0;
    ui_row(2, "RATE", v);
    fmt(v, lfo->depth, "%");
    ui_row(3, "DEPTH", v);
}

/* --- FX: the dry sound splits into three, side by side, and comes back together */
static void draw_fx_pic(void)
{
    static const char *const names[] = {"DELAY", "HALL", "CHORUS"};
    static const int32_t box_y[] = {2, 31, 60};      /* three boxes, one above the other */
    const uint8_t amounts[] = {fx.delay, fx.hall, fx.chorus};
    const int32_t bx = 52, bw = 106, bh = 16;
    cv_begin(UI_WIDTH, PIC_H, C_BG);
    cv_text(0, 33, &UI_SMALL, "IN", C_OFF, C_BG);
    cv_text(187, 33, &UI_SMALL, "OUT", C_OFF, C_BG);
    cv_rect(16, 39, 18, 1, C_OFF);                  /* in -> the split */
    cv_rect(176, 39, 9, 1, C_OFF);                  /* the join -> out */
    cv_rect(34, 10, 1, 59, C_OFF);
    cv_rect(176, 10, 1, 59, C_OFF);
    for (uint32_t i = 0; i < 3; i++) {
        int32_t by = box_y[i], mid = by + bh / 2;
        uint16_t c = amounts[i] ? C_ACC : C_RULE;
        cv_rect(34, mid, bx - 34, 1, c);
        cv_rect(bx + bw, mid, 176 - bx - bw, 1, c);
        cv_rect(bx, by, bw, bh + 1, C_BG);
        cv_rect(bx, by, bw, 1, c);
        cv_rect(bx, by + bh, bw, 1, c);
        cv_rect(bx, by, 1, bh, c);
        cv_rect(bx + bw - 1, by, 1, bh + 1, c);
        cv_text(bx + 5, by + 2, &UI_SMALL, names[i], amounts[i] ? C_KEY : C_OFF, C_BG);
        cv_rect(bx + 60, by + 7, 40, 3, C_GRAPH);
        cv_rect(bx + 60, by + 7, amounts[i] * 40 / 100, 3, c);
    }
    cv_blit(UI_LEFT, PIC_Y);
}

static void draw_fx_rows(void)
{
    char v[8];
    fmt(v, fx.delay, "%");
    ui_row(0, "DELAY", v);
    ui_row(1, "DELAY TIME", delay_division_names[fx.time]);
    fmt(v, fx.hall, "%");
    ui_row(2, "HALL", v);
    fmt(v, fx.chorus, "%");
    ui_row(3, "CHORUS", v);
}

/* --- GLO */
static void draw_glo_pic(void)
{
    char v[12];
    uint32_t bpm = external_clock() ? external_bpm_value : glo.bpm;
    fmt(v, bpm, "");
    cv_begin(UI_WIDTH, PIC_H, C_BG);
    int32_t w = text_w(&UI_LARGE, v);
    /* the tempo, twice the large font: drawn small at the bottom of the canvas, then doubled into place */
    cv_text(0, 60, &UI_LARGE, v, C_KEY, C_BG);
    int32_t tx = ((int32_t)UI_WIDTH - 2 * w) / 2;
    for (int32_t y = 0; y < 2 * (int32_t)UI_LARGE.h; y++)
        for (int32_t x = 0; x < 2 * w; x++)
            cv_px[(uint32_t)(y + 2) * cv_w + (uint32_t)(tx + x)] = cv_px[(uint32_t)(60 + y / 2) * cv_w + (uint32_t)(x / 2)];
    cv_rect(0, 60, w, UI_LARGE.h, C_BG);
    const char *src = external_clock() ? "MIDI CLOCK" : "BPM";
    cv_text(((int32_t)UI_WIDTH - text_w(&UI_SMALL, src)) / 2, 44, &UI_SMALL, src, C_OFF, C_BG);
    /* the swing: two steps, the second one late */
    int32_t late = (glo.swing - 50) * 2;
    cv_rect(55, 66, 2, 10, C_ACC);
    cv_rect(105 + late, 66, 2, 10, C_ACC);
    cv_rect(155, 66, 2, 10, C_ACC);
    cv_rect(55, 76, 102, 1, C_RULE);
    cv_blit(UI_LEFT, PIC_Y);
}

static void draw_glo_rows(void)
{
    char v[12];
    fmt(v, glo.bpm, " BPM");
    ui_row(0, "TEMPO", v);
    fmt(v, glo.swing, "%");
    ui_row(1, "SWING", glo.swing == 50 ? "STRAIGHT" : v);
    char c[8] = "CH ";
    put_num(c + 3, glo.ch, 0);
    ui_row(2, "MIDI IN", glo.ch ? c : "OMNI");
    char *p = put_num(v, glo.tuning, 1);
    const char *u = " CENTS";
    while (*u) *p++ = *u++;
    *p = 0;
    ui_row(3, "TUNING", v);
}

/* ------------------------------------------------------------------------------------- knobs and keys */
static int32_t step_clamp(int32_t v, int32_t d, int32_t lo, int32_t hi)
{
    v += d;
    return v < lo ? lo : v > hi ? hi : v;
}

static void edit_page(uint8_t page, const int32_t s[4])
{
    pluck_part_t *snd = &pluck_parts[selected];
    switch (page) {
    case P_HOME:
        for (uint32_t t = 0; t < TRACKS; t++)
            if (s[t]) pluck_set_level(&pluck_parts[t], (uint8_t)step_clamp(pluck_parts[t].level, s[t] * 3, 0, PLUCK_MAX_LEVEL));
        break;
    case P_ARP:
        arp.change = (uint8_t)step_clamp(arp.change, s[0] * 5, 0, 100);
        if (s[1]) {
            arp.length = (uint8_t)step_clamp(arp.length, s[1], 1, ARP_SLOTS);
            arp.pos = (uint8_t)(arp.pos % arp.length);
        }
        arp.range = (uint8_t)step_clamp(arp.range, s[2], 1, 4);
        arp.density = (uint8_t)step_clamp(arp.density, s[3] * 5, 0, 100);
        break;
    case P_SEQ:
        for (uint32_t t = 0; t < TRACKS; t++) tracks[t].move = (int8_t)step_clamp(tracks[t].move, s[t], -12, 12);
        break;
    case P_SCALE:
        if (s[0]) scale_select((uint8_t)step_clamp(scale_now.scale, s[0], 0, SCALE_COUNT - 1));
        scale_now.transpose = (int8_t)step_clamp(scale_now.transpose, s[1], -31, 43);
        break;
    case P_EDIT:
        snd->engine = (uint8_t)step_clamp(snd->engine, s[0], 0, ENGINES - 1);
        /* HIT takes no chords: its pings stop at SQUARE (a chord ping becomes white noise) */
        /* BLOW and HIT take no chords: their pings stop at SQUARE (a chord ping becomes white noise) */
        if (snd->ping > engine_last_ping(snd->engine)) snd->ping = PING_WHITE;
        snd->ping = (uint8_t)step_clamp(snd->ping, s[1], 0, engine_last_ping(snd->engine));
        snd->macro[PLUCK_BRIGHTNESS] = (uint8_t)step_clamp(snd->macro[PLUCK_BRIGHTNESS], s[2] * 4, 0, 127);
        snd->macro[PLUCK_POSITION] = (uint8_t)step_clamp(snd->macro[PLUCK_POSITION], s[3] * 4, 0, 127);
        break;
    case P_ENV: {
        pluck_adsr_t *a = &snd->adsr;
        int32_t k = 10;
        a->attack_ms = (uint16_t)step_clamp(a->attack_ms, s[0] * (a->attack_ms >= 200 ? 50 : k), 0, ENV_MAX_MS);
        a->decay_ms = (uint16_t)step_clamp(a->decay_ms, s[1] * (a->decay_ms >= 200 ? 50 : k), 0, ENV_MAX_MS);
        a->sustain = (uint8_t)step_clamp(a->sustain, s[2] * 5, 0, 100);
        a->release_ms = (uint16_t)step_clamp(a->release_ms, s[3] * (a->release_ms >= 200 ? 50 : k), 0, ENV_MAX_MS);
        break;
    }
    case P_LFO:
        snd->lfo.target = (uint8_t)step_clamp(snd->lfo.target, s[0], 0, LFO_TARGETS - 1);
        snd->lfo.wave = (uint8_t)step_clamp(snd->lfo.wave, s[1], 0, LFO_WAVES - 1);
        if (s[2]) pluck_lfo_rate(&snd->lfo, (uint8_t)step_clamp(snd->lfo.rate_tenths, s[2], 1, 120));
        snd->lfo.depth = (uint8_t)step_clamp(snd->lfo.depth, s[3] * 5, 0, 100);
        break;
    case P_FX:
        fx.delay = (uint8_t)step_clamp(fx.delay, s[0] * 5, 0, 100);
        fx.time = (uint8_t)step_clamp(fx.time, s[1], 0, DELAY_DIVS - 1);
        fx.hall = (uint8_t)step_clamp(fx.hall, s[2] * 5, 0, 100);
        fx.chorus = (uint8_t)step_clamp(fx.chorus, s[3] * 5, 0, 100);
        break;
    case P_GLO:
        glo.bpm = (uint8_t)step_clamp(glo.bpm, s[0], 40, 240);
        glo.swing = (uint8_t)step_clamp(glo.swing, s[1], 50, 75);
        glo.ch = (uint8_t)step_clamp(glo.ch, s[2], 0, 16);
        if (s[3]) {
            glo.tuning = (int8_t)step_clamp(glo.tuning, s[3], -50, 50);
            pluck_set_tuning(glo.tuning);
        }
        break;
    }
}

static void update_fx(void)
{
    static uint32_t previous = ~0u;
    uint32_t bpm = external_clock() && external_bpm_value >= 30 && external_bpm_value <= 300 ? external_bpm_value : glo.bpm;
    uint32_t state = bpm | (uint32_t)fx.time << 9 | (uint32_t)fx.delay << 12 | (uint32_t)fx.hall << 19 |
                     (uint32_t)fx.chorus << 26;
    if (state == previous) return;
    previous = state;
    delay_configure(fx.delay != 0, delay_tempo_samples(bpm, delay_division_clocks[fx.time]), DELAY_FEEDBACK, fx.delay);
    hall_configure(fx.hall != 0, HALL_DECAY, HALL_DAMPING, fx.hall);
    chorus_amount = fx.chorus;
}

/* the 27 keys: which are white (the ARP's pool) */
static const int8_t white_index[27] = {0, -1, 1, -1, 2, -1, 3, 4, -1, 5, -1, 6, 7, -1, 8, -1, 9, -1, 10, 11, -1,
                                       12, -1, 13, 14, -1, 15};

static uint8_t button_page(uint32_t button)
{
    switch (button) {
    case BTN_HOME: return P_HOME;
    case BTN_ARP: return P_ARP;
    case BTN_SEQ: return P_SEQ;
    case BTN_SEL: return P_SCALE;
    case BTN_EDIT: return P_EDIT;
    case BTN_ENV: return P_ENV;
    case BTN_LFO: return P_LFO;
    case BTN_FX: return P_FX;
    case BTN_GLO: return P_GLO;
    default: return 0xFF;
    }
}

static const char *page_title(uint8_t page, char *b)
{
    switch (page) {
    case P_HOME: return "HOME";
    case P_ARP: return arp.on ? "ARP  ON" : "ARP  OFF";
    case P_SEQ: { char *q = b; const char *w = "SEQ  CHANCE "; while (*w) *q++ = *w++; q = put_num(q, chance, 0); *q++ = '%'; *q = 0; return b; }
    case P_SCALE: return "SCALE";
    case P_EDIT: track_title(b, "EDIT"); return b;
    case P_ENV: track_title(b, "ENV"); return b;
    case P_LFO: track_title(b, "LFO"); return b;
    case P_FX: return "FX  PARALLEL";
    default: return "GLO";
    }
}

static const char *message;
static uint32_t message_at;
static void say(const char *text)
{
    message = text;
    message_at = fm1_ms;
}

static void draw_footer(uint8_t page)
{
    static char b[40];
    if (message) { ui_footer(message, C_SAND); return; }
    char *p = b;
    switch (page) {
    case P_HOME: ui_footer("HOLD HOME + KNOBS 1-4: PAN", C_OFF); break;
    case P_ARP: {
        const char *t = "SPEED ";
        while (*t) *p++ = *t++;
        t = div_names[arp.speed];
        while (*t) *p++ = *t++;
        t = "  INTO TRACK ";
        while (*t) *p++ = *t++;
        *p++ = (char)('1' + selected);
        *p = 0;
        ui_footer(b, C_OFF);
        break;
    }
    case P_SEQ:
        if (rec_track < TRACKS) {
            const char *t = punch ? "PUNCH IN TRACK " : "RECORDING TRACK ";
            while (*t) *p++ = *t++;
            *p++ = (char)('1' + rec_track);
            if (!punch) {
                *p++ = ' '; *p++ = ' ';
                p = put_num(p, tracks[rec_track].len, 0);
                t = "/16";
                while (*t) *p++ = *t++;
            }
            *p = 0;
            ui_footer(b, C_REC);
        } else {
            ui_footer(tracks[selected].len ? "REC: PUNCH IN  HOLD REC: CLEAR" : "REC: RECORD   HOLD REC: CLEAR", C_OFF);
        }
        break;
    case P_SCALE: ui_footer("KEYS: TRANSPOSE EVERYTHING", C_OFF); break;
    case P_EDIT: ui_footer("HOLD EDIT: RANDOM SOUND", C_OFF); break;
    case P_ENV:
    case P_LFO: ui_footer("SELECT: TRACK", C_OFF); break;
    case P_FX: ui_footer("EACH HEARS THE DRY SOUND", C_OFF); break;
    default: ui_footer("BEACHCOMBER " APP_VER, C_OFF); break;
    }
}

/* everything a page shows that can change, folded into one number: redraw when it changes */
static uint32_t page_state(uint8_t page)
{
    const pluck_part_t *s = &pluck_parts[selected];
    uint32_t h = 2166136261u;
#define MIX(v) (h = (h ^ (uint32_t)(v)) * 16777619u)
    MIX(selected);
    switch (page) {
    case P_HOME:
        for (uint32_t t = 0; t < TRACKS; t++) { MIX(pool_state(t)); MIX(pluck_parts[t].level); }
        MIX(glo.bpm); MIX(arp.on); MIX(external_clock() ? external_bpm_value : 0);
        break;
    case P_ARP:
        MIX(arp.on); MIX(arp.change); MIX(arp.length); MIX(arp.range); MIX(arp.density); MIX(arp.speed);
        MIX(arp.pool); MIX(clk.running ? arp.shown : 99); MIX(scale_now.size);
        for (uint32_t i = 0; i < ARP_SLOTS; i++) { MIX(arp.note[i]); MIX(arp.gate[i]); }
        break;
    case P_SEQ:
        for (uint32_t t = 0; t < TRACKS; t++) {
            MIX(pool_state(t)); MIX(tracks[t].move);
        }
        MIX(rec_track < TRACKS ? tracks[rec_track].len : 0);
        break;
    case P_SCALE: MIX(scale_now.scale); MIX(scale_now.transpose); MIX(scale_now.octave); break;
    case P_EDIT: MIX(s->engine); MIX(s->ping); MIX(s->macro[0]); MIX(s->macro[1]); break;
    case P_ENV: MIX(s->adsr.attack_ms); MIX(s->adsr.decay_ms); MIX(s->adsr.sustain); MIX(s->adsr.release_ms); break;
    case P_LFO: MIX(s->lfo.target); MIX(s->lfo.wave); MIX(s->lfo.rate_tenths); MIX(s->lfo.depth); break;
    case P_FX: MIX(fx.delay); MIX(fx.time); MIX(fx.hall); MIX(fx.chorus); break;
    default:
        MIX(glo.bpm); MIX(glo.swing); MIX(glo.ch); MIX(glo.tuning); MIX(external_clock());
        MIX(external_clock() ? external_bpm_value : 0);
        break;
    }
#undef MIX
    return h;
}

/* what the four knob rows show */
static uint32_t row_state(uint8_t page)
{
    const pluck_part_t *s = &pluck_parts[selected];
    uint32_t h = 2166136261u;
#define MIX(v) (h = (h ^ (uint32_t)(v)) * 16777619u)
    switch (page) {
    case P_HOME: for (uint32_t t = 0; t < TRACKS; t++) { MIX(pluck_parts[t].level); MIX(pluck_parts[t].pan); MIX(tracks[t].muted); } MIX(selected); break;
    case P_ARP: MIX(arp.change); MIX(arp.length); MIX(arp.range); MIX(arp.density); break;
    case P_SEQ: for (uint32_t t = 0; t < TRACKS; t++) { MIX(tracks[t].move); MIX(tracks[t].muted); } break;
    case P_SCALE: MIX(scale_now.scale); MIX(scale_now.transpose); MIX(scale_now.octave); break;
    case P_EDIT: MIX(selected); MIX(s->engine); MIX(s->ping); MIX(s->macro[0]); MIX(s->macro[1]); break;
    case P_ENV: MIX(selected); MIX(s->adsr.attack_ms); MIX(s->adsr.decay_ms); MIX(s->adsr.sustain); MIX(s->adsr.release_ms); break;
    case P_LFO: MIX(selected); MIX(s->lfo.target); MIX(s->lfo.wave); MIX(s->lfo.rate_tenths); MIX(s->lfo.depth); break;
    case P_FX: MIX(fx.delay); MIX(fx.time); MIX(fx.hall); MIX(fx.chorus); break;
    default: MIX(glo.bpm); MIX(glo.swing); MIX(glo.ch); MIX(glo.tuning); break;
    }
#undef MIX
    return h;
}

static uint32_t footer_state(uint8_t page)
{
    uint32_t h = (uint32_t)(uintptr_t)message ^ message_at * 31u ^ (uint32_t)selected << 1;
    if (page == P_HOME) h ^= glo.bpm << 4 ^ (uint32_t)clk.running << 13 ^ (uint32_t)arp.on << 14 ^
                             (external_clock() ? external_bpm_value << 16 : 0);
    if (page == P_ARP) h ^= (uint32_t)arp.speed << 4;
    if (page == P_SEQ) h ^= (rec_track < TRACKS ? 0x100u + tracks[rec_track].len + punch * 0x80u : 0) << 4 ^ (tracks[selected].len ? 1u << 20 : 0);
    return h;
}

static void draw_page(uint8_t page, uint8_t whole)
{
    const pluck_part_t *s = &pluck_parts[selected];
    switch (page) {
    case P_HOME: draw_pools(); if (whole) draw_home_rows(); break;
    case P_ARP: draw_arp_pic(); if (whole) draw_arp_rows(); break;
    case P_SEQ: draw_lanes(); if (whole) draw_seq_rows(); break;
    case P_SCALE: draw_scale_pic(); if (whole) draw_scale_rows(); break;
    case P_EDIT: draw_edit_pic(s); if (whole) draw_edit_rows(s); break;
    case P_ENV: draw_env_pic(&s->adsr); if (whole) draw_env_rows(&s->adsr); break;
    case P_LFO: draw_lfo_pic(&s->lfo); if (whole) draw_lfo_rows(&s->lfo); break;
    case P_FX: draw_fx_pic(); if (whole) draw_fx_rows(); break;
    default: draw_glo_pic(); if (whole) draw_glo_rows(); break;
    }
}

static int quiet(void)
{
    for (uint32_t i = 0; i < PLUCK_VOICES; i++)
        if (pluck_voices[i].active) return 0;
    return 1;
}

/* the moment, as finely as the chip can tell: a human touch never lands on the same microsecond twice */
static uint32_t entropy(void)
{
#ifdef DINGHY_HOST
    return fm1_random_word() ^ fm1_ms;
#else
    return fm1_ticks() ^ fm1_ms << 16 ^ fm1_random_word();
#endif
}

/* a new seed every power-on. The online emulator starts the chip's random number generator at the same value each
 * time, so there (only there) a counter kept in flash is mixed in, as LOOPHEADS does. */
/* any scale but CHROMATIC (that is no scale at all) */
static uint8_t random_scale(uint32_t *state)
{
    return (uint8_t)(1u + random_setting(state, SCALE_COUNT - 1u));
}

static uint32_t boot_seed(void)
{
    uint32_t seed = fm1_random_word();
    if (seed != 0 && seed != 0x0bf34dadu) return seed;
    uint32_t second = fm1_random_word();
    if ((seed || second) && (seed != 0x0bf34dadu || second != 0x026e6076u))
        return seed ? seed : second;
    const uint32_t boot_seed_blob = NBLOB - 1u;
    uint32_t saved = 0;
    blob_load(boot_seed_blob, &saved, sizeof saved);
    saved += 0x9e3779b9u;
    blob_save(boot_seed_blob, &saved, sizeof saved);
    return saved ^ seed;
}

/* ----------------------------------------------------------------------------------------------- the loop */
static void app_main(void)
{
    uint32_t prev_keys = 0, sound_random = boot_seed();
    uint32_t held_since[14] = {0};
    uint16_t long_done = 0, was_on_page = 0;
    uint8_t page = P_HOME, drawn_page = 0xFF, key_note[27] = {0};
    uint32_t drawn_state = 0, drawn_rows = 0, drawn_footer = ~0u, title_state = ~0u;
    uint8_t calm_index = CALM_NOTES - 1u, calm_draw = 0, force_whole = 0;
    uint32_t calm_until = 0;
    uint8_t stirred = 0;
#if !BOARD_SMK37
    uint8_t battery_drawn = UINT8_MAX;
#endif

    arp.rnd = sound_random | 1u;
    arp_reroll();
    for (uint32_t t = 0; t < TRACKS; t++) {
        sound_default(t);                       /* the levels and the LFOs' own randomness ... */
        randomize_sound(&pluck_parts[t], &sound_random);   /* ... and a random sound on every track */
        tracks[t].speed = DIV_SIXTEENTH;
        track_clear(t);
    }
    scale_select(random_scale(&sound_random));   /* a random scale at every power-on; the key stays C (0) */
    pluck_set_tuning(0);

    draw_boot_logo();
    uint32_t boot_started = fm1_ms;
    clk.last_ms = fm1_ms;
    for (;;) {
        platform_poll();
        uint32_t now = fm1_ms, rel;
        uint32_t b = fm1_input_edges(&rel);
        /* The first touch (a button or a key) throws the ARP's dice again with the exact moment it came: no two
         * power-ons start with the same melody, even in the emulator, whose random numbers repeat. */
        if (!stirred && (b || fm1_in.notes)) {
            stirred = 1;
            uint32_t moment = entropy();
            arp.rnd ^= moment * 2654435761u | 1u;
            arp_reroll();
            sound_random ^= moment * 40503u;
            chance_rnd ^= moment;
            scale_select(random_scale(&sound_random));   /* (for the emulator: a new random scale too) */
        }
        uint32_t held = fm1_in.buttons;
        uint8_t previous_page = page;

        /* ---- buttons: pages on press; hold actions; taps on release */
        for (uint32_t i = 0; i < 14; i++) {
            uint32_t bit = 1u << i;
            if (b & bit) {
                held_since[i] = now;
                long_done &= (uint16_t)~bit;
                uint8_t to = button_page(i);
                if (to != 0xFF) {
                    if (page == to) was_on_page |= (uint16_t)bit; else was_on_page &= (uint16_t)~bit;
                    page = to;
                }
            }
            if ((held & bit) && !(long_done & bit) && now - held_since[i] >= LONG_MS && !(b & bit)) {
                long_done |= (uint16_t)bit;
                if (i == BTN_ARP) { arp_reroll(); say("NEW DICE THROWN"); }
                if (i == BTN_EDIT) { randomize_sound(&pluck_parts[selected], &sound_random); say("RANDOM SOUND"); }
                if (i == BTN_REC && rec_track >= TRACKS) {
                    note_off(selected);
                    track_clear(selected);
                    say("TRACK CLEARED");
                }
            }
            if ((rel & bit) && !(long_done & bit)) {
                if (i == BTN_ARP && (was_on_page & bit)) {
                    arp.on = !arp.on;
                    if (!arp.on) { note_off(PLAYER_ARP); rec_stop(); }
                }
                if (i == BTN_REC) {
                    if (rec_track < TRACKS) {
                        rec_finish();
                    } else {
                        /* REC with the ARP off turns it back on: the dice start where they were left */
                        arp.on = 1;
                        if (!clk.running) transport_start();
                        rec_start(selected);
                    }
                }
            }
        }
        /* SAVE: a little note, not a save. Anything else touched puts the page back. */
        if (b & (1u << BTN_SAVE)) {
            /* a note at random, never the same one twice in a row */
            sound_random = sound_random * 1664525u + 1013904223u;
            calm_index = (uint8_t)((calm_index + 1u + (sound_random >> 16) % (CALM_NOTES - 1u)) % CALM_NOTES);
            calm_until = now + CALM_MS;
            calm_draw = 1;
        } else if (calm_until && (b || fm1_in.notes)) {
            calm_until = now;
        }
        if (b & (1u << BTN_PLAY)) {
            if (clk.running) transport_stop(); else transport_start();
        }
        if (b & (1u << BTN_OCT_DOWN) && scale_now.octave > -2) scale_now.octave--;
        if (b & (1u << BTN_OCT_UP) && scale_now.octave < 2) scale_now.octave++;

        /* ---- encoders: SELECT the track; ALGORITHM the engine, PRESETS the ping (both open EDIT) */
        int32_t e = fm1_enc_take(ENC_SELECT);
        if (e) {
            uint8_t next = (uint8_t)step_clamp(selected, e, 0, TRACKS - 1);
            if (next != selected) {
                if (rec_track < TRACKS) rec_finish();
                note_off(PLAYER_ARP);
                selected = next;
            }
        }
        e = fm1_enc_take(ENC_ALGORITHM);          /* the chance of the recorded notes playing, all tracks, any page */
        if (e) {
            chance = (uint8_t)step_clamp(chance, e * 5, 0, 100);
            static char text[24];
            char *q = text;
            const char *w = "NOTE CHANCE ";
            while (*w) *q++ = *w++;
            q = put_num(q, chance, 0);
            *q++ = '%';
            *q = 0;
            say(text);
        }
        e = fm1_enc_take(ENC_PRESETS);            /* the ARP's speed (what the track records at): 1/2 .. 1/32 T */
        if (e) {
            uint8_t speed = (uint8_t)step_clamp(arp.speed, e, PRESETS_SLOWEST, DIVS - 1);
            arp_set_speed(speed);
            static char text[24];
            char *q = text;
            const char *w = "ARP SPEED ";
            while (*w) *q++ = *w++;
            w = div_names[speed];
            while (*w) *q++ = *w++;
            *q = 0;
            say(text);
        }
        int32_t steps[4];
        for (uint32_t k = 0; k < 4; k++) steps[k] = fm1_enc_take(ENC_KNOB1 + k);
        if (steps[0] || steps[1] || steps[2] || steps[3]) {
            if (calm_until) calm_until = now;
            if (held & (1u << BTN_HOME)) {              /* HOME held: knobs 1-4 pan tracks 1-4 */
                static char text[24];
                for (uint32_t t = 0; t < TRACKS; t++) {
                    if (!steps[t]) continue;
                    pluck_parts[t].pan = (int8_t)step_clamp(pluck_parts[t].pan, steps[t] * 5, -PLUCK_PAN_MAX, PLUCK_PAN_MAX);
                    char *q = text;
                    const char *w = "TRACK 1 PAN ";
                    while (*w) *q++ = *w++;
                    text[6] = (char)('1' + t);
                    fmt_pan(q, pluck_parts[t].pan);
                    say(text);
                }
            } else {
                edit_page(page, steps);
            }
        }

        /* ---- the keys: what they do depends on the page */
        uint32_t keys = fm1_in.notes, down = keys & ~prev_keys, up = prev_keys & ~keys;
        prev_keys = keys;
        for (uint32_t k = 0; k < 27u; k++) {
            uint32_t bit = 1u << k;
            if (down & bit) {
                if (page == P_ARP) {
                    if (white_index[k] >= 0) arp.pool ^= (uint16_t)(1u << white_index[k]);   /* (black keys: nothing) */
                    key_note[k] = 0;
                } else if (page == P_SEQ) {
                    int8_t w = white_index[k];
                    if (w >= 0 && w < 4) {                  /* white keys 1-4: mute tracks 1-4 */
                        tracks[w].muted = !tracks[w].muted;
                        if (tracks[w].muted) note_off((uint32_t)w);
                    } else if (w >= 4 && w < 8) {           /* white keys 5-8: select tracks 1-4 */
                        if (rec_track < TRACKS) rec_finish();
                        selected = (uint8_t)(w - 4);
                    }
                    key_note[k] = 0;
                } else if (page == P_SCALE) {
                    scale_now.transpose = (int8_t)((int32_t)k - 7 + 12 * scale_now.octave);   /* C4 (the 8th key) = none; OCT: +- 12 */
                    key_note[k] = 0;
                } else {
                    key_note[k] = (uint8_t)(KEY0 + k);
                    live_on((uint8_t)(OWNER_KEYS + k), key_note[k], NOTE_VELOCITY);
                    midi_send(0x90u | midi_out_channel(selected), key_note[k], NOTE_VELOCITY);
                }
            }
            if ((up & bit) && key_note[k]) {
                live_off((uint8_t)(OWNER_KEYS + k));
                midi_send(0x80u | midi_out_channel(selected), key_note[k], 0);
                key_note[k] = 0;
            }
        }

        midi_in();
        clock_service();
        update_fx();
        if (message && now - message_at > 1500u) message = 0;

        /* ---- LEDs */
        uint32_t lit = 0;
        if (page == P_ARP) {
            for (uint32_t k = 0; k < 27u; k++)
                if (white_index[k] >= 0 && (arp.pool >> white_index[k] & 1u))
                    lit |= 1u << k;
        } else if (page == P_SEQ) {
            for (uint32_t k = 0; k < 27u; k++) {
                int8_t w = white_index[k];
                if ((w >= 0 && w < 4 && !tracks[w].muted) || (w >= 4 && w < 8 && w - 4 == selected)) lit |= 1u << k;
            }
        } else if (page == P_SCALE) {
            int32_t k = scale_now.transpose + 7 - 12 * scale_now.octave;
            if (k >= 0 && k < 27) lit = 1u << k;
        } else {
            lit = keys;
        }
        for (uint32_t k = 0; k < 27u; k++) fm1_led_key(KEY_LED(k), lit >> k & 1u);
        fm1_led_key(BTN_OCT_DOWN, scale_now.octave < 0);
        fm1_led_key(BTN_OCT_UP, scale_now.octave > 0);
        fm1_led_key(BTN_PLAY, clk.running && (clk.tick / 48u) % 2u == 0);
        fm1_led_key(BTN_REC, rec_track < TRACKS && (now / 200u) % 2u == 0);
        fm1_led_key(BTN_ARP, arp.on);
        fm1_led_key(BTN_SEQ, page == P_SEQ);
        fm1_led_key(BTN_HOME, page == P_HOME);
        fm1_led_key(BTN_SEL, page == P_SCALE);
        fm1_led_key(BTN_EDIT, page == P_EDIT);
        fm1_led_key(BTN_ENV, page == P_ENV);
        fm1_led_key(BTN_LFO, page == P_LFO);
        fm1_led_key(BTN_FX, page == P_FX || fx.delay || fx.hall || fx.chorus);
        fm1_led_key(BTN_GLO, page == P_GLO);
        fm1_led_key(BTN_SAVE, calm_until && now < calm_until);

        /* ---- the screen */
        if (drawn_page == 0xFF) {
            if (now - boot_started < BOOT_LOGO_MS && !b && !keys) continue;
        }
        idle_ok = 1;
        if (calm_until && now >= calm_until) {    /* the note is over: the page comes back */
            calm_until = 0;
            force_whole = 1;
        }
        uint8_t whole = page != drawn_page || force_whole;
        force_whole = 0;
        if (whole) {
            lcd_fill(0, 0, 240, 240, C_BG);
            ui_divider(40, C_RULE);
            ui_divider(211, C_RULE);
            drawn_page = page;
            drawn_footer = ~0u;
            title_state = ~0u;
#if !BOARD_SMK37
            battery_drawn = UINT8_MAX - 1u;
#endif
        }
        (void)previous_page;
        uint32_t ts = (uint32_t)selected | (uint32_t)arp.on << 3 | (uint32_t)page << 4 | (uint32_t)chance << 8;
        if (ts != title_state) {
            char tb[24];
            ui_title(page_title(page, tb));
            title_state = ts;
        }
        if (calm_until) {
            if (calm_draw || whole) {
                lcd_fill(0, 44, 240, 166, C_BG);
                cv_begin(BOX_W, 160, C_BG);
                cv_box(0, 0, (int32_t)BOX_W, 160, C_ACC);
                uint8_t heading = 1;                /* the lines before the first gap: the accent colour */
                for (uint32_t line = 0; line < 7; line++) {
                    const char *t = calm_notes[calm_index][line];
                    if (!t || !*t) { heading = 0; continue; }
                    cv_text(((int32_t)BOX_W - text_w(&UI_BODY, t)) / 2, 14 + (int32_t)line * 19, &UI_BODY, t,
                            heading ? C_ACC : C_KEY, C_BG);
                }
                cv_blit(BOX_X, 46);
                calm_draw = 0;
            }
            drawn_state = ~page_state(page);    /* the page redraws itself when the note is gone */
        } else {
        uint32_t ps = page_state(page), rs = row_state(page);
        if (whole || ps != drawn_state || rs != drawn_rows) {
            draw_page(page, whole || rs != drawn_rows);
            drawn_state = ps;
            drawn_rows = rs;
        }
        }
        uint32_t fs = footer_state(page);
        if (fs != drawn_footer) {
            draw_footer(page);
            drawn_footer = fs;
        }
#if !BOARD_SMK37
        uint8_t battery_level = read_battery_level(now);
        if (battery_drawn != battery_level) {
            draw_battery(battery_level);
            battery_drawn = battery_level;
        }
#endif
        idle_ok = 0;

    }
}
