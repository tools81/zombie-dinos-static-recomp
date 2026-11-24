/* High-level emulation of the OS-9/68K (CD-RTOS) kernel for a single process. */
#include "recomp.h"
#include "runtime.h"
#include "state.h"
#include "game_config.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#ifdef _WIN32
#include <io.h>
#endif

/* OS-9 error codes */
#define E_PTHFUL 0xC8
#define E_BPNUM_ 0xC9
#define E_BMODE_ 0xCB
#define E_MEMFUL 0xCF
#define E_UNKSVC_ 0xD0
#define E_EOF_   0xD3
#define E_PNNF_  0xD8
#define E_MNF    0xDD
#define E_NOCHLD 0xE2
#define E_IPRCID 0xE0

#define MODULE_BASE MAIN_MODULE_BASE   /* fixed load address of the recompiled main program */

/* modules that were statically recompiled for a fixed address (game.json) */
static uint32_t fixed_module_base(const char *name) {
    for (int i = 0; game_fixed_modules[i].name; i++)
        if (!_stricmp(game_fixed_modules[i].name, name)) return game_fixed_modules[i].base;
    return 0;
}

/* ------------------------------------------------------------ allocator */
typedef struct Block { uint32_t addr, size; struct Block *next; } Block;
static Block *free_list;
static uint32_t heap_hw = HEAP_BASE;     /* highest address ever handed out */
uint32_t mem_high_water(void) { return heap_hw; }
void mem_set_high_water(uint32_t hw) { heap_hw = hw; }

static void heap_init(void) {
    free_list = (Block *)calloc(1, sizeof(Block));
    free_list->addr = HEAP_BASE;
    free_list->size = HEAP_END - HEAP_BASE;
}

uint32_t mem_alloc(uint32_t size, uint32_t *actual) {
    size = (size + 15) & ~15u;
    if (size == 0) size = 16;
    for (Block **pp = &free_list; *pp; pp = &(*pp)->next) {
        Block *b = *pp;
        if (b->size >= size) {
            uint32_t a = b->addr;
            b->addr += size;
            b->size -= size;
            if (b->size == 0) { *pp = b->next; free(b); }
            memset(RAM + a, 0, size);
            if (a + size > heap_hw) heap_hw = a + size;
            if (actual) *actual = size;
            return a;
        }
    }
    return 0;
}

void mem_free(uint32_t addr, uint32_t size) {
    size = (size + 15) & ~15u;
    if (!addr || !size) return;
    Block *nb = (Block *)calloc(1, sizeof(Block));
    nb->addr = addr; nb->size = size;
    Block **pp = &free_list;
    while (*pp && (*pp)->addr < addr) pp = &(*pp)->next;
    nb->next = *pp;
    *pp = nb;
    /* coalesce */
    for (Block *b = free_list; b && b->next;) {
        if (b->addr + b->size == b->next->addr) {
            Block *n = b->next;
            b->size += n->size;
            b->next = n->next;
            free(n);
        } else b = b->next;
    }
}

/* ------------------------------------------------------------ signals */
#define MAXSIG 64
#define MAXPROC 4

typedef struct {
    uint32_t d[8], a[8], pc;
    uint8_t ccr, srh;
} Context;

/* Per-process kernel state.  Only one process runs at a time: a forked
   child runs to completion while its parent blocks in F$Wait. */
typedef struct {
    int used, pid;
    uint32_t q[MAXSIG];
    int nq;
    uint32_t handler, handler_a6;
    int mask;                       /* F$SigMask level + in-handler */
    Context saved[16];
    int nsaved;
    /* F$Sleep returns to the run loop after each scheduler event and is
       executed again; this remembers it */
    uint32_t block_pc;              /* address of the os9 instruction, 0 = none */
    int64_t block_until;            /* wake-up time (cycles) */
} Proc;
static Proc procs[MAXPROC];
static Proc *cur;

#define sigq (cur->q)
#define sig_pending_count (cur->nq)
#define icpt_pc (cur->handler)
#define icpt_a6 (cur->handler_a6)
#define sig_masked (cur->mask)
#define ctx_stack (cur->saved)
#define ctx_depth (cur->nsaved)

int os9_cur_pid(void) { return cur ? cur->pid : 1; }

static Proc *proc_by_pid(int pid) {
    for (int i = 0; i < MAXPROC; i++)
        if (procs[i].used && procs[i].pid == pid) return &procs[i];
    return NULL;
}

void sig_send_pid(int pid, uint32_t sig) {
    Proc *p = proc_by_pid(pid);
    if (!p) return;
    if (p->nq < MAXSIG) p->q[p->nq++] = sig;
    TRACE("signal %u queued for pid %d", sig, pid);
}

void sig_send(uint32_t sig) { sig_send_pid(os9_cur_pid(), sig); }

static void push_context(uint32_t resume_pc) {
    if (ctx_depth >= 16) fatal("signal context overflow");
    Context *c = &ctx_stack[ctx_depth++];
    memcpy(c->d, m68k.d, sizeof c->d);
    memcpy(c->a, m68k.a, sizeof c->a);
    c->pc = resume_pc;
    c->ccr = GET_CCR();
    c->srh = m68k.srh;
}

/* Deliver one pending signal: m68k.pc must hold the resume address. */
int sig_deliver(void) {
    if (!sig_pending_count || sig_masked || !icpt_pc) return 0;
    uint32_t sig = sigq[0];
    memmove(sigq, sigq + 1, (--sig_pending_count) * sizeof(uint32_t));
    push_context(m68k.pc);
    D(1) = sig;
    A(6) = icpt_a6;
    A(7) -= 0x50;           /* the kernel builds a frame on the user stack */
    m68k.pc = icpt_pc;
    sig_masked++;
    TRACE("deliver signal %u -> %06x", sig, icpt_pc);
    return 1;
}

