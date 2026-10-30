#ifndef TARGET_OUTPUT_H
#define TARGET_OUTPUT_H
#include <stdint.h>
#include "sound_class.h"

/* Display policy is separate from classification and direction confidence. */
typedef struct {
    int active;
    int direction_valid;
    float azimuth_deg;
    int band;
} target_output_t;

void target_output_reset(void);
void target_output_update(const doa_result_t *r, const snd_state_t *s,
                          uint32_t blk, target_output_t *out);
#endif
