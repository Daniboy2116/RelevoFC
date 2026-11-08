// Minimal copy/move engine. Plans everything first (no writes), so any unsupported case can
// fall back to Windows untouched; then executes: renames, folders, parallel file copies
// (moves verify each copy byte by byte), and only then deletes the sources of cross-volume moves.
#define _WIN32_WINNT 0x0A00
#include <windows.h>
#include <winioctl.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <wctype.h>
#include <stdio.h>
#include <stdlib.h>
#include "engine.h"

#define MAX_WORKERS 16
#define VERIFY_BLOCK (1024 * 1024)

// User settings, read at the start of every operation from %APPDATA%\RelevoFC\settings.ini
// (RELEVOFC_INI overrides the path; the tests use it). Missing file or key: the defaults.
//   [engine]  threads=8  big_file_mb=256  verify_copies=0
static int gWorkers = 8;
static ULONGLONG gBigFile = 256ull * 1024 * 1024;  // above this, copy without the system cache
static BOOL gVerifyCopies;                         // moves always verify; this adds copies
static int gOneBig = -1;                           // big files one at a time: -1 auto (SerialDevice), 0 never, 1 always
// Test seam: RELEVOFC_TEST_FAIL=N makes the Nth file of an operation fail as if its disk was unplugged.
static volatile LONG gTestFail;

static void LoadSettings(void)
{
    WCHAR ini[MAX_PATH];
    if (!GetEnvironmentVariableW(L"RELEVOFC_INI", ini, MAX_PATH)) {
        if (!GetEnvironmentVariableW(L"APPDATA", ini, MAX_PATH))
            ini[0] = 0;
        wcscat_s(ini, MAX_PATH, L"\\RelevoFC\\settings.ini");
    }
    gWorkers = GetPrivateProfileIntW(L"engine", L"threads", 8, ini);
    if (gWorkers < 1 || gWorkers > MAX_WORKERS)
        gWorkers = 8;
    int mb = GetPrivateProfileIntW(L"engine", L"big_file_mb", 256, ini);
    gBigFile = (ULONGLONG)(mb < 1 ? 256 : mb) * 1024 * 1024;
    gVerifyCopies = GetPrivateProfileIntW(L"engine", L"verify_copies", 0, ini) != 0;
    gOneBig = GetPrivateProfileIntW(L"engine", L"big_files_one_at_a_time", -1, ini);
    WCHAR fail[16];
    gTestFail = GetEnvironmentVariableW(L"RELEVOFC_TEST_FAIL", fail, 16) ? _wtoi(fail) : 0;
}

enum { J_RENAME, J_MKDIR, J_COPY, J_SKIP };

typedef struct {
    int kind;
    BOOL fromMove;
    BOOL done;  // copied (and verified, for moves)
    BOOL conflict, replace;  // J_COPY onto an existing file / overwrite it
    BOOL exists;             // J_MKDIR onto an existing folder (merge)
    PWSTR src, dst;
    ULONGLONG size;
    HRESULT hr;       // nonzero: the last attempt failed (FC_E_VERIFY: the copy differs from the source)
    LONG64 credited;  // bytes the failed attempt added to the progress, taken back on a retry
} Job;

#define FC_E_VERIFY ((HRESULT)0xE0000001)

typedef struct {
    Job *v;
    int n, cap;
    BOOL allow;  // conflicts can be resolved (a callback is set); otherwise they fall back
    int nconf;
    WCHAR firstConf[MAX_PATH];
} Plan;

// Paths this long (or longer) get the \\?\ prefix so Windows lifts the 260-character limit. The margin
// below MAX_PATH leaves room for "name (2)" and for folders, whose own limit is 248.
#define LONG_PATH 240

static BOOL IsExt(PCWSTR p)
{
    return !wcsncmp(p, L"\\\\?\\", 4);
}

static PCWSTR Plain(PCWSTR p)
{
    return IsExt(p) ? p + 4 : p;
}

static PWSTR Ext(PCWSTR p)
{
    size_t l = wcslen(p), cap = l + 5;
    PWSTR r = malloc(cap * sizeof(WCHAR));
    r[0] = 0;
    if (l >= LONG_PATH && !IsExt(p))
        wcscpy_s(r, cap, L"\\\\?\\");
    wcscat_s(r, cap, p);
    return r;
}

