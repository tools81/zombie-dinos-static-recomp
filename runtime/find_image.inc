#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <direct.h>
#include <commdlg.h>
#endif
#include "disc.h"

static char exe_dir[512] = ".";

static void init_exe_dir(void) {
#ifdef _WIN32
    char p[512];
    DWORD n = GetModuleFileNameA(NULL, p, sizeof p);
    if (n > 0 && n < sizeof p) {
        char *s = strrchr(p, '\\');
        if (s) { *s = 0; snprintf(exe_dir, sizeof exe_dir, "%s", p); }
    }
#endif
}

/* Everything the program writes (NVRAM saves, quicksaves, settings, disc location,
   log, downloaded updates) lives in <exe dir>/saves. Returns -1 if it can't be written. */
static int setup_data_dir(void) {
    snprintf(g_data_dir, sizeof g_data_dir, "%s/saves", exe_dir);
#ifdef _WIN32
    _mkdir(g_data_dir);
#endif
    char p[600];
    snprintf(p, sizeof p, "%s/write.tmp", g_data_dir);
    FILE *f = fopen(p, "wb");
    if (!f) return -1;
    fclose(f);
    remove(p);
    return 0;
}

/* Resolve a .cue to the .bin it references. */
static void cue_to_bin(const char *cue, char *out, size_t n) {
    snprintf(out, n, "%s", cue);
    FILE *f = fopen(cue, "r");
    char line[512];
    while (f && fgets(line, sizeof line, f)) {
        char *q1 = strchr(line, '"'), *q2 = q1 ? strchr(q1 + 1, '"') : NULL;
        if (!strncmp(line, "FILE", 4) && q1 && q2) {
            *q2 = 0;
            char dir[512];
            snprintf(dir, sizeof dir, "%s", cue);
            char *sl = strrchr(dir, '/'), *bs = strrchr(dir, '\\');
            if (bs > sl) sl = bs;
            if (sl) { sl[1] = 0; snprintf(out, n, "%s%s", dir, q1 + 1); }
            else snprintf(out, n, "%s", q1 + 1);
            break;
        }
    }
    if (f) fclose(f);
}

/* Is this a raw 2352-byte-sector CD-i image? (disc label "CD-I " at sector 16) */
static int is_cdi_image(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    char buf[8] = {0};
    int ok = fseek(f, 16L * 2352 + 24 + 1, SEEK_SET) == 0 && fread(buf, 1, 5, f) == 5 && !memcmp(buf, "CD-I ", 5);
    fclose(f);
    return ok && disc_has_file(path, MAIN_MODULE);      /* this game, not just any CD-i disc */
}

static int scan_dir_for_image(const char *dir, char *out, size_t n) {
#ifdef _WIN32
    char pat[1800];
    snprintf(pat, sizeof pat, "%s\\*.bin", dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return -1;
    do {
        char p[2100];
        snprintf(p, sizeof p, "%s\\%s", dir, fd.cFileName);
        if (is_cdi_image(p)) { FindClose(h); snprintf(out, n, "%s", p); return 0; }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    (void)dir; (void)out; (void)n;
#endif
    return -1;
}

static int is_dir(const char *p) {
#ifdef _WIN32
    DWORD a = GetFileAttributesA(p);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
#else
    (void)p; return 0;
#endif
}

/* ---- saves/disc_path.txt: a .cue, .bin or folder chosen in the installer or on first
   start. Inside the program folder it is stored relative to it, so the whole folder
   (or USB stick) can be moved. */
static int find_image(const char *arg, char *out, size_t n);

static int saved_image(char *out, size_t n) {
    char p[600], line[1024] = "", full[1600];
    snprintf(p, sizeof p, "%s/disc_path.txt", g_data_dir);
    FILE *f = fopen(p, "r");
    if (!f) return -1;
    if (!fgets(line, sizeof line, f)) line[0] = 0;
    fclose(f);
    char *s = line;
    if (!memcmp(s, "\xEF\xBB\xBF", 3)) s += 3;        /* the installer writes a BOM */
    s[strcspn(s, "\r\n")] = 0;
    if (!*s) return -1;
    if (s[1] == ':' || s[0] == '\\' || s[0] == '/') snprintf(full, sizeof full, "%s", s);
    else snprintf(full, sizeof full, "%s\\%s", exe_dir, s);
    return find_image(full, out, n);
}

static void remember_image(const char *img) {
    char p[600];
    size_t L = strlen(exe_dir);
    const char *s = img;
    if (!_strnicmp(img, exe_dir, L) && (img[L] == '\\' || img[L] == '/')) s = img + L + 1;
    snprintf(p, sizeof p, "%s/disc_path.txt", g_data_dir);
    FILE *f = fopen(p, "w");
    if (f) { fprintf(f, "%s\n", s); fclose(f); }
}

#ifdef _WIN32
/* first start of the portable copy: ask for the disc image */
static int pick_image(char *out, size_t n) {
    for (;;) {
        char sel[1024] = "";
        OPENFILENAMEA o = {0};
        o.lStructSize = sizeof o;
        o.lpstrFilter = "CD-i disc image (*.cue;*.bin)\0*.cue;*.bin\0All files\0*.*\0";
        o.lpstrFile = sel;
        o.nMaxFile = sizeof sel;
        o.lpstrInitialDir = exe_dir;
        o.lpstrTitle = "Select your " GAME_TITLE " disc image (.cue or .bin)";
        o.Flags = OFN_FILEMUSTEXIST | OFN_HIDEREADONLY | OFN_NOCHANGEDIR | OFN_DONTADDTORECENT;
        if (!GetOpenFileNameA(&o)) return -1;
        if (!find_image(sel, out, n)) { remember_image(out); return 0; }
        if (MessageBoxA(NULL, "That is not a readable image of the " GAME_TITLE " disc.\n\n"
                        "It must be the raw (2352 bytes per sector) .bin, or the .cue beside it.",
                        GAME_TITLE, MB_RETRYCANCEL | MB_ICONWARNING) != IDRETRY) return -1;
    }
}
#endif

static int find_image(const char *arg, char *out, size_t n) {
    if (arg) {
        if (is_dir(arg)) {
            char sub[1700];
            snprintf(sub, sizeof sub, "%s\\rom", arg);
            return !scan_dir_for_image(arg, out, n) ? 0 : scan_dir_for_image(sub, out, n);
        }
        size_t L = strlen(arg);
        if (L > 4 && !_stricmp(arg + L - 4, ".cue")) cue_to_bin(arg, out, n);
        else snprintf(out, n, "%s", arg);
        return is_cdi_image(out) ? 0 : -1;
    }
    if (!saved_image(out, n)) return 0;
    /* look for rom/*.bin next to the working directory or the executable, up to 4 levels up */
    const char *bases[2] = {".", exe_dir};
    for (int b = 0; b < 2; b++) {
        char dir[600];
        snprintf(dir, sizeof dir, "%s", bases[b]);
        for (int up = 0; up <= 4; up++) {
            char romdir[700];
            snprintf(romdir, sizeof romdir, "%s\\rom", dir);
            if (!scan_dir_for_image(romdir, out, n)) return 0;
            if (!scan_dir_for_image(dir, out, n)) return 0;
            size_t L = strlen(dir);
            if (L + 3 < sizeof dir) strcat(dir, "\\..");
        }
    }
    return -1;
}

