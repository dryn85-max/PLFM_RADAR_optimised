#include "ws_slots.h"
#include <errno.h>
#include <string.h>

static int in_range(int idx) { return idx >= 0 && idx < WS_SLOTS_MAX; }

void ws_slots_init(ws_slots_t *t) { memset(t, 0, sizeof *t); }

static int find(const ws_slots_t *t, int fd)
{
    for (int i = 0; i < WS_SLOTS_MAX; i++)
        if (t->active[i] && t->fd[i] == fd) return i;
    return -1;
}

int ws_slots_add(ws_slots_t *t, int fd)
{
    if (fd < 0) return -EINVAL;
    int i = find(t, fd);
    if (i >= 0) return i;
    for (i = 0; i < WS_SLOTS_MAX; i++) {
        if (!t->active[i] && !t->inflight[i]) {
            t->active[i] = 1;
            t->fd[i] = fd;
            return i;
        }
    }
    return -ENOSPC;
}

int ws_slots_remove(ws_slots_t *t, int fd)
{
    if (fd < 0) return -EINVAL;
    int i = find(t, fd);
    if (i < 0) return -ENOENT;
    t->active[i] = 0;
    t->fd[i] = -1;
    return 0;
}

int ws_slots_fd(const ws_slots_t *t, int idx)
{
    if (!in_range(idx) || !t->active[idx]) return -1;
    return t->fd[idx];
}

int ws_slots_count(const ws_slots_t *t)
{
    int n = 0;
    for (int i = 0; i < WS_SLOTS_MAX; i++) n += t->active[i] ? 1 : 0;
    return n;
}

int ws_slots_begin(ws_slots_t *t, int idx)
{
    if (!in_range(idx) || !t->active[idx] || t->inflight[idx]) return 0;
    t->inflight[idx] = 1;
    return 1;
}

void ws_slots_end(ws_slots_t *t, int idx)
{
    if (in_range(idx)) t->inflight[idx] = 0;
}
