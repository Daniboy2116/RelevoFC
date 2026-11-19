// Explorer side of an elevated run: starts RelevoFC.exe through UAC and keeps the progress window
// in Explorer in charge, so the whole operation stays in one window.
#define _WIN32_WINNT 0x0A00
#include <windows.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <stdio.h>
#include "elevate.h"

extern HINSTANCE gInst;

static void PutStr(BYTE **p, PCWSTR s)
{
    DWORD len = s ? (DWORD)wcslen(s) : 0xFFFFFFFF;
    memcpy(*p, &len, 4);
    *p += 4;
    if (s) {
        memcpy(*p, s, (len + 1) * sizeof(WCHAR));
        *p += (len + 1) * sizeof(WCHAR);
    }
}

// A new unnamed mapping: a zeroed FcShared, then the items. NULL on failure.
static HANDLE SerializeItems(const FcItem *items, int n)
{
    size_t size = sizeof(FcShared) + 4;
    for (int i = 0; i < n; i++)
        size += 16 + (wcslen(items[i].src) + wcslen(items[i].dstDir) + 2 +
                      (items[i].newName ? wcslen(items[i].newName) + 1 : 0)) * sizeof(WCHAR);
    HANDLE map = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, (DWORD)((ULONGLONG)size >> 32), (DWORD)size, NULL);
    BYTE *view = map ? MapViewOfFile(map, FILE_MAP_WRITE, 0, 0, size) : NULL, *p = view + sizeof(FcShared);
    if (!view) {
        if (map)
            CloseHandle(map);
        return NULL;
    }
    ((FcShared *)view)->answer = -1;
    DWORD count = n;
    memcpy(p, &count, 4);
    p += 4;
    for (int i = 0; i < n; i++) {
        DWORD move = items[i].move;
        memcpy(p, &move, 4);
        p += 4;
        PutStr(&p, items[i].src);
        PutStr(&p, items[i].dstDir);
        PutStr(&p, items[i].newName);
    }
    UnmapViewOfFile(view);
    return map;
}

static void Mirror(FcShared *sh, FcProgress *prog)
{
    prog->bytesTotal = sh->bytesTotal;
    prog->bytesDone = sh->bytesDone;
    prog->filesTotal = sh->filesTotal;
    prog->filesDone = sh->filesDone;
    prog->filesFailed = sh->filesFailed;
    prog->phase = sh->phase;
    AcquireSRWLockExclusive(&prog->lock);
    memcpy(prog->current, sh->current, sizeof(prog->current));
    prog->current[MAX_PATH - 1] = 0;
    ReleaseSRWLockExclusive(&prog->lock);
    sh->cancel = prog->cancel;
    sh->paused = prog->paused;
}

// Asks the helper's conflict question in our window; the file lists come in a mapping it gave us.
static LONG AnswerConflict(FcShared *sh, FcProgress *prog)
{
    HANDLE map = (HANDLE)(ULONG_PTR)sh->list;
    BYTE *view = map ? MapViewOfFile(map, FILE_MAP_WRITE, 0, 0, 0) : NULL;
    MEMORY_BASIC_INFORMATION mbi;
    LONG count = sh->count;
    PCWSTR *srcs = NULL, *dsts = NULL;
    // The lists only power "Elegir archivos…"; without them the question still works for all files.
    if (view && VirtualQuery(view, &mbi, sizeof(mbi)) && count > 0 && *(DWORD *)view == (DWORD)count &&
        (srcs = calloc(count, sizeof(PCWSTR))) && (dsts = calloc(count, sizeof(PCWSTR)))) {
        PCWSTR s = (PCWSTR)(view + 4 + count * sizeof(LONG)), end = (PCWSTR)(view + mbi.RegionSize);
        for (LONG i = 0; i < 2 * count && s < end; i++) {
            (i % 2 ? dsts : srcs)[i / 2] = s;
            s += wcsnlen(s, end - s) + 1;
        }
        if (s > end || !dsts[count - 1]) {
            free(srcs), free(dsts);
            srcs = dsts = NULL;
        }
    }
    prog->conflictSrc = srcs;
    prog->conflictDst = dsts;
    prog->conflictPick = srcs ? (volatile LONG *)(view + 4) : NULL;  // picks land straight in the helper's copy
    sh->example[MAX_PATH - 1] = 0;
    FcConflictChoice c = prog->onConflict(prog, count, sh->example);
    BOOL lists = srcs != NULL;
    prog->conflictSrc = prog->conflictDst = NULL;
    prog->conflictPick = NULL;
    free(srcs);
    free(dsts);
    if (view)
        UnmapViewOfFile(view);
    if (map)
        CloseHandle(map);
    sh->list = 0;
    return c == FC_CONFLICT_EACH && !lists ? FC_CONFLICT_BOTH : c;
}

