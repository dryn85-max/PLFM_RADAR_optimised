/* Live-view snapshot -> JSON text (plain C11). */
#ifndef SNAPSHOT_JSON_H
#define SNAPSHOT_JSON_H

#include <stddef.h>
#include <stdint.h>
#include "ld2410_frame.h"

typedef enum {
    SNAP_LINK_NO_DATA = 0, /* nothing received yet */
    SNAP_LINK_OK = 1,
    SNAP_LINK_LOST = 2     /* frames stopped arriving */
} snap_link_t;

typedef struct {
    uint32_t seq;
    uint64_t esp_time_us;
    snap_link_t link;
    uint8_t have_frame; /* 0: `data` not valid, JSON carries "data":null */
    ld_data_t data;
} snapshot_t;

/* Writes a NUL-terminated JSON object into out[cap]. Returns its length, or
 * -EINVAL, or -ENOSPC when it does not fit (never truncates; out is then an
 * empty string). Gate arrays appear only for engineering frames. */
int snapshot_json(char *out, size_t cap, const snapshot_t *s);

#endif