static PWSTR Join(PCWSTR a, PCWSTR b)
{
    size_t la = wcslen(a), cap = la + wcslen(b) + 2;
    PWSTR t = malloc(cap * sizeof(WCHAR));
    wcscpy_s(t, cap, a);
    if (la && a[la - 1] != L'\\')
        wcscat_s(t, cap, L"\\");
    wcscat_s(t, cap, b);
    PWSTR r = Ext(t);
    free(t);
    return r;
}

static BOOL Push(Plan *p, int kind, BOOL fromMove, PCWSTR src, PCWSTR dst, ULONGLONG size)
{
    if (wcslen(src) >= 32000 || wcslen(dst) >= 32000) {  // the \\?\ limit is 32767
        FcLog(L"fallback: path too long %s", src);
        return FALSE;
    }
    if (p->n == p->cap) {
        p->cap = p->cap ? p->cap * 2 : 64;
        p->v = realloc(p->v, p->cap * sizeof(Job));
    }
    Job *j = &p->v[p->n++];
    j->kind = kind;
    j->fromMove = fromMove;
    j->done = FALSE;
    j->hr = 0;
    j->credited = 0;
    j->conflict = j->replace = j->exists = FALSE;
    DWORD da = GetFileAttributesW(dst);
    if (da != INVALID_FILE_ATTRIBUTES) {
        BOOL dir = (da & FILE_ATTRIBUTE_DIRECTORY) != 0;
        if (!p->allow || (da & FILE_ATTRIBUTE_REPARSE_POINT) || dir != (kind == J_MKDIR) || kind == J_RENAME) {
            FcLog(L"fallback: target exists %s", dst);  // Windows shows its replace/skip dialog
            p->n--;
            return FALSE;
        }
        if (kind == J_MKDIR) {
            j->exists = TRUE;
        } else {
            j->conflict = TRUE;
            if (!p->nconf++)
                wcsncpy_s(p->firstConf, MAX_PATH, Plain(dst), _TRUNCATE);
        }
    }
    j->src = _wcsdup(src);
    j->dst = _wcsdup(dst);
    j->size = size;
    return TRUE;
}

static BOOL PlanDir(Plan *p, PCWSTR src, PCWSTR dst, BOOL move, FcProgress *prog)
{
    PWSTR pattern = Join(src, L"*");
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW(pattern, FindExInfoBasic, &fd, FindExSearchNameMatch, NULL, FIND_FIRST_EX_LARGE_FETCH);
    free(pattern);
    if (h == INVALID_HANDLE_VALUE) {
        FcLog(L"fallback: cannot list %s (%lu)", src, GetLastError());
        return FALSE;
    }
    BOOL ok = TRUE;
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L".."))
            continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
            FcLog(L"fallback: reparse point %s\\%s", src, fd.cFileName);
            ok = FALSE;
            break;
        }
        PWSTR s = Join(src, fd.cFileName), d = Join(dst, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            ok = Push(p, J_MKDIR, move, s, d, 0) && PlanDir(p, s, d, move, prog);
        else
            ok = Push(p, J_COPY, move, s, d, ((ULONGLONG)fd.nFileSizeHigh << 32) | fd.nFileSizeLow);
        free(s);
        free(d);
    } while (ok && !prog->cancel && FindNextFileW(h, &fd));
    FindClose(h);
    return ok;
}

static BOOL IsDrivePath(PCWSTR p)
{
    return iswalpha(p[0]) && p[1] == L':' && p[2] == L'\\';
}

static BOOL SameVolume(PCWSTR a, PCWSTR b)
{
    WCHAR va[MAX_PATH], vb[MAX_PATH];
    return GetVolumePathNameW(a, va, MAX_PATH) && GetVolumePathNameW(b, vb, MAX_PATH) &&
           !_wcsicmp(Plain(va), Plain(vb));
}

