/* Public-API behavior of target-only classifier and output policy. */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "sound_class.h"
#include "target_output.h"
#include "doa_config.h"

static doa_result_t r;
static snd_state_t sc;
static target_output_t out;
static unsigned blk;

static void reset(void)
{
    memset(&r, 0, sizeof(r));
    sc_init(); target_output_reset(); blk = 0;
    r.valid = 1; r.energy_db = -20; r.noise_db = -60;
    r.peak_ratio = 4; r.coherence = 0.7f; r.azimuth_deg = 359;
}

static void step(int tonal)
{
    r.tonality_db = tonal ? 25 : 0;
    r.peak_hz = blk % 20 < 10 ? 700 : 1200;
    sc_process(&r, blk, &sc);
    target_output_update(&r, &sc, blk++, &out);
}

int main(void)
{
    int i;
    reset();
    for (i = 0; i < 80; i++) {
        r.flatness = 0.5f; r.centroid_hz = 2000;
        r.sub_e[i % DOA_NSUB] = i % 10 == 0 ? 1 : 0;
        step(0);
        assert(sc.cls == SND_NONE && !sc.event && !sc.dir_ok);
        assert(!out.active && !out.direction_valid);
    }
    reset();
    for (i = 0; i < 47 + SC_SIREN_CONFIRM_BLOCKS - 1; i++) { step(1); assert(!out.active); }
    step(1);
    assert(sc.event && out.active && !out.direction_valid);
    step(1); step(1);
    assert(out.active && out.direction_valid);
    /* Wraparound observations remain in the same direction cluster. */
    for (i = 0; i < 8; i++) { r.azimuth_deg = i % 2 ? 359 : 1; step(1); }
    assert(out.direction_valid && (out.azimuth_deg < 5 || out.azimuth_deg > 355));
    /* Class persists, but no fresh directional evidence => UNKNOWN, not an old pointer. */
    r.peak_ratio = 1;
    for (i = 0; i < TARGET_DIR_FRESH_BLOCKS + 3; i++) step(1);
    assert(out.active && !out.direction_valid && out.band == BAND_NONE);
    /* A missing/invalid direction must not manufacture support. */
    r.peak_ratio = 4; r.azimuth_deg = NAN;
    for (i = 0; i < TARGET_DIR_FRESH_BLOCKS + 3; i++) step(1);
    assert(out.active && !out.direction_valid);
    for (i = 0; i < 60; i++) step(0);
    assert(!out.active && !out.direction_valid && sc.cls == SND_NONE);
    /* New event starts from empty direction history. */
    r.azimuth_deg = 90;
    for (i = 0; i < 60 + SC_SIREN_CONFIRM_BLOCKS; i++) step(1);
    assert(out.active && out.direction_valid && fabsf(out.azimuth_deg - 90) < 2);
    /* Following a siren with an indefinitely steady tone must eventually clear. */
    for (i = 0; i < 220; i++) {
        r.tonality_db = 25; r.peak_hz = 1000;
        sc_process(&r, blk, &sc);
        target_output_update(&r, &sc, blk++, &out);
    }
    assert(!out.active && sc.cls == SND_NONE);
    sc_init(); target_output_reset();
    step(1);
    assert(!out.active && !out.direction_valid);
    puts("TARGET MODE PASSED: background/clap suppression, confirmation, separate direction validity, expiry, reset, new event");
    return 0;
}
