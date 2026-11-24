/* Sprite interpolation: in-between pictures from object motion.

   The game shows new pictures 10-30 times a second.  frame.c finds the next
   picture N by running the machine ahead from the current one, C, so the
   in-betweens are true interpolations that add no delay.

   1. Motion C -> N is estimated per 8x8 block of a 384 x 240 luma image,
      coarse to fine, where the picture changed.
   2. Each connected group of changed blocks is an object.  One overall motion
      (move, scale, rotate, shear: an affine map) is fitted to its blocks,
      ignoring outliers.  Around it a smooth field of local motion follows
      parts that move differently.
   3. Every pixel of an in-between is one of: the object (C moved back and N
      moved forward agree), background (C = N), background the object uncovers
      or is about to cover (taken from the picture that shows it), or a
      cross-fade when nothing explains it.
   Objects whose change is mostly not explained by their motion (new drawn
   poses, cuts, colour effects) are left as they are. */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <emmintrin.h>
#include "motion.h"

#define W 768                   /* frame (texels) */
#define H 240
#define LW 384                  /* luma image (normal-resolution pixels) */
#define BS 8                    /* motion estimation block (luma pixels) */
#define BW (LW / BS)
#define BH (H / BS)
#define MAXOBJ 12

typedef struct {
    int x0, y0, x1, y1;         /* area (normal-resolution pixels) */
    float ax, bx, cx, ay, by, cy;   /* motion: vx = ax + bx*x + cx*y, vy = ay + by*x + cy*y */
    float *px, *py;             /* motion of every pixel (screen layout) */
} Object;

typedef struct {
    uint32_t *c, *n;            /* the two pictures */
    uint8_t *lc, *ln, *lc2, *ln2;   /* full and half resolution luma */
    uint8_t *mask;              /* changed pixels, spread by two */
    uint8_t obj[BH][BW];        /* object number + 1 of each block */
    int16_t vx[BH][BW], vy[BH][BW];
    uint8_t changed[BH][BW];
    Object o[MAXOBJ];
    int nobj, valid;
} Motion;
static Motion ctx[MOTION_REGIONS];

/* Regions with their own animation rate: the portrait (an ellipse), the
   landscape window and the whole screen.  Around the first two the control
   panel's lights cycle their colours all the time. */
static const struct { int x0, y0, x1, y1; } region[MOTION_REGIONS] = {
    {232, 35, 336, 121}, {49, 23, 200, 202}, {0, 0, LW, H},
};
static int in_region(int r, int x, int y) {          /* normal-resolution pixel */
    if (x < region[r].x0 || x >= region[r].x1 || y < region[r].y0 || y >= region[r].y1) return 0;
    if (r) return 1;
    int dx = (x - 284) * 43, dy = (y - 78) * 52;
    return dx * dx + dy * dy <= 2150 * 2150;
}

static int in_rects(const Rect *r, int n, int x0, int y0, int x1, int y1) {
    for (int i = 0; i < n; i++)
        if (x1 > r[i].x0 && x0 < r[i].x1 && y1 > r[i].y0 && y0 < r[i].y1) return 1;
    return 0;
}

static inline int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

static inline int iround(float f) { return (int)(f + 1024.5f) - 1024; }

void motion_reset(void) { for (int i = 0; i < MOTION_REGIONS; i++) ctx[i].valid = 0; }
void motion_drop(int reg) { ctx[reg].valid = 0; }

/* do two pictures differ inside a region, outside the given rectangles? */
int motion_differs(int reg, const uint32_t *a, const uint32_t *b, const Rect *skip, int nskip) {
    int x0 = region[reg].x0, x1 = region[reg].x1;
    for (int y = region[reg].y0; y < region[reg].y1; y++) {
        const uint32_t *p = a + y * W, *q = b + y * W;
        if (!memcmp(p + 2 * x0, q + 2 * x0, (size_t)(x1 - x0) * 8)) continue;
        for (int x = 2 * x0; x < 2 * x1; x++)
            if (p[x] != q[x] && in_region(reg, x / 2, y) && !in_rects(skip, nskip, x, y, x + 1, y + 1)) return 1;
    }
    return 0;
}

