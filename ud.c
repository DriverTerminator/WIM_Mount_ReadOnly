/*
 * Read-only mount of fbinst "UD" data (primary + extended areas) via WinFsp / Dokan.
 * Layout derived from fbinst 1.6 / 1.7 sources in fbinst/ (Bean, GPL).
 */
#include <windows.h>
#include <winioctl.h>
#include <sddl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include <winfsp/winfsp.h>

#include "ud.h"

#if defined(_M_ARM64)
#define UD_TRY_DOKAN 0
#else
#define UD_TRY_DOKAN 1
#include <dokan.h>
#endif

#define FB_MAGIC_LONG 0x46424246u
#define FB_AR_MAGIC_LONG 0x52414246u
#define DEF_BASE_SIZE 63
#define UD_PATH_CCH 2048
#define UD_SEG_MAX 260

#pragma pack(push, 1)
struct ud_fb_ar_head {
    UINT32 ar_magic;
    UCHAR ver_major;
    UCHAR ver_minor;
    UINT16 list_used;
    UINT16 list_size;
    UINT16 pri_size;
    UINT32 ext_size;
};
#pragma pack(pop)

typedef struct UdFile {
    wchar_t *name;
    UINT32 data_start_sector;
    UINT64 data_size;
    __time64_t mtime;
} UdFile;

typedef struct UdFs {
    HANDLE h;
    UINT64 image_bytes;
    int ar_mode;
    int fmt16; /* 1: 32-bit data_size in list; 0: 64-bit (1.7) */
    UINT32 fb_list_start_sector;
    UINT32 fb_pri_size_sectors;
    UINT64 fb_total_size_sectors;
    unsigned char *list;
    int list_bytes;
    UdFile *files;
    size_t nfiles;
    BYTE default_sd[512];
    SIZE_T default_sd_len;
    WCHAR volume_label[32];
} UdFs;

static UdFs *g_Ud;
static FSP_FILE_SYSTEM_INTERFACE g_UdIfs;
static FSP_FILE_SYSTEM *g_UdFs;

#if UD_TRY_DOKAN
extern WCHAR g_dokanMountPath[];
extern volatile LONG g_dokanMounted;
typedef int(WINAPI *PFN_DokanMain)(PDOKAN_OPTIONS, PDOKAN_OPERATIONS);
typedef BOOL(WINAPI *PFN_DokanRemoveMountPoint)(LPCWSTR);
static PFN_DokanMain g_pfn_DokanMain;
extern PFN_DokanRemoveMountPoint g_DokanRemoveMountPoint;

extern BOOL dokan_try_portable_stack_setup(void);
#endif

extern BOOL wim_mount_winfsp_stack_prepare(void);
extern BOOL wim_mount_preflight(PWSTR mountPoint);
extern int verify_mount_point_winfsp(PWSTR mountPath);
#if UD_TRY_DOKAN
extern int ensure_mount_point_dokan(PWSTR mountPath);
#endif

static void ud_strip_bs(wchar_t *p)
{
    size_t n = wcslen(p);
    while (n > 1 && p[n - 1] == L'\\')
        p[--n] = 0;
}

static void ud_norm_open(wchar_t *t)
{
    ud_strip_bs(t);
    if (!t[0]) {
        t[0] = L'\\';
        t[1] = 0;
    }
}

static void ud_fwdslashes_to_backslash(wchar_t *p)
{
    for (; *p; p++)
        if (*p == L'/')
            *p = L'\\';
}

/* WinFsp preflight/set/remove mount point must use the same absolute path the shell resolves. */
static int ud_abs_mount_path(const wchar_t *src, wchar_t *out, size_t outcch)
{
    DWORD n;

    if (!src || !out || outcch < 4)
        return 0;
    n = GetFullPathNameW(src, (DWORD)outcch, out, NULL);
    if (n == 0 || n >= outcch)
        return 0;
    ud_fwdslashes_to_backslash(out);
    ud_strip_bs(out);
    return out[0] != 0;
}

static void ud_file_fullpath(const UdFile *f, wchar_t *out, size_t cch)
{
    if (!f->name || !f->name[0])
        swprintf_s(out, cch, L"\\");
    else if (f->name[0] == L'\\')
        wcscpy_s(out, cch, f->name);
    else
        swprintf_s(out, cch, L"\\%ls", f->name);
}

static int ud_relpath_from_dir(const wchar_t *full, const wchar_t *D, const wchar_t **rel)
{
    size_t ld;

    if (!full || !D || full[0] != L'\\' || D[0] != L'\\')
        return 0;
    ld = wcslen(D);
    if (ld == 1 && D[1] == 0) {
        if (full[1] == 0)
            return 0;
        *rel = full + 1;
        return (*rel)[0] != 0;
    }
    if (_wcsnicmp(full, D, (int)ld) != 0)
        return 0;
    if (full[ld] != L'\\')
        return 0;
    *rel = full + ld + 1;
    return (*rel)[0] != 0;
}

static int ud_dir_has_child_entries(UdFs *U, const wchar_t *D)
{
    wchar_t full[UD_PATH_CCH];
    size_t i;
    const wchar_t *r;
    size_t ld;

    if (!D || D[0] != L'\\')
        return 0;
    ld = wcslen(D);
    if (ld == 1 && D[1] == 0)
        return 1;
    for (i = 0; i < U->nfiles; i++) {
        ud_file_fullpath(&U->files[i], full, UD_PATH_CCH);
        if (!_wcsicmp(full, D))
            continue;
        if (ud_relpath_from_dir(full, D, &r))
            return 1;
    }
    return 0;
}

static int ud_lookup_file_idx(UdFs *U, const wchar_t *norm, size_t *idx)
{
    wchar_t full[UD_PATH_CCH];
    size_t i;

    for (i = 0; i < U->nfiles; i++) {
        ud_file_fullpath(&U->files[i], full, UD_PATH_CCH);
        if (!_wcsicmp(full, norm)) {
            *idx = i;
            return 1;
        }
    }
    return 0;
}

static int ud_is_known_directory(UdFs *U, const wchar_t *t)
{
    if (!t || t[0] != L'\\')
        return 0;
    if (t[1] == 0)
        return 1;
    return ud_dir_has_child_entries(U, t);
}

typedef struct {
    wchar_t nm[UD_SEG_MAX];
    int is_file;
    size_t fidx;
} ud_ent;

static int ud_ent_cmp(const void *a, const void *b)
{
    return _wcsicmp(((const ud_ent *)a)->nm, ((const ud_ent *)b)->nm);
}

static int ud_col_merge(ud_ent **pa, int *pn, int *pcap, const wchar_t *nm, int is_file, size_t fidx)
{
    int i, n = *pn;

    for (i = 0; i < n; i++) {
        if (_wcsicmp((*pa)[i].nm, nm))
            continue;
        if (!is_file && (*pa)[i].is_file)
            (*pa)[i].is_file = 0;
        else if (is_file && (*pa)[i].is_file)
            return 0;
        return 0;
    }
    if (n >= *pcap) {
        int ncap = *pcap ? *pcap * 2 : 32;
        ud_ent *na = realloc(*pa, (size_t)ncap * sizeof(ud_ent));

        if (!na)
            return -1;
        *pa = na;
        *pcap = ncap;
    }
    wcscpy_s((*pa)[n].nm, UD_SEG_MAX, nm);
    (*pa)[n].is_file = is_file;
    (*pa)[n].fidx = fidx;
    *pn = n + 1;
    return 0;
}