static LONG AnswerError(FcShared *sh, FcProgress *prog)
{
    sh->failed[_countof(sh->failed) - 1] = 0;
    wcscpy_s(prog->failed, _countof(prog->failed), sh->failed);
    prog->errorHalted = sh->errorHalted;
    return prog->onError(prog, sh->count, prog->failed) ? 1 : 0;
}

FcResult RunElevated(const FcItem *items, int n, FcProgress *prog)
{
    WCHAR exe[MAX_PATH], args[64];
    GetModuleFileNameW(gInst, exe, MAX_PATH);
    PathRemoveFileSpecW(exe);
    PathAppendW(exe, L"RelevoFC.exe");
    if (GetFileAttributesW(exe) == INVALID_FILE_ATTRIBUTES) {
        FcLog(L"no %s, native", exe);
        return FC_FALLBACK;
    }
    HANDLE map = SerializeItems(items, n);
    FcShared *sh = map ? MapViewOfFile(map, FILE_MAP_WRITE, 0, 0, sizeof(FcShared)) : NULL;
    if (!sh) {
        if (map)
            CloseHandle(map);
        return FC_FALLBACK;
    }
    // The helper duplicates this handle out of our process, so no named object can be hijacked.
    swprintf_s(args, 64, L"%lu %llx", GetCurrentProcessId(), (ULONGLONG)(ULONG_PTR)map);
    SHELLEXECUTEINFOW sei = { sizeof(sei) };
    sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
    sei.hwnd = GetForegroundWindow();  // our window, where the user just pressed Continuar
#ifndef FC_ELEVATE_VERB
#define FC_ELEVATE_VERB L"runas"  // a test harness passes L"open" to check the channel without UAC
#endif
    sei.lpVerb = FC_ELEVATE_VERB;
    sei.lpFile = exe;
    sei.lpParameters = args;
    sei.nShow = SW_HIDE;
    FcResult r = FC_FALLBACK;
    if (!ShellExecuteExW(&sei) || !sei.hProcess) {
        DWORD e = GetLastError();
        FcLog(L"elevation not started (%lu)", e);
        if (e == ERROR_CANCELLED)
            r = FC_CANCELLED;  // the user said no on the UAC prompt
    } else {
        while (WaitForSingleObject(sei.hProcess, 50) == WAIT_TIMEOUT) {
            Mirror(sh, prog);
            if (sh->ask != FC_ASK_NONE && sh->answer < 0)
                InterlockedExchange(&sh->answer, sh->ask == FC_ASK_CONFLICT ? AnswerConflict(sh, prog) : AnswerError(sh, prog));
        }
        Mirror(sh, prog);
        sh->failed[_countof(sh->failed) - 1] = 0;
        wcscpy_s(prog->failed, _countof(prog->failed), sh->failed);
        DWORD code = FC_ERROR;
        GetExitCodeProcess(sei.hProcess, &code);
        CloseHandle(sei.hProcess);
        // Denied even elevated (TrustedInstaller), or a crash/loader failure: Windows does it instead.
        r = code <= FC_CANCELLED ? (FcResult)code : FC_FALLBACK;
        if (code != (DWORD)r)
            FcLog(L"RelevoFC.exe ended with 0x%08lX, native", code);
    }
    if (sh->list)  // a question left open by a helper that died
        CloseHandle((HANDLE)(ULONG_PTR)sh->list);
    UnmapViewOfFile(sh);
    CloseHandle(map);
    return r;
}