static uint32_t do_rte(void) {
    if (!ctx_depth) fatal("F$RTE without signal context");
    Context *c = &ctx_stack[--ctx_depth];
    memcpy(m68k.d, c->d, sizeof c->d);
    memcpy(m68k.a, c->a, sizeof c->a);
    SET_CCR(c->ccr);
    m68k.srh = c->srh;
    if (sig_masked > 0) sig_masked--;
    return c->pc;
}

/* ------------------------------------------------------------ alarms */
typedef struct { int used, pid; uint32_t id, sig; int64_t when, period; } Alarm;
static Alarm alarms[16];
static uint32_t next_alarm_id = 1;
#define TICKS_PER_SEC 100
#define TICK_CYC (CPU_IPS / TICKS_PER_SEC)

static void alarms_check(void) {
    int64_t now = m68k.cyc;
    for (int i = 0; i < 16; i++) {
        Alarm *al = &alarms[i];
        if (!al->used || now < al->when) continue;
        sig_send_pid(al->pid, al->sig);
        if (al->period > 0) al->when += al->period;
        else al->used = 0;
    }
}

static int64_t alarms_next(void) {
    int64_t n = INT64_MAX;
    for (int i = 0; i < 16; i++)
        if (alarms[i].used && alarms[i].when < n) n = alarms[i].when;
    return n;
}

/* ------------------------------------------------------------ scheduler */
static int64_t next_field, next_sector;
static int64_t field_period, sector_period;

void sched_init(void) {
    field_period = CPU_IPS / FIELD_RATE;
    sector_period = CPU_IPS / SECTOR_RATE;
    next_field = field_period;
    next_sector = sector_period;
    m68k.cyc_limit = next_sector < next_field ? next_sector : next_field;
}

int64_t sched_now(void) { return m68k.cyc; }

int64_t audio_next_event(void);
void audio_event(void);

static void run_events(void) {
    for (;;) {
        int progressed = 0;
        if (m68k.cyc >= audio_next_event()) { audio_event(); progressed = 1; }
        if (m68k.cyc >= next_sector) { next_sector += sector_period; cdfm_sector_tick(); progressed = 1; }
        if (m68k.cyc >= next_field) { next_field += field_period; video_field(); audio_field_tick(); progressed = 1; }
        alarms_check();
        if (!progressed) break;
    }
    int64_t lim = next_field;
    if (next_sector < lim) lim = next_sector;
    int64_t an = alarms_next();
    if (an < lim) lim = an;
    an = audio_next_event();
    if (an < lim) lim = an;
    m68k.cyc_limit = lim;
}

void sched_service(void) {
    run_events();
    if (sig_pending_count && !sig_masked && icpt_pc) {
        sig_deliver();
        /* make translated code yield again soon so nested deliveries work */
    }
}

/* Called from blocking syscalls: advance time to the next event. */
void sched_wait_event(void) {
    if (m68k.cyc < m68k.cyc_limit) m68k.cyc = m68k.cyc_limit;
    run_events();
}

/* ------------------------------------------------------------ paths */
enum { P_NONE, P_CONS, P_CD, P_NVR, P_VIDEO, P_PTR, P_AUDIO, P_NULL, P_KB };
typedef struct {
    int type;
    int handle;          /* CDFM handle */
    int nvr;             /* NVR file index (-1: the /nvr directory) */
    uint32_t pos;        /* NVR file position */
    char name[128];
} Path;
#define MAXPATH 32
static Path paths[MAXPATH];
char g_data_dir[512] = ".";

static void read_cstr(uint32_t a, char *out, int max) {
    int i = 0;
    for (; i < max - 1; i++) {
        uint8_t c = RB(a + i);
        if (c == 0 || c == '\r' || c == '\n' || c == ' ') break;
        out[i] = (char)c;
    }
    out[i] = 0;
}

static int path_alloc(void) {
    for (int i = 3; i < MAXPATH; i++)
        if (paths[i].type == P_NONE) return i;
    return -1;
}

/* ------------------------------------------------------------ NVRAM
   NVRAM files live in memory, so snapshots include them.  They are loaded
   from saves/nvr_<name> at start and written back by nvr_flush(), which the
   frame loop calls only on the real timeline (never for run-ahead frames). */
#define NVR_SIZE 8192
#define MAXNVR 24
#define E_FULL 0xF8
typedef struct {
    int used, dirty, removed;       /* removed: delete the host file */
    char name[32];
    uint32_t size;
    uint8_t data[NVR_SIZE];
} NvrFile;
static NvrFile nvr[MAXNVR];

static void nvr_host_path(const char *name, char *out, size_t n) {
    snprintf(out, n, "%s/nvr_%s", g_data_dir, name);
}

static int nvr_find(const char *name) {
    for (int i = 0; i < MAXNVR; i++)
        if (nvr[i].used && !_stricmp(nvr[i].name, name)) return i;
    return -1;
}

static int nvr_create(const char *name) {
    int i = nvr_find(name);
    if (i < 0) {
        for (i = 0; i < MAXNVR && nvr[i].used; i++) {}
        if (i == MAXNVR) return -1;
        memset(&nvr[i], 0, sizeof nvr[i]);
        nvr[i].used = 1;
        snprintf(nvr[i].name, sizeof nvr[i].name, "%s", name);
    }
    nvr[i].size = 0;
    nvr[i].removed = 0;
    nvr[i].dirty = 1;
    return i;
}

/* the CSD is not a game file: it is neither saved nor counted */
static int nvr_is_csd(int i) { return !_stricmp(nvr[i].name, "csd"); }

