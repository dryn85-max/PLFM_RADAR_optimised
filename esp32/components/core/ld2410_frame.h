/* Decode the payload of an LD2410C data frame (plain C11).
 * Offsets are relative to the payload (spec R3):
 *   0 type (1 engineering, 2 normal)  1 0xAA  2 target state
 *   3..4 moving dist cm  5 moving energy  6..7 still dist cm  8 still energy
 *   9..10 detection dist cm
 *   engineering: 11 max moving gate  12 max still gate
 *                13..21 moving energy gates 0..8  22..30 still gates 0..8
 *                then module-specific bytes (skipped)
 *   last two bytes 0x55 0x00. All u16 little-endian.
 * VERIFY against the Hi-Link manual / a real capture. */
#ifndef LD2410_FRAME_H
#define LD2410_FRAME_H

#include <stddef.h>
#include <stdint.h>

#define LD_GATES 9u

typedef struct {
    uint8_t data_type;       /* 1 engineering, 2 normal */
    uint8_t engineering;     /* 1 if gate fields are valid */
    uint8_t target_state;    /* 0 none, 1 moving, 2 still, 3 both */
    uint16_t moving_dist_cm;
    uint8_t moving_energy;
    uint16_t still_dist_cm;
    uint8_t still_energy;
    uint16_t detect_dist_cm;
    uint8_t max_moving_gate;                  /* engineering only, else 0 */
    uint8_t max_still_gate;                   /* engineering only, else 0 */
    uint8_t moving_gate_energy[LD_GATES];     /* engineering only, else 0 */
    uint8_t still_gate_energy[LD_GATES];      /* engineering only, else 0 */
} ld_data_t;

/* Returns 0, or a negative errno: -EINVAL (null), -EMSGSIZE (too short for
 * its type), -EBADMSG (bad type, marker, target state or tail). `out` is
 * zeroed on error. */
int ld_frame_decode(const uint8_t *payload, size_t len, ld_data_t *out);

#endif
