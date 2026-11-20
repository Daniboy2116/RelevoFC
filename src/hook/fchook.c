// Injected into explorer.exe. Records what Explorer queues on IFileOperation and, at
// PerformOperations, runs our engine instead of Windows' copy. Anything we don't fully
// understand falls back to the original PerformOperations untouched.
// Kill switch: create %TEMP%\relevofc_off to make every operation native.
#define _WIN32_WINNT 0x0A00
#define CINTERFACE
#define COBJMACROS
#include <windows.h>
#include <stdio.h>
#include <shobjidl.h>
#include <shlobj.h>
#include "MinHook.h"
#include "ui.h"

HINSTANCE gInst;

typedef struct {
    IFileOperation *op;
    FcItem *items;
    int n, cap;
    BOOL unsupported;
} OpEntry;

#define MAX_OPS 64
static OpEntry gOps[MAX_OPS];
static SRWLOCK gOpsLock = SRWLOCK_INIT;

// SetOperationFlags of the last few operations (a ring, so ops that are never performed can't fill it).
static struct {
    IFileOperation *op;
    DWORD flags;
} gFlags[16];
static int gFlagsNext;  // guarded by gOpsLock, like gFlags

// Caller holds gOpsLock.
static OpEntry *FindEntry(IFileOperation *op, BOOL create)
{
    OpEntry *empty = NULL;
    for (int i = 0; i < MAX_OPS; i++) {
        if (gOps[i].op == op)
            return &gOps[i];
        if (!gOps[i].op && !empty)
            empty = &gOps[i];
    }
    if (create && empty)
        empty->op = op;
    return create ? empty : NULL;
}

static void FreeEntry(OpEntry *e)
{
    for (int i = 0; i < e->n; i++) {
        free(e->items[i].src);
        free(e->items[i].dstDir);
        free(e->items[i].newName);
    }
    free(e->items);
    ZeroMemory(e, sizeof(*e));
}

static void MarkUnsupported(IFileOperation *op, PCWSTR why)
{
    AcquireSRWLockExclusive(&gOpsLock);
    OpEntry *e = FindEntry(op, TRUE);
    if (e)
        e->unsupported = TRUE;
    ReleaseSRWLockExclusive(&gOpsLock);
    FcLog(L"op=%p unsupported: %s", op, why);
}

static PWSTR FsPath(IShellItem *si)
{
    PWSTR p = NULL;
    if (!si || FAILED(IShellItem_GetDisplayName(si, SIGDN_FILESYSPATH, &p)))
        return NULL;
    PWSTR r = _wcsdup(p);
    CoTaskMemFree(p);
    return r;
}

static void AddItem(IFileOperation *op, BOOL move, IShellItem *src, IShellItem *dest, LPCWSTR newName)
{
    PWSTR s = FsPath(src), d = FsPath(dest);
    if (!s || !d) {
        free(s);
        free(d);
        MarkUnsupported(op, L"item is not a file system path");
        return;
    }
    AcquireSRWLockExclusive(&gOpsLock);
    OpEntry *e = FindEntry(op, TRUE);
    if (e) {
        if (e->n == e->cap) {
            e->cap = e->cap ? e->cap * 2 : 16;
            e->items = realloc(e->items, e->cap * sizeof(FcItem));
        }
        FcItem it = { move, s, d, newName ? _wcsdup(newName) : NULL };
        e->items[e->n++] = it;
    }
    ReleaseSRWLockExclusive(&gOpsLock);
    if (!e) {
        free(s);
        free(d);
        FcLog(L"op table full, op=%p stays native", op);
    }
}