static int ud_collect_children(UdFs *U, const wchar_t *D, ud_ent **out_arr, int *out_n)
{
    wchar_t full[UD_PATH_CCH];
    ud_ent *arr = NULL;
    int n = 0, cap = 0;
    size_t i;

    *out_arr = NULL;
    *out_n = 0;
    if (!D || D[0] != L'\\')
        return 0;

    for (i = 0; i < U->nfiles; i++) {
        const wchar_t *r, *sep;
        wchar_t seg[UD_SEG_MAX];
        size_t sl;

        ud_file_fullpath(&U->files[i], full, UD_PATH_CCH);
        if (!_wcsicmp(full, D))
            continue;
        if (!ud_relpath_from_dir(full, D, &r))
            continue;
        sep = wcschr(r, L'\\');
        if (!sep) {
            if (ud_col_merge(&arr, &n, &cap, r, 1, i) < 0)
                goto fail;
        } else {
            sl = (size_t)(sep - r);
            if (sl == 0 || sl >= UD_SEG_MAX)
                continue;
            wmemcpy(seg, r, sl * sizeof(wchar_t));
            seg[sl] = 0;
            if (ud_col_merge(&arr, &n, &cap, seg, 0, 0) < 0)
                goto fail;
        }
    }
    if (n > 1)
        qsort(arr, (size_t)n, sizeof *arr, ud_ent_cmp);
    *out_arr = arr;
    *out_n = n;
    return 0;
fail:
    free(arr);
    return -1;
}

/* Each on-disk list sector is 512 bytes with 510 bytes payload + 2-byte mark (fbinst). */
static void ud_destripe(const unsigned char *raw512, int sectors, unsigned char *out510)
{
    int i;

    for (i = 0; i < sectors; i++)
        memcpy(out510 + (size_t)i * 510, raw512 + (size_t)i * 512, 510);
}

static int ud_read_sec(UdFs *U, UINT64 sector, void *buf512)
{
    LARGE_INTEGER li;
    DWORD br;

    li.QuadPart = (LONGLONG)sector * 512;
    if (!SetFilePointerEx(U->h, li, NULL, FILE_BEGIN))
        return 0;
    if (!ReadFile(U->h, buf512, 512, &br, NULL) || br != 512)
        return 0;
    return 1;
}

static int ud_read_bytes(UdFs *U, UINT32 start_sec, UINT64 off, void *dst, UINT32 len)
{
    UINT32 n = (start_sec >= U->fb_pri_size_sectors) ? 512u : 510u;
    UINT8 *d = dst;
    UINT64 pos = off;

    while (len) {
        UINT64 sec_idx = pos / n;
        UINT32 skip = (UINT32)(pos % n);
        UINT32 chunk = n - skip;
        UINT8 tmp[512];

        if (chunk > len)
            chunk = len;
        if (!ud_read_sec(U, (UINT64)start_sec + sec_idx, tmp))
            return 0;
        memcpy(d, tmp + skip, chunk);
        d += chunk;
        pos += chunk;
        len -= chunk;
    }
    return 1;
}

static void ud_time_to_filetime(__time64_t t, FILETIME *ft)
{
    ULONGLONG u = (ULONGLONG)(t + 11644473600LL) * 10000000ULL;

    ft->dwLowDateTime = (DWORD)u;
    ft->dwHighDateTime = (DWORD)(u >> 32);
}

static void ud_time_to_winfsp_uint64(__time64_t t, UINT64 *p)
{
    *p = (UINT64)(t + 11644473600LL) * 10000000ULL;
}

static void ud_free_files(UdFs *U)
{
    size_t i;

    for (i = 0; i < U->nfiles; i++)
        free(U->files[i].name);
    free(U->files);
    U->files = NULL;
    U->nfiles = 0;
}

static int ud_parse_list_walk(UdFs *U)
{
    int ofs = 0;
    size_t cap = 0;
    size_t okn = 0;
    UdFile *arr = NULL;

    while (ofs < U->list_bytes && U->list[ofs]) {
        unsigned sz = U->list[ofs];
        int tot = (int)sz + 2;
        UINT32 ds;
        UINT64 dsz = 0;
        __time64_t tm = 0;
        int name_off;
        const char *nm;
        size_t nl;
        wchar_t *wn;

        memcpy(&ds, U->list + ofs + 2, 4);
        if (U->fmt16) {
            UINT32 z, tm32;

            memcpy(&z, U->list + ofs + 6, 4);
            dsz = z;
            memcpy(&tm32, U->list + ofs + 10, 4);
            tm = (__time64_t)tm32;
            name_off = 14;
        } else {
            memcpy(&dsz, U->list + ofs + 6, 8);
            memcpy(&tm, U->list + ofs + 14, sizeof(tm));
            name_off = 22;
        }
        if (ofs + tot > U->list_bytes || tot < name_off + 1)
            goto bad;
        nm = (const char *)(U->list + ofs + name_off);
        nl = strnlen(nm, (size_t)(ofs + tot - name_off));
        if (nm[nl] != 0 || nl == 0)
            goto bad;

        if (okn >= cap) {
            size_t ncap = cap ? cap * 2 : 32;
            UdFile *na = realloc(arr, ncap * sizeof(UdFile));

            if (!na)
                goto bad;
            arr = na;
            cap = ncap;
        }
        wn = malloc((nl + 1) * sizeof(wchar_t));
        if (!wn)
            goto bad;
        if (MultiByteToWideChar(CP_UTF8, 0, nm, (int)nl + 1, wn, (int)nl + 1) <= 0) {
            if (MultiByteToWideChar(CP_ACP, 0, nm, (int)nl + 1, wn, (int)nl + 1) <= 0) {
                free(wn);
                goto bad;
            }
        }
        ud_fwdslashes_to_backslash(wn);
        arr[okn].name = wn;
        arr[okn].data_start_sector = ds;
        arr[okn].data_size = dsz;
        arr[okn].mtime = tm;
        okn++;
        ofs += tot;
    }

    U->files = arr;
    U->nfiles = okn;
    return 1;

bad:
    {
        size_t j;

        for (j = 0; j < okn; j++)
            free(arr[j].name);
        free(arr);
    }
    U->files = NULL;
    U->nfiles = 0;
    return 0;
}

/*
 * fbinst 1.6 list entries use 32-bit data_size; 1.7 uses 64-bit (see fbm_file in fbinst 1.6/1.7).
 * On-disk ver_minor from fb_data / fb_ar_data matches fbinst version.h (e.g. 7 for 1.7).
 * Try the likely layout first, then the other — handles mis-labeled or old superblocks.
 */
static int ud_parse_list_auto(UdFs *U, UCHAR ver_major, UCHAR ver_minor)
{
    int prefer32 = (ver_minor < 7);

    (void)ver_major;
    U->fmt16 = prefer32 ? 1 : 0;
    if (ud_parse_list_walk(U))
        return 1;
    ud_free_files(U);
    U->fmt16 = prefer32 ? 0 : 1;
    if (ud_parse_list_walk(U))
        return 1;
    ud_free_files(U);
    return 0;
}

/* e.g. (hd0) -> \\.\PhysicalDrive0 (grub-style disk index). */
static int ud_resolve_source_path(const wchar_t *src, wchar_t *out, size_t outcch)
{
    const wchar_t *p = src;
    unsigned drv;

    while (*p == L' ' || *p == L'\t')
        p++;
    if (_wcsnicmp(p, L"(hd", 3) != 0) {
        if (wcslen(src) >= outcch)
            return 0;
        wcscpy_s(out, outcch, src);
        return 1;
    }
    p += 3;
    {
        const wchar_t *dig = p;

        drv = 0;
        while (*p >= L'0' && *p <= L'9')
            drv = drv * 10u + (unsigned)(*p++ - L'0');
        if (p == dig || *p != L')')
            return 0;
    }
    if (swprintf_s(out, outcch, L"\\\\.\\PhysicalDrive%u", drv) < 0)
        return 0;
    return 1;
}

