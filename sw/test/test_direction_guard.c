#include <assert.h>
#include <math.h>
#include <stdio.h>
#include "dir_filter.h"

static float diff(float a, float b)
{
    return fabsf(fmodf(a - b + 540.0f, 360.0f) - 180.0f);
}

int main(void)
{
    int b, k;
    float out;
    dir_filter_reset();
    dir_filter_set_guard(1);
    dir_filter_add(0, 9, 1);
    assert(diff(dir_filter_get(1), 0) < 1);
    dir_filter_add(220, 10000, 2);
    assert(diff(dir_filter_get(2), 0) < 1); /* one strong reflection */
    for (k = 0; k < 10; k++) dir_filter_add(220, 25, 3);
    assert(diff(dir_filter_get(3), 0) < 1); /* repeated calls aren't time evidence */
    assert(diff(dir_filter_get(3), 0) < 1);
    dir_filter_reset(); dir_filter_set_guard(1);
    for (b = 1; b <= 16; b++) {
        dir_filter_add(b % 2 ? 359 : 1, 9, b);
        out = dir_filter_get(b);
    }
    assert(diff(out, 0) < 2); /* zero-degree wraparound */
    for (b = 17; b <= 40; b++) {
        dir_filter_add(220, 9, b);
        out = dir_filter_get(b);
        if (b < 25) assert(diff(out, 0) < 2);
    }
    assert(diff(out, 220) < 1); /* sustained real change eventually accepted */
    assert(dir_filter_get(100) < 0); /* silence expires direction */
    dir_filter_add(90, 9, 101); assert(diff(dir_filter_get(101), 90) < 1);
    dir_filter_reset();
    dir_filter_add(0, 1, 1); dir_filter_get(1);
    dir_filter_add(220, 25, 2);
    assert(diff(dir_filter_get(2), 220) < 1); /* guard off: prior response */
    puts("DIRECTION GUARD PASSED: isolated/duplicate outliers, wraparound, genuine movement, expiry, guard off");
    return 0;
}