static void make_luma(const uint32_t *pic, uint8_t *l, uint8_t *l2) {
    for (int y = 0; y < H; y++)
        for (int x = 0; x < LW; x++) {
            uint32_t a = pic[y * W + 2 * x], b = pic[y * W + 2 * x + 1];
            int ya = (int)(((a >> 16) & 255) * 77 + ((a >> 8) & 255) * 150 + (a & 255) * 29) >> 8;
            int yb = (int)(((b >> 16) & 255) * 77 + ((b >> 8) & 255) * 150 + (b & 255) * 29) >> 8;
            l[y * LW + x] = (uint8_t)((ya + yb) >> 1);
        }
    for (int y = 0; y < H / 2; y++)
        for (int x = 0; x < LW / 2; x++) {
            const uint8_t *p = l + (2 * y) * LW + 2 * x;
            l2[y * (LW / 2) + x] = (uint8_t)((p[0] + p[1] + p[LW] + p[LW + 1] + 2) >> 2);
        }
}

/* SAD of an n x n block at (x, y) in a against (x + dx, y + dy) in b */
static int sad(const uint8_t *a, const uint8_t *b, int w, int h, int x, int y, int dx, int dy, int n, int best) {
    if (x + dx >= 0 && x + dx + n <= w && y + dy >= 0 && y + dy + n <= h) {     /* inside: SSE2 */
        const uint8_t *pa = a + y * w + x, *pb = b + (y + dy) * w + x + dx;
        int s = 0;
        for (int j = 0; j < n; j++, pa += w, pb += w) {
            if (n >= 8) s += _mm_cvtsi128_si32(_mm_sad_epu8(_mm_loadl_epi64((const __m128i *)pa), _mm_loadl_epi64((const __m128i *)pb)));
            if (n != 8) {
                int32_t u, v;
                memcpy(&u, pa + n - 4, 4);
                memcpy(&v, pb + n - 4, 4);
                s += _mm_cvtsi128_si32(_mm_sad_epu8(_mm_cvtsi32_si128(u), _mm_cvtsi32_si128(v)));
            }
            if (s >= best) return s;
        }
        return s;
    }
    int s = 0;
    for (int j = 0; j < n; j++) {
        int yb = clampi(y + j + dy, 0, h - 1);
        const uint8_t *pa = a + (y + j) * w + x;
        const uint8_t *pb = b + yb * w;
        if (x + dx >= 0 && x + dx + n <= w) {
            pb += x + dx;
            for (int i = 0; i < n; i++) { int d = pa[i] - pb[i]; s += d < 0 ? -d : d; }
        } else
            for (int i = 0; i < n; i++) { int d = pa[i] - pb[clampi(x + i + dx, 0, w - 1)]; s += d < 0 ? -d : d; }
        if (s >= best) return s;
    }
    return s;
}

/* solve a 3x3 system; returns 0 if it is singular */
static int solve3(double m[3][4], double *r) {
    for (int c = 0; c < 3; c++) {
        int p = c;
        for (int i = c + 1; i < 3; i++) if (fabs(m[i][c]) > fabs(m[p][c])) p = i;
        if (fabs(m[p][c]) < 1e-6) return 0;
        for (int k = 0; k < 4; k++) { double t = m[c][k]; m[c][k] = m[p][k]; m[p][k] = t; }
        for (int i = 0; i < 3; i++) {
            if (i == c) continue;
            double f = m[i][c] / m[c][c];
            for (int k = c; k < 4; k++) m[i][k] -= f * m[c][k];
        }
    }
    for (int i = 0; i < 3; i++) r[i] = m[i][3] / m[i][i];
    return 1;
}

static int cmp_int(const void *a, const void *b) { return *(const int *)a - *(const int *)b; }

typedef struct { int x, y, vx, vy; float w; } Vec;

