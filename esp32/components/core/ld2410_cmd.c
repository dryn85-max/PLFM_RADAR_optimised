#include "ld2410_cmd.h"
#include <errno.h>
#include <string.h>

int ld_cmd_encode(uint16_t cmd, const uint8_t *value, size_t value_len,
                  uint8_t *out, size_t cap)
{
    if (!out || (value_len > 0 && !value) || value_len > LD_CMD_MAX_VALUE)
        return -EINVAL;
    size_t total = 4 + 2 + 2 + value_len + 4;
    if (cap < total)
        return -ENOSPC;
    static const uint8_t hdr[4] = {0xFD, 0xFC, 0xFB, 0xFA};
    static const uint8_t ftr[4] = {0x04, 0x03, 0x02, 0x01};
    size_t len_field = 2 + value_len;
    memcpy(out, hdr, 4);
    out[4] = (uint8_t)(len_field & 0xFF);
    out[5] = (uint8_t)(len_field >> 8);
    out[6] = (uint8_t)(cmd & 0xFF);
    out[7] = (uint8_t)(cmd >> 8);
    if (value_len)
        memcpy(out + 8, value, value_len);
    memcpy(out + 8 + value_len, ftr, 4);
    return (int)total;
}

int ld_cmd_encode_enable_config(uint8_t *out, size_t cap)
{
    static const uint8_t v[2] = {0x01, 0x00};
    return ld_cmd_encode(LD_CMD_ENABLE_CONFIG, v, sizeof v, out, cap);
}

int ld_cmd_encode_enable_engineering(uint8_t *out, size_t cap)
{
    return ld_cmd_encode(LD_CMD_ENABLE_ENGINEERING, NULL, 0, out, cap);
}

int ld_cmd_encode_end_config(uint8_t *out, size_t cap)
{
    return ld_cmd_encode(LD_CMD_END_CONFIG, NULL, 0, out, cap);
}

int ld_cmd_decode_ack(const uint8_t *payload, size_t len, ld_ack_t *out)
{
    if (!payload || !out)
        return -EINVAL;
    if (len < 4)
        return -EMSGSIZE;
    uint16_t word = (uint16_t)(payload[0] | (payload[1] << 8));
    if (!(word & LD_CMD_ACK_FLAG))
        return -EBADMSG;
    out->cmd = (uint16_t)(word & ~LD_CMD_ACK_FLAG);
    out->status = (uint16_t)(payload[2] | (payload[3] << 8));
    return 0;
}
