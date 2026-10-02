#include "ld2410_frame.h"
#include <errno.h>
#include <string.h>

#define TYPE_ENGINEERING 0x01
#define TYPE_NORMAL 0x02
#define MARKER 0xAA
#define BASIC_LEN 11u      /* type .. detection distance */
#define ENG_FIXED_LEN (BASIC_LEN + 2u + 2u * LD_GATES)
#define TAIL_LEN 2u

static uint16_t le16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

int ld_frame_decode(const uint8_t *p, size_t len, ld_data_t *out)
{
    if (!p || !out)
        return -EINVAL;
    memset(out, 0, sizeof *out);
    if (len < BASIC_LEN + TAIL_LEN)
        return -EMSGSIZE;
    uint8_t type = p[0];
    if (type != TYPE_NORMAL && type != TYPE_ENGINEERING)
        return -EBADMSG;
    if (type == TYPE_ENGINEERING && len < ENG_FIXED_LEN + TAIL_LEN)
        return -EMSGSIZE;
    if (p[1] != MARKER || p[len - 2] != 0x55 || p[len - 1] != 0x00 || p[2] > 3)
        return -EBADMSG;

    out->data_type = type;
    out->target_state = p[2];
    out->moving_dist_cm = le16(p + 3);
    out->moving_energy = p[5];
    out->still_dist_cm = le16(p + 6);
    out->still_energy = p[8];
    out->detect_dist_cm = le16(p + 9);
    if (type == TYPE_ENGINEERING) {
        out->engineering = 1;
        out->max_moving_gate = p[11];
        out->max_still_gate = p[12];
        memcpy(out->moving_gate_energy, p + 13, LD_GATES);
        memcpy(out->still_gate_energy, p + 13 + LD_GATES, LD_GATES);
    }
    return 0;
}