static BOOL PlanItem(Plan *p, const FcItem *it, FcProgress *prog)
{
    if (!IsDrivePath(it->src) || !IsDrivePath(it->dstDir)) {
        FcLog(L"fallback: not a local drive path %s -> %s", it->src, it->dstDir);
        return FALSE;
    }
    PCWSTR name = it->newName && *it->newName ? it->newName : PathFindFileNameW(it->src);
    PWSTR dst = Join(it->dstDir, name), src = Ext(it->src), dstDir = Ext(it->dstDir);
    DWORD attr = GetFileAttributesW(src);
    size_t ls = wcslen(Plain(src));
    BOOL ok = FALSE;

    if (attr == INVALID_FILE_ATTRIBUTES && GetFileAttributesW(dst) != INVALID_FILE_ATTRIBUTES) {
        FcLog(L"fallback: target exists %s", dst);
    } else if (!_wcsicmp(Plain(src), Plain(dst))) {
        FcLog(L"fallback: copy onto itself %s", dst);  // Windows makes "name - Copia"
    } else if (GetFileAttributesW(dst) != INVALID_FILE_ATTRIBUTES && it->move && SameVolume(src, dstDir)) {
        FcLog(L"fallback: target exists %s", dst);  // same-volume move onto an existing name
    } else if (attr == INVALID_FILE_ATTRIBUTES || (attr & FILE_ATTRIBUTE_REPARSE_POINT)) {
        FcLog(L"fallback: source missing or reparse point %s", src);
    } else if ((attr & FILE_ATTRIBUTE_DIRECTORY) && !_wcsnicmp(Plain(dst), Plain(src), ls) && Plain(dst)[ls] == L'\\') {
        FcLog(L"fallback: destination inside source %s", dst);
    } else if (it->move && SameVolume(src, dstDir)) {
        ok = Push(p, J_RENAME, TRUE, src, dst, 0);
    } else if (attr & FILE_ATTRIBUTE_DIRECTORY) {
        ok = Push(p, J_MKDIR, it->move, src, dst, 0) && PlanDir(p, src, dst, it->move, prog);
    } else {
        WIN32_FILE_ATTRIBUTE_DATA fa;
        ok = GetFileAttributesExW(src, GetFileExInfoStandard, &fa) &&
             Push(p, J_COPY, it->move, src, dst, ((ULONGLONG)fa.nFileSizeHigh << 32) | fa.nFileSizeLow);
    }
    free(dst);
    free(src);
    free(dstDir);
    return ok;
}

// "name.ext" -> "name (2).ext", the first one that is free.
static PWSTR UniqueName(PCWSTR path)
{
    PCWSTR ext = PathFindExtensionW(path);
    size_t base = ext - path;
    PWSTR r = malloc((wcslen(path) + 16) * sizeof(WCHAR));
    for (int n = 2;; n++) {
        swprintf_s(r, wcslen(path) + 16, L"%.*s (%d)%s", (int)base, path, n, ext);
        if (GetFileAttributesW(r) == INVALID_FILE_ATTRIBUTES)
            return r;
    }
}

// Applies the user's answer to every conflicting file. FALSE: the operation was cancelled.
static BOOL ResolveConflicts(Plan *p, FcProgress *prog)
{
    if (!p->nconf)
        return TRUE;
    PCWSTR *srcs = malloc(p->nconf * sizeof(PCWSTR)), *dsts = malloc(p->nconf * sizeof(PCWSTR));
    volatile LONG *picks = malloc(p->nconf * sizeof(LONG));
    int k = 0;
    for (int i = 0; i < p->n; i++)
        if (p->v[i].conflict) {
            srcs[k] = p->v[i].src;
            dsts[k] = p->v[i].dst;
            picks[k++] = FC_CONFLICT_BOTH;  // nothing is lost unless the answer says so
        }
    prog->conflictSrc = srcs;
    prog->conflictDst = dsts;
    prog->conflictPick = picks;
    FcConflictChoice c = prog->onConflict(prog, p->nconf, p->firstConf);
    prog->conflictSrc = prog->conflictDst = NULL;
    prog->conflictPick = NULL;
    BOOL cancelled = c == FC_CONFLICT_CANCEL;
    for (int i = 0, n = 0; i < p->n && !cancelled; i++) {
        Job *j = &p->v[i];
        if (!j->conflict)
            continue;
        FcConflictChoice cj = c == FC_CONFLICT_EACH ? (FcConflictChoice)picks[n++] : c;
        if (cj == FC_CONFLICT_SKIP) {
            j->kind = J_SKIP;
        } else if (cj == FC_CONFLICT_REPLACE) {
            j->replace = TRUE;
        } else {  // BOTH, or anything unexpected: keep both
            PWSTR u = UniqueName(j->dst);
            free(j->dst);
            j->dst = u;
            j->conflict = FALSE;
        }
    }
    free(srcs);
    free(dsts);
    free((void *)picks);
    return !cancelled;
}

