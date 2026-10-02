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
#define LD_CMD_ACK_FLAG 0x0100u

#define LD_CMD_MAX_VALUE 8u
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

#endif
