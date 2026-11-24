/* Automatic update at start-up.

   Asks GitHub for the latest release of UPDATE_REPO (version.h). If its tag is
   newer than APP_VERSION, its "*-portable.zip" is downloaded, checked against
   the SHA-256 GitHub publishes for it, unpacked in saves/update with Windows'
   own tar.exe, and the new program must start and report the expected version.
   Only then are the files swapped: the running .exe is renamed (Windows allows
   that), the new one takes its place, and the program restarts itself. If any
   step fails the old version keeps running, and that release is not tried
   again for a day. Saves and settings are never touched by an update.

   Not done in headless/scripted runs, with --no-update-check, with
   RECOMP_NO_UPDATE set, or with "update=0" in saves/settings.ini. */
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>
#include <bcrypt.h>
#include <process.h>
#include <direct.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "version.h"
#include "runtime.h"
#include "game_config.h"

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")

int g_auto_update = 1;          /* settings.ini: update=0|1 */

#ifdef UPDATE_TEST_PORT         /* test builds only: a local server instead of GitHub */
#define API_HOST L"127.0.0.1"
#define API_PORT UPDATE_TEST_PORT
#define API_FLAGS 0
#define ASSET_PREFIX "http://127.0.0.1/"
#define SCHEME "http://"
#else
#define API_HOST L"api.github.com"
#define API_PORT INTERNET_DEFAULT_HTTPS_PORT
#define API_FLAGS WINHTTP_FLAG_SECURE
#define ASSET_PREFIX "https://github.com/" UPDATE_REPO "/releases/download/"
#define SCHEME "https://"
#endif

#define CHECK_WAIT_MS 5000      /* the game never waits longer than this for the check */
#define MAX_ZIP (64u << 20)

/* ---------------------------------------------------------------- HTTP */
typedef struct { volatile LONG got, total, cancel; } Progress;

