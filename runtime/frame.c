/* Frame loop: presentation, run-ahead, sprite interpolation and quicksaves.

   The run loop calls frame_boundary() at a safe point (no 68000 code in
   progress on the native stack) after the video module has rendered a field.

   Run-ahead (N fields): after each real field the machine state is saved, N
   more fields are emulated with the same input ("speculative": no audio, no
   logging, no disk writes), the last of them is shown, and the state is
   restored.  The reaction to new input appears N fields sooner; the real
   timeline (audio, saves) is unaffected.

   Interpolation: when a new picture C appears, the machine runs on (look-ahead)
   until the next one, N, appears, and the fields in between show C with its
   objects moved along their way to N (motion.c).  No delay is added.

   Quicksave: 10 slots, kept in memory and in saves/quicksave_<n>.state. */
#include "recomp.h"
#include "runtime.h"
#include "state.h"
#include "motion.h"
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

int g_speculative;
volatile int g_field_ready;

/* settings (host_win32.c loads and saves them) */
int g_runahead = 1;          /* fields shown ahead of the real machine (0-3) */
int g_motion_smooth = 1;     /* sprite interpolation */

/* command-line overrides (-1 = not given) */
int g_runahead_cli = -1, g_motion_cli = -1;

/* after the host has read settings.ini: the command line wins; headless
   (test) runs default to plain fields */
void frame_apply_options(void) {
    extern int g_headless;
    if (g_headless) { g_runahead = 0; g_motion_smooth = 0; }
    if (g_runahead_cli >= 0) g_runahead = g_runahead_cli > 3 ? 3 : g_runahead_cli;
    if (g_motion_cli >= 0) g_motion_smooth = g_motion_cli;
}

/* requests from the host (hotkeys, scripted input) */
volatile int g_req_save, g_req_load, g_req_slot, g_req_runahead, g_req_motion;

#define NSLOTS 10
static StateIO slots[NSLOTS];
static int slot;              /* 0-based */
static StateIO ra;            /* run-ahead snapshot */
static int spec_left;

void video_picture(uint32_t *out);
int  video_hand_rect(int *x0, int *y0, int *x1, int *y1);
void nvr_flush(void);
void nvr_mark_all_dirty(void);
int  os9_child_running(void);
void host_settings_changed(void);

/* ---------------------------------------------------------------- on-screen messages */
static char osd_text[64];
static int osd_frames;

static void osd(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(osd_text, sizeof osd_text, fmt, ap);
    va_end(ap);
    osd_frames = 120;
    LOG("%s", osd_text);
}

/* 5x7 font: one row per byte, bit 4 = leftmost column */
static const struct { char c; uint8_t r[7]; } font[] = {
    {'0', {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E}}, {'1', {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E}},
    {'2', {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F}}, {'3', {0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E}},
    {'4', {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02}}, {'5', {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E}},
    {'6', {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E}}, {'7', {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08}},
    {'8', {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E}}, {'9', {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C}},
    {'A', {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}}, {'B', {0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E}},
    {'C', {0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E}}, {'D', {0x1C, 0x12, 0x11, 0x11, 0x11, 0x12, 0x1C}},
    {'E', {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F}}, {'F', {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10}},
    {'G', {0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0F}}, {'H', {0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}},
    {'I', {0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E}}, {'J', {0x07, 0x02, 0x02, 0x02, 0x02, 0x12, 0x0C}},
    {'K', {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11}}, {'L', {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F}},
    {'M', {0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11}}, {'N', {0x11, 0x11, 0x19, 0x15, 0x13, 0x11, 0x11}},
    {'O', {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}}, {'P', {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10}},
    {'Q', {0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D}}, {'R', {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11}},
    {'S', {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E}}, {'T', {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04}},
    {'U', {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}}, {'V', {0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04}},
    {'W', {0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0A}}, {'X', {0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11}},
    {'Y', {0x11, 0x11, 0x11, 0x0A, 0x04, 0x04, 0x04}}, {'Z', {0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F}},
    {':', {0x00, 0x0C, 0x0C, 0x00, 0x0C, 0x0C, 0x00}}, {'-', {0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00}},
    {'+', {0x00, 0x04, 0x04, 0x1F, 0x04, 0x04, 0x00}}, {'.', {0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C}},
    {'/', {0x01, 0x01, 0x02, 0x04, 0x08, 0x10, 0x10}}, {'(', {0x02, 0x04, 0x08, 0x08, 0x08, 0x04, 0x02}},
    {')', {0x08, 0x04, 0x02, 0x02, 0x02, 0x04, 0x08}}, {'!', {0x04, 0x04, 0x04, 0x04, 0x04, 0x00, 0x04}},
};