static int ud_load(UdFs *U, const wchar_t *src, wchar_t *err, size_t errcch)
{
    unsigned char sec0[512], boot[512], *raw = NULL, *lin = NULL;
    UINT32 boot_base = 0;
    UINT16 boot_size = 0, list_sz = 0, pri = 0;
    UINT32 ext = 0;
    UCHAR ver_major = 0;
    UCHAR ver_minor = 0;
    int i;
    DWORD br;
    GET_LENGTH_INFORMATION gli;
    wchar_t pathbuf[512];

    if (!ud_resolve_source_path(src, pathbuf, _countof(pathbuf))) {
        swprintf_s(err, errcch, L"Invalid UD source path (try \\\\.\\PhysicalDriveN or (hdN)).");
        return 0;
    }

    U->h = CreateFileW(pathbuf, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        NULL, OPEN_EXISTING, FILE_FLAG_RANDOM_ACCESS, NULL);
    if (U->h == INVALID_HANDLE_VALUE) {
        swprintf_s(err, errcch, L"Cannot open \"%ls\" (%lu).", pathbuf, (unsigned long)GetLastError());
        return 0;
    }
    U->image_bytes = 0;
    if (GetFileSizeEx(U->h, (LARGE_INTEGER *)&U->image_bytes) && U->image_bytes >= sizeof(gli)) {
        if (DeviceIoControl(U->h, IOCTL_DISK_GET_LENGTH_INFO, NULL, 0, &gli, sizeof(gli), &br, NULL))
            U->image_bytes = gli.Length.QuadPart;
    }

    if (!ud_read_sec(U, 0, sec0)) {
        swprintf_s(err, errcch, L"Read sector 0 failed.");
        goto fail;
    }

    if (*(UINT32 *)sec0 == FB_AR_MAGIC_LONG) {
        struct ud_fb_ar_head *ah = (struct ud_fb_ar_head *)sec0;

        U->ar_mode = 1;
        list_sz = ah->list_size;
        pri = ah->pri_size;
        ext = ah->ext_size;
        ver_major = ah->ver_major;
        ver_minor = ah->ver_minor;
        U->fb_list_start_sector = 1;
        U->fb_pri_size_sectors = 1u + list_sz;
        U->fb_total_size_sectors = 0x7fffffffu;
    } else {
        if (*(UINT32 *)(sec0 + 0x1b4) != FB_MAGIC_LONG) {
            if (!ud_read_sec(U, DEF_BASE_SIZE, sec0))
                goto nofb;
            if (*(UINT32 *)(sec0 + 0x1b4) != FB_MAGIC_LONG) {
nofb:
                swprintf_s(err, errcch, L"Not an fbinst UD image (missing FBBF/FBAR).");
                goto fail;
            }
        }
        U->ar_mode = 0;
        memcpy(&boot_base, sec0 + 0x1b2, 2);
        if (!ud_read_sec(U, (UINT64)boot_base + 1, boot)) {
            swprintf_s(err, errcch, L"Read fb_data sector failed.");
            goto fail;
        }
        memcpy(&boot_size, boot + 0, 2);
        ver_major = boot[4];
        ver_minor = boot[5];
        memcpy(&list_sz, boot + 8, 2);
        memcpy(&pri, boot + 10, 2);
        memcpy(&ext, boot + 12, 4);
        U->fb_list_start_sector = boot_base + 1u + boot_size;
        U->fb_pri_size_sectors = pri;
        U->fb_total_size_sectors = (UINT64)pri + (UINT64)ext;
    }

    raw = malloc((size_t)list_sz * 512);
    lin = malloc((size_t)list_sz * 510);
    if (!raw || !lin) {
        swprintf_s(err, errcch, L"Out of memory.");
        goto fail;
    }
    for (i = 0; i < list_sz; i++) {
        if (!ud_read_sec(U, (UINT64)U->fb_list_start_sector + (unsigned)i, raw + (size_t)i * 512)) {
            swprintf_s(err, errcch, L"Read file list sector %u failed.", (unsigned)i);
            goto fail;
        }
    }
    ud_destripe(raw, list_sz, lin);
    free(raw);
    raw = NULL;
    U->list = lin;
    U->list_bytes = list_sz * 510;
    lin = NULL;

    if (!ud_parse_list_auto(U, ver_major, ver_minor)) {
        swprintf_s(err, errcch, L"Invalid UD file list (1.6/1.7 auto-detect failed).");
        goto fail;
    }

    wcscpy_s(U->volume_label, 32, L"UD");
    return 1;

fail:
    free(raw);
    free(lin);
    if (U->h != INVALID_HANDLE_VALUE) {
        CloseHandle(U->h);
        U->h = INVALID_HANDLE_VALUE;
    }
    free(U->list);
    U->list = NULL;
    return 0;
}

static void ud_free(UdFs *U)
{
    size_t i;

    if (!U)
        return;
    for (i = 0; i < U->nfiles; i++)
        free(U->files[i].name);
    free(U->files);
    free(U->list);
    if (U->h != INVALID_HANDLE_VALUE)
        CloseHandle(U->h);
    memset(U, 0, sizeof *U);
    U->h = INVALID_HANDLE_VALUE;
}

typedef struct UdWfCtx {
    LONG ref;
    int is_dir;
    size_t fidx;
    wchar_t dir[UD_PATH_CCH];
} UdWfCtx;

static UdWfCtx *ud_wf_alloc(int is_dir, size_t fidx, const wchar_t *opened_dir)
{
    UdWfCtx *c = calloc(1, sizeof *c);

    if (!c)
        return NULL;
    c->ref = 1;
    c->is_dir = is_dir;
    c->fidx = fidx;
    if (is_dir && opened_dir)
        wcscpy_s(c->dir, UD_PATH_CCH, opened_dir);
    return c;
}

static void ud_wf_unref(UdWfCtx *c)
{
    if (!c)
        return;
    if (InterlockedDecrement(&c->ref) == 0)
        free(c);
}

static NTSTATUS ud_GetVolumeInfo(FSP_FILE_SYSTEM *FileSystem, FSP_FSCTL_VOLUME_INFO *VolumeInfo)
{
    UdFs *U = FileSystem->UserContext;

    if (!VolumeInfo || !U)
        return STATUS_INVALID_PARAMETER;
    memset(VolumeInfo, 0, sizeof *VolumeInfo);
    VolumeInfo->TotalSize = 1ULL << 30;
    VolumeInfo->FreeSize = 0;
    wcscpy_s(VolumeInfo->VolumeLabel, sizeof(VolumeInfo->VolumeLabel) / sizeof(WCHAR), U->volume_label);
    VolumeInfo->VolumeLabelLength = (UINT16)(wcslen(VolumeInfo->VolumeLabel) * sizeof(WCHAR));
    return STATUS_SUCCESS;
}

static NTSTATUS ud_SetVolumeLabel_(FSP_FILE_SYSTEM *FileSystem, PWSTR VolumeLabel,
    FSP_FSCTL_VOLUME_INFO *VolumeInfo)
{
    (void)FileSystem;
    (void)VolumeLabel;
    (void)VolumeInfo;
    return STATUS_INVALID_DEVICE_REQUEST;
}

static NTSTATUS ud_GetSecurityByName(FSP_FILE_SYSTEM *FileSystem, PWSTR FileName, PUINT32 PFileAttributes,
    PSECURITY_DESCRIPTOR SecurityDescriptor, SIZE_T *PSecurityDescriptorSize)
{
    UdFs *U = FileSystem->UserContext;
    BYTE *sd = U->default_sd;
    SIZE_T sdlen = U->default_sd_len;
    wchar_t t[UD_PATH_CCH];
    size_t fidx;

    if (!PFileAttributes || !PSecurityDescriptorSize)
        return STATUS_INVALID_PARAMETER;
    *PFileAttributes = FILE_ATTRIBUTE_READONLY;
    wcscpy_s(t, UD_PATH_CCH, FileName);
    ud_norm_open(t);
    if (t[0] == L'\\' && t[1] == 0)
        *PFileAttributes |= FILE_ATTRIBUTE_DIRECTORY;
    else if (ud_lookup_file_idx(U, t, &fidx))
        *PFileAttributes = FILE_ATTRIBUTE_READONLY;
    else if (ud_is_known_directory(U, t))
        *PFileAttributes |= FILE_ATTRIBUTE_DIRECTORY;
    else
        return STATUS_OBJECT_NAME_NOT_FOUND;
    if (!SecurityDescriptor) {
        *PSecurityDescriptorSize = sdlen;
        return STATUS_BUFFER_OVERFLOW;
    }
    if (*PSecurityDescriptorSize < sdlen)
        return STATUS_BUFFER_TOO_SMALL;
    memcpy(SecurityDescriptor, sd, sdlen);
    *PSecurityDescriptorSize = sdlen;
    return STATUS_SUCCESS;
}

