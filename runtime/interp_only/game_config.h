/* copy of gen/game_config.h, used by the interpreter-only build */
#pragma once
#include <stdint.h>
#define GAME_TITLE "Zombie Dinos from Planet Zeltoid"
#define MAIN_MODULE "cdi_ZD"
#define MAIN_MODULE_BASE 0x100000u
#define MAIN_USES_A5 1
typedef struct { const char *name; uint32_t base; } FixedModule;
/* other recompiled modules: the runtime loads them at these addresses */
static const FixedModule game_fixed_modules[] = {
    {"cdi_bumper", 0xe0000u},
    {0, 0}
};