// Explorer passes IPersistIDList (one call per item); other callers may pass arrays or data objects.
static void AddItems(IFileOperation *op, BOOL move, IUnknown *punk, IShellItem *dest)
{
    IPersistIDList *pl = NULL;
    IShellItem *one = NULL;
    IShellItemArray *arr = NULL;
    IDataObject *dobj = NULL;

    if (SUCCEEDED(IUnknown_QueryInterface(punk, &IID_IPersistIDList, (void **)&pl))) {
        PIDLIST_ABSOLUTE id = NULL;
        if (SUCCEEDED(IPersistIDList_GetIDList(pl, &id)) &&
            SUCCEEDED(SHCreateItemFromIDList(id, &IID_IShellItem, (void **)&one))) {
            AddItem(op, move, one, dest, NULL);
            IShellItem_Release(one);
        } else {
            MarkUnsupported(op, L"IPersistIDList without item");
        }
        CoTaskMemFree(id);
        IPersistIDList_Release(pl);
        return;
    }
    if (SUCCEEDED(IUnknown_QueryInterface(punk, &IID_IShellItem, (void **)&one))) {
        AddItem(op, move, one, dest, NULL);
        IShellItem_Release(one);
        return;
    }
    if (FAILED(IUnknown_QueryInterface(punk, &IID_IShellItemArray, (void **)&arr)) &&
        SUCCEEDED(IUnknown_QueryInterface(punk, &IID_IDataObject, (void **)&dobj))) {
        SHCreateShellItemArrayFromDataObject(dobj, &IID_IShellItemArray, (void **)&arr);
        IDataObject_Release(dobj);
    }
    if (!arr) {
        MarkUnsupported(op, L"unknown item container");
        return;
    }
    DWORD count = 0;
    IShellItemArray_GetCount(arr, &count);
    for (DWORD i = 0; i < count; i++) {
        if (SUCCEEDED(IShellItemArray_GetItemAt(arr, i, &one))) {
            AddItem(op, move, one, dest, NULL);
            IShellItem_Release(one);
        }
    }
    IShellItemArray_Release(arr);
}

static BOOL Disabled(void)
{
    WCHAR p[MAX_PATH];
    GetTempPathW(MAX_PATH, p);
    wcscat_s(p, MAX_PATH, L"relevofc_off");
    return GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES;
}

typedef HRESULT (STDMETHODCALLTYPE *ItemsDest_t)(IFileOperation *, IUnknown *, IShellItem *);
typedef HRESULT (STDMETHODCALLTYPE *ItemDest_t)(IFileOperation *, IShellItem *, IShellItem *, LPCWSTR, IFileOperationProgressSink *);
typedef HRESULT (STDMETHODCALLTYPE *NoArgs_t)(IFileOperation *);
typedef HRESULT (STDMETHODCALLTYPE *SetFlags_t)(IFileOperation *, DWORD);
typedef HRESULT (STDMETHODCALLTYPE *OneItem_t)(IFileOperation *, IShellItem *);
typedef HRESULT (STDMETHODCALLTYPE *OneUnk_t)(IFileOperation *, IUnknown *);
typedef HRESULT (STDMETHODCALLTYPE *RenameItem_t)(IFileOperation *, IShellItem *, LPCWSTR, IFileOperationProgressSink *);
typedef HRESULT (STDMETHODCALLTYPE *RenameItems_t)(IFileOperation *, IUnknown *, LPCWSTR);
typedef HRESULT (STDMETHODCALLTYPE *DeleteItem_t)(IFileOperation *, IShellItem *, IFileOperationProgressSink *);
typedef HRESULT (STDMETHODCALLTYPE *NewItem_t)(IFileOperation *, IShellItem *, DWORD, LPCWSTR, LPCWSTR, IFileOperationProgressSink *);

static ItemsDest_t oCopyItems, oMoveItems;
static ItemDest_t oCopyItem, oMoveItem;
static NoArgs_t oPerform;
static SetFlags_t oSetFlags;
static OneItem_t oApplyPropsItem;
static OneUnk_t oApplyPropsItems, oDeleteItems;
static RenameItem_t oRenameItem;
static RenameItems_t oRenameItems;
static DeleteItem_t oDeleteItem;
static NewItem_t oNewItem;

// ---- Ctrl+Z ----
// Windows keeps an undo list of what it did; it never sees what RelevoFC did. gHist mirrors both, newest
// last: our entries carry what to take back, NULL items stand for one entry on Windows' list. Ctrl+Z in a
// folder view undoes ours when it is the newest and otherwise lets Windows undo its own.
#define FOFX_UNDO_OP 0x20000000  // seen on the operations Windows' undo/redo performs (not in the SDK)
typedef struct {
    FcUndoItem *items;
    int n;
} Done;
#define MAX_HIST 32
static Done gHist[MAX_HIST];
static int gHistN;
static SRWLOCK gHistLock = SRWLOCK_INIT;
static volatile ULONGLONG gRedoKeyAt;  // last Ctrl+Y in a folder view: the next Windows undo op is a redo