/* the object's motion from its blocks' vectors */
static void fit(Object *o, Vec *v, int n) {
    static int tx[BW * BH], ty[BW * BH];
    for (int i = 0; i < n; i++) { tx[i] = v[i].vx; ty[i] = v[i].vy; }
    qsort(tx, (size_t)n, sizeof(int), cmp_int);
    qsort(ty, (size_t)n, sizeof(int), cmp_int);
    o->ax = (float)tx[n / 2]; o->ay = (float)ty[n / 2];
    o->bx = o->cx = o->by = o->cy = 0;
    float lim = 6;
    for (int it = 0; it < 5; it++) {
        double m[3][4] = {{0}}, q[3][4] = {{0}}, rx[3], ry[3];
        int used = 0, minx = 9999, maxx = -1, miny = 9999, maxy = -1;
        for (int i = 0; i < n; i++) {
            float ex = o->ax + o->bx * v[i].x + o->cx * v[i].y - v[i].vx;
            float ey = o->ay + o->by * v[i].x + o->cy * v[i].y - v[i].vy;
            if (ex * ex + ey * ey > lim * lim) continue;
            used++;
            if (v[i].x < minx) minx = v[i].x;
            if (v[i].x > maxx) maxx = v[i].x;
            if (v[i].y < miny) miny = v[i].y;
            if (v[i].y > maxy) maxy = v[i].y;
            double p[3] = {1, v[i].x, v[i].y};
            for (int r = 0; r < 3; r++) {
                for (int c = 0; c < 3; c++) { m[r][c] += v[i].w * p[r] * p[c]; q[r][c] += v[i].w * p[r] * p[c]; }
                m[r][3] += v[i].w * p[r] * v[i].vx;
                q[r][3] += v[i].w * p[r] * v[i].vy;
            }
        }
        /* a small object only moves; a larger one may also scale and turn */
        if (used < 8 || maxx - minx < 24 || maxy - miny < 24) {
            if (used >= 2 && m[0][0] > 0) { o->ax = (float)(m[0][3] / m[0][0]); o->ay = (float)(q[0][3] / q[0][0]); o->bx = o->cx = o->by = o->cy = 0; }
        } else if (solve3(m, rx) && solve3(q, ry)) {
            o->ax = (float)rx[0]; o->bx = (float)rx[1]; o->cx = (float)rx[2];
            o->ay = (float)ry[0]; o->by = (float)ry[1]; o->cy = (float)ry[2];
        }
        lim = it < 2 ? 4.0f : 2.5f;
    }
}

/* Local motion: every block takes, from its own match, its neighbours' and the
   overall motion, the vector that fits best and is close to its neighbours';
   pixels get the field in between the block centres. */