static NTSTATUS ud_Create(FSP_FILE_SYSTEM *FileSystem, PWSTR FileName, UINT32 CreateOptions,
    UINT32 GrantedAccess, UINT32 FileAttributes, PSECURITY_DESCRIPTOR SecurityDescriptor, UINT64 AllocationSize,
    PVOID *PFileContext, FSP_FSCTL_FILE_INFO *FileInfo)
{
    (void)FileSystem;
    (void)FileName;
    (void)CreateOptions;
    (void)GrantedAccess;
    (void)FileAttributes;
    (void)SecurityDescriptor;
    (void)AllocationSize;
    (void)PFileContext;
    (void)FileInfo;
    return STATUS_ACCESS_DENIED;
}

static NTSTATUS ud_Open(FSP_FILE_SYSTEM *FileSystem, PWSTR FileName, UINT32 CreateOptions,
    UINT32 GrantedAccess, PVOID *PFileContext, FSP_FSCTL_FILE_INFO *FileInfo)
{
    UdFs *U = FileSystem->UserContext;
    wchar_t t[UD_PATH_CCH];
    UdWfCtx *ctx;
    size_t fidx;

    (void)CreateOptions;
    if (GrantedAccess & (FILE_WRITE_DATA | FILE_APPEND_DATA | DELETE | WRITE_DAC | WRITE_OWNER))
        return STATUS_ACCESS_DENIED;
    wcscpy_s(t, UD_PATH_CCH, FileName);
    ud_norm_open(t);
    if (t[0] == L'\\' && t[1] == 0) {
        ctx = ud_wf_alloc(1, 0, t);
        if (!ctx)
            return STATUS_INSUFFICIENT_RESOURCES;
        memset(FileInfo, 0, sizeof *FileInfo);
        FileInfo->FileAttributes = FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_READONLY;
        *PFileContext = ctx;
        return STATUS_SUCCESS;
    }
    if (ud_lookup_file_idx(U, t, &fidx)) {
        ctx = ud_wf_alloc(0, fidx, NULL);
        if (!ctx)
            return STATUS_INSUFFICIENT_RESOURCES;
        memset(FileInfo, 0, sizeof *FileInfo);
        FileInfo->FileAttributes = FILE_ATTRIBUTE_READONLY;
        FileInfo->FileSize = U->files[fidx].data_size;
        ud_time_to_winfsp_uint64(U->files[fidx].mtime, &FileInfo->LastWriteTime);
        *PFileContext = ctx;
        return STATUS_SUCCESS;
    }
    if (ud_is_known_directory(U, t)) {
        ctx = ud_wf_alloc(1, 0, t);
        if (!ctx)
            return STATUS_INSUFFICIENT_RESOURCES;
        memset(FileInfo, 0, sizeof *FileInfo);
        FileInfo->FileAttributes = FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_READONLY;
        *PFileContext = ctx;
        return STATUS_SUCCESS;
    }
    return STATUS_OBJECT_NAME_NOT_FOUND;
}

static NTSTATUS ud_Overwrite(FSP_FILE_SYSTEM *FileSystem, PVOID FileContext, UINT32 FileAttributes,
    BOOLEAN ReplaceFileAttributes, UINT64 AllocationSize, FSP_FSCTL_FILE_INFO *FileInfo)
{
    (void)FileSystem;
    (void)FileContext;
    (void)FileAttributes;
    (void)ReplaceFileAttributes;
    (void)AllocationSize;
    (void)FileInfo;
    return STATUS_ACCESS_DENIED;
}

static VOID ud_Cleanup(FSP_FILE_SYSTEM *FileSystem, PVOID FileContext, PWSTR FileName, ULONG Flags)
{
    (void)FileSystem;
    (void)FileContext;
    (void)FileName;
    (void)Flags;
}

static VOID ud_Close(FSP_FILE_SYSTEM *FileSystem, PVOID FileContext)
{
    (void)FileSystem;
    ud_wf_unref(FileContext);
}

static NTSTATUS ud_Read(FSP_FILE_SYSTEM *FileSystem, PVOID FileContext, PVOID Buffer, UINT64 Offset,
    ULONG Length, PULONG PBytesTransferred)
{
    UdFs *U = FileSystem->UserContext;
    UdWfCtx *c = FileContext;
    UdFile *f;

    *PBytesTransferred = 0;
    if (!c || c->is_dir)
        return STATUS_INVALID_PARAMETER;
    f = &U->files[c->fidx];
    if (Offset >= f->data_size)
        return STATUS_SUCCESS;
    if ((UINT64)Length > f->data_size - Offset)
        Length = (ULONG)(f->data_size - Offset);
    if (!ud_read_bytes(U, f->data_start_sector, Offset, Buffer, Length))
        return STATUS_IO_DEVICE_ERROR;
    *PBytesTransferred = Length;
    return STATUS_SUCCESS;
}

static NTSTATUS ud_Write(FSP_FILE_SYSTEM *FileSystem, PVOID FileContext, PVOID Buffer, UINT64 Offset,
    ULONG Length, BOOLEAN WriteToEndOfFile, BOOLEAN ConstrainedIo, PULONG PBytesTransferred,
    FSP_FSCTL_FILE_INFO *FileInfo)
{
    (void)FileSystem;
    (void)FileContext;
    (void)Buffer;
    (void)Offset;
    (void)Length;
    (void)WriteToEndOfFile;
    (void)ConstrainedIo;
    (void)PBytesTransferred;
    (void)FileInfo;
    return STATUS_ACCESS_DENIED;
}

static NTSTATUS ud_Flush(FSP_FILE_SYSTEM *FileSystem, PVOID FileContext, FSP_FSCTL_FILE_INFO *FileInfo)
{
    (void)FileSystem;
    (void)FileContext;
    (void)FileInfo;
    return STATUS_SUCCESS;
}

static NTSTATUS ud_GetFileInfo(FSP_FILE_SYSTEM *FileSystem, PVOID FileContext, FSP_FSCTL_FILE_INFO *FileInfo)
{
    UdFs *U = FileSystem->UserContext;
    UdWfCtx *c = FileContext;

    memset(FileInfo, 0, sizeof *FileInfo);
    if (!c)
        return STATUS_INVALID_PARAMETER;
    if (c->is_dir) {
        FileInfo->FileAttributes = FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_READONLY;
        return STATUS_SUCCESS;
    }
    FileInfo->FileAttributes = FILE_ATTRIBUTE_READONLY;
    FileInfo->FileSize = U->files[c->fidx].data_size;
    ud_time_to_winfsp_uint64(U->files[c->fidx].mtime, &FileInfo->LastWriteTime);
    return STATUS_SUCCESS;
}

static NTSTATUS ud_SetBasicInfo(FSP_FILE_SYSTEM *FileSystem, PVOID FileContext, UINT32 FileAttributes,
    UINT64 CreationTime, UINT64 LastAccessTime, UINT64 LastWriteTime, UINT64 ChangeTime,
    FSP_FSCTL_FILE_INFO *FileInfo)
{
    (void)FileSystem;
    (void)FileContext;
    (void)FileAttributes;
    (void)CreationTime;
    (void)LastAccessTime;
    (void)LastWriteTime;
    (void)ChangeTime;
    (void)FileInfo;
    return STATUS_ACCESS_DENIED;
}

