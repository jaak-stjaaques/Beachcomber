/* SPDX-License-Identifier: GPL-3.0-only */
/* BEACHCOMBER's scale stage: the last step before the sound engines.
 * The ARP and the SEQ only know "steps" (0 = the root, 1 = the next note of the scale, ...). Here a step becomes
 * a real note: the scale and the transposition (the keys on the SCALE page) are applied,
 * so a change of scale or key moves everything at once, recordings included. OCT- / OCT+ is not applied here:
 * it moves only the ARP and the keys (dinghy.c). */
static const struct { const char *name; uint16_t steps; } scales[] = {
    {"CHROMATIC", 0xfffu},
    {"MAJOR", 0xab5u},
    {"MINOR", 0x5adu},
    {"MAJOR PENT", 0x295u},
    {"MINOR PENT", 0x4a9u},
    {"BLUES", 0x4e9u},
    {"MAJOR BLUES", 0x29du},
    {"HARMONIC MINOR", 0x9adu},
    {"MELODIC MINOR", 0xaadu},
    {"DORIAN", 0x6adu},
    {"PHRYGIAN", 0x5abu},
    {"LYDIAN", 0xad5u},
    {"MIXOLYDIAN", 0x6b5u},
    {"LOCRIAN", 0x56bu},
    {"LYDIAN DOMINANT", 0x6d5u},
    {"BEBOP DOMINANT", 0xeb5u},
    {"BEBOP MAJOR", 0xbb5u},
    {"ALTERED", 0x55bu},
    {"WHOLE TONE", 0x555u},
    {"DIM HALF-WHOLE", 0x6dbu},
    {"DIM WHOLE-HALF", 0xb6du},
    {"AUGMENTED", 0x999u},
    {"HIRAJOSHI", 0x18du},
    {"IN SEN", 0x4a3u},
    {"IWATO", 0x463u},
    {"KUMOI", 0x28du},
    {"YO", 0x2a5u},
    {"PELOG", 0x18bu},
    {"EGYPTIAN", 0x4a5u},
    {"HIJAZ", 0x5b3u},
    {"DOUBLE HARMONIC", 0x9b3u},
    {"HUNGARIAN MINOR", 0x9cdu},
    {"HUNGARIAN MAJOR", 0x6d9u},
    {"UKRAINIAN DORIAN", 0x6cdu},
    {"PERSIAN", 0x973u},
    {"ORIENTAL", 0x673u},
    {"NEAPOLITAN MINOR", 0x9abu},
    {"NEAPOLITAN MAJOR", 0xaabu},
    {"ENIGMATIC", 0xd53u},
    {"PROMETHEUS", 0x655u},
    {"SPANISH 8-TONE", 0x57bu},
    {"RAGA TODI", 0x9cbu},
    {"RAGA MARWA", 0xad3u},

};
#define SCALE_COUNT (sizeof scales / sizeof scales[0])
#define SCALE_BASE 48                           /* step 0 with no transposition: C3 */

static struct {
    uint8_t scale;                              /* index into scales[] */
    int8_t transpose;                           /* semitones, -31 .. +43: the SCALE page's keys (with OCT) */
    int8_t octave;                              /* -2 .. +2: OCT- / OCT+ (the ARP, the keys; not the tracks) */
    uint8_t size;                               /* notes in the scale (5 .. 12) */
    uint8_t offset[12];                         /* semitones of each step above the root */
} scale_now = {1, 0, 0, 7, {0, 2, 4, 5, 7, 9, 11}};

static void scale_select(uint8_t scale)
{
    if (scale >= SCALE_COUNT) scale = 0;
    scale_now.scale = scale;
    uint8_t n = 0;
    for (uint8_t i = 0; i < 12; i++)
        if (scales[scale].steps >> i & 1u) scale_now.offset[n++] = i;
    scale_now.size = n;
}

static uint8_t scale_root_pc(void)              /* the key the scale stands on, 0 = C .. 11 = B */
{
    return (uint8_t)((SCALE_BASE + scale_now.transpose + 120) % 12);
}

/* a step (any integer, negative too) -> a MIDI note, within 12..120 */
static uint8_t scale_step_note(int32_t step)
{
    int32_t n = scale_now.size, octave = 0;
    while (step < 0) { step += n; octave--; }
    octave += step / n;
    int32_t note = SCALE_BASE + scale_now.transpose + 12 * octave + scale_now.offset[step % n];
    while (note < 12) note += 12;
    while (note > 120) note -= 12;
    return (uint8_t)note;
}

static int scale_contains(uint8_t pitch)
{
    return scales[scale_now.scale].steps >> ((pitch + 12u - scale_root_pc()) % 12u) & 1u;
}

/* a played note (the keys, MIDI in) -> the nearest note of the scale, the lower one at equal distance */
static uint8_t scale_snap(uint8_t pitch)
{
    if (scale_contains(pitch)) return pitch;
    for (uint8_t d = 1; d < 12; d++) {
        if (pitch >= d && scale_contains((uint8_t)(pitch - d))) return (uint8_t)(pitch - d);
        if (pitch + d <= 127 && scale_contains((uint8_t)(pitch + d))) return (uint8_t)(pitch + d);
    }
    return pitch;
}
