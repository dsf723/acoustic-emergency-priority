/* Behavioral checks through public sc_process: no metric distance involved. */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "sound_class.h"

static doa_result_t r;
static snd_state_t out;
static unsigned blk;

static void reset(void)
{
    memset(&r, 0, sizeof(r));
    sc_init();
    blk = 0;
    r.valid = 1;
    r.energy_db = -20.0f;
    r.noise_db = -60.0f;
    r.peak_ratio = 4.0f;
}

static void feed(float coh, int n)
{
    r.coherence = coh;
    while (n--) sc_process(&r, blk++, &out);
}

int main(void)
{
    int i;
    reset(); feed(0.75f, 4); assert(out.band == BAND_NONE);
    feed(0.75f, 8); assert(out.band == BAND_NEAR);
    /* Changing amplitude alone must not change the qualitative result. */
    r.energy_db = -40.0f; feed(0.75f, 8); assert(out.band == BAND_NEAR);
    r.valid = 0; feed(0.75f, 1); assert(out.band == BAND_NONE);
    reset(); feed(0.42f, 8); assert(out.band == BAND_MID);
    reset(); feed(0.15f, 8); assert(out.band == BAND_FAR);
    reset(); feed(0.55f, 8); assert(out.band == BAND_NONE);
    reset(); feed(0.30f, 8); assert(out.band == BAND_NONE);
    reset();
    for (i = 0; i < 16; i++) feed(i % 2 ? 0.15f : 0.75f, 1);
    assert(out.band == BAND_NONE);
    reset(); r.noise_db = -22.0f; feed(0.75f, 8); assert(out.band == BAND_NONE);
    reset(); feed(NAN, 8); assert(out.band == BAND_NONE);
    reset(); feed(1.5f, 8); assert(out.band == BAND_NONE);
    reset(); feed(0.75f, 8); sc_init(); feed(0.75f, 1); assert(out.band == BAND_NONE);
    assert(strcmp(sc_band_name(BAND_NONE), "UNKNOWN") == 0);
    puts("RANGE CHECKS PASSED: near/mid/far, warmup, silence, boundaries, instability, low SNR, invalid input, reset");
    return 0;
}