static NTSTATUS ud_SetFileSize(FSP_FILE_SYSTEM *FileSystem, PVOID FileContext, UINT64 NewSize,
    BOOLEAN SetAllocationSize, FSP_FSCTL_FILE_INFO *FileInfo)
{
    (void)FileSystem;
    (void)FileContext;
    (void)NewSize;
    (void)SetAllocationSize;
    (void)FileInfo;
    return STATUS_ACCESS_DENIED;
}

static NTSTATUS ud_CanDelete(FSP_FILE_SYSTEM *FileSystem, PVOID FileContext, PWSTR FileName)
{
    (void)FileSystem;
    (void)FileContext;
    (void)FileName;
    return STATUS_ACCESS_DENIED;
}

static NTSTATUS ud_Rename(FSP_FILE_SYSTEM *FileSystem, PVOID FileContext, PWSTR FileName, PWSTR NewFileName,
    BOOLEAN ReplaceIfExists)
{
    (void)FileSystem;
    (void)FileContext;
    (void)FileName;
    (void)NewFileName;
    (void)ReplaceIfExists;
    return STATUS_ACCESS_DENIED;
}

static NTSTATUS ud_GetSecurity(FSP_FILE_SYSTEM *FileSystem, PVOID FileContext,
    PSECURITY_DESCRIPTOR SecurityDescriptor, SIZE_T *PSecurityDescriptorSize)
{
    UdFs *U = FileSystem->UserContext;
    SIZE_T n = U->default_sd_len;

    if (*PSecurityDescriptorSize < n)
        return STATUS_BUFFER_TOO_SMALL;
    memcpy(SecurityDescriptor, U->default_sd, n);
    *PSecurityDescriptorSize = n;
    return STATUS_SUCCESS;
}

static NTSTATUS ud_SetSecurity(FSP_FILE_SYSTEM *FileSystem, PVOID FileContext,
    SECURITY_INFORMATION SecurityInformation, PSECURITY_DESCRIPTOR ModificationDescriptor)
{
    (void)FileSystem;
    (void)FileContext;
    (void)SecurityInformation;
    (void)ModificationDescriptor;
    return STATUS_ACCESS_DENIED;
}

static BOOLEAN ud_emit_dir(PVOID Buffer, ULONG Length, PULONG PTransferred, const wchar_t *name, const UdFile *f)
{
    FSP_FSCTL_DIR_INFO *di;
    size_t nb = wcslen(name) * sizeof(WCHAR);
    UINT32 rec = (UINT32)sizeof(FSP_FSCTL_DIR_INFO) + (UINT32)nb;
    UINT8 *raw;
    BOOLEAN ok;

    if (rec > 0xFFFF)
        return TRUE;
    raw = malloc(rec);
    if (!raw)
        return TRUE;
    di = (FSP_FSCTL_DIR_INFO *)raw;
    memset(di, 0, rec);
    memset(di->Padding, 0, sizeof di->Padding);
    di->Size = (UINT16)rec;
    if (f) {
        memset(&di->FileInfo, 0, sizeof di->FileInfo);
        di->FileInfo.FileAttributes = FILE_ATTRIBUTE_READONLY;
        di->FileInfo.FileSize = f->data_size;
        ud_time_to_winfsp_uint64(f->mtime, &di->FileInfo.LastWriteTime);
    } else {
        memset(&di->FileInfo, 0, sizeof di->FileInfo);
        di->FileInfo.FileAttributes = FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_READONLY;
    }
    memcpy(di->FileNameBuf, name, nb);
    ok = FspFileSystemAddDirInfo(di, Buffer, Length, PTransferred);
    free(raw);
    return ok;
}

static NTSTATUS ud_ReadDirectory(FSP_FILE_SYSTEM *FileSystem, PVOID FileContext, PWSTR Pattern, PWSTR Marker,
    PVOID Buffer, ULONG Length, PULONG PBytesTransferred)
{
    UdFs *U = FileSystem->UserContext;
    UdWfCtx *o = FileContext;
    const wchar_t *child_marker = Marker;
    ud_ent *ents = NULL;
    int nent = 0, ei;

    (void)Pattern;
    *PBytesTransferred = 0;
    if (!U || !o || !o->is_dir)
        return STATUS_INVALID_PARAMETER;

    if (ud_collect_children(U, o->dir, &ents, &nent) < 0)
        return STATUS_INSUFFICIENT_RESOURCES;

    if (!Marker) {
        if (!ud_emit_dir(Buffer, Length, PBytesTransferred, L".", NULL))
            goto out;
    }
    if (!Marker || (Marker[0] == L'.' && Marker[1] == 0)) {
        if (!ud_emit_dir(Buffer, Length, PBytesTransferred, L"..", NULL))
            goto out;
        child_marker = 0;
    } else if (Marker[0] == L'.' && Marker[1] == L'.' && Marker[2] == 0) {
        child_marker = 0;
    }

    for (ei = 0; ei < nent; ei++) {
        if (child_marker) {
            if (_wcsicmp(ents[ei].nm, child_marker) <= 0)
                continue;
        }
        if (ents[ei].is_file) {
            if (!ud_emit_dir(Buffer, Length, PBytesTransferred, ents[ei].nm, &U->files[ents[ei].fidx]))
                goto out;
        } else {
            if (!ud_emit_dir(Buffer, Length, PBytesTransferred, ents[ei].nm, NULL))
                goto out;
        }
    }

    FspFileSystemAddDirInfo(0, Buffer, Length, PBytesTransferred);
out:
    free(ents);
    return STATUS_SUCCESS;
}

static void ud_fill_ifs(void)
{
    memset(&g_UdIfs, 0, sizeof g_UdIfs);
    g_UdIfs.GetVolumeInfo = ud_GetVolumeInfo;
    g_UdIfs.SetVolumeLabel = ud_SetVolumeLabel_;
    g_UdIfs.GetSecurityByName = ud_GetSecurityByName;
    g_UdIfs.Create = ud_Create;
    g_UdIfs.Open = ud_Open;
    g_UdIfs.Overwrite = ud_Overwrite;
    g_UdIfs.Cleanup = ud_Cleanup;
    g_UdIfs.Close = ud_Close;
    g_UdIfs.Read = ud_Read;
    g_UdIfs.Write = ud_Write;
    g_UdIfs.Flush = ud_Flush;
    g_UdIfs.GetFileInfo = ud_GetFileInfo;
    g_UdIfs.SetBasicInfo = ud_SetBasicInfo;
    g_UdIfs.SetFileSize = ud_SetFileSize;
    g_UdIfs.CanDelete = ud_CanDelete;
    g_UdIfs.Rename = ud_Rename;
    g_UdIfs.GetSecurity = ud_GetSecurity;
    g_UdIfs.SetSecurity = ud_SetSecurity;
    g_UdIfs.ReadDirectory = ud_ReadDirectory;
}

#if UD_TRY_DOKAN
typedef struct UdDokanCtx {
    int is_dir;
    size_t fidx;
    wchar_t dir[UD_PATH_CCH];
} UdDokanCtx;