static const uint8_t *glyph(char c) {
    if (c >= 'a' && c <= 'z') c = (char)(c - 32);
    for (size_t i = 0; i < sizeof font / sizeof font[0]; i++) if (font[i].c == c) return font[i].r;
    return NULL;
}

/* text in the top-left corner; a font pixel is 3 x 1 frame pixels (about square on screen) */
static void osd_draw(uint32_t *fb, int w, int h, int tick) {
    if (osd_frames <= 0) return;
    if (tick) osd_frames--;
    const int sx = 3, cw = 6 * sx, x0 = 12, y0 = 6;
    int len = (int)strlen(osd_text);
    int bw = len * cw + 2 * sx * 2, bh = 7 + 4;
    for (int y = y0 - 2; y < y0 - 2 + bh && y < h; y++)
        for (int x = x0 - 2 * sx; x < x0 - 2 * sx + bw && x < w; x++) {
            uint32_t p = fb[y * w + x];
            fb[y * w + x] = (p >> 2) & 0x3F3F3F;           /* darkened box */
        }
    for (int i = 0; i < len; i++) {
        const uint8_t *g = glyph(osd_text[i]);
        if (!g) continue;
        for (int r = 0; r < 7; r++)
            for (int c = 0; c < 5; c++) {
                if (!(g[r] & (0x10 >> c))) continue;
                for (int k = 0; k < sx; k++) {
                    int x = x0 + i * cw + c * sx + k, y = y0 + r;
                    if (x < w && y < h) fb[y * w + x] = 0xFFFFE070;
                }
            }
    }
}

/* phases of the frame loop */
enum { PH_REAL, PH_AHEAD, PH_LOOK };
static int phase;

/* will the field being rendered now be shown or examined?  (run-ahead shows
   only the last field of its window; look-ahead examines every field) */
int frame_field_shown(void) {
    if (phase == PH_LOOK) return 1;
    if (phase == PH_AHEAD) return spec_left <= 1;
    return g_runahead == 0 || run_depth() != 1 || os9_child_running();
}

static uint32_t *cur, *pic, *out_fb;
static Rect handCur;                 /* hand cursor in cur */
static long long disp;               /* displayed fields */

/* Interpolation state of one screen region (motion.h).  The look-ahead runs
   the machine on from a displayed field, at most LOOK_STEP fields per displayed
   field, keeping where it got to in `fut` in between. */
#define LOOK_MAX  8                  /* longest interval that is interpolated */
#define LOOK_STEP 2
typedef struct {
    int have, searching, ready;      /* picture known; looking for the next; pair loaded */
    uint32_t *c;                     /* the picture on show */
    Rect hand;
    long long t, tn;                 /* when it appeared, when the next one will */
    int busy, pause;                 /* consecutive one-field intervals; fields to sit out */
} Track;
static Track trk[MOTION_REGIONS];
static StateIO fut;                  /* the look-ahead machine */
static int fut_valid, look_left;
static long long tF;                 /* the field it has reached */
static int full_active;              /* whole-screen tracking is in use this field */

/* statistics (RECOMP_FRAMETIME=1): logged at exit */
static int stat_on = -1, st_hist[6], st_pairs, st_inter, st_fields, st_look, st_gap[10], st_used[MOTION_REGIONS], st_rej;
static double st_last, st_max;
static void st_report(void) {
    logf_(0, "fields %d, pairs %d, interpolated fields %d (portrait %d, view %d, full %d; %d not usable), look-ahead fields %d; gaps 1-8: %d %d %d %d %d %d %d %d",
          st_fields, st_pairs, st_inter, st_used[0], st_used[1], st_used[2], st_rej, st_look,
          st_gap[1], st_gap[2], st_gap[3], st_gap[4], st_gap[5], st_gap[6], st_gap[7], st_gap[8]);
    logf_(0, "work per field ms: <4 %d, <8 %d, <12 %d, <16 %d, <25 %d, more %d; max %.1f",
          st_hist[0], st_hist[1], st_hist[2], st_hist[3], st_hist[4], st_hist[5], st_max);
}

static void init_buffers(void) {
    if (cur) return;
    size_t n = (size_t)video_w * video_h * 4;
    cur = (uint32_t *)calloc(1, n);
    pic = (uint32_t *)calloc(1, n);
    out_fb = (uint32_t *)calloc(1, n);
    for (int i = 0; i < MOTION_REGIONS; i++) trk[i].c = (uint32_t *)calloc(1, n);
}

