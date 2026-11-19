// RelevoFC.exe: started elevated (UAC) from the progress window in Explorer when a source or
// destination needs admin rights. Usage: RelevoFC.exe <explorer pid> <mapping handle, hex>.
// The mapping (see elevate.h) holds the items and a live FcShared channel: we run the engine with
// no window of our own, publish progress and questions there, and exit with the FcResult.
#define _WIN32_WINNT 0x0A00
#include <windows.h>
#include <stdlib.h>
#include "../hook/elevate.h"

static FcShared *gSh;
static HANDLE gExplorer;  // PROCESS_DUP_HANDLE | SYNCHRONIZE
static volatile LONG gDone;

// Over-the-shoulder elevation runs as another account, which needs SeDebug to open the user's Explorer.
static void EnableDebugPrivilege(void)
{
    HANDLE tok;
    TOKEN_PRIVILEGES tp = { 1 };
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES, &tok))
        return;
    if (LookupPrivilegeValueW(NULL, L"SeDebugPrivilege", &tp.Privileges[0].Luid)) {
        tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
        AdjustTokenPrivileges(tok, FALSE, &tp, 0, NULL, NULL);
    }
    CloseHandle(tok);
}

static BOOL ExplorerGone(void) { return WaitForSingleObject(gExplorer, 0) != WAIT_TIMEOUT; }

static void Publish(FcProgress *prog)
{
    gSh->bytesTotal = prog->bytesTotal;
    gSh->bytesDone = prog->bytesDone;
    gSh->filesTotal = prog->filesTotal;
    gSh->filesDone = prog->filesDone;
    gSh->filesFailed = prog->filesFailed;
    gSh->phase = prog->phase;
    AcquireSRWLockShared(&prog->lock);
    memcpy(gSh->current, prog->current, sizeof(gSh->current));
    ReleaseSRWLockShared(&prog->lock);
    // Explorer gone (restarted, crashed): nobody can answer or watch, so stop as on a cancel.
    prog->cancel = gSh->cancel || ExplorerGone();
    prog->paused = gSh->paused && !prog->cancel;
}

static DWORD WINAPI Pump(LPVOID arg)
{
    while (!gDone) {
        Publish(arg);
        Sleep(50);
    }
    return 0;
}

// Puts a question in the channel and waits for Explorer's answer (-1: cancelled or Explorer gone).
static LONG Ask(FcProgress *prog, LONG kind, LONG count)
{
    gSh->count = count;
    gSh->errorHalted = prog->errorHalted;
    gSh->answer = -1;
    MemoryBarrier();
    gSh->ask = kind;
    while (gSh->answer < 0 && !gSh->cancel && !ExplorerGone())
        Sleep(30);
    LONG a = gSh->answer;
    gSh->ask = FC_ASK_NONE;
    return a;
}

static FcConflictChoice RemoteConflict(FcProgress *prog, int count, PCWSTR example)
{
    wcsncpy_s(gSh->example, MAX_PATH, example, _TRUNCATE);
    // The file lists go in their own mapping, handed to Explorer, for "Elegir archivos…".
    size_t size = 4 + count * sizeof(LONG);
    for (int i = 0; i < count; i++)
        size += (wcslen(prog->conflictSrc[i]) + wcslen(prog->conflictDst[i]) + 2) * sizeof(WCHAR);
    HANDLE map = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, (DWORD)((ULONGLONG)size >> 32), (DWORD)size, NULL);
    BYTE *view = map ? MapViewOfFile(map, FILE_MAP_WRITE, 0, 0, size) : NULL;
    HANDLE remote = NULL;
    if (view) {
        *(DWORD *)view = count;
        LONG *picks = (LONG *)(view + 4);
        PWSTR s = (PWSTR)(picks + count);
        for (int i = 0; i < count; i++) {
            picks[i] = prog->conflictPick[i];
            for (int k = 0; k < 2; k++) {
                PCWSTR from = k ? prog->conflictDst[i] : prog->conflictSrc[i];
                size_t len = wcslen(from) + 1;
                memcpy(s, from, len * sizeof(WCHAR));
                s += len;
            }
        }
        DuplicateHandle(GetCurrentProcess(), map, gExplorer, &remote, 0, FALSE, DUPLICATE_SAME_ACCESS);
    }
    gSh->list = (ULONG_PTR)remote;
    LONG a = Ask(prog, FC_ASK_CONFLICT, count);
    if (a == FC_CONFLICT_EACH && view)
        for (int i = 0; i < count; i++)
            prog->conflictPick[i] = ((LONG *)(view + 4))[i];  // the engine treats unknown values as keep both
    if (view)
        UnmapViewOfFile(view);
    if (map)
        CloseHandle(map);
    return a >= FC_CONFLICT_REPLACE && a <= FC_CONFLICT_EACH && (a != FC_CONFLICT_EACH || view) ? (FcConflictChoice)a : FC_CONFLICT_CANCEL;
}