static int DOKAN_CALLBACK ud_dokan_CreateFile(LPCWSTR FileName, DWORD DesiredAccess, DWORD ShareMode,
    DWORD CreationDisposition, DWORD FlagsAndAttributes, PDOKAN_FILE_INFO Dfi)
{
    wchar_t t[UD_PATH_CCH];
    UdFs *U = (UdFs *)(ULONG_PTR)Dfi->DokanOptions->GlobalContext;
    UdDokanCtx *ctx;
    size_t fidx;

    (void)ShareMode;
    (void)FlagsAndAttributes;
    if (!U || !FileName)
        return -ERROR_INVALID_PARAMETER;
    if (DesiredAccess & (GENERIC_WRITE | DELETE | WRITE_DAC | WRITE_OWNER))
        return -ERROR_ACCESS_DENIED;
    wcscpy_s(t, UD_PATH_CCH, FileName);
    ud_norm_open(t);
    ctx = malloc(sizeof *ctx);
    if (!ctx)
        return -ERROR_NOT_ENOUGH_MEMORY;
    memset(ctx, 0, sizeof *ctx);
    if (t[0] == L'\\' && t[1] == 0) {
        ctx->is_dir = 1;
        ctx->fidx = (size_t)-1;
        wcscpy_s(ctx->dir, UD_PATH_CCH, t);
    } else if (ud_lookup_file_idx(U, t, &fidx)) {
        ctx->is_dir = 0;
        ctx->fidx = fidx;
    } else if (ud_is_known_directory(U, t)) {
        ctx->is_dir = 1;
        ctx->fidx = (size_t)-1;
        wcscpy_s(ctx->dir, UD_PATH_CCH, t);
    } else {
        free(ctx);
        return -ERROR_FILE_NOT_FOUND;
    }
    if (CreationDisposition == CREATE_NEW || CreationDisposition == CREATE_ALWAYS ||
        CreationDisposition == TRUNCATE_EXISTING) {
        free(ctx);
        return -ERROR_ACCESS_DENIED;
    }
    Dfi->IsDirectory = ctx->is_dir ? 1 : 0;
    Dfi->Context = (ULONG64)(ULONG_PTR)ctx;
    return 0;
}

static int DOKAN_CALLBACK ud_dokan_OpenDirectory(LPCWSTR FileName, PDOKAN_FILE_INFO Dfi)
{
    wchar_t t[UD_PATH_CCH];
    UdFs *U = (UdFs *)(ULONG_PTR)Dfi->DokanOptions->GlobalContext;
    UdDokanCtx *ctx;

    if (!U || !FileName)
        return -ERROR_INVALID_PARAMETER;
    wcscpy_s(t, UD_PATH_CCH, FileName);
    ud_norm_open(t);
    if (!ud_is_known_directory(U, t))
        return -ERROR_PATH_NOT_FOUND;
    ctx = malloc(sizeof *ctx);
    if (!ctx)
        return -ERROR_NOT_ENOUGH_MEMORY;
    memset(ctx, 0, sizeof *ctx);
    ctx->is_dir = 1;
    ctx->fidx = (size_t)-1;
    wcscpy_s(ctx->dir, UD_PATH_CCH, t);
    Dfi->Context = (ULONG64)(ULONG_PTR)ctx;
    Dfi->IsDirectory = 1;
    return 0;
}

static int DOKAN_CALLBACK ud_dokan_CreateDirectory(LPCWSTR FileName, PDOKAN_FILE_INFO Dfi)
{
    (void)FileName;
    (void)Dfi;
    return -ERROR_ACCESS_DENIED;
}

static int DOKAN_CALLBACK ud_dokan_Cleanup(LPCWSTR FileName, PDOKAN_FILE_INFO Dfi)
{
    (void)FileName;
    (void)Dfi;
    return 0;
}

static int DOKAN_CALLBACK ud_dokan_CloseFile(LPCWSTR FileName, PDOKAN_FILE_INFO Dfi)
{
    (void)FileName;
    if (Dfi->Context) {
        free((void *)(ULONG_PTR)Dfi->Context);
        Dfi->Context = 0;
    }
    return 0;
}

static int DOKAN_CALLBACK ud_dokan_ReadFile(LPCWSTR FileName, LPVOID Buffer, DWORD NumberOfBytesToRead,
    LPDWORD NumberOfBytesRead, LONGLONG Offset, PDOKAN_FILE_INFO Dfi)
{
    UdFs *U = (UdFs *)(ULONG_PTR)Dfi->DokanOptions->GlobalContext;
    UdDokanCtx *c = (UdDokanCtx *)(ULONG_PTR)Dfi->Context;

    (void)FileName;
    if (!NumberOfBytesRead)
        return -ERROR_INVALID_PARAMETER;
    *NumberOfBytesRead = 0;
    if (!c || c->is_dir)
        return -ERROR_INVALID_PARAMETER;
    {
        UdFile *f = &U->files[c->fidx];
        UINT64 off = (UINT64)Offset;

        if (off >= f->data_size)
            return 0;
        if ((UINT64)NumberOfBytesToRead > f->data_size - off)
            NumberOfBytesToRead = (DWORD)(f->data_size - off);
        if (!ud_read_bytes(U, f->data_start_sector, off, Buffer, NumberOfBytesToRead))
            return -ERROR_READ_FAULT;
        *NumberOfBytesRead = NumberOfBytesToRead;
    }
    return 0;
}

static int DOKAN_CALLBACK ud_dokan_WriteFile(LPCWSTR a, LPCVOID b, DWORD c, LPDWORD d, LONGLONG e, PDOKAN_FILE_INFO f)
{
    (void)a;
    (void)b;
    (void)c;
    (void)d;
    (void)e;
    (void)f;
    return -ERROR_ACCESS_DENIED;
}

static int DOKAN_CALLBACK ud_dokan_FlushFileBuffers(LPCWSTR a, PDOKAN_FILE_INFO b)
{
    (void)a;
    (void)b;
    return 0;
}

static int DOKAN_CALLBACK ud_dokan_GetFileInformation(LPCWSTR FileName, LPBY_HANDLE_FILE_INFORMATION Buf,
    PDOKAN_FILE_INFO Dfi)
{
    UdFs *U = (UdFs *)(ULONG_PTR)Dfi->DokanOptions->GlobalContext;
    UdDokanCtx *c = (UdDokanCtx *)(ULONG_PTR)Dfi->Context;

    (void)FileName;
    memset(Buf, 0, sizeof *Buf);
    if (!c)
        return -ERROR_INVALID_PARAMETER;
    if (c->is_dir) {
        Buf->dwFileAttributes = FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_READONLY;
        return 0;
    }
    Buf->dwFileAttributes = FILE_ATTRIBUTE_READONLY;
    Buf->nFileSizeHigh = (DWORD)(U->files[c->fidx].data_size >> 32);
    Buf->nFileSizeLow = (DWORD)U->files[c->fidx].data_size;
    ud_time_to_filetime(U->files[c->fidx].mtime, &Buf->ftLastWriteTime);
    return 0;
}

static int DOKAN_CALLBACK ud_dokan_FindFiles(LPCWSTR PathName, PFillFindData FillFindData, PDOKAN_FILE_INFO Dfi)
{
    UdFs *U = (UdFs *)(ULONG_PTR)Dfi->DokanOptions->GlobalContext;
    UdDokanCtx *c = (UdDokanCtx *)(ULONG_PTR)Dfi->Context;
    WIN32_FIND_DATAW fd;
    wchar_t t[UD_PATH_CCH];
    ud_ent *ents = NULL;
    int nent = 0, ei;

    (void)PathName;
    if (!U || !c || !c->is_dir || !FillFindData)
        return -ERROR_INVALID_PARAMETER;
    memset(&fd, 0, sizeof fd);
    fd.cFileName[0] = L'.';
    fd.cFileName[1] = 0;
    fd.dwFileAttributes = FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_READONLY;
    FillFindData(&fd, Dfi);
    fd.cFileName[0] = L'.';
    fd.cFileName[1] = L'.';
    fd.cFileName[2] = 0;
    FillFindData(&fd, Dfi);

    wcscpy_s(t, UD_PATH_CCH, c->dir);
    ud_norm_open(t);
    if (ud_collect_children(U, t, &ents, &nent) < 0)
        return -ERROR_NOT_ENOUGH_MEMORY;
    for (ei = 0; ei < nent; ei++) {
        memset(&fd, 0, sizeof fd);
        wcsncpy_s(fd.cFileName, sizeof(fd.cFileName) / sizeof(WCHAR), ents[ei].nm, _TRUNCATE);
        if (ents[ei].is_file) {
            fd.dwFileAttributes = FILE_ATTRIBUTE_READONLY;
            fd.nFileSizeHigh = (DWORD)(U->files[ents[ei].fidx].data_size >> 32);
            fd.nFileSizeLow = (DWORD)U->files[ents[ei].fidx].data_size;
            ud_time_to_filetime(U->files[ents[ei].fidx].mtime, &fd.ftLastWriteTime);
        } else
            fd.dwFileAttributes = FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_READONLY;
        FillFindData(&fd, Dfi);
    }
    free(ents);
    return 0;
}

