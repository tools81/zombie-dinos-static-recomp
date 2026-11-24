/* CPU support: dispatcher, ALU helpers that are too large to inline. */
#include "recomp.h"
#include "runtime.h"
#include "state.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

M68K m68k;
uint8_t *RAM;
uint32_t a5_expected;

static RecompFn *dispatch;      /* indexed by (pc - recomp_code_lo) / 2 */
static uint32_t dispatch_n;

void cpu_init(void) {
    dispatch_n = (recomp_code_hi - recomp_code_lo) / 2 + 1;
    dispatch = (RecompFn *)calloc(dispatch_n, sizeof(RecompFn));
    for (int i = 0; i < recomp_num_entries; i++) {
        uint32_t a = recomp_entries[i].addr;
        dispatch[(a - recomp_code_lo) / 2] = recomp_entries[i].fn;
    }
}

const char *func_name(uint32_t pc) {
    /* nearest MacsBug name at or below pc */
    const char *best = "?";
    uint32_t ba = 0;
    for (int i = 0; i < recomp_num_names; i++) {
        if (recomp_names[i].addr <= pc && recomp_names[i].addr >= ba) {
            ba = recomp_names[i].addr;
            best = recomp_names[i].name;
        }
    }
    return best;
}

/* Resolve pc to a host function; follows "jmp abs.l" stubs in the A5 jump
   table that live in RAM. */
static RecompFn lookup(uint32_t *ppc) {
    uint32_t pc = *ppc;
    for (int hops = 0; hops < 4; hops++) {
        if (pc >= recomp_code_lo && pc < recomp_code_hi && !(pc & 1)) {
            RecompFn f = dispatch[(pc - recomp_code_lo) / 2];
            if (f) { *ppc = pc; return f; }
            return NULL;
        }
        if (RW(pc) == 0x4EF9) {          /* jmp abs.l (jump table stub): count it as executed */
            pc = RL(pc + 2);
            m68k.cyc++;
            continue;
        }
        break;
    }
    *ppc = pc;
    return NULL;
}

void fatal(const char *fmt, ...);

uint32_t recomp_bad_entry(uint32_t pc, uint32_t func) {
    fatal("bad entry %06x into function %06x (%s)", pc, func, func_name(func));
    return 0;
}

uint32_t interp_run(uint32_t pc);

int cpu_has_code(uint32_t pc) {
    return pc >= recomp_code_lo && pc < recomp_code_hi && !(pc & 1) && dispatch[(pc - recomp_code_lo) / 2];
}

/* code inside a recompiled module that the analyzer did not find: it runs in
   the interpreter, and each address is logged once (add it to extra_entries) */
static void undiscovered(uint32_t pc) {
    static uint32_t seen[64];
    static int n;
    for (int i = 0; i < n; i++) if (seen[i] == pc) return;
    if (n < 64) seen[n++] = pc;
    LOG("undiscovered code address %06x (%s), interpreting", pc, func_name(pc));
}

uint32_t m68k_call(uint32_t target) {
    extern int g_interp_all;
    uint32_t pc = target;
    if (g_interp_all) return interp_run(pc);
    RecompFn f = lookup(&pc);
    if (!f) {
        if (pc >= recomp_code_lo && pc < recomp_code_hi)
            undiscovered(pc);
        return interp_run(pc);
    }
    return f(pc);
}

void a5_check(uint32_t addr) {
    fatal("A5 changed (%08x, expected %08x) at %06x", A(5), a5_expected, addr);
}

uint32_t m68k_exception(int vec, uint32_t addr) {
    fatal("CPU exception %d at %06x (%s)", vec, addr, func_name(addr));
    return 0;
}

uint32_t m68k_trap(int n, uint32_t func, uint32_t next) {
    fatal("unhandled trap #%d func %x at %06x", n, func, next);
    return next;
}

/* ---------------------------------------------------------------- run loop */
static int run_nesting;
int run_depth(void) { return run_nesting; }

void cpu_run_until(volatile int *flag) {
    extern int g_interp_all;
    run_nesting++;
    while (!*flag) {
        uint32_t pc = m68k.pc;
        RecompFn f = g_interp_all ? NULL : lookup(&pc);
        if (f) m68k.pc = f(pc);
        else if (!g_interp_all && pc >= recomp_code_lo && pc < recomp_code_hi)
            { undiscovered(pc); m68k.pc = interp_run(pc); }
        else m68k.pc = interp_run(pc);
        /* the scheduler runs only where code yielded (backward branches), so
           translated and interpreted execution see events at the same points */
        if (m68k.yield) {
            m68k.yield = 0;
            sched_service();
        }
        /* here no 68000 code is in progress on the native stack: the frame
           boundary may present, poll input, take or restore snapshots */
        if (g_field_ready) frame_boundary();
    }
    run_nesting--;
}

void cpu_run(void) {
    static volatile int never;
    cpu_run_until(&never);
}

/* ---------------------------------------------------------------- snapshots */
uint32_t mem_high_water(void);
void mem_set_high_water(uint32_t hw);

void cpu_state(StateIO *s) {
    ST(s, m68k);
    ST(s, a5_expected);
    /* memory above the allocator's high-water mark has never been used */
    uint32_t hw = mem_high_water(), now = hw;
    st_u32(s, &hw);
    if (s->loading && (hw > MEM_SIZE || s->err)) { s->err = 1; return; }
    if (s->loading && now > hw) memset(RAM + hw, 0, now - hw);
    st_bytes(s, RAM, hw);
    if (s->loading) mem_set_high_water(hw);
}