// Blocks while paused; FALSE once cancelled.
static BOOL WaitIfPaused(FcProgress *prog)
{
    while (prog->paused && !prog->cancel)
        Sleep(100);
    return !prog->cancel;
}

static void SetCurrent(FcProgress *prog, PCWSTR path)
{
    AcquireSRWLockExclusive(&prog->lock);
    wcsncpy_s(prog->current, MAX_PATH, Plain(path), _TRUNCATE);
    ReleaseSRWLockExclusive(&prog->lock);
}

typedef struct {
    FcProgress *prog;
    ULONGLONG reported;
} ChunkCtx;

// Test aid: %TEMP%astcopy_slow holding N throttles every file stream to N MB/s, so the UI can be watched on fast disks.
static ULONGLONG gSlowBps;

static void LoadSlow(void)
{
    WCHAR p[MAX_PATH];
    char buf[16] = "";
    DWORD got = 0;
    gSlowBps = 0;
    GetTempPathW(MAX_PATH, p);
    wcscat_s(p, MAX_PATH, L"relevofc_slow");
    HANDLE h = CreateFileW(p, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return;
    ReadFile(h, buf, sizeof(buf) - 1, &got, NULL);
    CloseHandle(h);
    gSlowBps = (ULONGLONG)atoi(buf) * 1024 * 1024;
}

static COPYFILE2_MESSAGE_ACTION CALLBACK OnChunk(const COPYFILE2_MESSAGE *m, PVOID arg)
{
    ChunkCtx *cc = arg;
    if (m->Type == COPYFILE2_CALLBACK_CHUNK_FINISHED) {
        ULONGLONG total = m->Info.ChunkFinished.uliTotalBytesTransferred.QuadPart;
        ULONGLONG delta = total - cc->reported;
        InterlockedAdd64(&cc->prog->bytesDone, (LONG64)delta);
        cc->reported = total;
        if (gSlowBps)
            Sleep((DWORD)(delta * 1000 / gSlowBps));
    }
    return WaitIfPaused(cc->prog) ? COPYFILE2_PROGRESS_CONTINUE : COPYFILE2_PROGRESS_CANCEL;
}

// Byte-by-byte comparison, used before deleting the source of a move (and for verify_copies).
// Measured ways to speed it up, none worth it: reading both files at once with overlapped I/O was
// slower (2 GB: 2.45 s vs 1.86 s, it defeats Windows' read-ahead); buffers allocated once per worker
// changed nothing (the per-file cost on small files is opening two more files, not memory); hashing
// while copying would mean replacing CopyFile2 (attributes, streams, ReFS block cloning) to save
// only the source re-read, which is usually still cached.
// `read` gets the bytes it added to the progress, so a failed verify can be accounted for exactly.
static BOOL SameContent(PCWSTR a, PCWSTR b, FcProgress *prog, LONG64 *read)
{
    *read = 0;
    HANDLE ha = CreateFileW(a, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    HANDLE hb = CreateFileW(b, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    char *ba = malloc(VERIFY_BLOCK), *bb = malloc(VERIFY_BLOCK);
    BOOL same = ha != INVALID_HANDLE_VALUE && hb != INVALID_HANDLE_VALUE && ba && bb;
    DWORD ra = 0, rb = 0;
    while (same && WaitIfPaused(prog)) {
        same = ReadFile(ha, ba, VERIFY_BLOCK, &ra, NULL) && ReadFile(hb, bb, VERIFY_BLOCK, &rb, NULL) &&
               ra == rb && !memcmp(ba, bb, ra);
        InterlockedAdd64(&prog->bytesDone, ra);
        *read += ra;
        if (!ra)
            break;
    }
    if (prog->cancel)
        same = FALSE;
    free(ba);
    free(bb);
    if (ha != INVALID_HANDLE_VALUE)
        CloseHandle(ha);
    if (hb != INVALID_HANDLE_VALUE)
        CloseHandle(hb);
    return same;
}

static BOOL ForceDelete(PCWSTR path, BOOL dir);

typedef struct {
    Job *v;
    int *idx;
    LONG n;
    FcProgress *prog;
    volatile LONG next;
    volatile LONG failed;
    volatile LONG halt;  // a device-level error: start no more files, every one would fail the same way
    HANDLE bigLane;      // semaphore of 1 on a sequential device (see SerialDevice); NULL otherwise
} CopyCtx;

// Files this big go one at a time on a sequential device. Measured on a USB 3.0 stick: 10 MB files ran
// 35-45 % faster one at a time than 8 at once, 4 MB files ~25 % faster; 1 MB and 64 KB files ran
// 25 % to 2.5x faster with 8 at once. NVMe to NVMe, and a SATA SSD over USB, gain from parallelism
// at every size (the USB SSD ran 10 MB files ~20 % slower one at a time).
#define SERIAL_BIG (2ull * 1024 * 1024)

// Spinning disks (seek penalty), and USB / SD / FireWire storage without TRIM: flash sticks, memory
// cards and external hard disks. An SSD in a USB enclosure reports TRIM and stays parallel.
static BOOL SerialDevice(PCWSTR path)
{
    WCHAR vol[MAX_PATH], dev[16];
    if (!GetVolumePathNameW(path, vol, MAX_PATH))
        return FALSE;
    PCWSTR v = Plain(vol);
    if (!iswalpha(v[0]) || v[1] != L':')
        return FALSE;
    swprintf_s(dev, 16, L"\\\\.\\%c:", v[0]);
    HANDLE h = CreateFileW(dev, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);  // no access: query only
    if (h == INVALID_HANDLE_VALUE)
        return FALSE;
    STORAGE_PROPERTY_QUERY q = { StorageDeviceSeekPenaltyProperty, PropertyStandardQuery };
    DEVICE_SEEK_PENALTY_DESCRIPTOR sp = { 0 };
    DWORD got;
    BOOL serial = DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &q, sizeof(q), &sp, sizeof(sp), &got, NULL) && sp.IncursSeekPenalty;
    BYTE buf[512];
    q.PropertyId = StorageDeviceProperty;
    if (!serial && DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &q, sizeof(q), buf, sizeof(buf), &got, NULL) &&
        got >= sizeof(STORAGE_DEVICE_DESCRIPTOR)) {
        STORAGE_BUS_TYPE bus = ((STORAGE_DEVICE_DESCRIPTOR *)buf)->BusType;
        if (bus == BusTypeUsb || bus == BusTypeSd || bus == BusTypeMmc || bus == BusType1394) {
            DEVICE_TRIM_DESCRIPTOR trim = { 0 };
            q.PropertyId = StorageDeviceTrimProperty;
            serial = !(DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &q, sizeof(q), &trim, sizeof(trim), &got, NULL) && trim.TrimEnabled);
        }
    }
    CloseHandle(h);
    return serial;
}