static uint32_t nvr_free_bytes(void) {
    long used = 0;
    for (int i = 0; i < MAXNVR; i++)
        if (nvr[i].used && !nvr_is_csd(i)) used += (long)nvr[i].size + 64;
    return used >= NVR_SIZE ? 0 : (uint32_t)(NVR_SIZE - used);
}

static void nvr_load_host_files(void) {
#ifdef _WIN32
    char pat[600];
    snprintf(pat, sizeof pat, "%s/nvr_*", g_data_dir);
    struct _finddata_t fd;
    intptr_t h = _findfirst(pat, &fd);
    if (h == -1) return;
    do {
        const char *name = fd.name + 4;
        if (!_stricmp(name, "csd") || strlen(name) >= 32) continue;
        char hp[600];
        nvr_host_path(name, hp, sizeof hp);
        FILE *f = fopen(hp, "rb");
        if (!f) continue;
        int i = nvr_create(name);
        if (i >= 0) {
            nvr[i].size = (uint32_t)fread(nvr[i].data, 1, NVR_SIZE, f);
            nvr[i].dirty = 0;
        }
        fclose(f);
    } while (_findnext(h, &fd) == 0);
    _findclose(h);
#endif
}

/* after loading a quicksave the host files must match the restored contents */
void nvr_mark_all_dirty(void) {
    for (int i = 0; i < MAXNVR; i++) if (nvr[i].used) nvr[i].dirty = 1;
}

/* write changed NVRAM files to the host (real timeline only) */
void nvr_flush(void) {
    for (int i = 0; i < MAXNVR; i++) {
        NvrFile *n = &nvr[i];
        char hp[600];
        if (n->removed) {
            nvr_host_path(n->name, hp, sizeof hp);
            remove(hp);
            memset(n, 0, sizeof *n);
            continue;
        }
        if (!n->used || !n->dirty || nvr_is_csd(i)) continue;
        nvr_host_path(n->name, hp, sizeof hp);
        FILE *f = fopen(hp, "wb");
        if (f) { fwrite(n->data, 1, n->size, f); fclose(f); }
        n->dirty = 0;
    }
}

/* returns path number or -error */
static int do_open(const char *pname, int mode, int create) {
    char lower[128];
    size_t L = strlen(pname);
    if (L >= sizeof lower) L = sizeof lower - 1;
    for (size_t i = 0; i <= L; i++) lower[i] = (char)tolower((unsigned char)pname[i]);
    lower[L] = 0;
    int p = path_alloc();
    if (p < 0) return -E_PTHFUL;
    Path *P = &paths[p];
    memset(P, 0, sizeof *P);
    snprintf(P->name, sizeof P->name, "%s", pname);

    const char *nm = lower;
    if (!strncmp(nm, "/nvr", 4)) {
        const char *f = nm + 4;
        if (*f == '/') f++;
        P->nvr = -1;
        if (!*f) { P->type = P_NVR; return p; }     /* directory itself */
        int i = create ? nvr_create(f) : nvr_find(f);
        if (i < 0) { P->type = P_NONE; return create ? -E_FULL : -E_PNNF_; }
        P->nvr = i;
        P->pos = 0;
        P->type = P_NVR;
        return p;
    }
    if (!strncmp(nm, "/vid", 4) || !strncmp(nm, "/video", 6) || !strncmp(nm, "/dev/vid", 8)) {
        P->type = P_VIDEO;
        return p;
    }
    if (!strncmp(nm, "/ptr", 4) || !strncmp(nm, "/cdikeys", 8) || !strncmp(nm, "/pt", 3)) {
        P->type = P_PTR;
        return p;
    }
    if (!strncmp(nm, "/ap", 3) || !strncmp(nm, "/audio", 6) || !strncmp(nm, "/adpcm", 6)) {
        P->type = P_AUDIO;
        return p;
    }
    if (!strncmp(nm, "/nil", 4) || !strncmp(nm, "/term", 5) || !strncmp(nm, "/t1", 3)) {
        P->type = P_NULL;
        return p;
    }
    if (!strncmp(nm, "/kb", 3)) {
        P->type = P_KB;
        return p;
    }
    /* CD files: "/cd/name" or plain "name" relative to /cd */
    const char *f = nm;
    if (!strncmp(f, "/cd", 3)) {
        f += 3;
        if (*f == '/') f++;
        if (!*f) { P->type = P_AUDIO; return p; }   /* "/cd" device itself: audio functions */
    } else if (*f == '/') {
        LOG("I$Open of unknown device '%s'", pname);
        P->type = P_NONE;
        return -E_PNNF_;
    }
    int h = cdfm_open(f, mode);
    if (h < 0) { P->type = P_NONE; return -E_PNNF_; }
    P->type = P_CD;
    P->handle = h;
    return p;
}

/* ------------------------------------------------------------ modules */
typedef struct { char name[32]; uint32_t addr; uint32_t size; int links; } ModEntry;
static ModEntry mods[16];
static int nmods;

static uint32_t mod_register(const char *name, uint32_t addr, uint32_t size) {
    ModEntry *m = &mods[nmods++];
    snprintf(m->name, sizeof m->name, "%s", name);
    m->addr = addr; m->size = size; m->links = 1;
    return addr;
}

static ModEntry *mod_find(const char *name) {
    for (int i = 0; i < nmods; i++)
        if (!_stricmp(mods[i].name, name)) return &mods[i];
    return NULL;
}

