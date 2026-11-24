/* Emulator snapshots (quicksave slots and run-ahead).

   Every module that holds emulation state has one "sync" function that both
   saves and loads, so the two directions cannot drift apart:

       void video_state(StateIO *s) {
           st_bytes(s, &vs, sizeof vs);
           ...
       }

   Snapshots are only taken at a safe point of the run loop (state.c), where
   no 68000 code is being executed by a native call chain. */
#pragma once
#include <stddef.h>
#include <stdint.h>

typedef struct {
    int loading;            /* 0 = saving into buf, 1 = loading from buf */
    uint8_t *buf;
    size_t len, cap, pos;
    int err;
    int full;               /* quicksave: include data only needed for display */
} StateIO;

void st_bytes(StateIO *s, void *p, size_t n);
void st_u32(StateIO *s, uint32_t *v);
void st_i32(StateIO *s, int *v);
/* variable-size heap block: *pp is (re)allocated on load, *n elements of size each */
void st_block(StateIO *s, void **pp, uint32_t *n, size_t size);
#define ST(s, x) st_bytes((s), &(x), sizeof(x))

/* per-module state (each file) */
void cpu_state(StateIO *s);
void os9_state(StateIO *s);
void cdfm_state(StateIO *s);
void audio_state(StateIO *s);
void video_state(StateIO *s);

/* whole-machine snapshot */
void state_save(StateIO *s, int full);
int  state_load(StateIO *s);          /* returns 0 on success */
void state_free(StateIO *s);

/* run loop integration (frame.c) */
extern int g_speculative;             /* running run-ahead frames: no audio, logs or disk writes */
extern volatile int g_field_ready;    /* a field has been rendered */
int  run_depth(void);                 /* nesting of cpu_run_until */
void frame_boundary(void);            /* called by the run loop at a safe point */