// Errors that are about the disk or connection rather than this one file (unplugged, asleep, full,
// network gone): the rest would fail too, so the operation stops and waits for the user.
static BOOL DeviceGone(HRESULT hr)
{
    if (HRESULT_FACILITY(hr) != FACILITY_WIN32)
        return FALSE;
    switch (HRESULT_CODE(hr)) {
    case ERROR_DEVICE_NOT_CONNECTED: case ERROR_NOT_READY: case ERROR_NO_SUCH_DEVICE: case ERROR_DEV_NOT_EXIST:
    case ERROR_DEVICE_REMOVED: case ERROR_INVALID_DRIVE: case ERROR_PATH_NOT_FOUND: case ERROR_IO_DEVICE:
    case ERROR_DISK_FULL: case ERROR_HANDLE_DISK_FULL: case ERROR_OPERATION_ABORTED: case ERROR_SEM_TIMEOUT:
    case ERROR_BAD_NETPATH: case ERROR_NETNAME_DELETED: case ERROR_UNEXP_NET_ERR: case ERROR_NETWORK_UNREACHABLE:
    case ERROR_CRC: case ERROR_GEN_FAILURE: case ERROR_WRONG_DISK:  // WRONG_DISK: seen on a real USB pulled mid-copy
        return TRUE;
    }
    return FALSE;
}