static void HistPush(FcUndoItem *items, int n)
{
    AcquireSRWLockExclusive(&gHistLock);
    if (gHistN == MAX_HIST) {
        FcFreeUndo(gHist[0].items, gHist[0].n);
        memmove(gHist, gHist + 1, (MAX_HIST - 1) * sizeof(Done));
        gHistN--;
    }
    gHist[gHistN].items = items;
    gHist[gHistN++].n = n;
    ReleaseSRWLockExclusive(&gHistLock);
}

// Windows undid one of its own (a Ctrl+Z we let through, or its Undo menu): drop its newest entry.
static void HistNativeUndone(void)
{
    AcquireSRWLockExclusive(&gHistLock);
    for (int i = gHistN - 1; i >= 0; i--)
        if (!gHist[i].items) {
            memmove(&gHist[i], &gHist[i + 1], (gHistN - i - 1) * sizeof(Done));
            gHistN--;
            break;
        }
    ReleaseSRWLockExclusive(&gHistLock);
}

typedef struct {
    Done d;
    HWND owner;
} UndoJob;

// Moves go back through our own engine (MoveItem is hooked); copies go to the Recycle Bin through Windows.
// Neither sets FOF_ALLOWUNDO: taking something back does not go on an undo list itself.
static DWORD WINAPI UndoThread(LPVOID arg)
{
    UndoJob *u = arg;
    HRESULT hrCo = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    for (int copies = 0; copies < 2; copies++) {
        IFileOperation *op = NULL;
        int queued = 0;
        if (FAILED(CoCreateInstance(&CLSID_FileOperation, NULL, CLSCTX_INPROC_SERVER, &IID_IFileOperation, (void **)&op)))
            break;
        IFileOperation_SetOperationFlags(op, copies ? FOF_NOCONFIRMATION | FOFX_RECYCLEONDELETE : FOF_NOCONFIRMMKDIR);
        IFileOperation_SetOwnerWindow(op, u->owner);
        for (int i = 0; i < u->d.n; i++) {
            FcUndoItem *it = &u->d.items[i];
            IShellItem *si = NULL, *folder = NULL;
            if (!it->src != copies)
                continue;
            if (FAILED(SHCreateItemFromParsingName(it->dst, NULL, &IID_IShellItem, (void **)&si))) {
                FcLog(L"undo: no longer there %s", it->dst);
                continue;
            }
            if (copies) {
                queued += SUCCEEDED(IFileOperation_DeleteItem(op, si, NULL));
            } else {
                PWSTR dir = _wcsdup(it->src), slash = wcsrchr(dir, L'\\'), name = slash ? _wcsdup(slash + 1) : NULL;
                if (slash) {
                    slash[slash - dir == 2] = 0;  // "C:\x" keeps its root backslash
                    SHCreateDirectoryExW(NULL, dir, NULL);  // the move removed the source folders it emptied
                    if (SUCCEEDED(SHCreateItemFromParsingName(dir, NULL, &IID_IShellItem, (void **)&folder)))
                        queued += SUCCEEDED(IFileOperation_MoveItem(op, si, folder, name, NULL));
                }
                if (folder)
                    IShellItem_Release(folder);
                free(name);
                free(dir);
            }
            IShellItem_Release(si);
        }
        if (queued) {
            HRESULT hr = IFileOperation_PerformOperations(op);
            FcLog(L"undo: %s %d item(s) hr=0x%08lX", copies ? L"recycled" : L"moved back", queued, hr);
        }
        IFileOperation_Release(op);
    }
    FcFreeUndo(u->d.items, u->d.n);
    free(u);
    if (SUCCEEDED(hrCo))
        CoUninitialize();
    return 0;
}

// The file list of a folder window or the desktop (not the rename box, address bar or search).
static BOOL InFolderView(HWND h)
{
    WCHAR c[32], pc[32];
    HWND p = GetParent(h);
    return p && GetClassNameW(h, c, 32) && GetClassNameW(p, pc, 32) && !wcscmp(pc, L"SHELLDLL_DefView") &&
           (!wcscmp(c, L"DirectUIHWND") || !wcscmp(c, L"SysListView32"));
}

