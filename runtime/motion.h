/* Sprite interpolation (motion.c) */
#pragma once
#include <stdint.h>

#define MOTION_REGIONS 3       /* portrait, landscape window, whole screen */
#define MOTION_FULL    2

typedef struct { int x0, y0, x1, y1; } Rect;     /* frame texels (768 x 240) */

void motion_reset(void);
void motion_drop(int reg);
int  motion_differs(int reg, const uint32_t *a, const uint32_t *b, const Rect *skip, int nskip);
int  motion_set_pair(int reg, const uint32_t *c, const uint32_t *n, const Rect *skip, int nskip);
int  motion_render(int reg, uint32_t *out, float a, const Rect *skip, int nskip);