static DWORD WINAPI CopyWorker(LPVOID arg)
{
    CopyCtx *c = arg;
    for (;;) {
        LONG i = InterlockedIncrement(&c->next) - 1;
        if (i >= c->n || c->prog->cancel || c->halt)
            return 0;
        Job *j = &c->v[c->idx[i]];
        SetCurrent(c->prog, j->src);

        ChunkCtx cc = { c->prog, 0 };
        COPYFILE2_EXTENDED_PARAMETERS ep = { sizeof(ep) };
        if (j->replace)
            SetFileAttributesW(j->dst, FILE_ATTRIBUTE_NORMAL);  // a read-only target would refuse the overwrite
        ep.dwCopyFlags = (j->replace ? 0 : COPY_FILE_FAIL_IF_EXISTS) | (j->size >= gBigFile ? COPY_FILE_NO_BUFFERING : 0);
        ep.pProgressRoutine = OnChunk;
        ep.pvCallbackContext = &cc;
        BOOL lane = c->bigLane && j->size >= SERIAL_BIG;
        if (lane)
            WaitForSingleObject(c->bigLane, INFINITE);
        HRESULT hr = gTestFail > 0 && InterlockedDecrement(&gTestFail) == 0 ? HRESULT_FROM_WIN32(ERROR_DEVICE_NOT_CONNECTED)
                                                                              : CopyFile2(j->src, j->dst, &ep);
        if (lane)
            ReleaseSemaphore(c->bigLane, 1, NULL);
        if (FAILED(hr)) {
            if (!c->prog->cancel) {
                FcLog(L"copy failed hr=0x%08lX %s", hr, j->src);
                InterlockedIncrement(&c->failed);
                if (DeviceGone(hr))
                    c->halt = 1;
                j->hr = hr;
                // `credited` is everything this file has put into the progress, so a retry takes back exactly that.
                // One bad file counts as handled so the bar still reaches the end; after a device error the
                // operation stops, and the bar shows only finished files: steady however often it is retried
                // and however many files were in flight when the disk went away.
                LONG64 whole = (LONG64)j->size * (j->fromMove || gVerifyCopies ? 2 : 1);
                j->credited = c->halt ? 0 : whole;
                InterlockedAdd64(&c->prog->bytesDone, j->credited - (LONG64)cc.reported);
            }
            continue;
        }
        if (cc.reported < j->size)  // small files may finish without a chunk callback
            InterlockedAdd64(&c->prog->bytesDone, (LONG64)(j->size - cc.reported));

        LONG64 verified = 0;
        if ((j->fromMove || gVerifyCopies) && !SameContent(j->src, j->dst, c->prog, &verified)) {
            if (!c->prog->cancel) {
                FcLog(L"verify failed, keeping source %s", j->src);
                InterlockedIncrement(&c->failed);
                j->hr = FC_E_VERIFY;
                j->credited = 2 * (LONG64)j->size;  // copy + verify, counted whole like any failed file
                InterlockedAdd64(&c->prog->bytesDone, (LONG64)j->size - verified);
                ForceDelete(j->dst, FALSE);  // a bad copy is worse than none
            }
            continue;
        }
        j->done = TRUE;
        InterlockedIncrement(&c->prog->filesDone);
    }
}

// Copies every job that is not done yet (all of them the first time, only the failed ones on a retry).
static BOOL RunCopies(Plan *p, FcProgress *prog, BOOL retry, BOOL serial)
{
    int ncopy = 0;
    for (int i = 0; i < p->n; i++) {
        Job *j = &p->v[i];
        if (j->kind != J_COPY || j->done)
            continue;
        ncopy++;
        // On a retry a pending target that exists now did not at planning time (that would have been a
        // conflict): it is a partial copy of ours, cut off when the disk went away. Checked here, not when
        // the user was asked, because the disk may only have come back in between.
        if (retry && GetFileAttributesW(j->dst) != INVALID_FILE_ATTRIBUTES)
            j->replace = TRUE;
    }
    CopyCtx c = { p->v, malloc(ncopy * sizeof(int)), ncopy, prog, 0, 0, 0, serial ? CreateSemaphoreW(NULL, 1, 1, NULL) : NULL };
    for (int i = 0, k = 0; i < p->n; i++)
        if (p->v[i].kind == J_COPY && !p->v[i].done)
            c.idx[k++] = i;

    HANDLE th[MAX_WORKERS];
    int nt = 0;
    while (nt < gWorkers && nt < ncopy && (th[nt] = CreateThread(NULL, 0, CopyWorker, &c, 0, NULL)))
        nt++;
    if (nt)
        WaitForMultipleObjects(nt, th, TRUE, INFINITE);
    else
        CopyWorker(&c);
    for (int t = 0; t < nt; t++)
        CloseHandle(th[t]);
    if (c.bigLane)
        CloseHandle(c.bigLane);
    free(c.idx);
    prog->errorHalted = c.halt;
    return c.failed == 0;
}