static ModEntry *mod_load_from_disc(const char *name) {
    ModEntry *m = mod_find(name);
    if (m) { m->links++; return m; }
    const DiscFile *df = disc_find(name);
    if (!df) return NULL;
    uint8_t *buf; uint32_t len;
    if (disc_read_file(df, &buf, &len)) return NULL;
    /* modules that were statically recompiled must sit at their fixed address */
    uint32_t a = fixed_module_base(name);
    if (!a) a = mem_alloc(len, NULL);
    memcpy(RAM + a, buf, len);
    free(buf);
    /* a file may contain several modules; register each */
    uint32_t off = 0;
    ModEntry *first = NULL;
    while (off + 0x30 <= len && RW(a + off) == 0x4AFC) {
        uint32_t sz = RL(a + off + 4);
        char nm[32];
        read_cstr(a + off + RL(a + off + 12), nm, sizeof nm);
        mod_register(nm, a + off, sz);
        if (!first) first = &mods[nmods - 1];
        LOG("loaded module '%s' at %06x size %x", nm, a + off, sz);
        off += sz;
        off = (off + 1) & ~1u;   /* modules are word aligned */
        if (!sz) break;
    }
    return first;
}

/* Set up a process for program module mb as the OS-9 kernel's F$Fork does:
   allocate static storage + stack + parameters, copy idata, apply irefs and
   load the initial registers into m68k. */
static uint32_t create_process_state(uint32_t mb, int pid, uint32_t params, uint32_t paramsz,
                                     uint32_t extra_mem, int npaths) {
    uint32_t execoff = RL(mb + 0x30);
    uint32_t memsize = RL(mb + 0x38), stacksize = RL(mb + 0x3C);
    uint32_t idata = RL(mb + 0x40), irefs = RL(mb + 0x44);
    uint32_t psz = (paramsz + 15) & ~15u;
    uint32_t total = memsize + stacksize + extra_mem + psz + 0x100;
    uint32_t data = mem_alloc(total, &total);
    if (!data) fatal("out of memory creating process");
    /* initialized data */
    uint32_t doff = RL(mb + idata), cnt = RL(mb + idata + 4);
    memcpy(RAM + data + doff, RAM + mb + idata + 8, cnt);
    /* irefs: two tables (code-relative, data-relative) */
    uint32_t o = mb + irefs;
    for (int t = 0; t < 2; t++) {
        for (;;) {
            uint32_t hi = RW(o), n = RW(o + 2);
            o += 4;
            if (!n) break;
            for (uint32_t k = 0; k < n; k++) {
                uint32_t off = (hi << 16) | RW(o);
                o += 2;
                WL(data + off, RL(data + off) + (t == 0 ? mb : data));
            }
        }
    }
    int64_t cyc = m68k.cyc, lim = m68k.cyc_limit;
    memset(&m68k, 0, sizeof m68k);
    m68k.cyc = cyc; m68k.cyc_limit = lim;
    uint32_t top = data + total;
    uint32_t pa = top - psz - 0x10;          /* parameter area at the top */
    for (uint32_t i = 0; i < paramsz; i++) WB(pa + i, RB(params + i));
    D(0) = (uint32_t)pid;
    D(1) = 0;               /* group/user */
    D(2) = 128;             /* priority */
    D(3) = (uint32_t)npaths;
    D(5) = paramsz;
    D(6) = total;
    A(1) = top;
    A(3) = mb;
    A(5) = pa;
    A(6) = data + 0x8000;
    A(7) = A(5);
    m68k.pc = mb + execoff;
    return data;
}

void os9_load_main_module(const uint8_t *mod, uint32_t len) {
    memcpy(RAM + MODULE_BASE, mod, len);
    uint32_t mb = MODULE_BASE;
    char nm[32];
    read_cstr(mb + RL(mb + 12), nm, sizeof nm);
    mod_register(nm, mb, RL(mb + 4));
    uint32_t data = create_process_state(mb, 1, 0, 0, 0, 3);
    /* MPW-style programs: the startup code sets a5 = data + 8 + idata[1] */
    a5_expected = MAIN_USES_A5 ? data + 8 + RL(data + 4) : 0;
    LOG("main module '%s' at %06x, data %06x, a5=%06x, entry %06x", nm, mb, data, a5_expected, m68k.pc);
}

static void write_csd(void) {
    /* Device Status Descriptors of a typical NTSC base-case player */
    static const char *dsd[] = {
        "0::RV=\"1.1\":", "1:/cd:SP#27:", "2:/ap:AD#27:", "3:/vid:PL#2:LI=\"525\":TV:",
        "4:/nvr:SZ#8:", "5:/ptr:CL=\"d\":", "6:CDDRIVE:SK#3:", "8:MONITOR:LI=\"525\":",
    };
    int i = nvr_create("csd");
    for (size_t k = 0; k < sizeof dsd / sizeof dsd[0]; k++) {
        size_t L = strlen(dsd[k]);
        memcpy(nvr[i].data + nvr[i].size, dsd[k], L);
        nvr[i].size += (uint32_t)L;
        nvr[i].data[nvr[i].size++] = 13;
    }
    nvr[i].dirty = 0;
}

void os9_init(void) {
    heap_init();
    nvr_load_host_files();
    write_csd();
    procs[0].used = 1;
    procs[0].pid = 1;
    cur = &procs[0];
    paths[0].type = P_CONS;
    paths[1].type = P_CONS;
    paths[2].type = P_CONS;
}

/* ------------------------------------------------------------ syscalls */
static uint32_t ret_err(uint32_t err, uint32_t next) {
    D(1) = (D(1) & 0xFFFF0000u) | (err & 0xFFFF);
    CF = 1;
    return next;
}
static uint32_t ret_ok(uint32_t next) { CF = 0; return next; }

#define CHECK_SIG_RETURN(next) do { \
    if (sig_pending_count && !sig_masked && icpt_pc) { m68k.pc = (next); sig_deliver(); return m68k.pc; } \
    return (next); } while (0)

#define E_PRCFUL 0xE5
static struct {
    int state;               /* 0 none, 1 forked (not yet run), 2 running */
    int pid;
    uint32_t mod;
    M68K regs;
    volatile int exited;
    uint32_t status;
} child;
static int next_pid = 2;
int os9_child_running(void) { return child.state == 2; }