static BOOL RemoteError(FcProgress *prog, int count, PCWSTR list)
{
    wcsncpy_s(gSh->failed, _countof(gSh->failed), list, _TRUNCATE);
    return Ask(prog, FC_ASK_ERROR, count) == 1;
}

// Item layout: see elevate.h.
static BOOL ReadStr(const BYTE **p, const BYTE *end, PWSTR *out)
{
    DWORD len;
    if (end - *p < 4)
        return FALSE;
    memcpy(&len, *p, 4);
    *p += 4;
    if (len == 0xFFFFFFFF) {
        *out = NULL;
        return TRUE;
    }
    size_t bytes = ((size_t)len + 1) * sizeof(WCHAR);
    if (len > 32767 || (size_t)(end - *p) < bytes || ((PCWSTR)*p)[len] != 0)
        return FALSE;
    *out = (PWSTR)*p;
    *p += bytes;
    return TRUE;
}

static int Parse(const BYTE *buf, size_t size, FcItem **items)
{
    const BYTE *p = buf, *end = buf + size;
    DWORD n, move;
    if (size < 4)
        return -1;
    memcpy(&n, p, 4);
    p += 4;
    if (!n || n > 1000000 || !(*items = calloc(n, sizeof(FcItem))))
        return -1;
    for (DWORD i = 0; i < n; i++) {
        if (end - p < 4)
            return -1;
        memcpy(&move, p, 4);
        p += 4;
        (*items)[i].move = move != 0;
        if (!ReadStr(&p, end, &(*items)[i].src) || !ReadStr(&p, end, &(*items)[i].dstDir) ||
            !ReadStr(&p, end, &(*items)[i].newName) || !(*items)[i].src || !(*items)[i].dstDir)
            return -1;
    }
    return (int)n;
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmd, int show)
{
    (void)inst, (void)prev, (void)show;
    PWSTR next;
    DWORD pid = wcstoul(cmd, &next, 10);
    HANDLE remote = (HANDLE)(ULONG_PTR)_wcstoui64(next, NULL, 16), map = NULL;
    EnableDebugPrivilege();
    gExplorer = pid ? OpenProcess(PROCESS_DUP_HANDLE | SYNCHRONIZE, FALSE, pid) : NULL;
    if (!gExplorer || !DuplicateHandle(gExplorer, remote, GetCurrentProcess(), &map, FILE_MAP_WRITE, FALSE, 0)) {
        FcLog(L"admin: cannot get the item list from pid=%lu (%lu)", pid, GetLastError());
        return FC_FALLBACK;
    }
    BYTE *view = MapViewOfFile(map, FILE_MAP_WRITE, 0, 0, 0);
    MEMORY_BASIC_INFORMATION mbi;
    FcItem *items = NULL;
    BYTE *buf = NULL;
    int n = -1;
    // Private copy of the items first: Explorer could still change the shared view while we parse it.
    if (view && VirtualQuery(view, &mbi, sizeof(mbi)) && mbi.RegionSize > sizeof(FcShared) &&
        (buf = malloc(mbi.RegionSize - sizeof(FcShared)))) {
        memcpy(buf, view + sizeof(FcShared), mbi.RegionSize - sizeof(FcShared));
        n = Parse(buf, mbi.RegionSize - sizeof(FcShared), &items);
    }
    if (n < 0) {
        FcLog(L"admin: bad item list");
        return FC_FALLBACK;
    }
    gSh = (FcShared *)view;
    FcLog(L"admin: elevated run, items=%d %s -> %s", n, items[0].src, items[0].dstDir);

    FcProgress prog = { 0 };
    InitializeSRWLock(&prog.lock);
    prog.onConflict = RemoteConflict;
    prog.onError = RemoteError;
    HANDLE pump = CreateThread(NULL, 0, Pump, &prog, 0, NULL);
    FcResult r = FcRun(items, n, &prog);
    gDone = 1;
    if (pump)
        WaitForSingleObject(pump, INFINITE);
    Publish(&prog);
    wcsncpy_s(gSh->failed, _countof(gSh->failed), prog.failed, _TRUNCATE);
    FcLog(L"admin: result=%d files=%ld/%ld failed=%ld", r, prog.filesDone, prog.filesTotal, prog.filesFailed);
    return r;
}
