/*
 * wimfs: read-only WIM/ESD/SWM image mount using WinFsp + wimlib.
 *
 * Usage: wim.exe [<--TempPath=DIR> | [--TempPath DIR]] <archive.wim|.esd> <mount-directory> [image-index]
 *         wim.exe /unmount <mount-directory>  (Dokan or WinFsp directory mounts; see unmount_mountpoint)
 * Optional --TempPath (-TempPath, /TempPath): parent for wimlib per-file extract (default: %%TEMP%%).
 * TempPath must already exist (caller-created). Directory mounts: caller ensures the parent path
 * exists (e.g. D:\\). The leaf (e.g. D:\\mount) is created here before Dokan if missing; for WinFsp
 * the leaf must not exist yet (WinFsp creates it on mount).
 *
 * Requires: wimlib (libwim.lib + libwim-*.dll). WinFsp: headers third_party\WinFsp\inc,
 * libs third_party\WinFsp\lib (winfsp-{x86|x64|a64}.lib); at runtime place winfsp-<arch>.dll
 * next to this exe. Optional portable kernel driver: same folder
 * winfsp-<arch>.sys (see winfsp_arch_tag_w) — on first use registers SCM service WinFsp if absent.
 * x86/x64: optional dokan.dll (Dokan 0.6.x) — try first unless MOUNT_BACKEND=winfsp. Headers from
 * third_party\dokany-0.6.0\dokan (reference). Runtime: dokan.dll; optional side-by-side dokan.sys +
 * mounter.exe — wim.exe can register/start SCM services Dokan + DokanMounter (like dokanctl /i a),
 * or use a Dokan MSI install (System32\drivers\dokan.sys + installed mounter).
 * See third_party/wimlib/README_WINDOWS.txt: wimlib.h must come from the same CPU folder
 * as libwim.lib (x86 / x64 / arm64); versions may differ per architecture (e.g. XP vs ARM64).
 *
 * Backends: on x86/x64, try Dokan 0.6 (dokan.dll) first unless MOUNT_BACKEND=winfsp; on failure
 * or MOUNT_BACKEND=auto (default), fall back to WinFsp. ARM64 uses WinFsp only. Set
 * MOUNT_BACKEND=dokan to require Dokan (no fallback). Legacy env name WIMFS_BACKEND is still read
 * if MOUNT_BACKEND is unset.
 */

//#define UNICODE
//#define _UNICODE
#define _CRT_SECURE_NO_WARNINGS

#include <windows.h>
#include <winfsp/winfsp.h>
#include <sddl.h>

#include <wimlib.h>

#if defined(_M_ARM64)
#define WIM_TRY_DOKAN 0
#else
#define WIM_TRY_DOKAN 1
#include <dokan.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <stdarg.h>
#include <share.h>
#include <winsvc.h>
#include <locale.h>

#include "ud.h"

#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "ntdll.lib")

/* Avoid calling WinFsp's FspNtStatusFromWin32 from Dokan paths (would force-load winfsp DLL). */
static NTSTATUS wim_ntstatus_from_win32(DWORD gle)
{
    if (gle == NO_ERROR)
        return STATUS_SUCCESS;
    return NTSTATUS_FROM_WIN32(gle);
}


#define ALLOC_UNIT 4096ULL
#define WIM_PATH_MAX 32768

typedef struct WimDentry {
    wchar_t *path;
    uint32_t attributes;
    uint32_t reparse_tag;
    uint64_t size;
    uint64_t ctime, atime, mtime, chtime;
    BYTE *sd;
    SIZE_T sd_len;
    int is_directory;
} WimDentry;

typedef struct WimFs {
    WIMStruct *wim;
    int image;
    WCHAR volume_label[32];

    WimDentry *e;
    int *sorted;
    size_t n;

    CRITICAL_SECTION extract_cs;

    /*
     * WinFsp default file context model: the same path must map to the same FileContext
     * (file node) until the last handle closes. We refcount OpenCtx and share one node
     * per dentry (or one for the volume root "\\").
     */
    CRITICAL_SECTION open_node_cs;
    struct OpenCtx *open_node_head;

    BYTE default_sd[512];
    SIZE_T default_sd_len;

    /* Empty: use GetTempPathW for wimlib extract scratch; else GetTempFileNameW under this directory */
    WCHAR scratch_root[MAX_PATH];
} WimFs;

typedef struct OpenCtx {
    LONG ref;
    struct OpenCtx *node_next;

    int is_directory;
    wchar_t dir_path[WIM_PATH_MAX];
    const WimDentry *ent;

    HANDLE data;
    wchar_t temp_dir[MAX_PATH];

    /* Shared file handle is not seek-safe across threads without serialization */
    CRITICAL_SECTION read_cs;
    int read_cs_inited;
} OpenCtx;

static int open_ctx_is_volume_root(const OpenCtx *o)
{
    return o && o->is_directory && o->dir_path[0] == L'\\' && o->dir_path[1] == 0;
}

static WimFs *g_Wim;
HANDLE g_ExitEvent = 0;
FSP_FILE_SYSTEM *g_FsInstance;

#if WIM_TRY_DOKAN
WCHAR g_dokanMountPath[MAX_PATH];
volatile LONG g_dokanMounted;
typedef int(WINAPI *PFN_DokanMain)(PDOKAN_OPTIONS, PDOKAN_OPERATIONS);
typedef BOOL(WINAPI *PFN_DokanRemoveMountPoint)(LPCWSTR);
static PFN_DokanMain g_pfn_DokanMain;
/*
 * Shared with ud.c: Ctrl+C calls DokanRemoveMountPoint via this pointer. UD mounts load dokan.dll
 * from ud_try_mount_dokan, not try_mount_dokan, so it must not be file-static in main.c only.
 */
PFN_DokanRemoveMountPoint g_DokanRemoveMountPoint;
#endif

/* Debug: set WIMFS_LOG=1, or pass --debug / -debug / /debug on the command line. Log: %TEMP%\\wimfs_trace.log */
static FILE *g_wimfs_trace;
static CRITICAL_SECTION g_wimfs_trace_lock;
static int g_wimfs_trace_inited;
static int g_wimfs_trace_atexit_registered;
static WCHAR g_wimfs_last_line[768];

static int wimfs_trace_wanted(int argc, wchar_t **argv)
{
    WCHAR ev[16];
    int i;

    if (GetEnvironmentVariableW(L"WIMFS_LOG", ev, 16) && ev[0] &&
        (ev[0] == L'1' || !_wcsicmp(ev, L"yes")))
        return 1;
    if (GetCommandLineW() && (wcsstr(GetCommandLineW(), L"--debug") || wcsstr(GetCommandLineW(), L"-debug")))
        return 1;
    for (i = 1; i < argc; i++) {
        if (!argv[i])
            continue;
        if (!_wcsicmp(argv[i], L"/debug") || !_wcsicmp(argv[i], L"-debug") || !_wcsicmp(argv[i], L"--debug"))
            return 1;
    }
    return 0;
}

static void wimfs_trace_close(void)
{
    if (g_wimfs_trace) {
        fwprintf(g_wimfs_trace, L"======== wimfs trace end ========\n");
        fflush(g_wimfs_trace);
        fclose(g_wimfs_trace);
        g_wimfs_trace = 0;
    }
    if (g_wimfs_trace_inited) {
        DeleteCriticalSection(&g_wimfs_trace_lock);
        g_wimfs_trace_inited = 0;
    }
}

static void wimfs_trace_open(void)
{
    wchar_t path[MAX_PATH];

    if (!GetTempPathW(MAX_PATH, path))
        wcscpy_s(path, MAX_PATH, L".\\");
    wcscat_s(path, MAX_PATH, L"wimfs_trace.log");
    g_wimfs_trace = _wfsopen(path, L"a, ccs=UTF-8", _SH_DENYWR);
    if (!g_wimfs_trace)
        g_wimfs_trace = _wfopen(L"wimfs_trace.log", L"a, ccs=UTF-8");
    if (g_wimfs_trace) {
        InitializeCriticalSection(&g_wimfs_trace_lock);
        g_wimfs_trace_inited = 1;
        fwprintf(g_wimfs_trace,
            L"\n======== wimfs trace start pid=%lu ========\n", (unsigned long)GetCurrentProcessId());
        fflush(g_wimfs_trace);
        if (!g_wimfs_trace_atexit_registered) {
            atexit(wimfs_trace_close);
            g_wimfs_trace_atexit_registered = 1;
        }
    }
}

static void wimfs_trace_fmt(const char *tag, const wchar_t *fmt, ...)
{
    wchar_t buf[704];
    va_list ap;

    if (!g_wimfs_trace || !g_wimfs_trace_inited)
        return;
    va_start(ap, fmt);
    vswprintf_s(buf, 704, fmt, ap);
    va_end(ap);
    wcsncpy_s(g_wimfs_last_line, 768, buf, _TRUNCATE);
    EnterCriticalSection(&g_wimfs_trace_lock);
    fwprintf(g_wimfs_trace, L"%04lX %hs %ls\n",
        (unsigned long)GetCurrentThreadId(), tag, buf);
    fflush(g_wimfs_trace);
    LeaveCriticalSection(&g_wimfs_trace_lock);
}

