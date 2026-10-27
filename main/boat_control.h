#ifndef BOAT_CONTROL_H
#define BOAT_CONTROL_H

#include <stdbool.h>
#include <stdint.h>

#include "boat_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Map held direction keys to thruster outputs.
 *
 * Priority: BACK > LEFT+RIGHT > LEFT > RIGHT > FORWARD > idle.
 * LEFT/RIGHT may be combined with FORWARD (turn while moving). */
static inline void boat_compute_command(bool forward, bool back,
                                        bool left, bool right,
                                        uint8_t* out_left, uint8_t* out_right,
                                        bool* out_brake)
{
    uint8_t left_throttle = 0;
    uint8_t right_throttle = 0;
    bool brake = false;

    if (back || (left && right)) {
        brake = true;
    } else if (left) {
        right_throttle = BOAT_THROTTLE_MAX;
    } else if (right) {
        left_throttle = BOAT_THROTTLE_MAX;
    } else if (forward) {
        left_throttle = BOAT_THROTTLE_MAX;
        right_throttle = BOAT_THROTTLE_MAX;
    }

    if (out_left != NULL) {
        *out_left = left_throttle;
    }
    if (out_right != NULL) {
        *out_right = right_throttle;
    }
    if (out_brake != NULL) {
        *out_brake = brake;
    }
}

#ifdef __cplusplus
}
#endif

#endif /* BOAT_CONTROL_H */
