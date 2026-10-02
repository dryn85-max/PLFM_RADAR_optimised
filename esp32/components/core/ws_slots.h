/* WebSocket client slot table with a per-client "send in flight" flag (plain C11).
 * Not thread-safe: the caller serialises access. */
#ifndef WS_SLOTS_H
#define WS_SLOTS_H

#define WS_SLOTS_MAX 4

typedef struct {
    int fd[WS_SLOTS_MAX];
    unsigned char active[WS_SLOTS_MAX];
    unsigned char inflight[WS_SLOTS_MAX];
} ws_slots_t;

void ws_slots_init(ws_slots_t *t);
/* Returns the slot index; the same fd again returns its existing slot.
 * -EINVAL for fd < 0, -ENOSPC when no slot is free. A slot whose queued send
 * has not ended is not reused. */
int ws_slots_add(ws_slots_t *t, int fd);
/* 0, -ENOENT for an unknown fd, -EINVAL for fd < 0. A pending send keeps the
 * slot busy until ws_slots_end(). */
int ws_slots_remove(ws_slots_t *t, int fd);
/* fd of an active slot, or -1 (inactive or index out of range). */
int ws_slots_fd(const ws_slots_t *t, int idx);
/* Number of active clients. */
int ws_slots_count(const ws_slots_t *t);
/* 1 and marks the slot in flight if it is active and idle; otherwise 0. */
int ws_slots_begin(ws_slots_t *t, int idx);
/* Marks the send complete (harmless on any index). */
void ws_slots_end(ws_slots_t *t, int idx);

#endif