// What Windows says about the error, in the user's language.
static void Reason(HRESULT hr, PWSTR out, int cap)
{
    if (hr == FC_E_VERIFY) {
        wcsncpy_s(out, cap, L"La copia no coincide con el original.", _TRUNCATE);
        return;
    }
    DWORD code = HRESULT_FACILITY(hr) == FACILITY_WIN32 ? HRESULT_CODE(hr) : (DWORD)hr;
    if (!FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, code, 0, out, cap, NULL))
        swprintf_s(out, cap, L"Error 0x%08lX", (unsigned long)hr);
    for (int n = (int)wcslen(out); n > 0 && wcschr(L"\r\n ", out[n - 1]); )
        out[--n] = 0;
}

// "path", newline, indented reason, per failed file, as many as fit.
static int FailedText(Plan *p, PWSTR out, int cap)
{
    int count = 0;
    out[0] = 0;
    for (int i = 0; i < p->n; i++) {
        Job *j = &p->v[i];
        if (j->kind != J_COPY || !j->hr)
            continue;
        count++;
        WCHAR why[256];
        Reason(j->hr, why, 256);
        if (wcslen(out) + wcslen(Plain(j->src)) + wcslen(why) + 8 < (size_t)cap)
            swprintf_s(out + wcslen(out), cap - wcslen(out), L"%s\n   %s\n", Plain(j->src), why);
    }
    return count;
}

// Asks whether to retry the failed files; on yes, takes back what they added to the progress.
static BOOL AskRetry(Plan *p, FcProgress *prog)
{
    WCHAR text[2048];
    int count = FailedText(p, text, _countof(text));
    wcscpy_s(prog->failed, _countof(prog->failed), text);  // the window reads the question from here
    if (!prog->onError || !prog->onError(prog, count, text))
        return FALSE;
    for (int i = 0; i < p->n; i++)
        if (p->v[i].kind == J_COPY && p->v[i].hr) {
            InterlockedAdd64(&prog->bytesDone, -p->v[i].credited);
            p->v[i].hr = 0;
            p->v[i].credited = 0;
        }
    return TRUE;
}

static BOOL ForceDelete(PCWSTR path, BOOL dir)
{
    SetFileAttributesW(path, FILE_ATTRIBUTE_NORMAL);  // read-only sources would otherwise refuse
    return dir ? RemoveDirectoryW(path) : DeleteFileW(path);
}

// Removes the sources of verified moved files, then the source folders left empty.
static BOOL DeleteMovedSources(Plan *p, BOOL quietDirs)
{
    BOOL ok = TRUE;
    for (int i = 0; i < p->n; i++)
        if (p->v[i].kind == J_COPY && p->v[i].fromMove && p->v[i].done && !ForceDelete(p->v[i].src, FALSE)) {
            FcLog(L"delete source failed (%lu) %s", GetLastError(), p->v[i].src);
            ok = FALSE;
        }
    for (int i = p->n - 1; i >= 0; i--)
        if (p->v[i].kind == J_MKDIR && p->v[i].fromMove && !ForceDelete(p->v[i].src, TRUE) && !quietDirs) {
            FcLog(L"remove source dir failed (%lu) %s", GetLastError(), p->v[i].src);
            ok = FALSE;
        }
    return ok;
}

static void Notify(const FcItem *items, int n)
{
    for (int i = 0; i < n; i++) {
        if (i == 0 || _wcsicmp(items[i].dstDir, items[i - 1].dstDir))
            SHChangeNotify(SHCNE_UPDATEDIR, SHCNF_PATHW | SHCNF_FLUSHNOWAIT, items[i].dstDir, NULL);
        if (items[i].move && wcslen(items[i].src) < MAX_PATH) {
            WCHAR parent[MAX_PATH];
            wcscpy_s(parent, MAX_PATH, items[i].src);
            PathRemoveFileSpecW(parent);
            SHChangeNotify(SHCNE_UPDATEDIR, SHCNF_PATHW | SHCNF_FLUSHNOWAIT, parent, NULL);
        }
    }
}

