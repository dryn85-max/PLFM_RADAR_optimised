/* LD2410C command frame encoder and ACK decoder (plain C11).
 *   command: FD FC FB FA | len u16 LE (= 2 + value len) | cmd u16 LE | value | 04 03 02 01
 *   ACK payload: (cmd | 0x0100) u16 LE | status u16 LE (0 = OK) | optional extra */
#ifndef LD2410_CMD_H
#define LD2410_CMD_H

#include <stddef.h>
#include <stdint.h>

#define LD_CMD_END_CONFIG 0x00FEu
#define LD_CMD_ENABLE_CONFIG 0x00FFu
#define LD_CMD_ENABLE_ENGINEERING 0x0062u
#define LD_CMD_SET_GATES 0x0060u    /* max gates + no-one duration, 2.2.3 p.10 */
#define LD_CMD_READ_PARAMS 0x0061u  /* 2.2.4 p.11 */
#define LD_CMD_SET_SENS 0x0064u     /* gate sensitivity, 2.2.7 p.12-13 */
#define LD_CMD_READ_VERSION 0x00A0u /* 2.2.8 p.13 */
#define LD_CMD_FACTORY_RESET 0x00A2u /* 2.2.10 p.14 */
#define LD_CMD_RESTART 0x00A3u      /* 2.2.11 p.15 */
#define LD_CMD_BLUETOOTH 0x00A4u    /* 2.2.12 p.16 */
#define LD_CMD_ACK_FLAG 0x0100u

#define LD_GATE_ALL 0xFFFFu         /* 0x0064 "all gates" (2.2.7 p.12) */
#define LD_GATE_COUNT 9u            /* gates 0..8 */
#define LD_GATE_MIN_MAX 2           /* smallest settable max gate (2.2.3 p.10) */
#define LD_GATE_MAX 8
#define LD_SENS_MAX 100

/* Largest command value: 0x0060 / 0x0064 carry three (u16 word + u32 value) = 18 bytes. */
#define LD_CMD_MAX_VALUE 18u
#define LD_CMD_MAX_FRAME (4u + 2u + 2u + LD_CMD_MAX_VALUE + 4u)

typedef struct {
    uint16_t cmd;    /* command word with the ACK flag removed */
    uint16_t status; /* 0 = OK */
} ld_ack_t;

/* Returns frame length written, or -EINVAL (null / value too long),
 * -ENOSPC (cap too small). */
int ld_cmd_encode(uint16_t cmd, const uint8_t *value, size_t value_len,
                  uint8_t *out, size_t cap);
int ld_cmd_encode_enable_config(uint8_t *out, size_t cap);      /* 0x00FF, value 0x0001 */
int ld_cmd_encode_enable_engineering(uint8_t *out, size_t cap); /* 0x0062 */
int ld_cmd_encode_end_config(uint8_t *out, size_t cap);         /* 0x00FE */

/* Decode the payload of a command-kind frame as an ACK. Returns 0, or
 * -EINVAL (null), -EMSGSIZE (< 4 bytes), -EBADMSG (ACK flag not set).
 * A non-zero `status` is not an error here; the caller checks it. */
int ld_cmd_decode_ack(const uint8_t *payload, size_t len, ld_ack_t *out);


/* Encoders for the commands below return the frame length, or -EINVAL (null out /
 * argument out of range), -ENOSPC (cap too small). Frame layouts are the examples of
 * the "HLK-LD2410C serial communication protocol" V1.00, section/page in the .c file. */
/* 0x0060: max motion gate and max still gate 2..8, no-one duration 0..65535 s. */
int ld_cmd_encode_set_gates(int max_move, int max_still, int duration_s, uint8_t *out, size_t cap);
int ld_cmd_encode_read_params(uint8_t *out, size_t cap);   /* 0x0061 */
/* 0x0064: gate 0..8 or LD_GATE_ALL; move, still 0..100. */
int ld_cmd_encode_set_sens(int gate, int move, int still, uint8_t *out, size_t cap);
int ld_cmd_encode_read_version(uint8_t *out, size_t cap);  /* 0x00A0 */
int ld_cmd_encode_factory_reset(uint8_t *out, size_t cap); /* 0x00A2 */
int ld_cmd_encode_restart(uint8_t *out, size_t cap);       /* 0x00A3 */
int ld_cmd_encode_bluetooth_off(uint8_t *out, size_t cap); /* 0x00A4, value 0x0000 */

/* Read-parameters ACK (2.2.4, p.11). Only N == 8 (nine gates) is accepted. */
typedef struct {
    uint8_t max_gate_n;    /* always 8 */
    uint8_t max_move_gate;
    uint8_t max_still_gate;
    uint8_t move_sens[LD_GATE_COUNT];
    uint8_t still_sens[LD_GATE_COUNT];
    uint16_t duration_s;
} ld_params_t;

/* Firmware-version ACK (2.2.8, p.13). */
typedef struct {
    uint16_t type;
    uint16_t major;
    uint32_t minor;
} ld_version_t;

/* Decode the ACK payload (as for ld_cmd_decode_ack). Return 0, -EINVAL (null),
 * -EMSGSIZE (too short, or length != the exact size for the layout),
 * -EBADMSG (wrong command word, status != 0, header != 0xAA, N != 8). */
int ld_cmd_decode_params(const uint8_t *payload, size_t len, ld_params_t *out);
int ld_cmd_decode_version(const uint8_t *payload, size_t len, ld_version_t *out);

#endif