// Sees every message an Explorer thread takes from its queue, before accelerators handle it.
static void OnMessage(MSG *m)
{
    if (m->message != WM_KEYDOWN || (m->wParam != 'Z' && m->wParam != 'Y') || GetKeyState(VK_CONTROL) >= 0 ||
        GetKeyState(VK_SHIFT) < 0 || GetKeyState(VK_MENU) < 0 || !InFolderView(m->hwnd))
        return;
    if (m->wParam == 'Y') {
        gRedoKeyAt = GetTickCount64();
        return;
    }
    Done d = { 0 };
    AcquireSRWLockExclusive(&gHistLock);
    if (gHistN && gHist[gHistN - 1].items)
        d = gHist[--gHistN];
    ReleaseSRWLockExclusive(&gHistLock);
    if (!d.items)
        return;  // the newest is Windows': its own Ctrl+Z handles it
    UndoJob *u = malloc(sizeof(*u));
    u->d = d;
    u->owner = GetAncestor(m->hwnd, GA_ROOT);
    FcLog(L"undo: Ctrl+Z takes back %d item(s), first %s", d.n, d.n ? d.items[0].dst : L"");
    HANDLE th = CreateThread(NULL, 0, UndoThread, u, 0, NULL);
    if (th) {
        CloseHandle(th);
        m->message = WM_NULL;  // swallowed: Windows must not undo something of its own as well
    } else {
        FcFreeUndo(d.items, d.n);
        free(u);
    }
}

typedef BOOL (WINAPI *GetMsg_t)(LPMSG, HWND, UINT, UINT);
typedef BOOL (WINAPI *PeekMsg_t)(LPMSG, HWND, UINT, UINT, UINT);
static GetMsg_t oGetMsg;
static PeekMsg_t oPeekMsg;

static BOOL WINAPI hkGetMsg(LPMSG m, HWND h, UINT a, UINT b)
{
    BOOL r = oGetMsg(m, h, a, b);
    if (r > 0)
        OnMessage(m);
    return r;
}

static BOOL WINAPI hkPeekMsg(LPMSG m, HWND h, UINT a, UINT b, UINT rm)
{
    BOOL r = oPeekMsg(m, h, a, b, rm);
    if (r && (rm & PM_REMOVE))
        OnMessage(m);
    return r;
}

static HRESULT STDMETHODCALLTYPE hkSetFlags(IFileOperation *op, DWORD flags)
{
    // Explorer calls this first on every new operation. shortcut: also used to drop stale entries
    // of a released op whose address got reused; hook Release if another caller skips this.
    AcquireSRWLockExclusive(&gOpsLock);
    OpEntry *e = FindEntry(op, FALSE);
    if (e && !e->n && !e->unsupported)
        e = NULL;
    if (e)
        FreeEntry(e);
    for (int i = 0; i < _countof(gFlags); i++)  // an older op at the same address that never ran
        if (gFlags[i].op == op)
            gFlags[i].op = NULL;
    gFlags[gFlagsNext].op = op;
    gFlags[gFlagsNext].flags = flags;
    gFlagsNext = (gFlagsNext + 1) % _countof(gFlags);
    ReleaseSRWLockExclusive(&gOpsLock);
    return oSetFlags(op, flags);
}

static HRESULT STDMETHODCALLTYPE hkCopyItems(IFileOperation *op, IUnknown *items, IShellItem *dest)
{
    AddItems(op, FALSE, items, dest);
    return oCopyItems(op, items, dest);  // still queue it natively so fallback stays possible
}

static HRESULT STDMETHODCALLTYPE hkMoveItems(IFileOperation *op, IUnknown *items, IShellItem *dest)
{
    AddItems(op, TRUE, items, dest);
    return oMoveItems(op, items, dest);
}

static HRESULT STDMETHODCALLTYPE hkCopyItem(IFileOperation *op, IShellItem *item, IShellItem *dest, LPCWSTR newName, IFileOperationProgressSink *sink)
{
    if (sink)
        MarkUnsupported(op, L"per-item progress sink");
    else
        AddItem(op, FALSE, item, dest, newName);
    return oCopyItem(op, item, dest, newName, sink);
}

static HRESULT STDMETHODCALLTYPE hkMoveItem(IFileOperation *op, IShellItem *item, IShellItem *dest, LPCWSTR newName, IFileOperationProgressSink *sink)
{
    if (sink)
        MarkUnsupported(op, L"per-item progress sink");
    else
        AddItem(op, TRUE, item, dest, newName);
    return oMoveItem(op, item, dest, newName, sink);
}