FcResult FcRun(const FcItem *items, int n, FcProgress *prog)
{
    Plan p = { 0 };
    FcResult res = FC_FALLBACK;
    ULONGLONG bytes = 0;
    int i, ncopy = 0;

    LoadSlow();
    LoadSettings();
    p.allow = prog->onConflict != NULL;
    prog->phase = FC_PHASE_PLAN;
    for (i = 0; i < n; i++) {
        BOOL ok = PlanItem(&p, &items[i], prog);
        if (prog->cancel) {
            res = FC_CANCELLED;
            goto out;
        }
        if (!ok)
            goto out;
    }
    if (!ResolveConflicts(&p, prog)) {
        res = FC_CANCELLED;
        goto out;
    }
    for (i = 0; i < p.n; i++)
        if (p.v[i].kind == J_COPY) {
            bytes += p.v[i].size;
            ncopy++;
        }
    // shortcut: assumes all items share one destination volume (true for Explorer drops/pastes).
    ULARGE_INTEGER freeBytes;
    PWSTR dstDirExt = Ext(items[0].dstDir);
    BOOL noSpace = bytes && (!GetDiskFreeSpaceExW(dstDirExt, &freeBytes, NULL, NULL) || freeBytes.QuadPart < bytes);
    free(dstDirExt);
    if (noSpace) {
        FcLog(L"fallback: not enough free space for %llu bytes", bytes);
        goto out;
    }
    LONG64 total = 0;
    for (i = 0; i < p.n; i++)
        if (p.v[i].kind == J_COPY)
            total += p.v[i].fromMove || gVerifyCopies ? 2 * p.v[i].size : p.v[i].size;
    prog->bytesTotal = total;
    prog->filesTotal = ncopy;
    prog->phase = FC_PHASE_COPY;

    // From here on we write: failures are FC_ERROR and move sources stay in place.
    res = FC_ERROR;
    for (i = 0; i < p.n; i++) {
        Job *j = &p.v[i];
        if (prog->cancel) {
            res = FC_CANCELLED;
            goto out;
        }
        if (j->kind == J_RENAME && !MoveFileExW(j->src, j->dst, 0)) {
            FcLog(L"rename failed (%lu) %s", GetLastError(), j->src);
            goto out;
        }
        if (j->kind == J_MKDIR && !j->exists && !CreateDirectoryExW(j->src, j->dst, NULL)) {
            FcLog(L"mkdir failed (%lu) %s", GetLastError(), j->dst);
            goto out;
        }
    }
    // Sequential source or target: big files one at a time (small ones stay parallel).
    BOOL serial = gOneBig >= 0 ? gOneBig : SerialDevice(items[0].src) || SerialDevice(items[0].dstDir);
    if (serial)
        FcLog(L"sequential device: files of 2 MB or more one at a time");
    BOOL copied = TRUE;
    for (int round = 0; ncopy && !(copied = RunCopies(&p, prog, round > 0, serial)) && !prog->cancel && AskRetry(&p, prog); round++)
        ;
    prog->phase = FC_PHASE_FINISH;
    if (prog->cancel) {
        // A move cancelled mid-verify leaves a full but unverified copy: drop it, the source is intact.
        for (i = 0; i < p.n; i++)
            if (p.v[i].kind == J_COPY && p.v[i].fromMove && !p.v[i].done)
                ForceDelete(p.v[i].dst, FALSE);
        DeleteMovedSources(&p, TRUE);  // only verified files; folders still holding the rest stay
        res = FC_CANCELLED;
        goto out;
    }
    if (!copied) {
        for (i = 0; i < p.n; i++)  // files a halt never got to are not done either: report them
            if (p.v[i].kind == J_COPY && !p.v[i].done && !p.v[i].hr)
                p.v[i].hr = HRESULT_FROM_WIN32(ERROR_CANCELLED);
        prog->filesFailed = FailedText(&p, prog->failed, _countof(prog->failed));
        DeleteMovedSources(&p, TRUE);  // the files that did make it; the failed ones and their folders stay
        goto out;
    }
    res = DeleteMovedSources(&p, FALSE) ? FC_OK : FC_ERROR;

out:
    if (res != FC_FALLBACK)
        Notify(items, n);
    for (i = 0; i < p.n; i++) {
        free(p.v[i].src);
        free(p.v[i].dst);
    }
    free(p.v);
    return res;
}