static int DOKAN_CALLBACK ud_dokan_SetFileAttributes(LPCWSTR a, DWORD b, PDOKAN_FILE_INFO c)
{
    (void)a;
    (void)b;
    (void)c;
    return -ERROR_ACCESS_DENIED;
}

static int DOKAN_CALLBACK ud_dokan_SetFileTime(LPCWSTR a, CONST FILETIME *b, CONST FILETIME *c, CONST FILETIME *d,
    PDOKAN_FILE_INFO e)
{
    (void)a;
    (void)b;
    (void)c;
    (void)d;
    (void)e;
    return -ERROR_ACCESS_DENIED;
}

static int DOKAN_CALLBACK ud_dokan_DeleteFile(LPCWSTR a, PDOKAN_FILE_INFO b)
{
    (void)a;
    (void)b;
    return -ERROR_ACCESS_DENIED;
}

static int DOKAN_CALLBACK ud_dokan_DeleteDirectory(LPCWSTR a, PDOKAN_FILE_INFO b)
{
    (void)a;
    (void)b;
    return -ERROR_ACCESS_DENIED;
}

static int DOKAN_CALLBACK ud_dokan_MoveFile(LPCWSTR a, LPCWSTR b, BOOL c, PDOKAN_FILE_INFO d)
{
    (void)a;
    (void)b;
    (void)c;
    (void)d;
    return -ERROR_ACCESS_DENIED;
}

static int DOKAN_CALLBACK ud_dokan_SetEndOfFile(LPCWSTR a, LONGLONG b, PDOKAN_FILE_INFO c)
{
    (void)a;
    (void)b;
    (void)c;
    return -ERROR_ACCESS_DENIED;
}

static int DOKAN_CALLBACK ud_dokan_SetAllocationSize(LPCWSTR a, LONGLONG b, PDOKAN_FILE_INFO c)
{
    (void)a;
    (void)b;
    (void)c;
    return -ERROR_ACCESS_DENIED;
}

static int DOKAN_CALLBACK ud_dokan_LockFile(LPCWSTR a, LONGLONG b, LONGLONG c, PDOKAN_FILE_INFO d)
{
    (void)a;
    (void)b;
    (void)c;
    (void)d;
    return 0;
}

static int DOKAN_CALLBACK ud_dokan_UnlockFile(LPCWSTR a, LONGLONG b, LONGLONG c, PDOKAN_FILE_INFO d)
{
    (void)a;
    (void)b;
    (void)c;
    (void)d;
    return 0;
}

static int DOKAN_CALLBACK ud_dokan_GetDiskFreeSpace(PULONGLONG FreeBytesAvailable, PULONGLONG TotalNumberOfBytes,
    PULONGLONG TotalNumberOfFreeBytes, PDOKAN_FILE_INFO Dfi)
{
    (void)Dfi;
    if (FreeBytesAvailable)
        *FreeBytesAvailable = 1ULL << 30;
    if (TotalNumberOfBytes)
        *TotalNumberOfBytes = 1ULL << 31;
    if (TotalNumberOfFreeBytes)
        *TotalNumberOfFreeBytes = 0;
    return 0;
}

static int DOKAN_CALLBACK ud_dokan_GetVolumeInformation(LPWSTR VolumeNameBuffer, DWORD VolumeNameSize,
    LPDWORD VolumeSerialNumber, LPDWORD MaximumComponentLength, LPDWORD FileSystemFlags,
    LPWSTR FileSystemNameBuffer, DWORD FileSystemNameSize, PDOKAN_FILE_INFO Dfi)
{
    UdFs *U = (UdFs *)(ULONG_PTR)Dfi->DokanOptions->GlobalContext;

    if (VolumeSerialNumber)
        *VolumeSerialNumber = 0;
    if (MaximumComponentLength)
        *MaximumComponentLength = 255;
    if (FileSystemFlags)
        *FileSystemFlags =
            FILE_CASE_PRESERVED_NAMES | FILE_UNICODE_ON_DISK | FILE_READ_ONLY_VOLUME;
    if (VolumeNameBuffer && VolumeNameSize)
        wcsncpy_s(VolumeNameBuffer, VolumeNameSize, U->volume_label, _TRUNCATE);
    if (FileSystemNameBuffer && FileSystemNameSize)
        wcscpy_s(FileSystemNameBuffer, FileSystemNameSize, L"NTFS");
    return 0;
}

static int DOKAN_CALLBACK ud_dokan_Unmount(PDOKAN_FILE_INFO Dfi)
{
    (void)Dfi;
    return 0;
}

