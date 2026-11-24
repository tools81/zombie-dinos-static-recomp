/* Snapshot serialization (see state.h). */
#include "recomp.h"
#include "runtime.h"
#include "state.h"
#include <stdlib.h>
#include <string.h>

#define STATE_MAGIC   0x43444953u   /* "CDIS" */
#define STATE_VERSION 1u

static void grow(StateIO *s, size_t need) {
    if (s->len + need <= s->cap) return;
    size_t nc = s->cap ? s->cap : 1 << 20;
    while (nc < s->len + need) nc *= 2;
    uint8_t *nb = (uint8_t *)realloc(s->buf, nc);
    if (!nb) fatal("out of memory saving a snapshot");
    s->buf = nb;
    s->cap = nc;
}

void st_bytes(StateIO *s, void *p, size_t n) {
    if (s->err) return;
    if (s->loading) {
        if (s->pos + n > s->len) { s->err = 1; return; }
        memcpy(p, s->buf + s->pos, n);
        s->pos += n;
    } else {
        grow(s, n);
        memcpy(s->buf + s->len, p, n);
        s->len += n;
    }
}

void st_u32(StateIO *s, uint32_t *v) { st_bytes(s, v, 4); }
void st_i32(StateIO *s, int *v) { st_bytes(s, v, 4); }

void st_block(StateIO *s, void **pp, uint32_t *n, size_t size) {
    st_u32(s, n);
    if (s->err) return;
    if (s->loading && (uint64_t)*n * size > s->len - s->pos) { s->err = 1; *n = 0; return; }
    if (s->loading) {
        free(*pp);
        *pp = *n ? malloc((size_t)*n * size + 16) : NULL;
        if (*n && !*pp) { s->err = 1; return; }
    }
    if (*n) st_bytes(s, *pp, (size_t)*n * size);
}

/* identifies the snapshot layout. It holds machine state only, so recompiled and
   interpreter builds share it. Raise STATE_VERSION when a *_state function or a
   structure it stores changes: quicksaves then survive every other update. */
static uint32_t build_id(void) {
    uint32_t h = 2166136261u;
#define MIX(v) do { h ^= (uint32_t)(v); h *= 16777619u; } while (0)
    MIX(STATE_VERSION);
    MIX(sizeof(M68K));
    MIX(CPU_IPS);
#undef MIX
    return h;
}

void state_save(StateIO *s, int full) {
    s->loading = 0;
    s->len = 0;
    s->pos = 0;
    s->err = 0;
    s->full = full;
    uint32_t hdr[3] = {STATE_MAGIC, build_id(), (uint32_t)full};
    st_bytes(s, hdr, sizeof hdr);
    cpu_state(s);
    os9_state(s);
    cdfm_state(s);
    audio_state(s);
    video_state(s);
    uint32_t end = STATE_MAGIC ^ 0xFFFFFFFFu;
    st_u32(s, &end);
}

int state_load(StateIO *s) {
    s->loading = 1;
    s->pos = 0;
    s->err = 0;
    uint32_t hdr[3];
    st_bytes(s, hdr, sizeof hdr);
    if (s->err || hdr[0] != STATE_MAGIC || hdr[1] != build_id()) return -1;
    s->full = (int)hdr[2];
    cpu_state(s);
    os9_state(s);
    cdfm_state(s);
    audio_state(s);
    video_state(s);
    uint32_t end = 0;
    st_u32(s, &end);
    if (s->err || end != (STATE_MAGIC ^ 0xFFFFFFFFu) || s->pos != s->len) return -1;
    return 0;
}

void state_free(StateIO *s) {
    free(s->buf);
    memset(s, 0, sizeof *s);
}