static void local_motion(Motion *m, int reg, Object *o, int tag) {
    static float fx[BH][BW], fy[BH][BW], gx[BH][BW], gy[BH][BW];
    int x0 = o->x0 / BS, y0 = o->y0 / BS, x1 = (o->x1 + BS - 1) / BS, y1 = (o->y1 + BS - 1) / BS;
    if (x1 > BW) x1 = BW;
    if (y1 > BH) y1 = BH;
    for (int by = y0; by < y1; by++)
        for (int bx = x0; bx < x1; bx++) {
            float x = bx * BS + BS / 2.0f, y = by * BS + BS / 2.0f;
            fx[by][bx] = o->ax + o->bx * x + o->cx * y;
            fy[by][bx] = o->ay + o->by * x + o->cy * y;
            if (m->changed[by][bx] == 1 && m->obj[by][bx] == tag &&
                fabsf(m->vx[by][bx] - fx[by][bx]) + fabsf(m->vy[by][bx] - fy[by][bx]) < 5) { fx[by][bx] = m->vx[by][bx]; fy[by][bx] = m->vy[by][bx]; }
        }
    /* background blocks around the object continue its nearest motion, so the
       field does not bend at the outline */
    static uint8_t known[BH][BW], knew[BH][BW];
    for (int by = y0; by < y1; by++)
        for (int bx = x0; bx < x1; bx++) known[by][bx] = m->changed[by][bx] && m->obj[by][bx] == tag;
    for (int pass = 0; pass < 3; pass++) {
        memcpy(knew, known, sizeof known);
        for (int by = y0; by < y1; by++)
            for (int bx = x0; bx < x1; bx++) {
                if (known[by][bx]) continue;
                float sx = 0, sy = 0;
                int n = 0;
                for (int j = -1; j <= 1; j++)
                    for (int i = -1; i <= 1; i++) {
                        int yy = by + j, xx = bx + i;
                        if (yy < y0 || yy >= y1 || xx < x0 || xx >= x1 || !known[yy][xx]) continue;
                        sx += fx[yy][xx]; sy += fy[yy][xx]; n++;
                    }
                if (n) { fx[by][bx] = sx / n; fy[by][bx] = sy / n; knew[by][bx] = 1; }
            }
        memcpy(known, knew, sizeof known);
    }
    for (int pass = 0; pass < 3; pass++) {
        for (int by = y0; by < y1; by++)
            for (int bx = x0; bx < x1; bx++) {
                float cx[11], cy[11], nx[8], ny[8];
                int nc = 0, nn = 0;
                for (int j = -1; j <= 1; j++)
                    for (int i = -1; i <= 1; i++) {
                        int yy = by + j, xx = bx + i;
                        if (yy < y0 || yy >= y1 || xx < x0 || xx >= x1) continue;
                        cx[nc] = fx[yy][xx]; cy[nc++] = fy[yy][xx];
                        if (i || j) { nx[nn] = fx[yy][xx]; ny[nn++] = fy[yy][xx]; }
                    }
                cx[nc] = o->ax + o->bx * (bx * BS + 4) + o->cx * (by * BS + 4);
                cy[nc++] = o->ay + o->by * (bx * BS + 4) + o->cy * (by * BS + 4);
                float bestc = 1e30f, bvx = 0, bvy = 0;
                int ex = clampi(bx * BS - 2, 0, LW - 12), ey = clampi(by * BS - 2, 0, H - 12);
                for (int k = 0; k < nc; k++) {
                    float cost = (float)sad(m->lc, m->ln, LW, H, ex, ey, iround(cx[k]), iround(cy[k]), 12, 1 << 30);
                    if (!m->changed[by][bx]) cost = 0;      /* background: smoothness decides */
                    float sm = 0;
                    for (int q = 0; q < nn; q++) sm += fabsf(cx[k] - nx[q]) + fabsf(cy[k] - ny[q]);
                    if (nn) cost += 150.0f * sm / nn;
                    if (cost < bestc) { bestc = cost; bvx = cx[k]; bvy = cy[k]; }
                }
                gx[by][bx] = bvx; gy[by][bx] = bvy;
            }
        for (int by = y0; by < y1; by++) {
            memcpy(&fx[by][x0], &gx[by][x0], (size_t)(x1 - x0) * sizeof(float));
            memcpy(&fy[by][x0], &gy[by][x0], (size_t)(x1 - x0) * sizeof(float));
        }
    }
    (void)reg;
    for (int y = o->y0; y < o->y1; y++)
        for (int x = o->x0; x < o->x1; x++) {
            float u = (x - BS / 2 + 0.5f) / BS, v = (y - BS / 2 + 0.5f) / BS;
            int bx = (int)floorf(u), by = (int)floorf(v);
            u -= bx; v -= by;
            int xa = clampi(bx, x0, x1 - 1), xb = clampi(bx + 1, x0, x1 - 1), ya = clampi(by, y0, y1 - 1), yb = clampi(by + 1, y0, y1 - 1);
            o->px[y * LW + x] = (fx[ya][xa] * (1 - u) + fx[ya][xb] * u) * (1 - v) + (fx[yb][xa] * (1 - u) + fx[yb][xb] * u) * v;
            o->py[y * LW + x] = (fy[ya][xa] * (1 - u) + fy[ya][xb] * u) * (1 - v) + (fy[yb][xa] * (1 - u) + fy[yb][xb] * u) * v;
        }
}

/* ---------------------------------------------------------------- per-pixel maps */
static int32_t *map_eo, *map_ez, *map_ec, *map_en;     /* box-filtered errors */
static int32_t *raw, *tmp;

static void box(int32_t *dst, int w, int h) {          /* raw -> dst, 5x5 sums */
    for (int y = 0; y < h; y++) {
        const int32_t *p = raw + y * w;
        int32_t *q = tmp + y * w;
        for (int x = 0; x < w; x++) {
            int32_t s = 0;
            for (int k = x - 2; k <= x + 2; k++) s += p[clampi(k, 0, w - 1)];
            q[x] = s;
        }
    }
    for (int x = 0; x < w; x++) {
        int32_t s = 0;
        for (int k = -2; k <= 2; k++) s += tmp[clampi(k, 0, h - 1) * w + x];
        for (int y = 0; y < h; y++) {
            dst[y * w + x] = s;
            s += tmp[clampi(y + 3, 0, h - 1) * w + x] - tmp[clampi(y - 2, 0, h - 1) * w + x];
        }
    }
}