/* A speculative run (run-ahead, look-ahead) must not exit the program or start
   a child process: let time pass at this instruction until it is undone. */
static uint32_t stall(uint32_t at) { sched_wait_event(); return at; }

static uint32_t io_getstt(int p, uint32_t next);
static uint32_t io_setstt(int p, uint32_t next);

static uint32_t stack_caller(void) {
    for (int i = 0; i < 24; i++) {
        uint32_t v = RL(A(7) + i * 2);
        if (v >= recomp_code_lo && v < recomp_code_hi && !(v & 1)) return v;
    }
    return 0;
}

uint32_t os9_syscall(uint32_t code, uint32_t next) {
    if (log_level >= 2) {
        uint32_t c = stack_caller();
        TRACE("os9 %02x d0=%08x d1=%08x d2=%08x a0=%06x from %06x caller %06x (%s)", code, D(0), D(1), D(2), A(0), next, c, func_name(c));
        if (log_level >= 3) {
            char bt[512]; int k = 0;
            for (int i = 0; i < 64 && k < 480; i++) {
                uint32_t v = RL(A(7) + i * 2);
                if (v >= recomp_code_lo && v < recomp_code_hi && !(v & 1)) k += sprintf(bt + k, " %06x", v);
            }
            TRACE("   bt:%s", bt);
        }
    }
    switch (code) {
    case 0x00: { /* F$Link */
        char nm[64]; read_cstr(A(0), nm, sizeof nm);
        ModEntry *m = mod_find(nm);
        if (!m) { LOG("F$Link '%s' failed", nm); return ret_err(E_MNF, next); }
        m->links++;
        A(1) = m->addr + RL(m->addr + 0x30);
        A(2) = m->addr;
        D(0) = RW(m->addr + 0x12) & 0xFFFF;
        D(1) = RW(m->addr + 0x14);
        LOG("F$Link '%s' -> %06x", nm, m->addr);
        return ret_ok(next);
    }
    case 0x01: { /* F$Load */
        char nm[64]; read_cstr(A(0), nm, sizeof nm);
        const char *base = strrchr(nm, '/');
        base = base ? base + 1 : nm;
        ModEntry *m = mod_load_from_disc(base);
        if (!m) { LOG("F$Load '%s' failed", nm); return ret_err(E_PNNF_, next); }
        A(1) = m->addr + RL(m->addr + 0x30);
        A(2) = m->addr;
        D(0) = RW(m->addr + 0x12) & 0xFFFF;
        D(1) = RW(m->addr + 0x14);
        LOG("F$Load '%s' -> %06x", nm, m->addr);
        return ret_ok(next);
    }
    case 0x02: /* F$UnLink */
        return ret_ok(next);
    case 0x03: { /* F$Fork: the child is created now and runs when the parent waits */
        char nm[64]; read_cstr(A(0), nm, sizeof nm);
        ModEntry *m = mod_find(nm);
        if (!m) m = mod_load_from_disc(nm);
        if (!m) { LOG("F$Fork '%s': module not found", nm); return ret_err(E_MNF, next); }
        uint32_t entry = m->addr + RL(m->addr + 0x30);
        if (entry < recomp_code_lo || entry >= recomp_code_hi)
            LOG("F$Fork '%s': module at %06x is not recompiled, it will be interpreted", nm, m->addr);
        if (child.state) return ret_err(E_PRCFUL, next);
        M68K parent = m68k;
        int pid = next_pid++;
        create_process_state(m->addr, pid, A(1), D(2), D(1), D(3) & 0xFFFF);
        child.regs = m68k;
        m68k = parent;
        child.state = 1;
        child.pid = pid;
        child.mod = m->addr;
        Proc *p = &procs[1];
        memset(p, 0, sizeof *p);
        p->used = 1;
        p->pid = pid;
        LOG("F$Fork '%s' -> pid %d", nm, pid);
        D(0) = (uint32_t)pid;
        while (RB(A(0)) > ' ') A(0)++;
        return ret_ok(next);
    }
    case 0x04: /* F$Wait: run the child to completion */
        if (child.state == 1) {
            if (g_speculative) return stall(next - 4);
            M68K parent = m68k;
            Proc *pp = cur;
            cur = proc_by_pid(child.pid);
            int64_t cyc = m68k.cyc, lim = m68k.cyc_limit;
            m68k = child.regs;
            m68k.cyc = cyc; m68k.cyc_limit = lim;
            child.state = 2;
            LOG("running child pid %d", child.pid);
            cpu_run_until(&child.exited);
            cyc = m68k.cyc; lim = m68k.cyc_limit;
            m68k = parent;
            m68k.cyc = cyc; m68k.cyc_limit = lim;
            cur = pp;
            proc_by_pid(child.pid)->used = 0;
            child.state = 0;
            child.exited = 0;
            LOG("child pid %d exited (%u)", child.pid, child.status);
            D(0) = (uint32_t)child.pid;
            D(1) = child.status;
            return ret_ok(next);
        }
        return ret_err(E_NOCHLD, next);
    case 0x05: /* F$Chain */
        fatal("F$Chain not supported");
        return next;
    case 0x06: /* F$Exit */
        if (cur && cur->pid != 1) {
            child.status = D(1) & 0xFFFF;
            child.exited = 1;
            return 0xFFFFFFFEu;       /* unwinds the child's native call chain */
        }
        if (g_speculative) return stall(next - 4);
        LOG("F$Exit(%u)", D(1));
        nvr_flush();
        exit((int)(D(1) & 0xFFFF));
        return next;
    case 0x1D: /* F$UnLoad */
        return ret_ok(next);
    case 0x21: /* F$TLink: trap handler modules are not needed by the recompiled code */
        return ret_ok(next);
    case 0x27: { /* F$SetSys: read/write a system global */
        uint32_t off = D(0) & 0xFFFF, sz = D(1);
        if (sz & 0x80000000u) {
            /* read: D_TckSec (ticks per second) and D_Ticks are what programs ask for */
            D(2) = off == 0x0E ? TICKS_PER_SEC : (uint32_t)(m68k.cyc / TICK_CYC);
            TRACE("F$SetSys read %x -> %u", off, D(2));
        } else {
            TRACE("F$SetSys write %x = %x (ignored)", off, D(2));
        }
        return ret_ok(next);
    }
    case 0x07: /* F$Mem */
        D(0) = 0;
        A(1) = 0;
        return ret_ok(next);
    case 0x08: { /* F$Send */
        int pid = D(0) & 0xFFFF;
        if (pid == 0) pid = os9_cur_pid();
        if (!proc_by_pid(pid)) return ret_err(E_IPRCID, next);
        sig_send_pid(pid, D(1) & 0xFFFF);
        CHECK_SIG_RETURN(ret_ok(next));
    }
    case 0x09: /* F$Icpt */
        icpt_pc = A(0);
        icpt_a6 = A(6);
        LOG("F$Icpt handler %06x a6=%06x", icpt_pc, icpt_a6);
        return ret_ok(next);
    case 0x0A: { /* F$Sleep (clears the signal mask, as OS-9 does) */
        /* Blocking: each execution advances time by one scheduler event and
           returns to the os9 instruction, so the run loop reaches its safe
           point (frame boundary) while the program sleeps. */
        uint32_t at = next - 4;
        if (cur->block_pc != at) {
            uint32_t ticks = D(0);
            sig_masked = 0;
            cur->block_until = ticks ? m68k.cyc + (int64_t)ticks * TICK_CYC : INT64_MAX;
            cur->block_pc = at;
        }
        if (sig_pending_count && !sig_masked && icpt_pc) {
            cur->block_pc = 0;
            D(0) = 0;
            CF = 0;
            m68k.pc = next;
            sig_deliver();
            return m68k.pc;
        }
        if (m68k.cyc >= cur->block_until) {
            cur->block_pc = 0;
            D(0) = 0;
            return ret_ok(next);
        }
        sched_wait_event();
        return at;
    }
    case 0x0C: /* F$ID */
        D(0) = 1;
        D(1) = 0;
        return ret_ok(next);
    case 0x0D: /* F$SPrior */
        return ret_ok(next);
    case 0x15: { /* F$Time */
        extern int g_headless;
        /* headless runs use a fixed epoch so test runs are reproducible */
        time_t t = g_headless ? (time_t)(820454400 + m68k.cyc / CPU_IPS) : time(NULL);
        struct tm *tm = localtime(&t);
        D(0) = ((uint32_t)tm->tm_hour << 16) | (tm->tm_min << 8) | tm->tm_sec;
        D(1) = ((uint32_t)(tm->tm_year + 1900) << 16) | ((tm->tm_mon + 1) << 8) | tm->tm_mday;
        D(2) = (TICKS_PER_SEC << 16) | (uint32_t)((m68k.cyc / TICK_CYC) % TICKS_PER_SEC);
        D(3) = (uint32_t)(m68k.cyc / TICK_CYC);
        return ret_ok(next);
    }
    case 0x18: { /* F$GPrDsc: d0 = pid, d1 = size, a0 = buffer.  clock() reads
                    P$UTicks + P$STicks from the copy */
        uint32_t n = D(1) > 0x800 ? 0x800 : D(1);
        for (uint32_t i = 0; i < n; i++) WB(A(0) + i, 0);
        if (n >= 2) WW(A(0), D(0) ? D(0) : (uint32_t)cur->pid);
        if (n >= 0x2BC) WL(A(0) + 0x2B4, (uint32_t)(m68k.cyc / TICK_CYC));
        return ret_ok(next);
    }
    case 0x1E: /* F$RTE */
        return do_rte();
    case 0x28: { /* F$SRqMem */
        uint32_t sz = D(0), act;
        if (sz == 0xFFFFFFFFu) sz = 0x40000;    /* "largest block" request */
        uint32_t a = mem_alloc(sz, &act);
        if (!a) return ret_err(E_MEMFUL, next);
        D(0) = act;
        A(2) = a;
        TRACE("F$SRqMem %x -> %06x", sz, a);
        return ret_ok(next);
    }
    case 0x29: /* F$SRtMem */
        mem_free(A(2), D(0));
        return ret_ok(next);
    case 0x56: { /* F$Alarm */
        uint32_t fn = D(1) & 0xFFFF;
        if (fn == 0) { /* A$Delete */
            for (int i = 0; i < 16; i++)
                if (alarms[i].used && (D(0) == 0 || alarms[i].id == D(0))) alarms[i].used = 0;
            return ret_ok(next);
        }
        int slot = -1;
        for (int i = 0; i < 16; i++) if (!alarms[i].used) { slot = i; break; }
        if (slot < 0) return ret_err(E_MEMFUL, next);
        Alarm *al = &alarms[slot];
        al->used = 1;
        al->pid = os9_cur_pid();
        al->id = next_alarm_id++;
        al->sig = D(2) & 0xFFFF;
        uint32_t t = D(3);
        int64_t cyc;
        if (t & 0x80000000u) cyc = (int64_t)(t & 0x7FFFFFFF) * CPU_IPS / 256; /* seconds/256 */
        else cyc = (int64_t)t * TICK_CYC;
        if (cyc < 1) cyc = 1;
        al->when = m68k.cyc + cyc;
        al->period = (fn == 2) ? cyc : 0;   /* A$Cycle */
        if (al->when < m68k.cyc_limit) m68k.cyc_limit = al->when;
        D(0) = al->id;
        TRACE("F$Alarm fn %u sig %u t %x -> id %u", fn, al->sig, t, al->id);
        return ret_ok(next);
    }
    case 0x57: /* F$SigMask */
        switch (D(1) & 0xFFFF) {
        case 0: sig_masked = 0; break;
        case 1: sig_masked++; break;
        case 0xFFFF: if (sig_masked) sig_masked--; break;
        default: sig_masked = (int)(D(1) & 0xFFFF);
        }
        CHECK_SIG_RETURN(ret_ok(next));
    case 0x5C: { /* F$SRqCMem: colored memory request */
        uint32_t sz = D(0), act;
        if (sz == 0xFFFFFFFFu) sz = 0x40000;
        uint32_t a = mem_alloc(sz, &act);
        if (!a) return ret_err(E_MEMFUL, next);
        D(0) = act;
        A(2) = a;
        TRACE("F$SRqCMem %x type %x -> %06x", sz, D(1), a);
        return ret_ok(next);
    }

    /* ---- I/O */
    case 0x83: case 0x84: { /* I$Create / I$Open */
        char nm[128]; read_cstr(A(0), nm, sizeof nm);
        int r = do_open(nm, D(0) & 0xFF, code == 0x83);
        LOG("I$%s '%s' mode %x -> %d", code == 0x83 ? "Create" : "Open", nm, D(0) & 0xFF, r);
        if (r < 0) return ret_err((uint32_t)-r, next);
        uint32_t a = A(0);
        while (RB(a) > ' ') a++;
        A(0) = a;
        D(0) = (uint32_t)r;
        return ret_ok(next);
    }
    case 0x87: { /* I$Delete */
        char nm[128]; read_cstr(A(0), nm, sizeof nm);
        LOG("I$Delete '%s'", nm);
        if (!_strnicmp(nm, "/nvr/", 5)) {
            int i = nvr_find(nm + 5);
            if (i < 0) return ret_err(E_PNNF_, next);
            nvr[i].used = 0;
            nvr[i].removed = 1;
            return ret_ok(next);
        }
        return ret_err(E_PNNF_, next);
    }
    case 0x82: { /* I$Dup */
        int p = path_alloc();
        if (p < 0) return ret_err(E_PTHFUL, next);
        paths[p] = paths[D(0) & 0xFFFF];
        D(0) = (uint32_t)p;
        return ret_ok(next);
    }
    case 0x8F: { /* I$Close */
        int p = D(0) & 0xFFFF;
        if (p >= MAXPATH || paths[p].type == P_NONE) return ret_err(E_BPNUM_, next);
        Path *P = &paths[p];
        if (P->type == P_CD) cdfm_close(P->handle);
        if (p >= 3) memset(P, 0, sizeof *P);
        return ret_ok(next);
    }
    case 0x88: { /* I$Seek */
        int p = D(0) & 0xFFFF;
        if (p >= MAXPATH) return ret_err(E_BPNUM_, next);
        Path *P = &paths[p];
        if (P->type == P_CD) { cdfm_seek(P->handle, D(1)); return ret_ok(next); }
        if (P->type == P_NVR && P->nvr >= 0) { P->pos = D(1); return ret_ok(next); }
        return ret_ok(next);
    }
    case 0x89: case 0x8B: { /* I$Read / I$ReadLn */
        int p = D(0) & 0xFFFF;
        if (p >= MAXPATH || paths[p].type == P_NONE) return ret_err(E_BPNUM_, next);
        Path *P = &paths[p];
        uint32_t n = D(1), got = 0;
        if (P->type == P_CD) {
            int e = cdfm_read(P->handle, A(0), n, &got);
            if (e) return ret_err((uint32_t)e, next);
            D(1) = got;
            return ret_ok(next);
        }
        if (P->type == P_NVR && P->nvr >= 0) {
            NvrFile *nf = &nvr[P->nvr];
            for (got = 0; got < n; got++) {
                if (P->pos >= nf->size) break;
                uint8_t c = nf->data[P->pos++];
                WB(A(0) + got, c);
                if (code == 0x8B && c == '\r') { got++; break; }
            }
            if (!got && n) return ret_err(E_EOF_, next);
            D(1) = got;
            return ret_ok(next);
        }
        return ret_err(E_EOF_, next);
    }
    case 0x8A: case 0x8C: { /* I$Write / I$WritLn */
        int p = D(0) & 0xFFFF;
        if (p >= MAXPATH || paths[p].type == P_NONE) return ret_err(E_BPNUM_, next);
        Path *P = &paths[p];
        uint32_t n = D(1);
        if (P->type == P_NVR && P->nvr >= 0) {
            NvrFile *nf = &nvr[P->nvr];
            uint32_t k = 0;
            for (; k < n; k++) {
                if (P->pos >= NVR_SIZE) break;
                uint8_t c = RB(A(0) + k);
                nf->data[P->pos++] = c;
                if (P->pos > nf->size) nf->size = P->pos;
                if (code == 0x8C && c == '\r') { k++; break; }
            }
            nf->dirty = 1;
            D(1) = k;
            if (k < n && P->pos >= NVR_SIZE) return ret_err(E_FULL, next);
            return ret_ok(next);
        }
        if (P->type == P_CONS) {
            char buf[512];
            uint32_t k = 0;
            for (; k < n && k < sizeof buf - 1; k++) {
                char c = (char)RB(A(0) + k);
                buf[k] = c == '\r' ? '\n' : c;
                if (code == 0x8C && c == '\r') { k++; break; }
            }
            buf[k] = 0;
            LOG("[console] %s", buf);
            D(1) = k;
            return ret_ok(next);
        }
        D(1) = n;
        return ret_ok(next);
    }
    case 0x8D: return io_getstt(D(0) & 0xFFFF, next);
    case 0x8E: return io_setstt(D(0) & 0xFFFF, next);
    }
    {
        static uint8_t warned[256];
        if (!warned[code & 0xFF]) {
            warned[code & 0xFF] = 1;
            LOG("unimplemented OS-9 call $%02x at %06x (%s): returning E$UnkSvc", code, next - 4, func_name(next));
        }
    }
    return ret_err(E_UNKSVC_, next);
}