static int http_get(const wchar_t *host, const wchar_t *path, char **out, size_t *outlen, size_t limit, Progress *pr) {
    int ok = 0;
    *out = NULL; *outlen = 0;
    HINTERNET s = WinHttpOpen(L"cdi-recomp-updater/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                              WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s) return 0;
    WinHttpSetTimeouts(s, 3000, 3000, 4000, pr ? 15000 : 4000);
    HINTERNET c = WinHttpConnect(s, host, API_PORT, 0);
    HINTERNET r = c ? WinHttpOpenRequest(c, L"GET", path, NULL, WINHTTP_NO_REFERER,
                                         WINHTTP_DEFAULT_ACCEPT_TYPES, API_FLAGS) : NULL;
    if (r && WinHttpSendRequest(r, pr ? L"Accept: application/octet-stream\r\n"
                                      : L"Accept: application/vnd.github+json\r\n",
                                (DWORD)-1L, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(r, NULL)) {
        DWORD code = 0, sz = sizeof code, total = 0;
        WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                            &code, &sz, WINHTTP_NO_HEADER_INDEX);
        sz = sizeof total;
        WinHttpQueryHeaders(r, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                            &total, &sz, WINHTTP_NO_HEADER_INDEX);
        if (pr) pr->total = (LONG)total;
        size_t cap = 1 << 16, len = 0;
        char *buf = code == 200 ? (char *)malloc(cap + 1) : NULL;
        while (buf) {
            DWORD avail = 0, got = 0;
            if (!WinHttpQueryDataAvailable(r, &avail)) break;
            if (!avail) { ok = 1; break; }
            if (len + avail > limit || (pr && pr->cancel)) break;
            while (len + avail > cap) cap *= 2;
            char *nb = (char *)realloc(buf, cap + 1);
            if (!nb) break;
            buf = nb;
            if (!WinHttpReadData(r, buf + len, avail, &got) || !got) break;
            len += got;
            if (pr) pr->got = (LONG)len;
        }
        if (ok) { buf[len] = 0; *out = buf; *outlen = len; }
        else free(buf);
    }
    if (r) WinHttpCloseHandle(r);
    if (c) WinHttpCloseHandle(c);
    WinHttpCloseHandle(s);
    return ok;
}

/* ---------------------------------------------------------------- release info */
typedef struct { char tag[64], url[1024], sha[65]; } Release;

static int parse_ver(const char *v, int out[3]) {
    if (*v == 'v' || *v == 'V') v++;
    out[0] = out[1] = out[2] = 0;
    return sscanf(v, "%d.%d.%d", &out[0], &out[1], &out[2]) >= 2;
}

static int newer(const char *a, const char *b) {
    int x[3], y[3];
    if (!parse_ver(a, x) || !parse_ver(b, y)) return 0;
    for (int i = 0; i < 3; i++) if (x[i] != y[i]) return x[i] > y[i];
    return 0;
}

/* value of the first "key": "..." in [p, end); values with escapes are rejected */
static int json_str(const char *p, const char *end, const char *key, char *out, size_t n) {
    char pat[64];
    size_t L = (size_t)snprintf(pat, sizeof pat, "\"%s\"", key);
    const char *k = strstr(p, pat);
    if (!k || k >= end) return 0;
    k += L;
    while (*k == ' ' || *k == ':') k++;
    if (*k++ != '"') return 0;
    size_t i = 0;
    while (*k && *k != '"' && *k != '\\' && i + 1 < n) out[i++] = *k++;
    out[i] = 0;
    return *k == '"';
}

static int tag_ok(const char *t) {       /* it goes into command lines and messages */
    if (!*t) return 0;
    for (; *t; t++)
        if (!((*t >= '0' && *t <= '9') || (*t >= 'a' && *t <= 'z') || (*t >= 'A' && *t <= 'Z') || *t == '.' || *t == '-'))
            return 0;
    return 1;
}

static int parse_release(const char *json, Release *rel) {
    const char *jend = json + strlen(json);
    memset(rel, 0, sizeof *rel);
    if (!json_str(json, jend, "tag_name", rel->tag, sizeof rel->tag) || !tag_ok(rel->tag)) { rel->tag[0] = 0; return 0; }
    const char *as = strstr(json, "\"assets\"");
    while (as) {
        /* an asset's "digest" comes before its "browser_download_url", its last key */
        const char *e = strstr(as, "\"browser_download_url\"");
        if (!e) return 0;
        const char *close = strchr(e, '}');
        const char *end = close ? close : jend;
        char url[1024] = "", dig[128] = "";
        if (!json_str(e, end, "browser_download_url", url, sizeof url)) return 0;
        json_str(as, end, "digest", dig, sizeof dig);
        size_t L = strlen(url);
        if (L > 13 && !_stricmp(url + L - 13, "-portable.zip") && !strncmp(url, ASSET_PREFIX, strlen(ASSET_PREFIX))) {
            if (strncmp(dig, "sha256:", 7) || strlen(dig) != 71) return 0;
            snprintf(rel->url, sizeof rel->url, "%s", url);
            snprintf(rel->sha, sizeof rel->sha, "%s", dig + 7);
            return 1;
        }
        as = end;
    }
    return 0;
}

static unsigned __stdcall check_thread(void *arg) {
    size_t len;
    wchar_t path[256] = L"/repos/";
    MultiByteToWideChar(CP_UTF8, 0, UPDATE_REPO, -1, path + 7, 200);
    wcscat(path, L"/releases/latest");
    http_get(API_HOST, path, (char **)arg, &len, 1 << 20, NULL);
    return 0;
}

/* ---------------------------------------------------------------- download */
typedef struct { Release *rel; Progress pr; char *data; size_t len; int ok; } Download;

static int sha256_hex(const char *p, size_t n, char out[65]) {
    BCRYPT_ALG_HANDLE alg;
    BCRYPT_HASH_HANDLE h;
    UCHAR dig[32];
    int ok = 0;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, NULL, 0) < 0) return 0;
    if (BCryptCreateHash(alg, &h, NULL, 0, NULL, 0, 0) >= 0) {
        ok = BCryptHashData(h, (PUCHAR)p, (ULONG)n, 0) >= 0 && BCryptFinishHash(h, dig, 32, 0) >= 0;
        BCryptDestroyHash(h);
    }
    BCryptCloseAlgorithmProvider(alg, 0);
    for (int i = 0; ok && i < 32; i++) sprintf(out + i * 2, "%02x", dig[i]);
    return ok;
}

static unsigned __stdcall download_thread(void *arg) {
    Download *d = (Download *)arg;
    wchar_t host[256], path[1024];
    char hb[256];
    const char *h = d->rel->url + strlen(SCHEME), *sl = strchr(h, '/');
    size_t L = sl ? (size_t)(sl - h) : 0;
    if (!L || L >= sizeof hb) return 0;
    memcpy(hb, h, L); hb[L] = 0;
    MultiByteToWideChar(CP_UTF8, 0, hb, -1, host, 256);
    MultiByteToWideChar(CP_UTF8, 0, sl, -1, path, 1024);
    /* WinHTTP follows GitHub's redirect to its download host (https only) */
    d->ok = http_get(host, path, &d->data, &d->len, MAX_ZIP, &d->pr);
    return 0;
}

static LRESULT CALLBACK upd_proc(HWND w, UINT m, WPARAM wp, LPARAM lp) {
    if (m == WM_CLOSE) {
        Progress *p = (Progress *)GetWindowLongPtrA(w, GWLP_USERDATA);
        if (p) p->cancel = 1;
        return 0;
    }
    return DefWindowProcA(w, m, wp, lp);
}