enum { K_KEEP, K_OBJ, K_FROM_C, K_FROM_N, K_FADE };
static uint8_t *kind2;
static uint8_t *kind;           /* of each pixel of the object's area */

/* an undecided pixel: the picture in which it stands out less from its
   surroundings (leftover outline pixels stand out) */
static uint8_t plainer(const Motion *m, int x, int y) {
    int s = 0;
    for (int j = -2; j <= 2; j++)
        for (int i = -2; i <= 2; i++) {
            int k = clampi(y + j, 0, H - 1) * LW + clampi(x + i, 0, LW - 1);
            s += m->lc[k] + m->ln[k];
        }
    return abs(m->lc[y * LW + x] * 50 - s) <= abs(m->ln[y * LW + x] * 50 - s) ? K_FROM_C : K_FROM_N;
}

/* A changed pixel the object does not cover at this phase: it is there in one of
   the pictures only.  With unchanged background behind it (against the motion)
   the object has just left, so N shows the background; with it ahead the
   object is yet to come, so C does. */
static uint8_t uncovered(const Motion *m, int x, int y, float vx, float vy) {
    float len = sqrtf(vx * vx + vy * vy);
    if (len >= 0.3f) {
        vx /= len; vy /= len;
        for (int d = 1; d <= 20; d++) {
            int bk = !m->mask[clampi(iround(y - d * vy), 0, H - 1) * LW + clampi(iround(x - d * vx), 0, LW - 1)];
            int ah = !m->mask[clampi(iround(y + d * vy), 0, H - 1) * LW + clampi(iround(x + d * vx), 0, LW - 1)];
            if (bk != ah) return bk ? K_FROM_N : K_FROM_C;
            if (bk) break;
        }
    }
    return plainer(m, x, y);
}

/* classify the object's pixels at phase a; returns the percentage of its
   changed pixels that its motion explains */
static int classify(Motion *m, int reg, const Object *o, float a) {
    int w = o->x1 - o->x0, h = o->y1 - o->y0;
    const uint8_t *lc = m->lc, *ln = m->ln;
    int rx0 = region[reg].x0, rx1 = region[reg].x1 - 1, ry0 = region[reg].y0, ry1 = region[reg].y1 - 1;
#define EACH(expr) \
    for (int j = 0; j < h; j++) { \
        int y = o->y0 + j; \
        for (int i = 0; i < w; i++) { \
            int x = o->x0 + i; \
            float vx = o->px[y * LW + x], vy = o->py[y * LW + x]; \
            (void)vx; (void)vy; \
            int d = (expr); \
            raw[j * w + i] = d < 0 ? -d : d; \
        } \
    }
#define AT(l, fx, fy) (l[clampi(iround(fy), ry0, ry1) * LW + clampi(iround(fx), rx0, rx1)])
    EACH(AT(lc, x - a * vx, y - a * vy) - AT(ln, x + (1 - a) * vx, y + (1 - a) * vy)) box(map_eo, w, h);
    EACH(lc[y * LW + x] - ln[y * LW + x]) box(map_ez, w, h);
    /* does C (N) show the object here?  Judged by the overall motion, which also
       holds just outside the object */
#define OX (o->ax + o->bx * x + o->cx * y)
#define OY (o->ay + o->by * x + o->cy * y)
    EACH(lc[y * LW + x] - AT(ln, x + OX, y + OY)) box(map_ec, w, h);
    EACH(ln[y * LW + x] - AT(lc, x - OX, y - OY)) box(map_en, w, h);
    int nchg = 0, nexp = 0;
    const int small = 25 * 5;
    for (int j = 0; j < h; j++) {
        int y = o->y0 + j;
        for (int i = 0; i < w; i++) {
            int x = o->x0 + i, k = j * w + i;
            int eo = map_eo[k], ez = map_ez[k];
            float vx = o->px[y * LW + x], vy = o->py[y * LW + x];
            /* the object passes here if it covers both places it would come from and go to */
            int on = AT(m->mask, x - a * vx, y - a * vy) && AT(m->mask, x + (1 - a) * vx, y + (1 - a) * vy);
            uint8_t t;
            if (on && (eo <= ez + ez / 4 || eo <= small * 2)) t = eo <= ez / 2 || eo <= small ? K_OBJ : K_FADE + 1;
            else if (ez <= small && abs(lc[y * LW + x] - ln[y * LW + x]) <= 8) t = K_KEEP;
            else {
                int ec = map_ec[k], en = map_en[k];
                if (ec > small * 2 && en > small * 2 && ec > ez / 2 && en > ez / 2) t = K_FADE;
                else t = uncovered(m, x, y, vx, vy);
            }
            int chg = ez > small;
            if (chg) { nchg++; if (t != K_FADE && t != K_FADE + 1) nexp++; }
            if (t == K_FADE + 1) t = K_OBJ;       /* poorly explained, but still the best guess */
            kind[k] = t;
        }
    }
    /* no speckles: a pixel between object pixels belongs to the object, a lone one does not */
    memcpy(kind2, kind, (size_t)w * h);
    for (int j = 2; j < h - 2; j++)
        for (int i = 2; i < w - 2; i++) {
            int n[5] = {0}, k = j * w + i, t = kind2[k];
            for (int q = -2; q <= 2; q++)
                for (int p = -2; p <= 2; p++) n[kind2[(j + q) * w + i + p]]++;
            if (t != K_OBJ && n[K_OBJ] >= 15) kind[k] = K_OBJ;
            else if (t == K_OBJ && n[K_OBJ] <= 8) kind[k] = uncovered(m, o->x0 + i, o->y0 + j, o->px[(o->y0 + j) * LW + o->x0 + i], o->py[(o->y0 + j) * LW + o->x0 + i]);
            /* uncovered background: one picture for the whole patch */
            else if (t == K_FROM_C && n[K_FROM_N] > n[K_FROM_C]) kind[k] = K_FROM_N;
            else if (t == K_FROM_N && n[K_FROM_C] > n[K_FROM_N]) kind[k] = K_FROM_C;
            else if (t == K_FADE && n[K_FROM_C] + n[K_FROM_N] > 8) kind[k] = n[K_FROM_C] > n[K_FROM_N] ? K_FROM_C : K_FROM_N;
        }
    return nchg ? nexp * 100 / nchg : 0;
}