static int DOKAN_CALLBACK ud_dokan_GetFileSecurity(LPCWSTR FileName, PSECURITY_INFORMATION SecInfo,
    PSECURITY_DESCRIPTOR SecurityDescriptor, ULONG Length, PULONG LengthNeeded, PDOKAN_FILE_INFO Dfi)
{
    UdFs *U = (UdFs *)(ULONG_PTR)Dfi->DokanOptions->GlobalContext;
    const BYTE *sd = U->default_sd;
    SIZE_T sdlen = U->default_sd_len;

    (void)FileName;
    (void)SecInfo;
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

static int DOKAN_CALLBACK ud_dokan_SetFileSecurity(LPCWSTR a, PSECURITY_INFORMATION b, PSECURITY_DESCRIPTOR c,
    ULONG d, PDOKAN_FILE_INFO e)
{
    (void)a;
    (void)b;
    (void)c;
    (void)d;
    (void)e;
    return -ERROR_ACCESS_DENIED;
}

static int ud_try_mount_dokan(UdFs *U, wchar_t *mountPath, int be)
{
    HMODULE h;
    DOKAN_OPTIONS opt;
    DOKAN_OPERATIONS ops;
    int r;

    if (be == 2 /* WINFSP */)
        return 0;
    h = LoadLibraryW(L"dokan.dll");
    if (!h) {
        if (be == 1 /* DOKAN */)
            return -1;
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
        g_pfn_DokanMain = 0;
        g_DokanRemoveMountPoint = 0;
        FreeLibrary(h);
        return be == 1 ? -1 : 0;
    }
    if (!dokan_try_portable_stack_setup()) {
        g_pfn_DokanMain = 0;
        g_DokanRemoveMountPoint = 0;
        FreeLibrary(h);
        return be == 1 ? -1 : 0;
    }
    if (!ensure_mount_point_dokan(mountPath)) {
        g_pfn_DokanMain = 0;
        g_DokanRemoveMountPoint = 0;
        FreeLibrary(h);
        return -1;
    }
    memset(&ops, 0, sizeof ops);
    ops.CreateFile = ud_dokan_CreateFile;
    ops.OpenDirectory = ud_dokan_OpenDirectory;
    ops.CreateDirectory = ud_dokan_CreateDirectory;
    ops.Cleanup = ud_dokan_Cleanup;
    ops.CloseFile = ud_dokan_CloseFile;
    ops.ReadFile = ud_dokan_ReadFile;
    ops.WriteFile = ud_dokan_WriteFile;
    ops.FlushFileBuffers = ud_dokan_FlushFileBuffers;
    ops.GetFileInformation = ud_dokan_GetFileInformation;
    ops.FindFiles = ud_dokan_FindFiles;
    ops.SetFileAttributes = ud_dokan_SetFileAttributes;
    ops.SetFileTime = ud_dokan_SetFileTime;
    ops.DeleteFile = ud_dokan_DeleteFile;
    ops.DeleteDirectory = ud_dokan_DeleteDirectory;
    ops.MoveFile = ud_dokan_MoveFile;
    ops.SetEndOfFile = ud_dokan_SetEndOfFile;
    ops.SetAllocationSize = ud_dokan_SetAllocationSize;
    ops.LockFile = ud_dokan_LockFile;
    ops.UnlockFile = ud_dokan_UnlockFile;
    ops.GetDiskFreeSpace = ud_dokan_GetDiskFreeSpace;
    ops.GetVolumeInformation = ud_dokan_GetVolumeInformation;
    ops.Unmount = ud_dokan_Unmount;
    ops.GetFileSecurity = ud_dokan_GetFileSecurity;
    ops.SetFileSecurity = ud_dokan_SetFileSecurity;
    memset(&opt, 0, sizeof opt);
    opt.Version = DOKAN_VERSION;
    opt.ThreadCount = 4;
    opt.Options = 0;
    opt.GlobalContext = (ULONG64)(ULONG_PTR)U;
    {
        wchar_t mp_norm[MAX_PATH];
        DWORD gpl = GetFullPathNameW(mountPath, MAX_PATH, mp_norm, NULL);

        if (gpl == 0 || gpl >= MAX_PATH)
            wcscpy_s(mp_norm, MAX_PATH, mountPath);
        ud_fwdslashes_to_backslash(mp_norm);
        ud_strip_bs(mp_norm);
        opt.MountPoint = mp_norm;
        wcscpy_s(g_dokanMountPath, MAX_PATH, mp_norm);
    }
    SetConsoleCtrlHandler(wim_console_ctrl, TRUE);
    InterlockedExchange(&g_dokanMounted, 1);
    wprintf(L"Mounted UD (Dokan) at\n  %ls\nPress Ctrl+C to unmount.\n", g_dokanMountPath);
    r = g_pfn_DokanMain(&opt, &ops);
    InterlockedExchange(&g_dokanMounted, 0);
    g_pfn_DokanMain = 0;
    g_DokanRemoveMountPoint = 0;
    FreeLibrary(h);
    if (r == DOKAN_SUCCESS) {
        wprintf(L"Dokan unmounted.\n");
        return 1;
    }
    fwprintf(stderr, L"DokanMain failed (%d)%ls\n", r, (be == 0) ? L"; falling back to WinFsp" : L"");
    if (be == 0) {
        wchar_t normRm[32768];

        if (GetFullPathNameW(mountPath, 32768, normRm, NULL))
            RemoveDirectoryW(normRm);
        else
            RemoveDirectoryW(mountPath);
    }
    return be == 1 ? -1 : 0;
}
#endif /* UD_TRY_DOKAN */

int ud_mount_main(const wchar_t *ud_source, wchar_t *mount_path, int mount_backend)
{
    UdFs Ustack, *U = &Ustack;
    WCHAR err[512];
    NTSTATUS st;
    FSP_FSCTL_VOLUME_PARAMS vp;
    PSECURITY_DESCRIPTOR pSD = 0;
    HANDLE ev = NULL;

    memset(U, 0, sizeof *U);
    U->h = INVALID_HANDLE_VALUE;
    err[0] = 0;
    if (!ud_load(U, ud_source, err, 512)) {
        fwprintf(stderr, L"UD: %ls\n", err);
        return 0;
    }
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            L"O:BAG:BAD:P(A;;FA;;;SY)(A;;FA;;;BA)(A;;FA;;;WD)",
            SDDL_REVISION_1, &pSD, 0)) {
        fwprintf(stderr, L"UD: default SD failed %u\n", (unsigned)GetLastError());
        ud_free(U);
        return 0;
    }
    U->default_sd_len = GetSecurityDescriptorLength(pSD);
    if (U->default_sd_len > sizeof U->default_sd) {
        fwprintf(stderr, L"UD: default SD too large\n");
        LocalFree(pSD);
        ud_free(U);
        return 0;
    }
    memcpy(U->default_sd, pSD, U->default_sd_len);
    LocalFree(pSD);

    g_Ud = U;
    ud_fill_ifs();

    {
        wchar_t mount_abs[UD_PATH_CCH];

        if (!ud_abs_mount_path(mount_path, mount_abs, UD_PATH_CCH)) {
            fwprintf(stderr, L"UD: invalid or too long mount directory path.\n");
            g_Ud = NULL;
            ud_free(U);
            return 0;
        }

#if UD_TRY_DOKAN
        if (mount_backend != 2) {
            int dr = ud_try_mount_dokan(U, mount_abs, mount_backend);

            if (dr == 1) {
                g_Ud = NULL;
                ud_free(U);
                return 1;
            }
            if (dr < 0) {
                g_Ud = NULL;
                ud_free(U);
                return 0;
            }
        }
#endif

#if UD_TRY_DOKAN
        if (mount_backend != 1) {
            if (!verify_mount_point_winfsp(mount_abs)) {
                g_Ud = NULL;
                ud_free(U);
                return 0;
            }
        }
#else
        if (!verify_mount_point_winfsp(mount_abs)) {
            g_Ud = NULL;
            ud_free(U);
            return 0;
        }
#endif

        if (!wim_mount_winfsp_stack_prepare()) {
            g_Ud = NULL;
            ud_free(U);
            return 0;
        }
        if (!wim_mount_preflight(mount_abs)) {
            g_Ud = NULL;
            ud_free(U);
            return 0;
        }

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

        st = FspFileSystemCreate(L"" FSP_FSCTL_DISK_DEVICE_NAME, &vp, &g_UdIfs, &g_UdFs);
        if (!NT_SUCCESS(st)) {
            fwprintf(stderr, L"UD: FspFileSystemCreate 0x%08X\n", (unsigned)st);
            g_Ud = NULL;
            ud_free(U);
            return 0;
        }
        g_UdFs->UserContext = U;
        st = FspFileSystemSetMountPoint(g_UdFs, mount_abs);
        if (!NT_SUCCESS(st)) {
            fwprintf(stderr, L"UD: SetMountPoint 0x%08X\n", (unsigned)st);
            FspFileSystemDelete(g_UdFs);
            g_UdFs = NULL;
            g_Ud = NULL;
            ud_free(U);
            return 0;
        }
        ev = CreateEventW(NULL, TRUE, FALSE, NULL);
        if (!ev) {
            g_UdFs->UserContext = NULL;
            FspFileSystemRemoveMountPoint(g_UdFs);
            Sleep(50);
            FspFileSystemDelete(g_UdFs);
            g_UdFs = NULL;
            g_Ud = NULL;
            ud_free(U);
            return 0;
        }
        /* Reuse main.c globals for Ctrl+C path */
        extern HANDLE g_ExitEvent;
        extern FSP_FILE_SYSTEM *g_FsInstance;

        g_ExitEvent = ev;
        g_FsInstance = g_UdFs;
        SetConsoleCtrlHandler(wim_console_ctrl, TRUE);
        st = FspFileSystemStartDispatcher(g_UdFs, 0);
        if (!NT_SUCCESS(st)) {
            fwprintf(stderr, L"UD: StartDispatcher 0x%08X\n", (unsigned)st);
            g_ExitEvent = NULL;
            g_FsInstance = NULL;
            CloseHandle(ev);
            FspFileSystemStopDispatcher(g_UdFs);
            g_UdFs->UserContext = NULL;
            FspFileSystemRemoveMountPoint(g_UdFs);
            Sleep(50);
            FspFileSystemDelete(g_UdFs);
            g_UdFs = NULL;
            g_Ud = NULL;
            ud_free(U);
            return 0;
        }
        wprintf(L"Mounted UD onto\n  %ls\n(WinFsp) Press Ctrl+C to unmount.\n", mount_abs);
        WaitForSingleObject(ev, INFINITE);
        g_ExitEvent = NULL;
        g_FsInstance = NULL;
        CloseHandle(ev);
        FspFileSystemStopDispatcher(g_UdFs);
        g_UdFs->UserContext = NULL;
        FspFileSystemRemoveMountPoint(g_UdFs);
        Sleep(100);
        FspFileSystemDelete(g_UdFs);
        g_UdFs = g_FsInstance = NULL;
        g_Ud = NULL;
        ud_free(U);
        return 1;
    }
}