static void forget_pictures(void) {
    for (int i = 0; i < MOTION_REGIONS; i++) trk[i].have = trk[i].searching = trk[i].ready = trk[i].busy = trk[i].pause = 0;
    fut_valid = 0;
    motion_reset();
}

static Rect hand(void) {
    Rect r = {0, 0, 0, 0};
    video_hand_rect(&r.x0, &r.y0, &r.x1, &r.y1);
    return r;
}

static void show(const uint32_t *fb) {
    memcpy(out_fb, fb, (size_t)video_w * video_h * 4);
    if (stat_on < 0) { stat_on = getenv("RECOMP_FRAMETIME") != NULL; if (stat_on) atexit(st_report); }
    if (g_motion_smooth) {
        int any = 0;
        for (int i = 0; i < MOTION_REGIONS; i++) {
            Track *t = &trk[i];
            if ((i == MOTION_FULL) != full_active) continue;
            if (!t->ready || disp <= t->t || disp >= t->tn) continue;
            Rect skip[2] = {handCur, t->hand};
            if (motion_render(i, out_fb, (float)(disp - t->t) / (float)(t->tn - t->t), skip, 2)) { any = 1; st_used[i]++; }
            else st_rej++;
        }
        st_inter += any;
    }
    osd_draw(out_fb, video_w, video_h, 1);
    if (stat_on) {
        double now = host_time();
        st_fields++;
        if (st_last > 0) {
            double ms = (now - st_last) * 1000.0;
            st_hist[ms < 4 ? 0 : ms < 8 ? 1 : ms < 12 ? 2 : ms < 16 ? 3 : ms < 25 ? 4 : 5]++;
            if (ms > st_max) st_max = ms;
        }
    }
    host_present_frame(out_fb, video_w, video_h);
    host_poll();
    if (stat_on) st_last = host_time();
}

/* back to the real machine after run-ahead or look-ahead */
static void end_speculation(void) {
    if (phase == PH_REAL) return;
    phase = PH_REAL;
    g_speculative = 0;
    g_field_ready = 0;
    if (state_load(&ra)) fatal("run-ahead snapshot could not be restored");
}

static int searching(void) {
    for (int i = 0; i < MOTION_REGIONS; i++) if (trk[i].searching) return 1;
    return 0;
}

/* the field to show has been rendered (real, or the end of a run-ahead window) */
static void display(void) {
    disp++;
    video_picture(cur);
    handCur = hand();
    if (!g_motion_smooth) { end_speculation(); show(cur); return; }
    full_active = trk[MOTION_FULL].busy < 6;
    int restart = 0;
    for (int i = 0; i < MOTION_REGIONS; i++) {
        Track *t = &trk[i];
        Rect skip[2] = {handCur, t->hand};
        int changed = !t->have || motion_differs(i, t->c, cur, skip, 2);
        if (!changed) {
            if (t->busy >= 6 && disp - t->t > 1) t->busy = 0;
            continue;
        }
        /* a region that changes every field needs no in-betweens */
        if (t->have && disp - t->t == 1) t->busy++; else t->busy = 0;
        memcpy(t->c, cur, (size_t)video_w * video_h * 4);
        t->hand = handCur;
        t->have = 1;
        t->t = disp;
        t->ready = 0;
        t->searching = t->busy < 6 && ((i == MOTION_FULL) == full_active);
        motion_drop(i);
        if (t->searching) restart = 1;
    }
    if (!searching()) { fut_valid = 0; end_speculation(); show(cur); return; }
    if (phase == PH_REAL) state_save(&ra, 0);
    if (restart || !fut_valid) tF = disp;          /* look ahead from here */
    else if (state_load(&fut)) fatal("look-ahead snapshot could not be restored");
    fut_valid = 0;
    phase = PH_LOOK;
    g_speculative = 1;
    look_left = LOOK_STEP;
}

/* a look-ahead field has been rendered */
static void look_field(int fields) {
    tF += fields;
    st_look++;
    look_left -= fields;
    video_picture(pic);
    Rect hn = hand();
    for (int i = 0; i < MOTION_REGIONS; i++) {
        Track *t = &trk[i];
        if (!t->searching) continue;
        Rect skip[2] = {hn, t->hand};
        if (tF > t->t && motion_differs(i, t->c, pic, skip, 2)) {
            t->searching = 0;
            t->tn = tF;
            st_pairs++;
            st_gap[tF - t->t > 9 ? 9 : tF - t->t]++;
            if (tF - t->t >= 2 && tF > disp) t->ready = motion_set_pair(i, t->c, pic, skip, 2);
        } else if (tF - t->t >= LOOK_MAX) t->searching = 0;
    }
    if (!searching() || look_left <= 0) {
        if (searching()) { state_save(&fut, 0); fut_valid = 1; }
        end_speculation();
        show(cur);
    }
}