static LONG WINAPI wimfs_unhandled_filter(PEXCEPTION_POINTERS Info)
{
    wchar_t path[MAX_PATH];
    FILE *f;

    if (!GetTempPathW(MAX_PATH, path))
        wcscpy_s(path, MAX_PATH, L".\\");
    wcscat_s(path, MAX_PATH, L"wimfs_lastcrash.txt");
    f = _wfopen(path, L"a, ccs=UTF-8");
    if (f) {
        fwprintf(f,
            L"UnhandledException pid=%lu code=0x%08X addr=%p\nlast_trace=%ls\n",
            (unsigned long)GetCurrentProcessId(),
            (unsigned)Info->ExceptionRecord->ExceptionCode,
            (void *)Info->ExceptionRecord->ExceptionAddress,
            g_wimfs_last_line);
        fflush(f);
        fclose(f);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

static UINT64 timespec_to_filetime(const struct wimlib_timespec *ts)
{
    if (!ts)
        return 0;
    /* 100ns intervals since 1601-01-01 */
    INT64 sec = (INT64)ts->tv_sec;
    UINT64 ticks = (UINT64)(sec + 11644473600LL) * 10000000ULL;
    ticks += (UINT64)(ts->tv_nsec / 100);
    return ticks;
}

static VOID fill_file_info(const WimDentry *d, FSP_FSCTL_FILE_INFO *fi)
{
    memset(fi, 0, sizeof *fi);
    fi->FileAttributes = d->attributes;
    fi->ReparseTag = d->reparse_tag;
    fi->FileSize = d->size;
    fi->AllocationSize = (d->size + ALLOC_UNIT - 1) / ALLOC_UNIT * ALLOC_UNIT;
    fi->CreationTime = d->ctime;
    fi->LastAccessTime = d->atime;
    fi->LastWriteTime = d->mtime;
    fi->ChangeTime = d->chtime;
    fi->IndexNumber = 0;
    fi->HardLinks = 0;
}

static void u64_to_filetime(UINT64 u, FILETIME *ft)
{
    if (!ft)
        return;
    ft->dwLowDateTime = (DWORD)u;
    ft->dwHighDateTime = (DWORD)(u >> 32);
}

static void strip_trailing_backslash(PWSTR p)
{
    size_t n = wcslen(p);
    while (n > 1 && p[n - 1] == L'\\')
        p[--n] = 0;
}

/*
 * WinFsp sometimes passes an empty string for the volume root. Normalize to L"\\"
 * after stripping redundant trailing backslashes.
 */
static void normalize_open_path(wchar_t *tmp)
{
    strip_trailing_backslash(tmp);
    if (tmp[0] == 0) {
        tmp[0] = L'\\';
        tmp[1] = 0;
    }
}

static wchar_t *dup_norm_path(const wchar_t *src)
{
    wchar_t *p;
    size_t i, len;

    if (!src)
        return 0;
    p = _wcsdup(src);
    if (!p)
        return 0;
    for (i = 0; p[i]; i++) {
        if (p[i] == L'/')
            p[i] = L'\\';
    }
    if (p[0] != L'\\') {
        len = wcslen(p);
        wchar_t *n2 = malloc((len + 2) * sizeof(wchar_t));
        if (!n2) {
            free(p);
            return 0;
        }
        n2[0] = L'\\';
        memcpy(n2 + 1, p, (len + 1) * sizeof(wchar_t));
        free(p);
        p = n2;
    }
    strip_trailing_backslash(p);
    return p;
}

static int is_root_child(const wchar_t *path)
{
    if (!path || path[0] != L'\\')
        return 0;
    return wcschr(path + 1, L'\\') == NULL;
}

static int is_direct_child(const wchar_t *parent, const wchar_t *path)
{
    size_t pl;

    if (!parent || !path)
        return 0;
    if (parent[0] == L'\\' && parent[1] == 0)
        return is_root_child(path);

    pl = wcslen(parent);
    if (wcslen(path) <= pl)
        return 0;
    if (_wcsnicmp(path, parent, pl) != 0)
        return 0;
    if (path[pl] != L'\\')
        return 0;
    return wcschr(path + pl + 1, L'\\') == NULL;
}

static const wchar_t *leaf_name(const wchar_t *path)
{
    const wchar_t *s = wcsrchr(path, L'\\');
    return s ? (s + 1) : path;
}

static void dentry_to_by_handle_information(const WimDentry *d, BY_HANDLE_FILE_INFORMATION *bhi)
{
    memset(bhi, 0, sizeof *bhi);
    if (!d) {
        bhi->dwFileAttributes = FILE_ATTRIBUTE_DIRECTORY;
        bhi->nNumberOfLinks = 1;
        bhi->dwVolumeSerialNumber = 0x19831120;
        return;
    }
    bhi->dwFileAttributes = d->attributes;
    bhi->dwVolumeSerialNumber = 0x19831120;
    u64_to_filetime(d->ctime, &bhi->ftCreationTime);
    u64_to_filetime(d->atime, &bhi->ftLastAccessTime);
    u64_to_filetime(d->mtime, &bhi->ftLastWriteTime);
    bhi->nFileSizeHigh = (DWORD)(d->size >> 32);
    bhi->nFileSizeLow = (DWORD)d->size;
    bhi->nNumberOfLinks = 1;
}

static void dentry_to_find_dataw(const WimDentry *d, WIN32_FIND_DATAW *fd)
{
    memset(fd, 0, sizeof *fd);
    if (!d) {
        fd->dwFileAttributes = FILE_ATTRIBUTE_DIRECTORY;
        fd->cFileName[0] = L'.';
        fd->cFileName[1] = 0;
        return;
    }
    fd->dwFileAttributes = d->attributes;
    wcsncpy_s(fd->cFileName, MAX_PATH, leaf_name(d->path), _TRUNCATE);
    u64_to_filetime(d->ctime, &fd->ftCreationTime);
    u64_to_filetime(d->atime, &fd->ftLastAccessTime);
    u64_to_filetime(d->mtime, &fd->ftLastWriteTime);
    fd->nFileSizeHigh = (DWORD)(d->size >> 32);
    fd->nFileSizeLow = (DWORD)d->size;
}

static int __cdecl qsort_idx_cmp(void *ctx, const void *a, const void *b)
{
    WimFs *W = ctx;
    int ia = *(const int *)a;
    int ib = *(const int *)b;
    return _wcsicmp(W->e[ia].path, W->e[ib].path);
}

static const WimDentry *lookup_path(WimFs *W, const wchar_t *path)
{
    int lo, hi;

    if (!W || !path || !path[0])
        return 0;
    lo = 0;
    hi = (int)W->n - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        int ix = W->sorted[mid];
        int c = _wcsicmp(W->e[ix].path, path);
        if (c == 0)
            return &W->e[ix];
        if (c < 0)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    return 0;
}

typedef struct {
    WimDentry *e;
    int *sorted;
    size_t n, cap;
} BuildCtx;

static int grow_entries(BuildCtx *B)
{
    if (B->n < B->cap)
        return 0;
    {
        size_t ncap = B->cap ? B->cap * 2 : 4096;
        WimDentry *ne = realloc(B->e, ncap * sizeof(WimDentry));
        int *ns = realloc(B->sorted, ncap * sizeof(int));
        if (!ne || !ns) {
            free(ne);
            free(ns);
            return -1;
        }
        B->e = ne;
        B->sorted = ns;
        B->cap = ncap;
    }
    return 0;
}

static int iterate_cb(const struct wimlib_dir_entry *d, void *user)
{
    BuildCtx *B = user;
    WimDentry *ent;
    const wchar_t *fp;
    wchar_t *path;

    if (!d->filename && d->depth == 0)
        return 0;

    fp = d->full_path;
    if (!fp || !*fp)
        return 0;

    path = dup_norm_path(fp);
    if (!path)
        return WIMLIB_ERR_NOMEM;

    if (grow_entries(B)) {
        free(path);
        return WIMLIB_ERR_NOMEM;
    }

    ent = &B->e[B->n];
    memset(ent, 0, sizeof *ent);
    ent->path = path;
    ent->attributes = d->attributes;
    ent->reparse_tag = (d->attributes & WIMLIB_FILE_ATTRIBUTE_REPARSE_POINT) ? d->reparse_tag : 0;
    ent->is_directory = (d->attributes & WIMLIB_FILE_ATTRIBUTE_DIRECTORY) ? 1 : 0;
    if (!ent->is_directory)
        ent->size = d->streams[0].resource.uncompressed_size;
    else
        ent->size = 0;

    ent->ctime = timespec_to_filetime(&d->creation_time);
    ent->atime = timespec_to_filetime(&d->last_access_time);
    ent->mtime = timespec_to_filetime(&d->last_write_time);
    ent->chtime = ent->mtime;

    if (d->security_descriptor && d->security_descriptor_size) {
        ent->sd = malloc(d->security_descriptor_size);
        if (!ent->sd) {
            free(path);
            ent->path = 0;
            return WIMLIB_ERR_NOMEM;
        }
        memcpy(ent->sd, d->security_descriptor, d->security_descriptor_size);
        ent->sd_len = d->security_descriptor_size;
    }

    B->sorted[B->n] = (int)B->n;
    B->n++;
    return 0;
}

static void free_wim_index(WimFs *W)
{
    size_t i;

    if (!W)
        return;
    for (i = 0; i < W->n; i++) {
        free(W->e[i].path);
        free(W->e[i].sd);
    }
    free(W->e);
    free(W->sorted);
    W->e = 0;
    W->sorted = 0;
    W->n = 0;
}

static int build_index(WimFs *W)
{
    BuildCtx B;
    int err;

    memset(&B, 0, sizeof B);
    err = wimlib_iterate_dir_tree(W->wim, W->image, L"\\",
        WIMLIB_ITERATE_DIR_TREE_FLAG_RECURSIVE | WIMLIB_ITERATE_DIR_TREE_FLAG_RESOURCES_NEEDED,
        iterate_cb, &B);
    if (err)
        goto fail;

    W->e = B.e;
    W->sorted = B.sorted;
    W->n = B.n;
    if (W->n == 0) {
        err = WIMLIB_ERR_INVALID_IMAGE;
        goto fail;
    }

    qsort_s(W->sorted, W->n, sizeof(int), qsort_idx_cmp, W);
    return 0;

fail:
    free(B.e);
    free(B.sorted);
    return err;
}

/*
 * Official WinFsp binary names (see winfsp fsctl.h FSP_FSCTL_PRODUCT_FILE_ARCH):
 *   x86   -> winfsp-x86.sys / winfsp-x86.dll
 *   x64   -> winfsp-x64.sys / winfsp-x64.dll
 *   ARM64 -> winfsp-a64.sys / winfsp-a64.dll
 * This .exe must be built for the matching architecture; we do not load cross-arch drivers.
 */
static const wchar_t *winfsp_arch_tag_w(void)
{
#if defined(_ARM64_)
    return L"a64";
#elif defined(_AMD64_)
    return L"x64";
#elif defined(_X86_)
    return L"x86";
#else
#error WinFsp: unsupported target architecture (need _X86_, _AMD64_, or _ARM64_)
#endif
}

static BOOL winfsp_build_sys_path_next_to_exe(wchar_t *out, size_t cch)
{
    wchar_t exe[MAX_PATH];
    wchar_t *slash;

    if (!out || cch < MAX_PATH)
        return FALSE;
    if (!GetModuleFileNameW(NULL, exe, MAX_PATH))
        return FALSE;
    slash = wcsrchr(exe, L'\\');
    if (!slash)
        return FALSE;
    slash[1] = 0;
    if (swprintf_s(out, cch, L"%swinfsp-%ls.sys", exe, winfsp_arch_tag_w()) < 0)
        return FALSE;
    return TRUE;
}

#define WINFSP_SVC_NAME L"WinFsp"

/*
 * If winfsp-<arch>.sys exists next to this executable, register the WinFsp kernel
 * service to load that file (demand start) and start it. If the file is absent,
 * do nothing so a normal WinFsp MSI install can still satisfy preflight.
 * Returns FALSE only when the local .sys is present but SCM setup/start fails.
 */
static BOOL winfsp_try_portable_driver_setup(void)
{
    wchar_t sys_path[MAX_PATH];
    DWORD att;
    SC_HANDLE scm = NULL;
    SC_HANDLE svc = NULL;
    BOOL ok = TRUE;

    if (!winfsp_build_sys_path_next_to_exe(sys_path, MAX_PATH))
        return TRUE;

    att = GetFileAttributesW(sys_path);
    if (att == INVALID_FILE_ATTRIBUTES)
        return TRUE;

    scm = OpenSCManagerW(NULL, SERVICES_ACTIVE_DATABASE,
        SC_MANAGER_CONNECT | SC_MANAGER_CREATE_SERVICE);
    if (!scm) {
        fwprintf(stderr, L"WinFsp portable driver: OpenSCManagerW failed (%u). Run as administrator.\n",
            (unsigned)GetLastError());
        return FALSE;
    }

    svc = OpenServiceW(scm, WINFSP_SVC_NAME, SERVICE_START | SERVICE_QUERY_STATUS | SERVICE_CHANGE_CONFIG);
    if (!svc) {
        if (GetLastError() != ERROR_SERVICE_DOES_NOT_EXIST) {
            fwprintf(stderr, L"WinFsp portable driver: OpenService failed (%u).\n", (unsigned)GetLastError());
            ok = FALSE;
            goto out;
        }
        svc = CreateServiceW(
            scm,
            WINFSP_SVC_NAME,
            L"WinFsp",
            SERVICE_ALL_ACCESS,
            SERVICE_KERNEL_DRIVER,
            SERVICE_DEMAND_START,
            SERVICE_ERROR_NORMAL,
            sys_path,
            NULL,
            NULL,
            NULL,
            NULL,
            NULL);
        if (!svc) {
            DWORD gle = GetLastError();

            if (gle == ERROR_SERVICE_EXISTS) {
                svc = OpenServiceW(scm, WINFSP_SVC_NAME,
                    SERVICE_START | SERVICE_QUERY_STATUS | SERVICE_CHANGE_CONFIG);
            }
            if (!svc) {
                fwprintf(stderr, L"WinFsp portable driver: CreateService failed (%u).\n", (unsigned)gle);
                ok = FALSE;
                goto out;
            }
        }
    }

    {
        SERVICE_STATUS ss;

        if (!QueryServiceStatus(svc, &ss)) {
            fwprintf(stderr, L"WinFsp portable driver: QueryServiceStatus failed (%u).\n",
                (unsigned)GetLastError());
            ok = FALSE;
            goto out;
        }
        if (ss.dwCurrentState != SERVICE_RUNNING) {
            if (!StartServiceW(svc, 0, NULL)) {
                DWORD gle = GetLastError();

                if (gle != ERROR_SERVICE_ALREADY_RUNNING) {
                    fwprintf(stderr, L"WinFsp portable driver: StartService failed (%u).\n", (unsigned)gle);
                    ok = FALSE;
                    goto out;
                }
            }
        }
    }

out:
    if (svc)
        CloseServiceHandle(svc);
    if (scm)
        CloseServiceHandle(scm);
    return ok;
}

static void start_winfsp_service(void)
{
    NTSTATUS st;
    ULONG ver;

    st = FspFsctlServiceVersion(&ver);
    if (NT_SUCCESS(st))
        return;

    st = FspFsctlStartService();
    if (NT_SUCCESS(st))
        return;

    {
        SC_HANDLE scm = OpenSCManagerW(0, SERVICES_ACTIVE_DATABASE, SC_MANAGER_CONNECT);
        SC_HANDLE svc;
        if (!scm)
            return;
        svc = OpenServiceW(scm, L"WinFsp", SERVICE_START | SERVICE_QUERY_STATUS);
        if (svc) {
            if (!StartServiceW(svc, 0, 0)) {
                if (GetLastError() != ERROR_SERVICE_ALREADY_RUNNING) {
                }
            }
            CloseServiceHandle(svc);
        }
        CloseServiceHandle(scm);
    }
}

#if WIM_TRY_DOKAN
/*
 * Portable Dokan stack: if dokan.sys and/or mounter.exe sit next to this exe, register those paths
 * in SCM (admin) and start "Dokan" (kernel) + "DokanMounter" (user), mirroring dokanctl /i a.
 * If absent, do nothing so an MSI-installed Dokan still works.
 */
#define DOKAN_PORTABLE_DRIVER_SVC L"Dokan"
#define DOKAN_PORTABLE_MOUNTER_SVC L"DokanMounter"

static BOOL dokan_build_sys_path_next_to_exe(wchar_t *out, size_t cch)
{
    wchar_t exe[MAX_PATH];
    wchar_t *slash;

    if (!out || cch < MAX_PATH)
        return FALSE;
    if (!GetModuleFileNameW(NULL, exe, MAX_PATH))
        return FALSE;
    slash = wcsrchr(exe, L'\\');
    if (!slash)
        return FALSE;
    slash[1] = 0;
    if (swprintf_s(out, cch, L"%ls%ls", exe, DOKAN_DRIVER_NAME) < 0)
        return FALSE;
    return TRUE;
}

static BOOL dokan_build_mounter_path_next_to_exe(wchar_t *out, size_t cch)
{
    wchar_t exe[MAX_PATH];
    wchar_t *slash;

    if (!out || cch < MAX_PATH)
        return FALSE;
    if (!GetModuleFileNameW(NULL, exe, MAX_PATH))
        return FALSE;
    slash = wcsrchr(exe, L'\\');
    if (!slash)
        return FALSE;
    slash[1] = 0;
    if (swprintf_s(out, cch, L"%lsmounter.exe", exe) < 0)
        return FALSE;
    return TRUE;
}

static BOOL dokan_try_portable_scm_service(const wchar_t *svcName, const wchar_t *displayName,
    DWORD serviceType, const wchar_t *binPath, const wchar_t *whatLabel)
{
    DWORD att;
    SC_HANDLE scm = NULL;
    SC_HANDLE svc = NULL;
    BOOL ok = TRUE;

    att = GetFileAttributesW(binPath);
    if (att == INVALID_FILE_ATTRIBUTES)
        return TRUE;

    scm = OpenSCManagerW(NULL, SERVICES_ACTIVE_DATABASE,
        SC_MANAGER_CONNECT | SC_MANAGER_CREATE_SERVICE);
    if (!scm) {
        fwprintf(stderr, L"%ls: OpenSCManagerW failed (%u). Run as administrator.\n",
            whatLabel, (unsigned)GetLastError());
        return FALSE;
    }

    svc = OpenServiceW(scm, svcName, SERVICE_START | SERVICE_QUERY_STATUS | SERVICE_CHANGE_CONFIG);
    if (!svc) {
        if (GetLastError() != ERROR_SERVICE_DOES_NOT_EXIST) {
            fwprintf(stderr, L"%ls: OpenService failed (%u).\n", whatLabel, (unsigned)GetLastError());
            ok = FALSE;
            goto out;
        }
        svc = CreateServiceW(
            scm,
            svcName,
            displayName,
            SERVICE_ALL_ACCESS,
            serviceType,
            SERVICE_DEMAND_START,
            SERVICE_ERROR_NORMAL,
            binPath,
            NULL,
            NULL,
            NULL,
            NULL,
            NULL);
        if (!svc) {
            DWORD gle = GetLastError();

            if (gle == ERROR_SERVICE_EXISTS) {
                svc = OpenServiceW(scm, svcName,
                    SERVICE_START | SERVICE_QUERY_STATUS | SERVICE_CHANGE_CONFIG);
            }
            if (!svc) {
                fwprintf(stderr, L"%ls: CreateService failed (%u).\n", whatLabel, (unsigned)gle);
                ok = FALSE;
                goto out;
            }
        }
    }

    {
        SERVICE_STATUS ss;

        if (!QueryServiceStatus(svc, &ss)) {
            fwprintf(stderr, L"%ls: QueryServiceStatus failed (%u).\n", whatLabel,
                (unsigned)GetLastError());
            ok = FALSE;
            goto out;
        }
        if (ss.dwCurrentState != SERVICE_RUNNING) {
            if (!StartServiceW(svc, 0, NULL)) {
                DWORD gle = GetLastError();

                if (gle != ERROR_SERVICE_ALREADY_RUNNING) {
                    fwprintf(stderr, L"%ls: StartService failed (%u).\n", whatLabel, (unsigned)gle);
                    ok = FALSE;
                    goto out;
                }
            }
        }
    }

out:
    if (svc)
        CloseServiceHandle(svc);
    if (scm)
        CloseServiceHandle(scm);
    return ok;
}

static BOOL dokan_try_portable_driver_setup(void)
{
    wchar_t sys_path[MAX_PATH];

    if (!dokan_build_sys_path_next_to_exe(sys_path, MAX_PATH))
        return TRUE;
    return dokan_try_portable_scm_service(DOKAN_PORTABLE_DRIVER_SVC, DOKAN_PORTABLE_DRIVER_SVC,
        SERVICE_FILE_SYSTEM_DRIVER, sys_path, L"Dokan portable driver");
}

static BOOL dokan_try_portable_mounter_setup(void)
{
    wchar_t exe_path[MAX_PATH];

    if (!dokan_build_mounter_path_next_to_exe(exe_path, MAX_PATH))
        return TRUE;
    return dokan_try_portable_scm_service(DOKAN_PORTABLE_MOUNTER_SVC, DOKAN_PORTABLE_MOUNTER_SVC,
        SERVICE_WIN32_OWN_PROCESS, exe_path, L"Dokan portable mounter");
}

BOOL dokan_try_portable_stack_setup(void)
{
    if (!dokan_try_portable_driver_setup())
        return FALSE;
    if (!dokan_try_portable_mounter_setup())
        return FALSE;
    return TRUE;
}
#endif /* WIM_TRY_DOKAN */

BOOL preflight_mount(PWSTR mountPoint)
{
    NTSTATUS st;

    /*
     * Do not use FspFsctlServiceVersion() as a readiness gate: it returns
     * STATUS_UNSUCCESSFUL (0xC0000001) when it cannot read the driver version
     * from the WinFsp *service* entry (SCM), which is common on misconfigured
     * installs but is not the same as "wrong CPU architecture".
     * FspFileSystemPreflight starts the service and probes the control device.
     */
    st = FspFileSystemPreflight(L"" FSP_FSCTL_DISK_DEVICE_NAME, mountPoint);
    if (!NT_SUCCESS(st)) {
        fwprintf(stderr, L"FspFileSystemPreflight failed: 0x%08X\n", (unsigned)st);
        if ((unsigned)st == 0xC000000Eu) {
            fwprintf(stderr,
                L"  STATUS_NO_SUCH_DEVICE: cannot open the WinFsp control device.\n"
                L"  Place winfsp-x86.sys / winfsp-x64.sys / winfsp-a64.sys (match this exe)\n"
                L"  next to wim.exe and run as administrator, or install WinFsp from https://winfsp.dev/\n"
                L"  and ensure the WinFsp service can start (services.msc -> WinFsp).\n");
        } else if ((unsigned)st == 0xC0000035u) {
            fwprintf(stderr,
                L"  STATUS_OBJECT_NAME_COLLISION: WinFsp directory mounts expect the mount path\n"
                L"  not to exist yet (WinFsp creates the leaf directory). If you pre-created an empty\n"
                L"  folder, use a parent path and let WinFsp create the final directory name, or use Dokan.\n");
        } else if ((unsigned)st == 0xC0000001u) {
            fwprintf(stderr,
                L"  STATUS_UNSUCCESSFUL: WinFsp preflight could not complete (driver/service).\n"
                L"  Reinstall WinFsp; if the WinFsp service is missing, the installer did not finish.\n");
        }
        return FALSE;
    }
    return TRUE;
}

BOOL wim_mount_winfsp_stack_prepare(void)
{
    if (!winfsp_try_portable_driver_setup())
        return FALSE;
    start_winfsp_service();
    return TRUE;
}

BOOL wim_mount_preflight(PWSTR mountPoint)
{
    return preflight_mount(mountPoint);
}

static NTSTATUS GetVolumeInfo(FSP_FILE_SYSTEM *FileSystem, FSP_FSCTL_VOLUME_INFO *VolumeInfo)
{
    WimFs *W;
    struct wimlib_wim_info wi;

    if (!FileSystem || !VolumeInfo)
        return STATUS_INVALID_PARAMETER;
    W = FileSystem->UserContext;
    wimfs_trace_fmt("GetVolumeInfo", L"W=%p", (void *)W);

    if (!W)
        return STATUS_INVALID_PARAMETER;

    memset(VolumeInfo, 0, sizeof *VolumeInfo);
    memset(&wi, 0, sizeof wi);
    if (W && W->wim && wimlib_get_wim_info(W->wim, &wi) == 0) {
        VolumeInfo->TotalSize = wi.total_bytes;
        if (VolumeInfo->TotalSize == 0)
            VolumeInfo->TotalSize = 1ULL << 30;
        VolumeInfo->FreeSize = 0;
    } else {
        VolumeInfo->TotalSize = 1ULL << 30;
        VolumeInfo->FreeSize = 0;
    }

    wcscpy_s(VolumeInfo->VolumeLabel,
        sizeof(VolumeInfo->VolumeLabel) / sizeof(WCHAR),
        W->volume_label);
    VolumeInfo->VolumeLabelLength =
        (UINT16)(wcslen(VolumeInfo->VolumeLabel) * sizeof(WCHAR));

    return STATUS_SUCCESS;
}

static NTSTATUS SetVolumeLabel_(FSP_FILE_SYSTEM *FileSystem,
    PWSTR VolumeLabel, FSP_FSCTL_VOLUME_INFO *VolumeInfo)
{
    UNREFERENCED_PARAMETER(FileSystem);
    UNREFERENCED_PARAMETER(VolumeLabel);
    UNREFERENCED_PARAMETER(VolumeInfo);
    return STATUS_INVALID_DEVICE_REQUEST;
}

static NTSTATUS GetSecurityByName(FSP_FILE_SYSTEM *FileSystem,
    PWSTR FileName, PUINT32 PFileAttributes,
    PSECURITY_DESCRIPTOR SecurityDescriptor, SIZE_T *PSecurityDescriptorSize)
{
    WimFs *W = FileSystem->UserContext;
    wchar_t tmp[WIM_PATH_MAX];
    const WimDentry *d;
    const BYTE *sd;
    SIZE_T sdlen;

    if (!FileSystem || !W)
        return STATUS_INVALID_PARAMETER;

    if (!FileName)
        return STATUS_INVALID_PARAMETER;

    wcscpy_s(tmp, WIM_PATH_MAX, FileName);
    normalize_open_path(tmp);
    wimfs_trace_fmt("GetSecurityByName", L"[%ls] attrp=%p sdp=%p",
        tmp, (void *)PFileAttributes, (void *)SecurityDescriptor);

    if (tmp[0] == L'\\' && tmp[1] == 0) {
        d = 0;
        if (PFileAttributes)
            *PFileAttributes = FILE_ATTRIBUTE_DIRECTORY;
        sd = W->default_sd;
        sdlen = W->default_sd_len;
    } else {
        d = lookup_path(W, tmp);
        if (!d)
            return STATUS_OBJECT_NAME_NOT_FOUND;
        if (PFileAttributes)
            *PFileAttributes = d->attributes;
        if (d->sd && d->sd_len &&
            IsValidSecurityDescriptor((PSECURITY_DESCRIPTOR)d->sd)) {
            sd = d->sd;
            sdlen = d->sd_len;
        } else {
            sd = W->default_sd;
            sdlen = W->default_sd_len;
        }
    }

    if (!PSecurityDescriptorSize)
        return STATUS_SUCCESS;

    if (sdlen > *PSecurityDescriptorSize) {
        *PSecurityDescriptorSize = sdlen;
        return STATUS_BUFFER_OVERFLOW;
    }

    *PSecurityDescriptorSize = sdlen;
    if (SecurityDescriptor && sdlen)
        memcpy(SecurityDescriptor, sd, sdlen);

    return STATUS_SUCCESS;
}

static NTSTATUS Create_(FSP_FILE_SYSTEM *FileSystem,
    PWSTR FileName, UINT32 CreateOptions, UINT32 GrantedAccess,
    UINT32 FileAttributes, PSECURITY_DESCRIPTOR SecurityDescriptor, UINT64 AllocationSize,
    PVOID *PFileContext, FSP_FSCTL_FILE_INFO *FileInfo)
{
    UNREFERENCED_PARAMETER(FileSystem);
    UNREFERENCED_PARAMETER(FileName);
    UNREFERENCED_PARAMETER(CreateOptions);
    UNREFERENCED_PARAMETER(GrantedAccess);
    UNREFERENCED_PARAMETER(FileAttributes);
    UNREFERENCED_PARAMETER(SecurityDescriptor);
    UNREFERENCED_PARAMETER(AllocationSize);
    UNREFERENCED_PARAMETER(PFileContext);
    UNREFERENCED_PARAMETER(FileInfo);
    return STATUS_ACCESS_DENIED;
}

static NTSTATUS wim_open_ntpath(WimFs *W, wchar_t *tmp, OpenCtx **Pctx)
{
    const WimDentry *d;
    OpenCtx *o;
    OpenCtx *p;
    int vol_root;

    if (!W || !tmp || !Pctx)
        return STATUS_INVALID_PARAMETER;

    vol_root = (tmp[0] == L'\\' && tmp[1] == 0);
    d = vol_root ? 0 : lookup_path(W, tmp);
    if (!vol_root && !d)
        return STATUS_OBJECT_NAME_NOT_FOUND;

    EnterCriticalSection(&W->open_node_cs);
    for (p = W->open_node_head; p; p = p->node_next) {
        if (vol_root) {
            if (open_ctx_is_volume_root(p)) {
                p->ref++;
                LeaveCriticalSection(&W->open_node_cs);
                *Pctx = p;
                wimfs_trace_fmt("Open", L"reuse volroot ctx=%p ref=%ld", (void *)p, p->ref);
                return STATUS_SUCCESS;
            }
        } else if (p->ent == d) {
            p->ref++;
            LeaveCriticalSection(&W->open_node_cs);
            *Pctx = p;
            wimfs_trace_fmt("Open", L"reuse ctx=%p ref=%ld path=[%.200ls]", (void *)p, p->ref, d->path);
            return STATUS_SUCCESS;
        }
    }

    o = calloc(1, sizeof *o);
    if (!o) {
        LeaveCriticalSection(&W->open_node_cs);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    o->data = INVALID_HANDLE_VALUE;
    o->ref = 1;
    o->node_next = W->open_node_head;
    W->open_node_head = o;

    if (vol_root) {
        o->is_directory = 1;
        wcscpy_s(o->dir_path, WIM_PATH_MAX, L"\\");
    } else {
        o->ent = d;
        o->is_directory = d->is_directory ? 1 : 0;
        wcscpy_s(o->dir_path, WIM_PATH_MAX, d->path);
        o->temp_dir[0] = 0;
        if (!o->is_directory) {
            InitializeCriticalSection(&o->read_cs);
            o->read_cs_inited = 1;
        }
    }

    LeaveCriticalSection(&W->open_node_cs);
    *Pctx = o;
    wimfs_trace_fmt("Open", L"new ctx=%p path=[%.200ls]", (void *)o, vol_root ? L"\\" : d->path);
    return STATUS_SUCCESS;
}

static NTSTATUS Open(FSP_FILE_SYSTEM *FileSystem,
    PWSTR FileName, UINT32 CreateOptions, UINT32 GrantedAccess,
    PVOID *PFileContext, FSP_FSCTL_FILE_INFO *FileInfo)
{
    WimFs *W;
    wchar_t tmp[WIM_PATH_MAX];
    NTSTATUS st;
    OpenCtx *o;
    int vol_root;

    UNREFERENCED_PARAMETER(CreateOptions);
    UNREFERENCED_PARAMETER(GrantedAccess);

    if (!FileSystem || !FileSystem->UserContext || !PFileContext || !FileInfo)
        return STATUS_INVALID_PARAMETER;
    W = FileSystem->UserContext;

    if (!FileName)
        return STATUS_INVALID_PARAMETER;

    wcscpy_s(tmp, WIM_PATH_MAX, FileName);
    normalize_open_path(tmp);
    wimfs_trace_fmt("Open", L"[%ls]", tmp);

    st = wim_open_ntpath(W, tmp, &o);
    if (!NT_SUCCESS(st))
        return st;

    vol_root = open_ctx_is_volume_root(o);
    if (vol_root) {
        memset(FileInfo, 0, sizeof *FileInfo);
        FileInfo->FileAttributes = FILE_ATTRIBUTE_DIRECTORY;
    } else
        fill_file_info(o->ent, FileInfo);

    *PFileContext = o;
    return STATUS_SUCCESS;
}

static NTSTATUS Overwrite_(FSP_FILE_SYSTEM *FileSystem,
    PVOID FileContext, UINT32 FileAttributes, BOOLEAN ReplaceFileAttributes, UINT64 AllocationSize,
    FSP_FSCTL_FILE_INFO *FileInfo)
{
    UNREFERENCED_PARAMETER(FileSystem);
    UNREFERENCED_PARAMETER(FileContext);
    UNREFERENCED_PARAMETER(FileAttributes);
    UNREFERENCED_PARAMETER(ReplaceFileAttributes);
    UNREFERENCED_PARAMETER(AllocationSize);
    UNREFERENCED_PARAMETER(FileInfo);
    return STATUS_ACCESS_DENIED;
}

static VOID Cleanup(FSP_FILE_SYSTEM *FileSystem,
    PVOID FileContext, PWSTR FileName, ULONG Flags)
{
    UNREFERENCED_PARAMETER(FileSystem);
    UNREFERENCED_PARAMETER(FileContext);
    UNREFERENCED_PARAMETER(FileName);
    UNREFERENCED_PARAMETER(Flags);
}

static void wim_close_open_ctx(WimFs *W, OpenCtx *o)
{
    OpenCtx **pp;

    if (!W || !o)
        return;

    wimfs_trace_fmt("Close", L"dir=%d path=[%.200ls] data=%p ref_in=%ld",
        o->is_directory, o->dir_path, (void *)o->data, o->ref);

    EnterCriticalSection(&W->open_node_cs);
    if (o->ref > 0)
        o->ref--;
    if (o->ref > 0) {
        LeaveCriticalSection(&W->open_node_cs);
        return;
    }
    for (pp = &W->open_node_head; *pp; pp = &(*pp)->node_next) {
        if (*pp == o) {
            *pp = o->node_next;
            break;
        }
    }
    LeaveCriticalSection(&W->open_node_cs);

    if (o->read_cs_inited) {
        DeleteCriticalSection(&o->read_cs);
        o->read_cs_inited = 0;
    }
    if (o->data != INVALID_HANDLE_VALUE && o->data != NULL) {
        CloseHandle(o->data);
        o->data = INVALID_HANDLE_VALUE;
    }
    if (o->temp_dir[0]) {
        RemoveDirectoryW(o->temp_dir);
        o->temp_dir[0] = 0;
    }
    free(o);
}

static VOID Close(FSP_FILE_SYSTEM *FileSystem, PVOID FileContext)
{
    WimFs *W = FileSystem ? FileSystem->UserContext : 0;
    wim_close_open_ctx(W, (OpenCtx *)FileContext);
}

static int ensure_file_extracted(WimFs *W, OpenCtx *o)
{
    wchar_t dir[MAX_PATH];
    wchar_t leaf[512];
    wchar_t fullpath[MAX_PATH];
    const wchar_t *paths[1];
    int err;
    DWORD gle;

    if (!o->ent || o->ent->is_directory)
        return -1;

    EnterCriticalSection(&W->extract_cs);
    if (o->data != INVALID_HANDLE_VALUE && o->data != NULL) {
        LeaveCriticalSection(&W->extract_cs);
        return 0;
    }

    if (W->scratch_root[0]) {
        if (!GetFullPathNameW(W->scratch_root, MAX_PATH, dir, NULL) || !dir[0]) {
            LeaveCriticalSection(&W->extract_cs);
            return -1;
        }
        strip_trailing_backslash(dir);
    } else if (!GetTempPathW(MAX_PATH, dir)) {
        LeaveCriticalSection(&W->extract_cs);
        return -1;
    }

    if (!GetTempFileNameW(dir, L"wfs", 0, o->temp_dir)) {
        LeaveCriticalSection(&W->extract_cs);
        return -1;
    }
    DeleteFileW(o->temp_dir);
    if (!CreateDirectoryW(o->temp_dir, 0)) {
        o->temp_dir[0] = 0;
        LeaveCriticalSection(&W->extract_cs);
        return -1;
    }

    paths[0] = o->ent->path;
    err = wimlib_extract_paths(W->wim, W->image, o->temp_dir, paths, 1,
        WIMLIB_EXTRACT_FLAG_NO_PRESERVE_DIR_STRUCTURE | WIMLIB_EXTRACT_FLAG_NO_ACLS);

    if (err) {
        wimfs_trace_fmt("extract", L"wimlib_extract_paths failed err=%d path=[%.200ls]", err, o->ent->path);
        RemoveDirectoryW(o->temp_dir);
        o->temp_dir[0] = 0;
        LeaveCriticalSection(&W->extract_cs);
        return err;
    }

    wcscpy_s(leaf, 512, leaf_name(o->ent->path));
    swprintf_s(fullpath, MAX_PATH, L"%s\\%s", o->temp_dir, leaf);

    o->data = CreateFileW(fullpath, GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        0, OPEN_EXISTING,
        FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, 0);
    if (o->data == INVALID_HANDLE_VALUE) {
        gle = GetLastError();
        wimfs_trace_fmt("extract", L"CreateFile failed gle=%lu full=[%.260ls]", (unsigned long)gle, fullpath);
        DeleteFileW(fullpath);
        RemoveDirectoryW(o->temp_dir);
        o->temp_dir[0] = 0;
        LeaveCriticalSection(&W->extract_cs);
        UNREFERENCED_PARAMETER(gle);
        return -1;
    }
    wimfs_trace_fmt("extract", L"ok handle=%p leaf=[%.200ls]", (void *)o->data, leaf);
    LeaveCriticalSection(&W->extract_cs);
    return 0;
}

static NTSTATUS wim_read_file_common(WimFs *W, OpenCtx *o, PVOID Buffer, UINT64 Offset, ULONG Length,
    PULONG PBytesTransferred)
{
    LARGE_INTEGER li;
    DWORD rd;

    UNREFERENCED_PARAMETER(W);

    if (!o || o->is_directory)
        return STATUS_INVALID_DEVICE_REQUEST;

    wimfs_trace_fmt("Read", L"off=%llu len=%lu ctx=%p", (unsigned long long)Offset, (unsigned long)Length, (void *)o);

    if (ensure_file_extracted(W, o))
        return STATUS_IO_DEVICE_ERROR;

    if (o->read_cs_inited)
        EnterCriticalSection(&o->read_cs);
    li.QuadPart = (LONGLONG)Offset;
    if (!SetFilePointerEx(o->data, li, 0, FILE_BEGIN)) {
        if (o->read_cs_inited)
            LeaveCriticalSection(&o->read_cs);
        return wim_ntstatus_from_win32(GetLastError());
    }

    if (!ReadFile(o->data, Buffer, Length, &rd, 0)) {
        if (o->read_cs_inited)
            LeaveCriticalSection(&o->read_cs);
        return wim_ntstatus_from_win32(GetLastError());
    }

    if (o->read_cs_inited)
        LeaveCriticalSection(&o->read_cs);

    *PBytesTransferred = rd;
    return STATUS_SUCCESS;
}

static NTSTATUS Read(FSP_FILE_SYSTEM *FileSystem,
    PVOID FileContext, PVOID Buffer, UINT64 Offset, ULONG Length,
    PULONG PBytesTransferred)
{
    WimFs *W = FileSystem->UserContext;
    OpenCtx *o = FileContext;
    return wim_read_file_common(W, o, Buffer, Offset, Length, PBytesTransferred);
}

static NTSTATUS Write_(FSP_FILE_SYSTEM *FileSystem,
    PVOID FileContext, PVOID Buffer, UINT64 Offset, ULONG Length,
    BOOLEAN WriteToEndOfFile, BOOLEAN ConstrainedIo,
    PULONG PBytesTransferred, FSP_FSCTL_FILE_INFO *FileInfo)
{
    UNREFERENCED_PARAMETER(FileSystem);
    UNREFERENCED_PARAMETER(FileContext);
    UNREFERENCED_PARAMETER(Buffer);
    UNREFERENCED_PARAMETER(Offset);
    UNREFERENCED_PARAMETER(Length);
    UNREFERENCED_PARAMETER(WriteToEndOfFile);
    UNREFERENCED_PARAMETER(ConstrainedIo);
    UNREFERENCED_PARAMETER(PBytesTransferred);
    UNREFERENCED_PARAMETER(FileInfo);
    return STATUS_ACCESS_DENIED;
}

static NTSTATUS Flush_(FSP_FILE_SYSTEM *FileSystem,
    PVOID FileContext, FSP_FSCTL_FILE_INFO *FileInfo)
{
    UNREFERENCED_PARAMETER(FileSystem);
    UNREFERENCED_PARAMETER(FileContext);
    UNREFERENCED_PARAMETER(FileInfo);
    return STATUS_SUCCESS;
}

static NTSTATUS GetFileInfo(FSP_FILE_SYSTEM *FileSystem,
    PVOID FileContext, FSP_FSCTL_FILE_INFO *FileInfo)
{
    WimFs *W = FileSystem->UserContext;
    OpenCtx *o = FileContext;

    if (!o)
        return STATUS_INVALID_PARAMETER;
    wimfs_trace_fmt("GetFileInfo", L"dir=%d path=[%.200ls] ent=%p",
        o->is_directory, o->dir_path, (void *)o->ent);
    if (o->is_directory && o->dir_path[0] == L'\\' && o->dir_path[1] == 0) {
        memset(FileInfo, 0, sizeof *FileInfo);
        FileInfo->FileAttributes = FILE_ATTRIBUTE_DIRECTORY;
        return STATUS_SUCCESS;
    }
    if (o->ent)
        fill_file_info(o->ent, FileInfo);
    else
        return STATUS_INVALID_PARAMETER;

    UNREFERENCED_PARAMETER(W);
    return STATUS_SUCCESS;
}

static NTSTATUS SetBasicInfo_(FSP_FILE_SYSTEM *FileSystem,
    PVOID FileContext, UINT32 FileAttributes,
    UINT64 CreationTime, UINT64 LastAccessTime, UINT64 LastWriteTime, UINT64 ChangeTime,
    FSP_FSCTL_FILE_INFO *FileInfo)
{
    UNREFERENCED_PARAMETER(FileSystem);
    UNREFERENCED_PARAMETER(FileContext);
    UNREFERENCED_PARAMETER(FileAttributes);
    UNREFERENCED_PARAMETER(CreationTime);
    UNREFERENCED_PARAMETER(LastAccessTime);
    UNREFERENCED_PARAMETER(LastWriteTime);
    UNREFERENCED_PARAMETER(ChangeTime);
    UNREFERENCED_PARAMETER(FileInfo);
    return STATUS_ACCESS_DENIED;
}

static NTSTATUS SetFileSize_(FSP_FILE_SYSTEM *FileSystem,
    PVOID FileContext, UINT64 NewSize, BOOLEAN SetAllocationSize,
    FSP_FSCTL_FILE_INFO *FileInfo)
{
    UNREFERENCED_PARAMETER(FileSystem);
    UNREFERENCED_PARAMETER(FileContext);
    UNREFERENCED_PARAMETER(NewSize);
    UNREFERENCED_PARAMETER(SetAllocationSize);
    UNREFERENCED_PARAMETER(FileInfo);
    return STATUS_ACCESS_DENIED;
}

static NTSTATUS CanDelete_(FSP_FILE_SYSTEM *FileSystem,
    PVOID FileContext, PWSTR FileName)
{
    UNREFERENCED_PARAMETER(FileSystem);
    UNREFERENCED_PARAMETER(FileContext);
    UNREFERENCED_PARAMETER(FileName);
    return STATUS_ACCESS_DENIED;
}

static NTSTATUS Rename_(FSP_FILE_SYSTEM *FileSystem,
    PVOID FileContext, PWSTR FileName, PWSTR NewFileName, BOOLEAN ReplaceIfExists)
{
    UNREFERENCED_PARAMETER(FileSystem);
    UNREFERENCED_PARAMETER(FileContext);
    UNREFERENCED_PARAMETER(FileName);
    UNREFERENCED_PARAMETER(NewFileName);
    UNREFERENCED_PARAMETER(ReplaceIfExists);
    return STATUS_ACCESS_DENIED;
}

static NTSTATUS GetSecurity(FSP_FILE_SYSTEM *FileSystem,
    PVOID FileContext, PSECURITY_DESCRIPTOR SecurityDescriptor, SIZE_T *PSecurityDescriptorSize)
{
    WimFs *W = FileSystem->UserContext;
    OpenCtx *o = FileContext;
    const BYTE *sd;
    SIZE_T sdlen;

    if (!o || !PSecurityDescriptorSize)
        return STATUS_INVALID_PARAMETER;

    wimfs_trace_fmt("GetSecurity", L"ctx=%p dir=%d path=[%.200ls]", (void *)o, o->is_directory, o->dir_path);

    if (o->is_directory && o->dir_path[0] == L'\\' && o->dir_path[1] == 0) {
        sd = W->default_sd;
        sdlen = W->default_sd_len;
    } else if (o->ent && o->ent->sd && o->ent->sd_len &&
        IsValidSecurityDescriptor((PSECURITY_DESCRIPTOR)o->ent->sd)) {
        sd = o->ent->sd;
        sdlen = o->ent->sd_len;
    } else {
        sd = W->default_sd;
        sdlen = W->default_sd_len;
    }

    if (sdlen > *PSecurityDescriptorSize) {
        *PSecurityDescriptorSize = sdlen;
        return STATUS_BUFFER_OVERFLOW;
    }
    *PSecurityDescriptorSize = sdlen;
    if (SecurityDescriptor && sdlen)
        memcpy(SecurityDescriptor, sd, sdlen);
    return STATUS_SUCCESS;
}

static NTSTATUS SetSecurity_(FSP_FILE_SYSTEM *FileSystem,
    PVOID FileContext, SECURITY_INFORMATION SecurityInformation,
    PSECURITY_DESCRIPTOR ModificationDescriptor)
{
    UNREFERENCED_PARAMETER(FileSystem);
    UNREFERENCED_PARAMETER(FileContext);
    UNREFERENCED_PARAMETER(SecurityInformation);
    UNREFERENCED_PARAMETER(ModificationDescriptor);
    return STATUS_ACCESS_DENIED;
}

typedef struct {
    const wchar_t **names;
    size_t n, cap;
} NameList;

static int grow_names(NameList *L)
{
    if (L->n < L->cap)
        return 0;
    {
        size_t ncap = L->cap ? L->cap * 2 : 64;
        const wchar_t **nn = realloc(L->names, ncap * sizeof(wchar_t *));
        if (!nn)
            return -1;
        L->names = nn;
        L->cap = ncap;
    }
    return 0;
}

static int __cdecl qsort_name_cmp(void *ctx, const void *a, const void *b)
{
    const wchar_t *const *pa = a;
    const wchar_t *const *pb = b;
    UNREFERENCED_PARAMETER(ctx);
    return _wcsicmp(*pa, *pb);
}

static int get_parent_dir_path(const wchar_t *dir_path, wchar_t *parent, size_t parent_cch)
{
    wchar_t buf[WIM_PATH_MAX];
    wchar_t *slash;

    if (!dir_path || !parent || parent_cch == 0)
        return -1;
    if (dir_path[0] == L'\\' && dir_path[1] == 0)
        return -1;
    wcscpy_s(buf, WIM_PATH_MAX, dir_path);
    strip_trailing_backslash(buf);
    slash = wcsrchr(buf, L'\\');
    if (!slash)
        return -1;
    if (slash == buf) {
        parent[0] = L'\\';
        parent[1] = 0;
        return 0;
    }
    *slash = 0;
    wcscpy_s(parent, parent_cch, buf);
    return 0;
}

static BOOLEAN emit_dir_info_wim(PVOID Buffer, ULONG Length, PULONG PTransferred,
    const wchar_t *name, const WimDentry *d)
{
    FSP_FSCTL_DIR_INFO *di;
    size_t nb = wcslen(name) * sizeof(WCHAR);
    UINT32 rec = (UINT32)sizeof(FSP_FSCTL_DIR_INFO) + (UINT32)nb;
    UINT8 *raw;
    BOOLEAN ok;

    if (rec > 0xFFFF)
        return TRUE;
    raw = (UINT8 *)malloc(rec);
    if (!raw)
        return TRUE;
    di = (FSP_FSCTL_DIR_INFO *)raw;
    memset(di, 0, rec);
    memset(di->Padding, 0, sizeof di->Padding);
    di->Size = (UINT16)rec;
    if (d)
        fill_file_info(d, &di->FileInfo);
    else {
        memset(&di->FileInfo, 0, sizeof di->FileInfo);
        di->FileInfo.FileAttributes = FILE_ATTRIBUTE_DIRECTORY;
    }
    memcpy(di->FileNameBuf, name, nb);
    ok = FspFileSystemAddDirInfo(di, Buffer, Length, PTransferred);
    free(raw);
    return ok;
}

static NTSTATUS ReadDirectory(FSP_FILE_SYSTEM *FileSystem,
    PVOID FileContext, PWSTR Pattern, PWSTR Marker,
    PVOID Buffer, ULONG Length, PULONG PBytesTransferred)
{
    WimFs *W = FileSystem->UserContext;
    OpenCtx *o = FileContext;
    NameList list;
    size_t i;
    int cmp;
    PWSTR child_marker = Marker;
    int not_volume_root;

    UNREFERENCED_PARAMETER(Pattern);

    memset(&list, 0, sizeof list);
    *PBytesTransferred = 0;

    if (!o || !o->is_directory)
        return STATUS_INVALID_PARAMETER;

    wimfs_trace_fmt("ReadDirectory", L"path=[%.200ls] marker=%ls len=%lu",
        o->dir_path, Marker ? Marker : L"(null)", (unsigned long)Length);

    not_volume_root = !(o->dir_path[0] == L'\\' && o->dir_path[1] == 0);

    if (not_volume_root) {
        const WimDentry *here = lookup_path(W, o->dir_path);

        if (!Marker) {
            if (!emit_dir_info_wim(Buffer, Length, PBytesTransferred, L".", here))
                return STATUS_SUCCESS;
        }
        if (!Marker || (Marker[0] == L'.' && Marker[1] == 0)) {
            wchar_t parent[WIM_PATH_MAX];
            const WimDentry *parent_ent = 0;

            if (get_parent_dir_path(o->dir_path, parent, WIM_PATH_MAX) == 0)
                parent_ent = lookup_path(W, parent);
            if (!emit_dir_info_wim(Buffer, Length, PBytesTransferred, L"..", parent_ent))
                return STATUS_SUCCESS;
            child_marker = 0;
        } else if (Marker[0] == L'.' && Marker[1] == L'.' && Marker[2] == 0) {
            /* Same as memfs after "..": continue children without "<= .." filtering. */
            child_marker = 0;
        }
    }

    for (i = 0; i < W->n; i++) {
        const WimDentry *d = &W->e[i];
        if (!is_direct_child(o->dir_path, d->path))
            continue;
        if (grow_names(&list)) {
            free(list.names);
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        list.names[list.n++] = leaf_name(d->path);
    }

    if (list.n)
        qsort_s(list.names, list.n, sizeof(list.names[0]), qsort_name_cmp, 0);

    for (i = 0; i < list.n; i++) {
        const wchar_t *nm = list.names[i];
        const WimDentry *full = 0;
        wchar_t fullpath[WIM_PATH_MAX];

        if (child_marker) {
            cmp = _wcsicmp(nm, child_marker);
            if (cmp <= 0)
                continue;
        }

        if (o->dir_path[0] == L'\\' && o->dir_path[1] == 0)
            swprintf_s(fullpath, WIM_PATH_MAX, L"\\%s", nm);
        else
            swprintf_s(fullpath, WIM_PATH_MAX, L"%s\\%s", o->dir_path, nm);
        full = lookup_path(W, fullpath);
        if (!full)
            continue;

        if (!emit_dir_info_wim(Buffer, Length, PBytesTransferred, nm, full)) {
            free(list.names);
            return STATUS_SUCCESS;
        }
    }

    FspFileSystemAddDirInfo(0, Buffer, Length, PBytesTransferred);
    wimfs_trace_fmt("ReadDirectory", L"eof child_count=%zu out=%lu", list.n, (unsigned long)*PBytesTransferred);
    free(list.names);
    return STATUS_SUCCESS;
}

typedef enum {
    WIM_BACKEND_AUTO = 0,
    WIM_BACKEND_DOKAN,
    WIM_BACKEND_WINFSP
} WimBackend;

static WimBackend wim_read_backend_env(void)
{
    WCHAR b[32];

    if (!GetEnvironmentVariableW(L"MOUNT_BACKEND", b, 32) || !b[0]) {
        if (!GetEnvironmentVariableW(L"WIMFS_BACKEND", b, 32) || !b[0])
            return WIM_BACKEND_AUTO;
    }
    if (!_wcsicmp(b, L"winfsp"))
        return WIM_BACKEND_WINFSP;
    if (!_wcsicmp(b, L"dokan"))
        return WIM_BACKEND_DOKAN;
    return WIM_BACKEND_AUTO;
}

#if WIM_TRY_DOKAN
int ensure_mount_point_dokan(PWSTR mountPath);

static int DOKAN_CALLBACK dokan_CreateFile(LPCWSTR FileName, DWORD DesiredAccess, DWORD ShareMode,
    DWORD CreationDisposition, DWORD FlagsAndAttributes, PDOKAN_FILE_INFO Dfi)
{
    WimFs *W = (WimFs *)(ULONG_PTR)Dfi->DokanOptions->GlobalContext;
    wchar_t tmp[WIM_PATH_MAX];
    NTSTATUS st;
    OpenCtx *o;

    UNREFERENCED_PARAMETER(ShareMode);
    UNREFERENCED_PARAMETER(FlagsAndAttributes);

    if (!W || !FileName)
        return -ERROR_INVALID_PARAMETER;

    if (DesiredAccess & (GENERIC_WRITE | DELETE | WRITE_DAC | WRITE_OWNER))
        return -ERROR_ACCESS_DENIED;

    wcscpy_s(tmp, WIM_PATH_MAX, FileName);
    normalize_open_path(tmp);
    st = wim_open_ntpath(W, tmp, &o);
    if (!NT_SUCCESS(st))
        return -(int)RtlNtStatusToDosError(st);

    if (CreationDisposition == CREATE_NEW || CreationDisposition == CREATE_ALWAYS ||
        CreationDisposition == TRUNCATE_EXISTING) {
        wim_close_open_ctx(W, o);
        return -ERROR_ACCESS_DENIED;
    }

    Dfi->IsDirectory = o->is_directory ? 1 : 0;
    Dfi->Context = (ULONG64)(SIZE_T)o;
    if (!o->is_directory && CreationDisposition == OPEN_ALWAYS)
        return ERROR_ALREADY_EXISTS;
    return 0;
}

static int DOKAN_CALLBACK dokan_OpenDirectory(LPCWSTR FileName, PDOKAN_FILE_INFO Dfi)
{
    WimFs *W = (WimFs *)(ULONG_PTR)Dfi->DokanOptions->GlobalContext;
    wchar_t tmp[WIM_PATH_MAX];
    NTSTATUS st;
    OpenCtx *o;

    wcscpy_s(tmp, WIM_PATH_MAX, FileName);
    normalize_open_path(tmp);
    st = wim_open_ntpath(W, tmp, &o);
    if (!NT_SUCCESS(st))
        return -(int)RtlNtStatusToDosError(st);
    if (!o->is_directory) {
        wim_close_open_ctx(W, o);
        return -ERROR_DIRECTORY;
    }
    Dfi->Context = (ULONG64)(SIZE_T)o;
    Dfi->IsDirectory = 1;
    return 0;
}

static int DOKAN_CALLBACK dokan_CreateDirectory(LPCWSTR FileName, PDOKAN_FILE_INFO Dfi)
{
    UNREFERENCED_PARAMETER(FileName);
    UNREFERENCED_PARAMETER(Dfi);
    return -ERROR_ACCESS_DENIED;
}

static int DOKAN_CALLBACK dokan_Cleanup(LPCWSTR FileName, PDOKAN_FILE_INFO Dfi)
{
    UNREFERENCED_PARAMETER(FileName);
    UNREFERENCED_PARAMETER(Dfi);
    return 0;
}

static int DOKAN_CALLBACK dokan_CloseFile(LPCWSTR FileName, PDOKAN_FILE_INFO Dfi)
{
    WimFs *W = (WimFs *)(ULONG_PTR)Dfi->DokanOptions->GlobalContext;
    OpenCtx *o = (OpenCtx *)(SIZE_T)Dfi->Context;

    UNREFERENCED_PARAMETER(FileName);
    if (o && W)
        wim_close_open_ctx(W, o);
    Dfi->Context = 0;
    return 0;
}

static int DOKAN_CALLBACK dokan_ReadFile(LPCWSTR FileName, LPVOID Buffer, DWORD NumberOfBytesToRead,
    LPDWORD NumberOfBytesRead, LONGLONG Offset, PDOKAN_FILE_INFO Dfi)
{
    WimFs *W = (WimFs *)(ULONG_PTR)Dfi->DokanOptions->GlobalContext;
    OpenCtx *o = (OpenCtx *)(SIZE_T)Dfi->Context;
    NTSTATUS st;

    UNREFERENCED_PARAMETER(FileName);
    if (!NumberOfBytesRead)
        return -ERROR_INVALID_PARAMETER;
    *NumberOfBytesRead = 0;
    st = wim_read_file_common(W, o, Buffer, (UINT64)Offset, NumberOfBytesToRead, NumberOfBytesRead);
    if (!NT_SUCCESS(st))
        return -(int)RtlNtStatusToDosError(st);
    return 0;
}

static int DOKAN_CALLBACK dokan_WriteFile(LPCWSTR FileName, LPCVOID Buffer, DWORD NumberOfBytesToWrite,
    LPDWORD NumberOfBytesWritten, LONGLONG Offset, PDOKAN_FILE_INFO Dfi)
{
    UNREFERENCED_PARAMETER(FileName);
    UNREFERENCED_PARAMETER(Buffer);
    UNREFERENCED_PARAMETER(NumberOfBytesToWrite);
    UNREFERENCED_PARAMETER(NumberOfBytesWritten);
    UNREFERENCED_PARAMETER(Offset);
    UNREFERENCED_PARAMETER(Dfi);
    return -ERROR_WRITE_PROTECT;
}

static int DOKAN_CALLBACK dokan_FlushFileBuffers(LPCWSTR FileName, PDOKAN_FILE_INFO Dfi)
{
    UNREFERENCED_PARAMETER(FileName);
    UNREFERENCED_PARAMETER(Dfi);
    return 0;
}

static int DOKAN_CALLBACK dokan_GetFileInformation(LPCWSTR FileName, LPBY_HANDLE_FILE_INFORMATION Buf,
    PDOKAN_FILE_INFO Dfi)
{
    OpenCtx *o = (OpenCtx *)(SIZE_T)Dfi->Context;

    UNREFERENCED_PARAMETER(FileName);
    if (!Buf)
        return -ERROR_INVALID_PARAMETER;
    if (!o)
        return -ERROR_INVALID_PARAMETER;
    if (open_ctx_is_volume_root(o))
        dentry_to_by_handle_information(0, Buf);
    else if (o->ent)
        dentry_to_by_handle_information(o->ent, Buf);
    else
        return -ERROR_INVALID_PARAMETER;
    return 0;
}

static int DOKAN_CALLBACK dokan_FindFiles(LPCWSTR PathName, PFillFindData FillFindData, PDOKAN_FILE_INFO Dfi)
{
    WimFs *W = (WimFs *)(ULONG_PTR)Dfi->DokanOptions->GlobalContext;
    OpenCtx *o = (OpenCtx *)(SIZE_T)Dfi->Context;
    NameList list;
    size_t i;
    int not_volume_root;
    WIN32_FIND_DATAW fd;

    UNREFERENCED_PARAMETER(PathName);
    memset(&list, 0, sizeof list);
    if (!W || !o || !o->is_directory || !FillFindData)
        return -ERROR_INVALID_PARAMETER;

    not_volume_root = !(o->dir_path[0] == L'\\' && o->dir_path[1] == 0);

    if (not_volume_root) {
        const WimDentry *here = lookup_path(W, o->dir_path);

        memset(&fd, 0, sizeof fd);
        dentry_to_find_dataw(here, &fd);
        fd.cFileName[0] = L'.';
        fd.cFileName[1] = 0;
        if (FillFindData(&fd, Dfi))
            goto out;

        memset(&fd, 0, sizeof fd);
        {
            wchar_t parent[WIM_PATH_MAX];
            const WimDentry *parent_ent = 0;

            if (get_parent_dir_path(o->dir_path, parent, WIM_PATH_MAX) == 0)
                parent_ent = lookup_path(W, parent);
            dentry_to_find_dataw(parent_ent, &fd);
            fd.cFileName[0] = L'.';
            fd.cFileName[1] = L'.';
            fd.cFileName[2] = 0;
        }
        if (FillFindData(&fd, Dfi))
            goto out;
    }

    for (i = 0; i < W->n; i++) {
        const WimDentry *d = &W->e[i];
        if (!is_direct_child(o->dir_path, d->path))
            continue;
        if (grow_names(&list)) {
            free(list.names);
            return -ERROR_NOT_ENOUGH_MEMORY;
        }
        list.names[list.n++] = leaf_name(d->path);
    }

    if (list.n)
        qsort_s(list.names, list.n, sizeof(list.names[0]), qsort_name_cmp, 0);

    for (i = 0; i < list.n; i++) {
        const wchar_t *nm = list.names[i];
        wchar_t fullpath[WIM_PATH_MAX];
        const WimDentry *full;

        if (o->dir_path[0] == L'\\' && o->dir_path[1] == 0)
            swprintf_s(fullpath, WIM_PATH_MAX, L"\\%s", nm);
        else
            swprintf_s(fullpath, WIM_PATH_MAX, L"%s\\%s", o->dir_path, nm);
        full = lookup_path(W, fullpath);
        if (!full)
            continue;
        memset(&fd, 0, sizeof fd);
        dentry_to_find_dataw(full, &fd);
        if (FillFindData(&fd, Dfi))
            break;
    }

out:
    free(list.names);
    return 0;
}

static int DOKAN_CALLBACK dokan_SetFileAttributes(LPCWSTR FileName, DWORD FileAttributes, PDOKAN_FILE_INFO Dfi)
{
    UNREFERENCED_PARAMETER(FileName);
    UNREFERENCED_PARAMETER(FileAttributes);
    UNREFERENCED_PARAMETER(Dfi);
    return -ERROR_ACCESS_DENIED;
}

static int DOKAN_CALLBACK dokan_SetFileTime(LPCWSTR FileName, CONST FILETIME *CreationTime,
    CONST FILETIME *LastAccessTime, CONST FILETIME *LastWriteTime, PDOKAN_FILE_INFO Dfi)
{
    UNREFERENCED_PARAMETER(FileName);
    UNREFERENCED_PARAMETER(CreationTime);
    UNREFERENCED_PARAMETER(LastAccessTime);
    UNREFERENCED_PARAMETER(LastWriteTime);
    UNREFERENCED_PARAMETER(Dfi);
    return -ERROR_ACCESS_DENIED;
}

static int DOKAN_CALLBACK dokan_DeleteFile(LPCWSTR FileName, PDOKAN_FILE_INFO Dfi)
{
    UNREFERENCED_PARAMETER(FileName);
    UNREFERENCED_PARAMETER(Dfi);
    return -ERROR_ACCESS_DENIED;
}

static int DOKAN_CALLBACK dokan_DeleteDirectory(LPCWSTR FileName, PDOKAN_FILE_INFO Dfi)
{
    UNREFERENCED_PARAMETER(FileName);
    UNREFERENCED_PARAMETER(Dfi);
    return -ERROR_ACCESS_DENIED;
}

static int DOKAN_CALLBACK dokan_MoveFile(LPCWSTR Existing, LPCWSTR NewName, BOOL Replace, PDOKAN_FILE_INFO Dfi)
{
    UNREFERENCED_PARAMETER(Existing);
    UNREFERENCED_PARAMETER(NewName);
    UNREFERENCED_PARAMETER(Replace);
    UNREFERENCED_PARAMETER(Dfi);
    return -ERROR_ACCESS_DENIED;
}

static int DOKAN_CALLBACK dokan_SetEndOfFile(LPCWSTR FileName, LONGLONG Length, PDOKAN_FILE_INFO Dfi)
{
    UNREFERENCED_PARAMETER(FileName);
    UNREFERENCED_PARAMETER(Length);
    UNREFERENCED_PARAMETER(Dfi);
    return -ERROR_ACCESS_DENIED;
}

static int DOKAN_CALLBACK dokan_SetAllocationSize(LPCWSTR FileName, LONGLONG Length, PDOKAN_FILE_INFO Dfi)
{
    UNREFERENCED_PARAMETER(FileName);
    UNREFERENCED_PARAMETER(Length);
    UNREFERENCED_PARAMETER(Dfi);
    return -ERROR_ACCESS_DENIED;
}

static int DOKAN_CALLBACK dokan_LockFile(LPCWSTR FileName, LONGLONG ByteOffset, LONGLONG Length, PDOKAN_FILE_INFO Dfi)
{
    UNREFERENCED_PARAMETER(FileName);
    UNREFERENCED_PARAMETER(ByteOffset);
    UNREFERENCED_PARAMETER(Length);
    UNREFERENCED_PARAMETER(Dfi);
    return 0;
}

static int DOKAN_CALLBACK dokan_UnlockFile(LPCWSTR FileName, LONGLONG ByteOffset, LONGLONG Length, PDOKAN_FILE_INFO Dfi)
{
    UNREFERENCED_PARAMETER(FileName);
    UNREFERENCED_PARAMETER(ByteOffset);
    UNREFERENCED_PARAMETER(Length);
    UNREFERENCED_PARAMETER(Dfi);
    return 0;
}

static int DOKAN_CALLBACK dokan_GetDiskFreeSpace(PULONGLONG FreeBytesAvailable, PULONGLONG TotalNumberOfBytes,
    PULONGLONG TotalNumberOfFreeBytes, PDOKAN_FILE_INFO Dfi)
{
    UNREFERENCED_PARAMETER(Dfi);
    if (FreeBytesAvailable)
        *FreeBytesAvailable = 1ULL << 30;
    if (TotalNumberOfBytes)
        *TotalNumberOfBytes = 1ULL << 31;
    if (TotalNumberOfFreeBytes)
        *TotalNumberOfFreeBytes = 0;
    return 0;
}

static int DOKAN_CALLBACK dokan_GetVolumeInformation(LPWSTR VolumeNameBuffer, DWORD VolumeNameSize,
    LPDWORD VolumeSerialNumber, LPDWORD MaximumComponentLength, LPDWORD FileSystemFlags,
    LPWSTR FileSystemNameBuffer, DWORD FileSystemNameSize, PDOKAN_FILE_INFO Dfi)
{
    WimFs *W = (WimFs *)(ULONG_PTR)Dfi->DokanOptions->GlobalContext;

    if (VolumeSerialNumber)
        *VolumeSerialNumber = 0x19831120;
    if (MaximumComponentLength)
        *MaximumComponentLength = 255;
    if (FileSystemFlags)
        *FileSystemFlags =
            FILE_CASE_PRESERVED_NAMES | FILE_UNICODE_ON_DISK | FILE_READ_ONLY_VOLUME;
    if (VolumeNameBuffer && VolumeNameSize && W)
        wcsncpy_s(VolumeNameBuffer, VolumeNameSize, W->volume_label, _TRUNCATE);
    if (FileSystemNameBuffer && FileSystemNameSize)
        wcscpy_s(FileSystemNameBuffer, FileSystemNameSize, L"NTFS");
    return 0;
}

static int DOKAN_CALLBACK dokan_Unmount(PDOKAN_FILE_INFO Dfi)
{
    UNREFERENCED_PARAMETER(Dfi);
    return 0;
}

static int DOKAN_CALLBACK dokan_GetFileSecurity(LPCWSTR FileName, PSECURITY_INFORMATION SecInfo,
    PSECURITY_DESCRIPTOR SecurityDescriptor, ULONG Length, PULONG LengthNeeded, PDOKAN_FILE_INFO Dfi)
{
    WimFs *W = (WimFs *)(ULONG_PTR)Dfi->DokanOptions->GlobalContext;
    wchar_t tmp[WIM_PATH_MAX];
    const WimDentry *d;
    const BYTE *sd;
    SIZE_T sdlen;

    UNREFERENCED_PARAMETER(SecInfo);
    wcscpy_s(tmp, WIM_PATH_MAX, FileName);
    normalize_open_path(tmp);
    if (tmp[0] == L'\\' && tmp[1] == 0) {
        d = 0;
        sd = W->default_sd;
        sdlen = W->default_sd_len;
    } else {
        d = lookup_path(W, tmp);
        if (!d)
            return -ERROR_FILE_NOT_FOUND;
        if (d->sd && d->sd_len && IsValidSecurityDescriptor((PSECURITY_DESCRIPTOR)d->sd)) {
            sd = d->sd;
            sdlen = d->sd_len;
        } else {
            sd = W->default_sd;
            sdlen = W->default_sd_len;
        }
    }
    if (!LengthNeeded)
        return 0;
    if (sdlen > Length) {
        *LengthNeeded = (ULONG)sdlen;
        return -ERROR_INSUFFICIENT_BUFFER;
    }
    *LengthNeeded = (ULONG)sdlen;
    if (SecurityDescriptor && sdlen)
        memcpy(SecurityDescriptor, sd, sdlen);
    return 0;
}

static int DOKAN_CALLBACK dokan_SetFileSecurity(LPCWSTR FileName, PSECURITY_INFORMATION SecInfo,
    PSECURITY_DESCRIPTOR SecurityDescriptor, ULONG Length, PDOKAN_FILE_INFO Dfi)
{
    UNREFERENCED_PARAMETER(FileName);
    UNREFERENCED_PARAMETER(SecInfo);
    UNREFERENCED_PARAMETER(SecurityDescriptor);
    UNREFERENCED_PARAMETER(Length);
    UNREFERENCED_PARAMETER(Dfi);
    return -ERROR_ACCESS_DENIED;
}

static int mount_path_normalize(PWSTR mountPath, wchar_t *norm, size_t normCch);

static int try_mount_dokan(WimFs *W, PWSTR mountPath, WimBackend be)
{
    HMODULE h;
    DOKAN_OPTIONS opt;
    DOKAN_OPERATIONS ops;
    int r;

    if (be == WIM_BACKEND_WINFSP)
        return 0;

    h = LoadLibraryW(L"dokan.dll");
    if (!h) {
        if (be == WIM_BACKEND_DOKAN) {
            fwprintf(stderr, L"dokan.dll not found (MOUNT_BACKEND=dokan or legacy WIMFS_BACKEND=dokan).\n");
            return -1;
        }
        return 0;
    }

    g_pfn_DokanMain = (PFN_DokanMain)GetProcAddress(h, "DokanMain");
#if defined(_M_IX86)
    if (!g_pfn_DokanMain)
        g_pfn_DokanMain = (PFN_DokanMain)GetProcAddress(h, "_DokanMain@8");
#endif
    g_DokanRemoveMountPoint = (PFN_DokanRemoveMountPoint)GetProcAddress(h, "DokanRemoveMountPoint");
#if defined(_M_IX86)
    if (!g_DokanRemoveMountPoint)
        g_DokanRemoveMountPoint =
            (PFN_DokanRemoveMountPoint)GetProcAddress(h, "_DokanRemoveMountPoint@4");
#endif

    if (!g_pfn_DokanMain || !g_DokanRemoveMountPoint) {
        fwprintf(stderr, L"dokan.dll missing DokanMain/DokanRemoveMountPoint exports.\n");
        g_pfn_DokanMain = 0;
        g_DokanRemoveMountPoint = 0;
        FreeLibrary(h);
        return be == WIM_BACKEND_DOKAN ? -1 : 0;
    }

    if (!dokan_try_portable_stack_setup()) {
        fwprintf(stderr,
            L"Dokan: registering/starting Dokan or DokanMounter failed. If dokan.sys or mounter.exe\n"
            L"  is next to wim.exe, run as Administrator once (same idea as winfsp-*.sys portable).\n");
        g_pfn_DokanMain = 0;
        g_DokanRemoveMountPoint = 0;
        FreeLibrary(h);
        return be == WIM_BACKEND_DOKAN ? -1 : 0;
    }

    if (!ensure_mount_point_dokan(mountPath)) {
        g_pfn_DokanMain = 0;
        g_DokanRemoveMountPoint = 0;
        FreeLibrary(h);
        return -1;
    }

    memset(&ops, 0, sizeof ops);
    ops.CreateFile = dokan_CreateFile;
    ops.OpenDirectory = dokan_OpenDirectory;
    ops.CreateDirectory = dokan_CreateDirectory;
    ops.Cleanup = dokan_Cleanup;
    ops.CloseFile = dokan_CloseFile;
    ops.ReadFile = dokan_ReadFile;
    ops.WriteFile = dokan_WriteFile;
    ops.FlushFileBuffers = dokan_FlushFileBuffers;
    ops.GetFileInformation = dokan_GetFileInformation;
    ops.FindFiles = dokan_FindFiles;
    ops.SetFileAttributes = dokan_SetFileAttributes;
    ops.SetFileTime = dokan_SetFileTime;
    ops.DeleteFile = dokan_DeleteFile;
    ops.DeleteDirectory = dokan_DeleteDirectory;
    ops.MoveFile = dokan_MoveFile;
    ops.SetEndOfFile = dokan_SetEndOfFile;
    ops.SetAllocationSize = dokan_SetAllocationSize;
    ops.LockFile = dokan_LockFile;
    ops.UnlockFile = dokan_UnlockFile;
    ops.GetDiskFreeSpace = dokan_GetDiskFreeSpace;
    ops.GetVolumeInformation = dokan_GetVolumeInformation;
    ops.Unmount = dokan_Unmount;
    ops.GetFileSecurity = dokan_GetFileSecurity;
    ops.SetFileSecurity = dokan_SetFileSecurity;

    memset(&opt, 0, sizeof opt);
    opt.Version = DOKAN_VERSION;
    opt.ThreadCount = 4;
    opt.Options = 0;
    opt.GlobalContext = (ULONG64)(ULONG_PTR)W;
    opt.MountPoint = mountPath;

    wcscpy_s(g_dokanMountPath, MAX_PATH, mountPath);
    strip_trailing_backslash(g_dokanMountPath);

    g_Wim = W;
    InterlockedExchange(&g_dokanMounted, 1);
    SetConsoleCtrlHandler(wim_console_ctrl, TRUE);

    wprintf(L"Mounted image %d (Dokan) at\n  %ls\nPress Ctrl+C to unmount.\n", W->image, mountPath);
    r = g_pfn_DokanMain(&opt, &ops);

    InterlockedExchange(&g_dokanMounted, 0);
    g_pfn_DokanMain = 0;
    g_DokanRemoveMountPoint = 0;
    g_Wim = 0;
    FreeLibrary(h);

    if (r == DOKAN_SUCCESS) {
        wprintf(L"Dokan unmounted.\n");
        return 1;
    }

    /*
     * Dokan 0.6.x returns DOKAN_DRIVER_INSTALL_ERROR (-3) when CreateFile(L"\\\\.\\Dokan")
     * fails inside DokanMain — i.e. dokan.sys is not loaded / device does not exist.
     * This is not an x64 struct layout bug in the host app; Win7 x64 PE often has no driver.
     */
    if (r == DOKAN_DRIVER_INSTALL_ERROR) {
        HANDLE dev = CreateFileW(L"\\\\.\\Dokan", GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
        if (dev == INVALID_HANDLE_VALUE) {
            fwprintf(stderr,
                L"Dokan kernel device \\\\.\\Dokan not available (Win32 error %lu). "
                L"Install/load dokan.sys (same Dokan build as dokan.dll).\n",
                (unsigned long)GetLastError());
        } else
            CloseHandle(dev);
    }

    fwprintf(stderr, L"DokanMain failed (%d)%ls\n", r,
        (be == WIM_BACKEND_AUTO) ? L"; falling back to WinFsp" : L"");
    if (be == WIM_BACKEND_AUTO) {
        wchar_t normRm[WIM_PATH_MAX];

        if (mount_path_normalize(mountPath, normRm, WIM_PATH_MAX))
            RemoveDirectoryW(normRm);
        else
            RemoveDirectoryW(mountPath);
    }

    return be == WIM_BACKEND_DOKAN ? -1 : 0;
}
#endif /* WIM_TRY_DOKAN */

static FSP_FILE_SYSTEM_INTERFACE g_Interface;

BOOL WINAPI wim_console_ctrl(DWORD dwCtrlType)
{
    UNREFERENCED_PARAMETER(dwCtrlType);
    /*
     * Wake the main thread first. Do not call FspFileSystemStopDispatcher here: it can
     * block on worker threads that may still be inside our callbacks, which deadlocks
     * Ctrl+C (seen with UD WinFsp). Teardown runs on the main thread after WaitForSingleObject.
     */
    if (g_ExitEvent)
        SetEvent(g_ExitEvent);
#if WIM_TRY_DOKAN
    if (g_dokanMounted && g_DokanRemoveMountPoint && g_dokanMountPath[0])
        g_DokanRemoveMountPoint(g_dokanMountPath);
#endif
    return TRUE;
}

static int mount_path_normalize(PWSTR mountPath, wchar_t *norm, size_t normCch)
{
    DWORD n;

    if (!mountPath || !mountPath[0]) {
        fwprintf(stderr, L"Mount path is empty.\n");
        return 0;
    }
    n = GetFullPathNameW(mountPath, (DWORD)normCch, norm, NULL);
    if (n == 0 || n >= normCch) {
        fwprintf(stderr, L"Mount path: GetFullPathNameW failed (%lu).\n", (unsigned long)GetLastError());
        return 0;
    }
    strip_trailing_backslash(norm);
    return 1;
}

static int mount_norm_is_drive(const wchar_t *norm)
{
    size_t len = wcslen(norm);

    return (len == 2 && norm[1] == L':') || (len == 3 && norm[1] == L':' && norm[2] == L'\\');
}

static int verify_mount_point_drive_access(const wchar_t *norm)
{
    wchar_t root[8];
    DWORD att;

    if (wcslen(norm) == 2)
        swprintf_s(root, 8, L"%ls\\", norm);
    else
        wcscpy_s(root, 8, norm);
    att = GetFileAttributesW(root);
    if (att == INVALID_FILE_ATTRIBUTES) {
        fwprintf(stderr, L"Mount path: drive is not accessible: %ls\n", root);
        return 0;
    }
    return 1;
}

/* Parent of a directory mount leaf (e.g. D:\\mount -> D:\\). norm is normalized, non-drive. */
static int mount_parent_from_leaf_norm(const wchar_t *norm, wchar_t *parent, size_t parentCch)
{
    wchar_t *slash;

    if (!norm || !parent || parentCch < 4)
        return 0;
    wcscpy_s(parent, parentCch, norm);
    slash = wcsrchr(parent, L'\\');
    if (!slash || slash == parent)
        return 0;
    *slash = 0;
    if (wcslen(parent) == 2 && parent[1] == L':') {
        parent[2] = L'\\';
        parent[3] = 0;
    }
    return 1;
}

#if WIM_TRY_DOKAN
static int is_directory_empty_for_mount(const wchar_t *dirPath)
{
    wchar_t pat[WIM_PATH_MAX];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    int n;

    n = swprintf_s(pat, WIM_PATH_MAX, L"%ls\\*", dirPath);
    if (n < 0 || n >= (int)WIM_PATH_MAX)
        return 0;
    h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return 1;
    do {
        if (wcscmp(fd.cFileName, L".") && wcscmp(fd.cFileName, L"..")) {
            FindClose(h);
            return 0;
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return 1;
}

/*
 * Dokan directory mounts: parent must exist; wim.exe creates the leaf if missing (must be empty
 * if it already exists). Drive mounts: same accessibility check as WinFsp.
 */
int ensure_mount_point_dokan(PWSTR mountPath)
{
    wchar_t norm[WIM_PATH_MAX];
    wchar_t parent[WIM_PATH_MAX];
    DWORD att;

    if (!mount_path_normalize(mountPath, norm, WIM_PATH_MAX))
        return 0;
    if (mount_norm_is_drive(norm))
        return verify_mount_point_drive_access(norm);

    if (!mount_parent_from_leaf_norm(norm, parent, _countof(parent))) {
        fwprintf(stderr, L"Dokan mount: invalid directory mount path: %ls\n", norm);
        return 0;
    }

    att = GetFileAttributesW(parent);
    if (att == INVALID_FILE_ATTRIBUTES || !(att & FILE_ATTRIBUTE_DIRECTORY)) {
        fwprintf(stderr, L"Dokan mount: parent directory must already exist: %ls\n", parent);
        return 0;
    }

    att = GetFileAttributesW(norm);
    if (att == INVALID_FILE_ATTRIBUTES) {
        if (!CreateDirectoryW(norm, NULL)) {
            fwprintf(stderr, L"Dokan mount: cannot create mount directory \"%ls\" (%lu).\n", norm,
                (unsigned long)GetLastError());
            return 0;
        }
        return 1;
    }
    if (!(att & FILE_ATTRIBUTE_DIRECTORY)) {
        fwprintf(stderr, L"Dokan mount: path exists and is not a directory: %ls\n", norm);
        return 0;
    }
    if (!is_directory_empty_for_mount(norm)) {
        fwprintf(stderr,
            L"Dokan mount: mount directory must be empty (or not exist yet): %ls\n", norm);
        return 0;
    }
    return 1;
}
#endif /* WIM_TRY_DOKAN */

/*
 * WinFsp: caller creates parent chain only; the leaf mount path must not exist (WinFsp creates it).
 * Drive mounts: same drive accessibility check.
 */
int verify_mount_point_winfsp(PWSTR mountPath)
{
    wchar_t norm[WIM_PATH_MAX];
    wchar_t parent[WIM_PATH_MAX];
    DWORD att;

    if (!mount_path_normalize(mountPath, norm, WIM_PATH_MAX))
        return 0;
    if (mount_norm_is_drive(norm))
        return verify_mount_point_drive_access(norm);

    if (!mount_parent_from_leaf_norm(norm, parent, _countof(parent))) {
        fwprintf(stderr, L"WinFsp mount: invalid directory mount path: %ls\n", norm);
        return 0;
    }
    att = GetFileAttributesW(parent);
    if (att == INVALID_FILE_ATTRIBUTES || !(att & FILE_ATTRIBUTE_DIRECTORY)) {
        fwprintf(stderr,
            L"WinFsp mount: parent directory must already exist (create parents only): %ls\n", parent);
        return 0;
    }

    att = GetFileAttributesW(norm);
    if (att != INVALID_FILE_ATTRIBUTES) {
        fwprintf(stderr,
            L"WinFsp mount: final path must not exist yet (WinFsp will create it): %ls\n"
            L"  Create parent folders only, not this leaf; remove the leaf if it is an empty folder.\n",
            norm);
        return 0;
    }
    return 1;
}

static int cmdline_is_unmount_switch(const wchar_t *a)
{
    if (!a)
        return 0;
    return !_wcsicmp(a, L"/unmount") || !_wcsicmp(a, L"-unmount") || !_wcsicmp(a, L"--unmount") ||
        !_wcsicmp(a, L"unmount");
}

/*
 * Unmount a directory mount created by this tool (Dokan or WinFsp path), without knowing the PID
 * of the serving wim.exe. Dokan: DokanRemoveMountPoint (any process). WinFsp: open the mount
 * directory the same way FspMountSet_Directory does (DELETE_ON_CLOSE) and FspMountRemove.
 * Drive-letter mounts: Dokan only here; WinFsp drive removal needs the hosting process or fsptool.
 */
static int unmount_mountpoint(PWSTR mountPath)
{
#if WIM_TRY_DOKAN
    HMODULE hDokan;
    PFN_DokanRemoveMountPoint pfnRm;
#endif
    wchar_t path[WIM_PATH_MAX];
    DWORD n;
    size_t len;

    if (!mountPath || !mountPath[0]) {
        fwprintf(stderr, L"Unmount: empty mount path.\n");
        return 0;
    }
    n = GetFullPathNameW(mountPath, WIM_PATH_MAX, path, NULL);
    if (n == 0 || n >= WIM_PATH_MAX) {
        fwprintf(stderr, L"Unmount: GetFullPathNameW failed for \"%ls\" (%lu).\n", mountPath,
            (unsigned long)GetLastError());
        return 0;
    }
    strip_trailing_backslash(path);
    len = wcslen(path);
    if (len == 2 && path[1] == L':') {
        path[2] = L'\\';
        path[3] = 0;
    }

#if WIM_TRY_DOKAN
    hDokan = LoadLibraryW(L"dokan.dll");
    if (hDokan) {
        pfnRm = (PFN_DokanRemoveMountPoint)GetProcAddress(hDokan, "DokanRemoveMountPoint");
#if defined(_M_IX86)
        if (!pfnRm)
            pfnRm = (PFN_DokanRemoveMountPoint)GetProcAddress(hDokan, "_DokanRemoveMountPoint@4");
#endif
        if (pfnRm && pfnRm(path)) {
            wprintf(L"Unmount (Dokan): %ls\n", path);
            FreeLibrary(hDokan);
            return 1;
        }
        FreeLibrary(hDokan);
    }
#endif

    {
        DWORD att = GetFileAttributesW(path);

        if (att == INVALID_FILE_ATTRIBUTES || !(att & FILE_ATTRIBUTE_DIRECTORY)) {
            fwprintf(stderr,
                L"Unmount: \"%ls\" is not an existing directory (or Dokan unmount already removed it).\n",
                path);
            return 0;
        }
        if (!(att & FILE_ATTRIBUTE_REPARSE_POINT)) {
            fwprintf(stderr,
                L"Unmount: \"%ls\" has no reparse point (not a WinFsp-style directory mount). "
                L"If this was Dokan, ensure dokan.dll is loadable and DokanMounter is running.\n",
                path);
            return 0;
        }
    }

    {
        FSP_MOUNT_DESC desc;
        NTSTATUS st;
        HANDLE mh;

        memset(&desc, 0, sizeof desc);
        desc.MountPoint = path;
        mh = CreateFileW(path, FILE_WRITE_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
            FILE_ATTRIBUTE_DIRECTORY | FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_POSIX_SEMANTICS |
                FILE_FLAG_DELETE_ON_CLOSE,
            NULL);
        if (mh == INVALID_HANDLE_VALUE) {
            fwprintf(stderr,
                L"Unmount (WinFsp): cannot open \"%ls\" with DELETE_ON_CLOSE (error %lu). "
                L"The serving wim.exe may still be running; stop it (Ctrl+C) then retry, "
                L"or use WinFsp fsptool remove.\n",
                path, (unsigned long)GetLastError());
            return 0;
        }
        desc.MountHandle = mh;
        st = FspMountRemove(&desc);
        if (!NT_SUCCESS(st)) {
            fwprintf(stderr, L"Unmount (WinFsp): FspMountRemove failed: 0x%08X\n", (unsigned)st);
            return 0;
        }
        wprintf(L"Unmount (WinFsp): %ls\n", path);
        return 1;
    }
}

static int ensure_scratch_root_dir(PWSTR normPath)
{
    DWORD a = GetFileAttributesW(normPath);

    if (a == INVALID_FILE_ATTRIBUTES) {
        fwprintf(stderr,
            L"TempPath: directory does not exist: %ls\n"
            L"  Create it (and ensure it is writable) before running wim.exe.\n",
            normPath);
        return 0;
    }
    if (!(a & FILE_ATTRIBUTE_DIRECTORY)) {
        fwprintf(stderr, L"TempPath: path exists but is not a directory: %ls\n", normPath);
        return 0;
    }
    return 1;
}

/*
 * Recognize --TempPath=DIR, --TempPath DIR, and / - forms (prefix match is case-insensitive).
 * Updates *pi to the next unprocessed index.
 * Returns 1 if consumed, 0 if argv[*pi] is not this option, -1 on error (prints message).
 */
static int consume_temp_path_option(int *pi, int argc, wchar_t **argv, wchar_t *scratch, size_t scratchCch)
{
    static const wchar_t *const keys[] = { L"--TempPath", L"-TempPath", L"/TempPath" };
    unsigned k;
    int i = *pi;
    wchar_t *a;

    if (i >= argc || !argv[i])
        return 0;
    a = argv[i];
    for (k = 0; k < sizeof keys / sizeof keys[0]; k++) {
        size_t L = wcslen(keys[k]);

        if (_wcsnicmp(a, keys[k], L) != 0)
            continue;
        if (a[L] == L'=') {
            if (!a[L + 1]) {
                fwprintf(stderr, L"%ls: directory path must not be empty.\n", keys[k]);
                return -1;
            }
            if (wcslen(a + L + 1) >= scratchCch) {
                fwprintf(stderr, L"TempPath: path too long.\n");
                return -1;
            }
            wcscpy_s(scratch, scratchCch, a + L + 1);
            strip_trailing_backslash(scratch);
            *pi = i + 1;
            return 1;
        }
        if (a[L] == 0) {
            if (i + 1 >= argc || !argv[i + 1][0]) {
                fwprintf(stderr, L"%ls requires a directory path argument.\n", keys[k]);
                return -1;
            }
            if (wcslen(argv[i + 1]) >= scratchCch) {
                fwprintf(stderr, L"TempPath: path too long.\n");
                return -1;
            }
            wcscpy_s(scratch, scratchCch, argv[i + 1]);
            strip_trailing_backslash(scratch);
            *pi = i + 2;
            return 1;
        }
        break;
    }
    return 0;
}

/*
 * --ud / -ud / /ud: mount fbinst UD area (see ud_mount_main).
 */
static int consume_ud_option(int *pi, int argc, wchar_t **argv, int *udOut)
{
    static const wchar_t *const keys[] = { L"--ud", L"-ud", L"/ud" };
    unsigned k;
    int i = *pi;
    wchar_t *a;

    if (i >= argc || !argv[i])
        return 0;
    a = argv[i];
    for (k = 0; k < sizeof keys / sizeof keys[0]; k++) {
        size_t L = wcslen(keys[k]);

        if (_wcsnicmp(a, keys[k], L) != 0)
            continue;
        if (a[L] != 0)
            return 0;
        *udOut = 1;
        *pi = i + 1;
        return 1;
    }
    return 0;
}

/*
 * Build scratch_root (optional) and positional args (archive, mount, [image]) from argv.
 * Returns 0 on success; -1 on error.
 */
static int parse_cli_filtered(int argc, wchar_t **argv, wchar_t *scratch, size_t scratchCch,
    wchar_t **pos, int maxPos, int *outCount, int *udMode)
{
    int i;

    *outCount = 0;
    *udMode = 0;
    scratch[0] = 0;
    for (i = 1; i < argc;) {
        int c;

        c = consume_ud_option(&i, argc, argv, udMode);
        if (c > 0)
            continue;

        c = consume_temp_path_option(&i, argc, argv, scratch, scratchCch);

        if (c < 0)
            return -1;
        if (c > 0)
            continue;
        if (*outCount >= maxPos) {
            fwprintf(stderr, L"Too many positional arguments.\n");
            return -1;
        }
        pos[(*outCount)++] = argv[i++];
    }
    return 0;
}

int wmain(int argc, wchar_t **argv)
{
    WimFs W;
    NTSTATUS status;
    FSP_FILE_SYSTEM *fs = 0;
    FSP_FSCTL_VOLUME_PARAMS vp;
    int wim_err;
    DWORD att;
    ULONG image = 1;
    WimBackend mount_wb;
    PWSTR wimPath, mountPath;
    PSECURITY_DESCRIPTOR pSD = 0;
    wchar_t scratchOpt[MAX_PATH];
    wchar_t *posArgs[8];
    int nPos;
    wchar_t scratchNorm[MAX_PATH];
    int udMode = 0;

    setlocale(LC_ALL, "");

    if (parse_cli_filtered(argc, argv, scratchOpt, MAX_PATH, posArgs, 8, &nPos, &udMode) < 0)
        return 1;

    if (nPos >= 2 && cmdline_is_unmount_switch(posArgs[0])) {
        if (!unmount_mountpoint(posArgs[1]))
            return 1;
        return 0;
    }

    if (nPos < 2) {
        fwprintf(stderr,
            L"Usage: %s [--TempPath=DIR | --TempPath DIR] <archive.wim|.esd> <mount-directory> [image-index]\n"
            L"       %s --ud <\\\\.\\PhysicalDriveN|image.bin> <mount-directory>\n"
            L"       %s /unmount <mount-directory>   (also -unmount, --unmount, or \"unmount\" as first arg)\n"
            L"  --TempPath: parent directory for wimlib extract scratch (default: %%TEMP%%); must already exist.\n"
            L"  --ud: read-only mount fbinst UD (1.6/1.7 auto); source is physical drive or FBAR .bin; admin may be required.\n"
            L"  Mount path: ensure parent exists; leaf is created for Dokan if missing, or by WinFsp (leaf must not exist before WinFsp).\n"
            L"  MOUNT_BACKEND: auto (default on x86/x64: try Dokan then WinFsp), dokan, or winfsp.\n"
            L"  Legacy: WIMFS_BACKEND is used if MOUNT_BACKEND is unset. ARM64: WinFsp only.\n"
            L"  Diagnostics: set WIMFS_LOG=1 or add --debug to the command line; see %%TEMP%%\\wimfs_trace.log\n",
            argv[0], argv[0], argv[0]);
        return 1;
    }

    if (udMode) {
        if (nPos != 2) {
            fwprintf(stderr, L"--ud requires exactly two arguments: <source> <mount-directory>\n");
            return 1;
        }
        mount_wb = wim_read_backend_env();
        if (!ud_mount_main(posArgs[0], posArgs[1], (int)mount_wb))
            return 1;
        return 0;
    }

    if (scratchOpt[0]) {
        if (!GetFullPathNameW(scratchOpt, MAX_PATH, scratchNorm, NULL) || !scratchNorm[0]) {
            fwprintf(stderr, L"TempPath: GetFullPathNameW failed (%lu).\n", (unsigned long)GetLastError());
            return 1;
        }
        strip_trailing_backslash(scratchNorm);
        if (!ensure_scratch_root_dir(scratchNorm)) {
            fwprintf(stderr, L"TempPath: cannot use directory: %ls\n", scratchNorm);
            return 1;
        }
    }

    wimPath = posArgs[0];
    mountPath = posArgs[1];
    if (nPos >= 3)
        image = (ULONG)wcstoul(posArgs[2], 0, 10);

    mount_wb = wim_read_backend_env();

    att = GetFileAttributesW(wimPath);
    if (att == INVALID_FILE_ATTRIBUTES) {
        fwprintf(stderr, L"Cannot open archive: %s\n", wimPath);
        return 1;
    }

    memset(&W, 0, sizeof W);
    if (scratchOpt[0])
        wcscpy_s(W.scratch_root, MAX_PATH, scratchNorm);
    InitializeCriticalSection(&W.extract_cs);
    InitializeCriticalSection(&W.open_node_cs);

    wim_err = wimlib_global_init(0);
    if (wim_err) {
        fwprintf(stderr, L"wimlib_global_init: %s\n", wimlib_get_error_string(wim_err));
        DeleteCriticalSection(&W.open_node_cs);
        DeleteCriticalSection(&W.extract_cs);
        return 1;
    }

    wim_err = wimlib_open_wim(wimPath, WIMLIB_OPEN_FLAG_CHECK_INTEGRITY, &W.wim);
    if (wim_err) {
        fwprintf(stderr, L"wimlib_open_wim: %s\n", wimlib_get_error_string(wim_err));
        wimlib_global_cleanup();
        DeleteCriticalSection(&W.open_node_cs);
        DeleteCriticalSection(&W.extract_cs);
        return 1;
    }

    {
        struct wimlib_wim_info wi0;

        memset(&wi0, 0, sizeof wi0);
        wimlib_get_wim_info(W.wim, &wi0);
        if (image < 1 || image > wi0.image_count) {
            fwprintf(stderr, L"Invalid image index %u (file has %u images).\n",
                image, wi0.image_count);
            wimlib_free(W.wim);
            wimlib_global_cleanup();
            DeleteCriticalSection(&W.open_node_cs);
            DeleteCriticalSection(&W.extract_cs);
            return 1;
        }
    }
    W.image = (int)image;

    wim_err = build_index(&W);
    if (wim_err) {
        fwprintf(stderr, L"Failed to index WIM image: %s\n", wimlib_get_error_string(wim_err));
        wimlib_free(W.wim);
        wimlib_global_cleanup();
        DeleteCriticalSection(&W.open_node_cs);
        DeleteCriticalSection(&W.extract_cs);
        return 1;
    }

    {
        const wchar_t *nm = wimlib_get_image_name(W.wim, W.image);
        if (nm && nm[0])
            wcscpy_s(W.volume_label, 32, nm);
        else
            wcscpy_s(W.volume_label, 32, L"WIM");
    }

    /*
     * Self-relative SD used when the WIM has no stored descriptor. Must include
     * owner and group (O: G:) — a DACL-only D:P(...) string triggers
     * "invalid security descriptor" when traversing the mount (e.g. cmd cd).
     * Same default as WinFsp memfs RootSddl.
     */
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            L"O:BAG:BAD:P(A;;FA;;;SY)(A;;FA;;;BA)(A;;FA;;;WD)",
            SDDL_REVISION_1, &pSD, 0)) {
        fwprintf(stderr, L"ConvertStringSecurityDescriptorToSecurityDescriptorW failed: %u\n",
            (unsigned)GetLastError());
        goto fail_index;
    }
    W.default_sd_len = GetSecurityDescriptorLength(pSD);
    if (W.default_sd_len > sizeof W.default_sd) {
        fwprintf(stderr, L"Default security descriptor too large.\n");
        LocalFree(pSD);
        goto fail_index;
    }
    memcpy(W.default_sd, pSD, W.default_sd_len);
    LocalFree(pSD);
    pSD = 0;

    if (wimfs_trace_wanted(argc, argv)) {
        wimfs_trace_open();
        if (g_wimfs_trace)
            SetUnhandledExceptionFilter(wimfs_unhandled_filter);
    }
    wimfs_trace_fmt("main", L"ready wim=[%.200ls] mount=[%.200ls] image=%lu entries=%zu",
        wimPath, mountPath, (unsigned long)image, W.n);

#if WIM_TRY_DOKAN
    if (mount_wb != WIM_BACKEND_WINFSP) {
        int dr = try_mount_dokan(&W, mountPath, mount_wb);

        if (dr == 1)
            goto cleanup_and_exit_ok;
        if (dr < 0)
            goto fail_index;
    }
#endif

#if WIM_TRY_DOKAN
    if (mount_wb != WIM_BACKEND_DOKAN) {
        if (!verify_mount_point_winfsp(mountPath))
            goto fail_index;
    }
#else
    if (!verify_mount_point_winfsp(mountPath))
        goto fail_index;
#endif

    if (!winfsp_try_portable_driver_setup())
        goto fail_index;

    start_winfsp_service();

    if (!preflight_mount(mountPath))
        goto fail_index;

    memset(&g_Interface, 0, sizeof g_Interface);
    g_Interface.GetVolumeInfo = GetVolumeInfo;
    g_Interface.SetVolumeLabel = SetVolumeLabel_;
    g_Interface.GetSecurityByName = GetSecurityByName;
    g_Interface.Create = Create_;
    g_Interface.Open = Open;
    g_Interface.Overwrite = Overwrite_;
    g_Interface.Cleanup = Cleanup;
    g_Interface.Close = Close;
    g_Interface.Read = Read;
    g_Interface.Write = Write_;
    g_Interface.Flush = Flush_;
    g_Interface.GetFileInfo = GetFileInfo;
    g_Interface.SetBasicInfo = SetBasicInfo_;
    g_Interface.SetFileSize = SetFileSize_;
    g_Interface.CanDelete = CanDelete_;
    g_Interface.Rename = Rename_;
    g_Interface.GetSecurity = GetSecurity;
    g_Interface.SetSecurity = SetSecurity_;
    g_Interface.ReadDirectory = ReadDirectory;

    memset(&vp, 0, sizeof vp);
    vp.SectorSize = 512;
    vp.SectorsPerAllocationUnit = 1;
    vp.MaxComponentLength = 255;
    vp.FileInfoTimeout = 1000;
    vp.CaseSensitiveSearch = 0;
    vp.CasePreservedNames = 1;
    vp.UnicodeOnDisk = 1;
    vp.PersistentAcls = 1;
    vp.ReparsePoints = 1;
    vp.ReparsePointsAccessCheck = 0;
    vp.NamedStreams = 0;
    vp.ReadOnlyVolume = 1;
    vp.PostCleanupWhenModifiedOnly = 1;
    vp.FlushAndPurgeOnCleanup = 1;

    status = FspFileSystemCreate(L"" FSP_FSCTL_DISK_DEVICE_NAME, &vp, &g_Interface, &fs);
    if (!NT_SUCCESS(status)) {
        fwprintf(stderr, L"FspFileSystemCreate failed: 0x%08X\n", (unsigned)status);
        goto fail_index;
    }

    fs->UserContext = &W;
    g_Wim = &W;
    g_FsInstance = fs;

    /* Let the OS apply default security on the host mount folder; our SD is for in-volume nodes. */
    status = FspFileSystemSetMountPoint(fs, mountPath);
    if (!NT_SUCCESS(status)) {
        fwprintf(stderr, L"FspFileSystemSetMountPoint failed: 0x%08X\n", (unsigned)status);
        FspFileSystemDelete(fs);
        goto fail_index;
    }

    g_ExitEvent = CreateEventW(0, TRUE, FALSE, 0);
    if (!g_ExitEvent) {
        fwprintf(stderr, L"CreateEvent failed: %u\n", (unsigned)GetLastError());
        FspFileSystemRemoveMountPoint(fs);
        FspFileSystemDelete(fs);
        goto fail_index;
    }
    SetConsoleCtrlHandler(wim_console_ctrl, TRUE);

    status = FspFileSystemStartDispatcher(fs, 0);
    if (!NT_SUCCESS(status)) {
        fwprintf(stderr, L"FspFileSystemStartDispatcher failed: 0x%08X\n", (unsigned)status);
        if (g_ExitEvent) {
            CloseHandle(g_ExitEvent);
            g_ExitEvent = 0;
        }
        FspFileSystemRemoveMountPoint(fs);
        FspFileSystemDelete(fs);
        goto fail_index;
    }

    wprintf(L"Mounted image %u of\n  %s\nonto\n  %s\n(WinFsp) Press Ctrl+C to unmount.\n",
        image, wimPath, mountPath);
    if (g_wimfs_trace)
        wprintf(L"(debug trace enabled: %%TEMP%%\\wimfs_trace.log , crash dump: %%TEMP%%\\wimfs_lastcrash.txt)\n");

    wimfs_trace_fmt("main", L"dispatcher running");

    WaitForSingleObject(g_ExitEvent, INFINITE);
    {
        HANDLE evW = g_ExitEvent;

        g_ExitEvent = 0;
        if (evW)
            CloseHandle(evW);
    }

    FspFileSystemStopDispatcher(fs);
    FspFileSystemRemoveMountPoint(fs);
    FspFileSystemDelete(fs);
    g_FsInstance = 0;
    g_Wim = 0;
    goto cleanup_and_exit_ok;

cleanup_and_exit_ok:
    wimlib_free(W.wim);
    free_wim_index(&W);
    DeleteCriticalSection(&W.open_node_cs);
    DeleteCriticalSection(&W.extract_cs);
    wimlib_global_cleanup();
    return 0;

fail_index:
    wimfs_trace_fmt("main", L"fail_index");
    free_wim_index(&W);
    if (W.wim)
        wimlib_free(W.wim);
    DeleteCriticalSection(&W.open_node_cs);
    DeleteCriticalSection(&W.extract_cs);
    wimlib_global_cleanup();
    return 1;
}