// Any other queued action means we can't skip the native PerformOperations.
static HRESULT STDMETHODCALLTYPE hkApplyPropsItem(IFileOperation *op, IShellItem *i)
{
    MarkUnsupported(op, L"ApplyPropertiesToItem");
    return oApplyPropsItem(op, i);
}
static HRESULT STDMETHODCALLTYPE hkApplyPropsItems(IFileOperation *op, IUnknown *i)
{
    MarkUnsupported(op, L"ApplyPropertiesToItems");
    return oApplyPropsItems(op, i);
}
static HRESULT STDMETHODCALLTYPE hkRenameItem(IFileOperation *op, IShellItem *i, LPCWSTR n, IFileOperationProgressSink *s)
{
    MarkUnsupported(op, L"RenameItem");
    return oRenameItem(op, i, n, s);
}
static HRESULT STDMETHODCALLTYPE hkRenameItems(IFileOperation *op, IUnknown *i, LPCWSTR n)
{
    MarkUnsupported(op, L"RenameItems");
    return oRenameItems(op, i, n);
}
static HRESULT STDMETHODCALLTYPE hkDeleteItem(IFileOperation *op, IShellItem *i, IFileOperationProgressSink *s)
{
    MarkUnsupported(op, L"DeleteItem");
    return oDeleteItem(op, i, s);
}
static HRESULT STDMETHODCALLTYPE hkDeleteItems(IFileOperation *op, IUnknown *i)
{
    MarkUnsupported(op, L"DeleteItems");
    return oDeleteItems(op, i);
}
static HRESULT STDMETHODCALLTYPE hkNewItem(IFileOperation *op, IShellItem *d, DWORD a, LPCWSTR n, LPCWSTR t, IFileOperationProgressSink *s)
{
    MarkUnsupported(op, L"NewItem");
    return oNewItem(op, d, a, n, t, s);
}

static HRESULT STDMETHODCALLTYPE hkPerform(IFileOperation *op)
{
    OpEntry mine = { 0 };
    AcquireSRWLockExclusive(&gOpsLock);
    OpEntry *e = FindEntry(op, FALSE);
    if (e) {
        mine = *e;
        ZeroMemory(e, sizeof(*e));  // ownership of items moves to `mine`
    }
    DWORD flags = 0;
    for (int i = 0; i < _countof(gFlags); i++)
        if (gFlags[i].op == op) {
            flags = gFlags[i].flags;
            gFlags[i].op = NULL;
        }
    ReleaseSRWLockExclusive(&gOpsLock);

    ULONGLONG t0 = GetTickCount64();
    FcProgress prog = { 0 };
    prog.wantUndo = (flags & FOF_ALLOWUNDO) != 0;
    FcResult r = FC_FALLBACK;
    if (mine.n && !mine.unsupported) {
        if (Disabled())
            FcLog(L"op=%p kill switch on, native", op);
        else
            r = RunWithProgress(mine.items, mine.n, &prog);
    }
    int n = mine.n;
    FreeEntry(&mine);

    if (r == FC_FALLBACK) {
        HRESULT hr = oPerform(op);
        if (flags & FOFX_UNDO_OP) {
            if (GetTickCount64() - gRedoKeyAt < 3000)
                HistPush(NULL, 0);
            else
                HistNativeUndone();
        } else if ((flags & FOF_ALLOWUNDO) && SUCCEEDED(hr)) {
            HistPush(NULL, 0);
        }
        FcLog(L"op=%p NATIVE items=%d flags=0x%08lX hr=0x%08lX %llu ms", op, n, flags, hr, GetTickCount64() - t0);
        return hr;
    }
    if (r == FC_OK && prog.undo)
        HistPush(prog.undo, prog.nundo);
    else
        FcFreeUndo(prog.undo, prog.nundo);
    static const PCWSTR names[] = { L"OK", L"FALLBACK", L"ERROR", L"CANCELLED", L"NEEDS_ADMIN" };
    ULONGLONG ms = GetTickCount64() - t0;
    LONG64 bytes = prog.bytesDone;
    FcLog(L"op=%p RELEVOFC %s items=%d files=%ld/%ld bytes=%lld %llu ms (%.1f MB/s)", op, names[r], n,
          prog.filesDone, prog.filesTotal, bytes, ms, ms ? bytes / 1048576.0 / (ms / 1000.0) : 0.0);
    if (r == FC_ERROR && prog.filesFailed) {
        FcLog(L"op=%p %ld file(s) failed:\n%s", op, prog.filesFailed, prog.failed);
        return HRESULT_FROM_WIN32(ERROR_CANCELLED);  // the user already saw them and chose to skip: no second error from Explorer
    }
    return r == FC_OK ? S_OK : r == FC_CANCELLED ? HRESULT_FROM_WIN32(ERROR_CANCELLED) : E_FAIL;
}

