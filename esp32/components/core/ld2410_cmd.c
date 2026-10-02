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

/* Frame examples cited below: "HLK-LD2410C serial communication protocol" V1.00
 * (hardware/datasheets/Protocolo_comunicacion_serial_LD2410C.pdf). */

static void put_u16(uint8_t *p, unsigned v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
}

/* One "u16 parameter word + u32 value" triple (value < 65536 here). */
static void put_triple(uint8_t *p, unsigned word, unsigned value)
{
    put_u16(p, word);
    put_u16(p + 2, value);
    p[4] = 0;
    p[5] = 0;
}

/* 2.2.3, p.10: 14 00 60 00 | 00 00 mm 00 00 00 | 01 00 ss 00 00 00 | 02 00 dd dd 00 00 */
int ld_cmd_encode_set_gates(int max_move, int max_still, int duration_s, uint8_t *out, size_t cap)
{
    if (max_move < LD_GATE_MIN_MAX || max_move > LD_GATE_MAX ||
        max_still < LD_GATE_MIN_MAX || max_still > LD_GATE_MAX ||
        duration_s < 0 || duration_s > 65535)
        return -EINVAL;
    uint8_t v[18];
    put_triple(v, 0x0000, (unsigned)max_move);
    put_triple(v + 6, 0x0001, (unsigned)max_still);
    put_triple(v + 12, 0x0002, (unsigned)duration_s);
    return ld_cmd_encode(LD_CMD_SET_GATES, v, sizeof v, out, cap);
}

/* 2.2.4, p.11 */
int ld_cmd_encode_read_params(uint8_t *out, size_t cap)
{
    return ld_cmd_encode(LD_CMD_READ_PARAMS, NULL, 0, out, cap);
}

/* 2.2.7, p.12-13: 00 00 gg gg 00 00 | 01 00 mm 00 00 00 | 02 00 ss 00 00 00 */
int ld_cmd_encode_set_sens(int gate, int move, int still, uint8_t *out, size_t cap)
{
    if (!((gate >= 0 && gate <= LD_GATE_MAX) || gate == (int)LD_GATE_ALL) ||
        move < 0 || move > LD_SENS_MAX || still < 0 || still > LD_SENS_MAX)
        return -EINVAL;
    uint8_t v[18];
    put_triple(v, 0x0000, (unsigned)gate);
    put_triple(v + 6, 0x0001, (unsigned)move);
    put_triple(v + 12, 0x0002, (unsigned)still);
    return ld_cmd_encode(LD_CMD_SET_SENS, v, sizeof v, out, cap);
}

/* 2.2.8, p.13 */
int ld_cmd_encode_read_version(uint8_t *out, size_t cap)
{
    return ld_cmd_encode(LD_CMD_READ_VERSION, NULL, 0, out, cap);
}

/* 2.2.10, p.14 */
int ld_cmd_encode_factory_reset(uint8_t *out, size_t cap)
{
    return ld_cmd_encode(LD_CMD_FACTORY_RESET, NULL, 0, out, cap);
}

/* 2.2.11, p.15 */
int ld_cmd_encode_restart(uint8_t *out, size_t cap)
{
    return ld_cmd_encode(LD_CMD_RESTART, NULL, 0, out, cap);
}

/* 2.2.12, p.16: value 0x0000 = off (the document's "on" is inconsistent: text 0x0100,
 * example bytes 01 00). Only "off" is implemented. */
int ld_cmd_encode_bluetooth_off(uint8_t *out, size_t cap)
{
    static const uint8_t v[2] = {0x00, 0x00};
    return ld_cmd_encode(LD_CMD_BLUETOOTH, v, sizeof v, out, cap);
}

/* Layout, 2.2.4 p.11 (ACK payload, 28 bytes for N = 8):
 *  0-1 61 01 | 2-3 status | 4 AA | 5 N | 6 max move gate | 7 max still gate |
 *  8..8+N motion sens (N+1) | then still sens (N+1) | duration u16 LE.
 * N other than 8 is rejected (-EBADMSG): the settings model has fixed nine gates. */
#define PARAMS_N 8u
#define PARAMS_LEN (2u + 2u + 1u + 1u + 1u + 1u + 2u * (PARAMS_N + 1u) + 2u)

int ld_cmd_decode_params(const uint8_t *payload, size_t len, ld_params_t *out)
{
    if (!payload || !out)
        return -EINVAL;
    if (len < 4)
        return -EMSGSIZE;
    if (payload[0] != (LD_CMD_READ_PARAMS & 0xFF) || payload[1] != 0x01)
        return -EBADMSG;
    if (payload[2] != 0 || payload[3] != 0)
        return -EBADMSG;
    if (len < 6)
        return -EMSGSIZE;
    if (payload[4] != 0xAA || payload[5] != PARAMS_N)
        return -EBADMSG;
    if (len != PARAMS_LEN)
        return -EMSGSIZE;
    ld_params_t p;
    p.max_gate_n = payload[5];
    p.max_move_gate = payload[6];
    p.max_still_gate = payload[7];
    memcpy(p.move_sens, payload + 8, LD_GATE_COUNT);
    memcpy(p.still_sens, payload + 8 + LD_GATE_COUNT, LD_GATE_COUNT);
    const uint8_t *d = payload + 8 + 2 * LD_GATE_COUNT;
    p.duration_s = (uint16_t)(d[0] | (d[1] << 8));
    *out = p;
    return 0;
}

/* Layout, 2.2.8 p.13 (12 bytes): A0 01 | status | type u16 | major u16 | minor u32, all LE. */
int ld_cmd_decode_version(const uint8_t *payload, size_t len, ld_version_t *out)
{
    if (!payload || !out)
        return -EINVAL;
    if (len < 4)
        return -EMSGSIZE;
    if (payload[0] != (LD_CMD_READ_VERSION & 0xFF) || payload[1] != 0x01)
        return -EBADMSG;
    if (payload[2] != 0 || payload[3] != 0)
        return -EBADMSG;
    if (len != 12)
        return -EMSGSIZE;
    out->type = (uint16_t)(payload[4] | (payload[5] << 8));
    out->major = (uint16_t)(payload[6] | (payload[7] << 8));
    out->minor = (uint32_t)payload[8] | ((uint32_t)payload[9] << 8) |
                 ((uint32_t)payload[10] << 16) | ((uint32_t)payload[11] << 24);
    return 0;
}