static uint32_t io_getstt(int p, uint32_t next) {
    if (p >= MAXPATH || paths[p].type == P_NONE) return ret_err(E_BPNUM_, next);
    Path *P = &paths[p];
    uint32_t fn = D(1) & 0xFFFF;
    int e;
    switch (P->type) {
    case P_CD:
        if (fn == 2) { D(2) = cdfm_size(P->handle); return ret_ok(next); }   /* SS_Size */
        if (fn == 5) { D(2) = cdfm_getpos(P->handle); return ret_ok(next); } /* SS_Pos */
        if (fn == 6) { /* SS_EOF */
            if (cdfm_getpos(P->handle) >= cdfm_size(P->handle)) return ret_err(E_EOF_, next);
            D(1) = 0; return ret_ok(next);
        }
        e = cdfm_getstat(P->handle, fn);
        return e ? ret_err((uint32_t)e, next) : ret_ok(next);
    case P_NVR:
        if (fn == 2 && P->nvr >= 0) { D(2) = nvr[P->nvr].size; return ret_ok(next); }
        if (fn == 5 && P->nvr >= 0) { D(2) = P->pos; return ret_ok(next); }
        if (fn == 6 && P->nvr >= 0) {
            if (P->pos >= nvr[P->nvr].size) return ret_err(E_EOF_, next);
            return ret_ok(next);
        }
        if (fn == 0) { /* SS_Opt */
            for (int i = 0; i < 128; i++) WB(A(0) + i, 0);
            return ret_ok(next);
        }
        if (fn == 0x43) { /* NVRAM free space */
            D(2) = nvr_free_bytes();
            return ret_ok(next);
        }
        LOG("GetStt %x on nvr path", fn);
        return ret_err(E_UNKSVC_, next);
    case P_VIDEO:
        e = ucm_getstat(fn);
        return e ? ret_err((uint32_t)e, next) : ret_ok(next);
    case P_PTR:
        e = ptr_getstat(fn);
        return e ? ret_err((uint32_t)e, next) : ret_ok(next);
    case P_AUDIO:
        e = audio_getstat(fn);
        return e ? ret_err((uint32_t)e, next) : ret_ok(next);
    case P_CONS: case P_NULL: case P_KB:
        if (fn == 0) { for (int i = 0; i < 128; i++) WB(A(0) + i, 0); return ret_ok(next); }
        if (fn == 1) return ret_err(0xF6, next); /* SS_Ready: not ready */
        return ret_err(E_UNKSVC_, next);
    }
    return ret_err(E_UNKSVC_, next);
}