/* ---------------------------------------------------------------- quicksave */
static void slot_path(int n, char *out, size_t len) {
    snprintf(out, len, "%s/quicksave_%d.state", g_data_dir, n + 1);
}

static uint32_t checksum(const uint8_t *p, size_t n) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 16777619u;
    return h;
}

static void quicksave(void) {
    state_save(&slots[slot], 1);
    char p[600];
    slot_path(slot, p, sizeof p);
    FILE *f = fopen(p, "wb");
    int ok = 0;
    if (f) {
        uint32_t sum = checksum(slots[slot].buf, slots[slot].len);
        ok = fwrite(slots[slot].buf, 1, slots[slot].len, f) == slots[slot].len && fwrite(&sum, 4, 1, f) == 1;
        ok = fclose(f) == 0 && ok;
    }
    if (ok) osd("Saved slot %d", slot + 1);
    else osd("Saved slot %d (not written to disk)", slot + 1);
}

static void quickload(void) {
    StateIO *s = &slots[slot];
    if (!s->len) {
        char p[600];
        slot_path(slot, p, sizeof p);
        FILE *f = fopen(p, "rb");
        if (f) {
            fseek(f, 0, SEEK_END);
            long n = ftell(f);
            fseek(f, 0, SEEK_SET);
            if (n > 0) {
                s->buf = (uint8_t *)realloc(s->buf, (size_t)n);
                s->cap = (size_t)n;
                size_t got = fread(s->buf, 1, (size_t)n, f);
                uint32_t sum = 0;
                if (got == (size_t)n && n > 4) memcpy(&sum, s->buf + n - 4, 4);
                if (got != (size_t)n || n <= 4 || sum != checksum(s->buf, (size_t)n - 4)) {
                    fclose(f);
                    s->len = 0;
                    osd("Slot %d: damaged file", slot + 1);
                    return;
                }
                s->len = (size_t)n - 4;
            }
            fclose(f);
        }
    }
    if (!s->len) { osd("Slot %d is empty", slot + 1); return; }
    /* keep the current state in case the slot does not fit this build */
    StateIO backup = {0};
    state_save(&backup, 1);
    if (state_load(s)) {
        state_load(&backup);
        s->len = 0;                       /* read the file again next time */
        osd("Slot %d: saved by another version", slot + 1);
    } else {
        nvr_mark_all_dirty();
        forget_pictures();
        osd("Loaded slot %d", slot + 1);
    }
    state_free(&backup);
}

static void handle_requests(void) {
    while (g_req_slot > 0) {
        g_req_slot--;
        slot = (slot + 1) % NSLOTS;
        char p[600];
        slot_path(slot, p, sizeof p);
        FILE *f = fopen(p, "rb");
        int used = slots[slot].len || f;
        if (f) fclose(f);
        osd("Slot %d%s", slot + 1, used ? "" : " (empty)");
    }
    if (g_req_save) { g_req_save = 0; quicksave(); }
    if (g_req_load) { g_req_load = 0; quickload(); }
    if (g_req_runahead) {
        g_req_runahead = 0;
        g_runahead = (g_runahead + 1) % 4;
        osd("Run-ahead %d", g_runahead);
        host_settings_changed();
    }
    if (g_req_motion) {
        g_req_motion = 0;
        g_motion_smooth = !g_motion_smooth;
        forget_pictures();
        osd("Interpolation %s", g_motion_smooth ? "on" : "off");
        host_settings_changed();
    }
}

/* ---------------------------------------------------------------- frame boundary */
void frame_boundary(void) {
    int fields = g_field_ready;
    g_field_ready = 0;
    init_buffers();
    /* snapshots need the outermost run loop, outside child processes */
    if (run_depth() != 1 || os9_child_running()) {
        forget_pictures();
        video_picture(cur);               /* keep the display alive, nothing else */
        show(cur);
        return;
    }
    if (phase == PH_AHEAD) {
        spec_left -= fields;
        if (spec_left <= 0) display();    /* the picture from the future */
        return;
    }
    if (phase == PH_LOOK) { look_field(fields); return; }
    /* a real field: side effects of the real timeline happen here */
    nvr_flush();
    handle_requests();
    if (g_runahead > 0) {
        state_save(&ra, 0);
        phase = PH_AHEAD;
        g_speculative = 1;
        spec_left = g_runahead;
        return;
    }
    display();
}