/* load the pair C -> N and estimate motion; returns 1 if it is worth interpolating */
int motion_set_pair(int reg, const uint32_t *c, const uint32_t *n, const Rect *skip, int nskip) {
    Motion *m = &ctx[reg];
    if (!m->c) {
        m->c = (uint32_t *)malloc(W * H * 4); m->n = (uint32_t *)malloc(W * H * 4);
        m->lc = (uint8_t *)malloc(LW * H); m->ln = (uint8_t *)malloc(LW * H);
        m->lc2 = (uint8_t *)malloc(LW * H / 4); m->ln2 = (uint8_t *)malloc(LW * H / 4);
        m->mask = (uint8_t *)malloc(LW * H);
        for (int i = 0; i < MAXOBJ; i++) { m->o[i].px = (float *)malloc(LW * H * sizeof(float)); m->o[i].py = (float *)malloc(LW * H * sizeof(float)); }
    }
    if (!map_eo) {
        map_eo = (int32_t *)malloc(LW * H * 4); map_ez = (int32_t *)malloc(LW * H * 4);
        map_ec = (int32_t *)malloc(LW * H * 4); map_en = (int32_t *)malloc(LW * H * 4);
        raw = (int32_t *)malloc(LW * H * 4); tmp = (int32_t *)malloc(LW * H * 4);
        kind = (uint8_t *)malloc(LW * H);
        kind2 = (uint8_t *)malloc(LW * H);
    }
    memcpy(m->c, c, W * H * 4);
    memcpy(m->n, n, W * H * 4);
    /* the hand cursor is not part of the motion */
    for (int i = 0; i < nskip; i++)
        for (int y = clampi(skip[i].y0, 0, H); y < clampi(skip[i].y1, 0, H); y++)
            for (int x = clampi(skip[i].x0, 0, W); x < clampi(skip[i].x1, 0, W); x++) m->n[y * W + x] = m->c[y * W + x];
    make_luma(m->c, m->lc, m->lc2);
    make_luma(m->n, m->ln, m->ln2);
    m->valid = 0;
    m->nobj = 0;
    memset(m->mask, 0, LW * H);
    for (int y = region[reg].y0; y < region[reg].y1; y++)
        for (int x = region[reg].x0; x < region[reg].x1; x++) {
            int d = m->lc[y * LW + x] - m->ln[y * LW + x];
            if ((d > 6 || d < -6) && in_region(reg, x, y))
                for (int yy = clampi(y - 2, 0, H - 1); yy <= clampi(y + 2, 0, H - 1); yy++)
                    memset(m->mask + yy * LW + clampi(x - 2, 0, LW - 5), 1, 5);
        }
    int nchanged = 0;
    for (int by = 0; by < BH; by++)
        for (int bx = 0; bx < BW; bx++) {
            int x = bx * BS, y = by * BS;
            m->vx[by][bx] = m->vy[by][bx] = 0;
            m->obj[by][bx] = 0;
            m->changed[by][bx] = 0;
            if (x + BS <= region[reg].x0 || x >= region[reg].x1 || y + BS <= region[reg].y0 || y >= region[reg].y1) continue;
            int z = sad(m->lc, m->ln, LW, H, x, y, 0, 0, BS, 1 << 30);
            if (z <= BS * BS * 2) continue;
            m->changed[by][bx] = 1;
            nchanged++;
            /* coarse: half resolution, +-12 x +-8 (= +-24 x +-16 full) */
            int best = 1 << 30, bdx = 0, bdy = 0;
            for (int dy = -8; dy <= 8; dy++)
                for (int dx = -12; dx <= 12; dx++) {
                    int sv = sad(m->lc2, m->ln2, LW / 2, H / 2, x / 2, y / 2, dx, dy, BS / 2, best + 1);
                    if (sv < best || (sv == best && abs(dx) + abs(dy) < abs(bdx) + abs(bdy))) { best = sv; bdx = dx; bdy = dy; }
                }
            /* fine: full resolution, +-2 around the coarse vector */
            best = 1 << 30;
            int fx = 0, fy = 0;
            for (int dy = 2 * bdy - 2; dy <= 2 * bdy + 2; dy++)
                for (int dx = 2 * bdx - 2; dx <= 2 * bdx + 2; dx++) {
                    int sv = sad(m->lc, m->ln, LW, H, x, y, dx, dy, BS, best);
                    if (sv < best) { best = sv; fx = dx; fy = dy; }
                }
            m->vx[by][bx] = (int16_t)fx;
            m->vy[by][bx] = (int16_t)fy;
            /* a match that is hardly better than not moving says nothing */
            if (best * 2 > z) m->changed[by][bx] = 2;
        }
    /* a cut or a full redraw is not motion */
    int area = (region[reg].x1 - region[reg].x0) * (region[reg].y1 - region[reg].y0) / (BS * BS);
    if (!nchanged || (reg == MOTION_FULL && nchanged * 10 > area * 7)) return 0;
    /* objects: connected groups of changed blocks (gaps of one block allowed) */
    static int stack[BW * BH];
    static Vec vec[BW * BH];
    for (int s0 = 0; s0 < BW * BH && m->nobj < MAXOBJ; s0++) {
        if (!m->changed[s0 / BW][s0 % BW] || m->obj[s0 / BW][s0 % BW]) continue;
        int sp = 0, nv = 0, nb = 0, x0 = BW, y0 = BH, x1 = -1, y1 = -1;
        stack[sp++] = s0;
        m->obj[s0 / BW][s0 % BW] = 255;
        while (sp) {
            int k = stack[--sp], ky = k / BW, kx = k % BW;
            nb++;
            if (kx < x0) x0 = kx;
            if (kx > x1) x1 = kx;
            if (ky < y0) y0 = ky;
            if (ky > y1) y1 = ky;
            if (m->changed[ky][kx] == 1) {
                Vec *v = &vec[nv++];
                v->x = kx * BS + BS / 2; v->y = ky * BS + BS / 2;
                v->vx = m->vx[ky][kx]; v->vy = m->vy[ky][kx];
                v->w = 1;
            }
            for (int dy = -2; dy <= 2; dy++)
                for (int dx = -2; dx <= 2; dx++) {
                    int yy = ky + dy, xx = kx + dx;
                    if (yy < 0 || yy >= BH || xx < 0 || xx >= BW || m->obj[yy][xx] || !m->changed[yy][xx]) continue;
                    m->obj[yy][xx] = 255;
                    stack[sp++] = yy * BW + xx;
                }
        }
        if (nv < 1 || nv * 5 < nb) continue;
        Object *o = &m->o[m->nobj];
        fit(o, vec, nv);
        o->x0 = clampi((x0 - 1) * BS, region[reg].x0, region[reg].x1);
        o->y0 = clampi((y0 - 1) * BS, region[reg].y0, region[reg].y1);
        o->x1 = clampi((x1 + 2) * BS, region[reg].x0, region[reg].x1);
        o->y1 = clampi((y1 + 2) * BS, region[reg].y0, region[reg].y1);
        if (o->x1 <= o->x0 || o->y1 <= o->y0) continue;
        float mx = o->ax + o->bx * (o->x0 + o->x1) / 2 + o->cx * (o->y0 + o->y1) / 2;
        float my = o->ay + o->by * (o->x0 + o->x1) / 2 + o->cy * (o->y0 + o->y1) / 2;
        float def = fabsf(o->bx) + fabsf(o->cx) + fabsf(o->by) + fabsf(o->cy);
        if (def > 1.2f) continue;
        local_motion(m, reg, o, 255);
        int ex = classify(m, reg, o, 0.5f);
        /* the faster it moves, the more visible a wrong guess is */
        float speed = fabsf(mx) + fabsf(my);
        int need = (reg == 0 ? 15 : 30) + (int)(speed > 2 ? 9 * (speed - 2) : 0);
        if (ex < (need > 85 ? 85 : need)) continue;
        m->nobj++;
    }
    m->valid = m->nobj > 0;
    return m->valid;
}