static uint32_t io_setstt(int p, uint32_t next) {
    if (p >= MAXPATH || paths[p].type == P_NONE) return ret_err(E_BPNUM_, next);
    Path *P = &paths[p];
    uint32_t fn = D(1) & 0xFFFF;
    int e;
    switch (P->type) {
    case P_CD:
        e = cdfm_setstat(P->handle, fn);
        if (e) return ret_err((uint32_t)e, next);
        CHECK_SIG_RETURN(ret_ok(next));
    case P_VIDEO:
        e = ucm_setstat(fn);
        return e ? ret_err((uint32_t)e, next) : ret_ok(next);
    case P_PTR:
        e = ptr_setstat(fn);
        return e ? ret_err((uint32_t)e, next) : ret_ok(next);
    case P_AUDIO:
        e = audio_setstat(fn);
        if (e) return ret_err((uint32_t)e, next);
        CHECK_SIG_RETURN(ret_ok(next));
    case P_NVR:
        if (fn == 2) return ret_ok(next);   /* SS_Size */
        if (fn == 0) return ret_ok(next);
        LOG("SetStt %x on nvr path", fn);
        return ret_ok(next);
    default:
        return ret_ok(next);
    }
}

/* ------------------------------------------------------------ snapshots */
void os9_state(StateIO *s) {
    /* allocator: free list as (addr, size) pairs */
    uint32_t n = 0;
    if (!s->loading) for (Block *b = free_list; b; b = b->next) n++;
    st_u32(s, &n);
    if (s->err) return;
    if (s->loading) {
        while (free_list) { Block *b = free_list; free_list = b->next; free(b); }
        Block **tail = &free_list;
        for (uint32_t i = 0; i < n && !s->err; i++) {
            Block *b = (Block *)calloc(1, sizeof(Block));
            st_u32(s, &b->addr);
            st_u32(s, &b->size);
            *tail = b;
            tail = &b->next;
        }
    } else {
        for (Block *b = free_list; b; b = b->next) { st_u32(s, &b->addr); st_u32(s, &b->size); }
    }
    ST(s, heap_hw);
    ST(s, procs);
    int ci = cur ? (int)(cur - procs) : 0;
    st_i32(s, &ci);
    if (s->loading) cur = &procs[ci];
    ST(s, alarms);
    ST(s, next_alarm_id);
    ST(s, next_field);
    ST(s, next_sector);
    ST(s, field_period);
    ST(s, sector_period);
    ST(s, paths);
    ST(s, mods);
    ST(s, nmods);
    ST(s, child);
    ST(s, next_pid);
    ST(s, nvr);
}