static HWND progress_window(Progress *pr, HWND *label) {
    WNDCLASSA wc = {0};
    wc.lpfnWndProc = upd_proc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "recomp_update";
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    RegisterClassA(&wc);
    int w = 420, h = 110;
    HWND win = CreateWindowExA(0, "recomp_update", GAME_TITLE, WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
                               (GetSystemMetrics(SM_CXSCREEN) - w) / 2, (GetSystemMetrics(SM_CYSCREEN) - h) / 2, w, h,
                               NULL, NULL, wc.hInstance, NULL);
    SetWindowLongPtrA(win, GWLP_USERDATA, (LONG_PTR)pr);
    *label = CreateWindowA("STATIC", "", WS_CHILD | WS_VISIBLE | SS_CENTER, 10, 25, w - 36, 30, win, NULL, wc.hInstance, NULL);
    SendMessageA(*label, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), 1);
    return win;
}

static void pump(void) {
    MSG m;
    while (PeekMessageA(&m, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageA(&m); }
}

static int fetch(Release *rel, char **data, size_t *len, int *cancelled) {
    Download d = {0};
    d.rel = rel;
    HWND label, win = progress_window(&d.pr, &label);
    HANDLE t = (HANDLE)_beginthreadex(NULL, 0, download_thread, &d, 0, NULL);
    if (t) {
        while (WaitForSingleObject(t, 60) == WAIT_TIMEOUT) {
            char s[160];
            if (d.pr.total > 0)
                snprintf(s, sizeof s, "Updating to %s... %d%%", rel->tag, (int)(100.0 * d.pr.got / d.pr.total));
            else snprintf(s, sizeof s, "Updating to %s...", rel->tag);
            SetWindowTextA(label, s);
            pump();
        }
        CloseHandle(t);
    }
    DestroyWindow(win);
    pump();
    char sum[65] = "";
    *cancelled = d.pr.cancel;
    if (d.ok && (!sha256_hex(d.data, d.len, sum) || _stricmp(sum, rel->sha))) {
        LOG("update: checksum mismatch (%s, expected %s)", sum, rel->sha);
        d.ok = 0;
    }
    if (d.pr.cancel) d.ok = 0;
    if (!d.ok) { free(d.data); return 0; }
    *data = d.data; *len = d.len;
    return 1;
}

/* ---------------------------------------------------------------- install */
static int run_wait(const char *app, char *cmd, const char *dir, DWORD ms) {
    STARTUPINFOA si = {0};
    PROCESS_INFORMATION pi;
    DWORD code = 1;
    si.cb = sizeof si;
    UINT em = SetErrorMode(SEM_FAILCRITICALERRORS);      /* no system message box for a broken program */
    BOOL made = CreateProcessA(app, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, dir, &si, &pi);
    SetErrorMode(em);
    if (!made) return -1;
    if (WaitForSingleObject(pi.hProcess, ms) != WAIT_OBJECT_0) TerminateProcess(pi.hProcess, 1);
    else GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    return (int)code;
}

/* delete the files of a flat folder, and the folder */
static void wipe(const char *dir) {
    char p[1400];
    WIN32_FIND_DATAA fd;
    snprintf(p, sizeof p, "%s\\*", dir);
    HANDLE h = FindFirstFileA(p, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        snprintf(p, sizeof p, "%s\\%s", dir, fd.cFileName);
        DeleteFileA(p);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    RemoveDirectoryA(dir);
}

static int install(const Release *rel, const char *data, size_t len, const char *exe_dir, const char *udir) {
    char p[1400], q[1400], cmd[700], self[600], old[640], sys[300], files[900];
    GetModuleFileNameA(NULL, self, sizeof self);
    snprintf(files, sizeof files, "%s\\files", udir);
    wipe(files);
    _mkdir(udir);
    if (_mkdir(files)) return 0;
    snprintf(p, sizeof p, "%s\\update.zip", udir);
    FILE *f = fopen(p, "wb");
    int ok = f && fwrite(data, 1, len, f) == len;
    if (f && fclose(f)) ok = 0;
    if (!ok) { LOG("update: cannot write %s", p); return 0; }

    GetSystemDirectoryA(sys, sizeof sys - 16);
    strcat(sys, "\\tar.exe");
    snprintf(cmd, sizeof cmd, "tar.exe -xf ..\\update.zip");
    ok = run_wait(sys, cmd, files, 60000) == 0;
    DeleteFileA(p);
    if (!ok) { LOG("update: unpacking failed"); return 0; }

    /* the package's program has its release name, whatever this copy is called */
    snprintf(p, sizeof p, "%s\\" APP_EXE, files);
    const char *v = rel->tag + (*rel->tag == 'v' || *rel->tag == 'V');
    snprintf(cmd, sizeof cmd, "\"" APP_EXE "\" --check-version %s", v);
    if (run_wait(p, cmd, files, 20000) != 0) { LOG("update: the new program did not report version %s", v); return 0; }

    snprintf(old, sizeof old, "%s.old", self);
    DeleteFileA(old);
    if (!MoveFileA(self, old)) { LOG("update: cannot rename %s (%lu)", self, GetLastError()); return 0; }
    if (!CopyFileA(p, self, TRUE)) {
        LOG("update: cannot write %s (%lu)", self, GetLastError());
        DeleteFileA(self);
        MoveFileA(old, self);
        return 0;
    }
    DeleteFileA(p);
    /* documents that go with it; a failure here is harmless */
    WIN32_FIND_DATAA fd;
    snprintf(p, sizeof p, "%s\\*", files);
    HANDLE h = FindFirstFileA(p, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            snprintf(p, sizeof p, "%s\\%s", files, fd.cFileName);
            snprintf(q, sizeof q, "%s\\%s", exe_dir, fd.cFileName);
            CopyFileA(p, q, FALSE);
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    wipe(files);
    wipe(udir);
    return 1;
}

static int auto_update_setting(const char *data_dir) {
    char p[600], line[128];
    int v = 1, x;
    snprintf(p, sizeof p, "%s/settings.ini", data_dir);
    FILE *f = fopen(p, "r");
    if (!f) return 1;
    while (fgets(line, sizeof line, f)) if (sscanf(line, "update=%d", &x) == 1) v = x != 0;
    fclose(f);
    return v;
}

void update_check(const char *exe_dir, const char *data_dir) {
    char p[700], self[600];
    GetModuleFileNameA(NULL, self, sizeof self);
    snprintf(p, sizeof p, "%s.old", self);
    /* left by the previous update; the old process may need a moment to end */
    for (int i = 0; i < 20 && GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES && !DeleteFileA(p); i++) Sleep(100);

    g_auto_update = auto_update_setting(data_dir);
    if (!g_auto_update || getenv("RECOMP_NO_UPDATE")) return;

    /* never freed: a check that outlives the wait may still write to it */
    static char *json;
    HANDLE t = (HANDLE)_beginthreadex(NULL, 0, check_thread, &json, 0, NULL);
    if (!t) return;
    DWORD w = WaitForSingleObject(t, CHECK_WAIT_MS);
    CloseHandle(t);
    if (w != WAIT_OBJECT_0 || !json) { LOG("update check: no release information"); return; }
    Release rel;
    int ok = parse_release(json, &rel);
    LOG("update check: this is %s, latest release %s%s", APP_VERSION, rel.tag[0] ? rel.tag : "?",
        ok ? "" : " (no verifiable package)");
    if (!ok || !newer(rel.tag, APP_VERSION)) return;

    /* a release that failed to install is left alone for a day */
    char fail[600], ftag[64] = "";
    long long when = 0;
    snprintf(fail, sizeof fail, "%s/update_failed.txt", data_dir);
    FILE *f = fopen(fail, "r");
    if (f) { if (fscanf(f, "%63s %lld", ftag, &when) != 2) ftag[0] = 0; fclose(f); }
    if (!strcmp(ftag, rel.tag) && (long long)time(NULL) - when < 86400) {
        LOG("update: %s failed recently, trying again later", rel.tag);
        return;
    }

    char udir[700], *data;
    size_t len;
    int cancelled = 0;
    snprintf(udir, sizeof udir, "%s\\update", data_dir);
    for (char *c = udir; *c; c++) if (*c == '/') *c = '\\';
    ok = fetch(&rel, &data, &len, &cancelled);
    if (ok) { ok = install(&rel, data, len, exe_dir, udir); free(data); }
    if (!ok) {
        char files[800];
        snprintf(files, sizeof files, "%s\\files", udir);
        wipe(files);
        wipe(udir);
        LOG("update to %s %s; continuing with %s", rel.tag, cancelled ? "cancelled" : "failed", APP_VERSION);
        if ((f = fopen(fail, "w")) != NULL) { fprintf(f, "%s %lld\n", rel.tag, (long long)time(NULL)); fclose(f); }
        return;
    }
    remove(fail);
    LOG("update: installed %s, restarting", rel.tag);
    SetEnvironmentVariableA("RECOMP_NO_UPDATE", "1");      /* for the restarted program only */
    STARTUPINFOA si = {0};
    PROCESS_INFORMATION pi;
    si.cb = sizeof si;
    char *cmd = _strdup(GetCommandLineA());
    if (CreateProcessA(self, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
        exit(0);
    }
    /* could not start it: go on with the old code, the new one is used next time */
}
#endif