static BOOL Hook(void *target, void *detour, void **orig, PCWSTR name)
{
    MH_STATUS s = MH_CreateHook(target, detour, orig);
    if (s != MH_OK)
        FcLog(L"MH_CreateHook(%s) failed: %d", name, s);
    return s == MH_OK;
}

static DWORD WINAPI InitThread(LPVOID unused)
{
    (void)unused;
    FcLog(L"fc_hook loaded into pid=%lu", GetCurrentProcessId());
    HRESULT hrCo = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    IFileOperation *op = NULL;
    HRESULT hr = CoCreateInstance(&CLSID_FileOperation, NULL, CLSCTX_INPROC_SERVER, &IID_IFileOperation, (void **)&op);
    if (FAILED(hr) || MH_Initialize() != MH_OK) {
        FcLog(L"init failed hr=0x%08lX, staying inactive", hr);
        goto done;
    }
    // Every FileOperation instance shares this vtable, so hooking the bodies covers Explorer's.
    IFileOperationVtbl *vt = op->lpVtbl;
    BOOL ok = Hook(vt->CopyItems, hkCopyItems, (void **)&oCopyItems, L"CopyItems")
            && Hook(vt->MoveItems, hkMoveItems, (void **)&oMoveItems, L"MoveItems")
            && Hook(vt->CopyItem, hkCopyItem, (void **)&oCopyItem, L"CopyItem")
            && Hook(vt->MoveItem, hkMoveItem, (void **)&oMoveItem, L"MoveItem")
            && Hook(vt->SetOperationFlags, hkSetFlags, (void **)&oSetFlags, L"SetOperationFlags")
            && Hook(vt->ApplyPropertiesToItem, hkApplyPropsItem, (void **)&oApplyPropsItem, L"ApplyPropertiesToItem")
            && Hook(vt->ApplyPropertiesToItems, hkApplyPropsItems, (void **)&oApplyPropsItems, L"ApplyPropertiesToItems")
            && Hook(vt->RenameItem, hkRenameItem, (void **)&oRenameItem, L"RenameItem")
            && Hook(vt->RenameItems, hkRenameItems, (void **)&oRenameItems, L"RenameItems")
            && Hook(vt->DeleteItem, hkDeleteItem, (void **)&oDeleteItem, L"DeleteItem")
            && Hook(vt->DeleteItems, hkDeleteItems, (void **)&oDeleteItems, L"DeleteItems")
            && Hook(vt->NewItem, hkNewItem, (void **)&oNewItem, L"NewItem")
            && Hook(vt->PerformOperations, hkPerform, (void **)&oPerform, L"PerformOperations")
            && Hook(GetMessageW, hkGetMsg, (void **)&oGetMsg, L"GetMessageW")
            && Hook(PeekMessageW, hkPeekMsg, (void **)&oPeekMsg, L"PeekMessageW");
    // All or nothing: a partial set could skip PerformOperations with unrecorded work queued.
    if (ok && MH_EnableHook(MH_ALL_HOOKS) == MH_OK)
        FcLog(L"hooks active");
    else
        FcLog(L"hooks NOT enabled, Explorer stays native");

done:
    if (op)
        IFileOperation_Release(op);
    if (SUCCEEDED(hrCo))
        CoUninitialize();
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        gInst = inst;
        DisableThreadLibraryCalls(inst);
        // COM and MinHook must not run under the loader lock.
        HANDLE th = CreateThread(NULL, 0, InitThread, NULL, 0, NULL);
        if (th)
            CloseHandle(th);
    }
    return TRUE;
}