static inline uint32_t blend(uint32_t c, uint32_t n, int wn) {   /* wn: 0-256 */
    int wc = 256 - wn;
    uint32_t r = ((((c >> 16) & 255) * wc + ((n >> 16) & 255) * wn) >> 8);
    uint32_t g = ((((c >> 8) & 255) * wc + ((n >> 8) & 255) * wn) >> 8);
    uint32_t b = (((c & 255) * wc + (n & 255) * wn) >> 8);
    return (r << 16) | (g << 8) | b;
}

/* render the in-between at phase a into out (which holds the real picture);
   returns 1 if the picture was changed */
int motion_render(int reg, uint32_t *out, float a, const Rect *skip, int nskip) {
    Motion *m = &ctx[reg];
    if (!m->valid || a <= 0.0f || a >= 1.0f) return 0;
    int wn = (int)(a * 256 + 0.5f);
    int rx0 = region[reg].x0 * 2, rx1 = region[reg].x1 * 2 - 1, ry0 = region[reg].y0, ry1 = region[reg].y1 - 1;
    for (int q = 0; q < m->nobj; q++) {
        Object *o = &m->o[q];
        classify(m, reg, o, a);
        int w = o->x1 - o->x0;
        for (int y = o->y0; y < o->y1; y++)
            for (int x = o->x0; x < o->x1; x++) {
                int k = kind[(y - o->y0) * w + x - o->x0];
                if (k == K_KEEP || !in_region(reg, x, y)) continue;
                if (in_rects(skip, nskip, x * 2, y, x * 2 + 2, y + 1)) continue;     /* the hand cursor */
                for (int t = 2 * x; t < 2 * x + 2; t++) {
                    int p = y * W + t;
                    if (k == K_FROM_C) out[p] = m->c[p];
                    else if (k == K_FROM_N) out[p] = m->n[p];
                    else if (k == K_FADE) out[p] = blend(m->c[p], m->n[p], wn);
                    else {
                        float vx = 2 * o->px[y * LW + x], vy = o->py[y * LW + x];
                        int xc = clampi(iround(t - a * vx), rx0, rx1), yc = clampi(iround(y - a * vy), ry0, ry1);
                        int xn = clampi(iround(t + (1 - a) * vx), rx0, rx1), yn = clampi(iround(y + (1 - a) * vy), ry0, ry1);
                        out[p] = blend(m->c[yc * W + xc], m->n[yn * W + xn], wn);
                    }
                }
            }
    }
    return 1;
}
