#pragma once
#include <windows.h>

// One queued Explorer operation: copy or move `src` into folder `dstDir`, optionally renamed.
typedef struct {
    BOOL move;
    PWSTR src;
    PWSTR dstDir;
    PWSTR newName;  // NULL keeps the source name
} FcItem;

enum { FC_PHASE_PLAN, FC_PHASE_COPY, FC_PHASE_FINISH };

// What to do with files that already exist at the destination (asked once per operation).
// FC_CONFLICT_EACH: the answer is per file, in prog->conflictPick (REPLACE, SKIP or BOTH each).
typedef enum { FC_CONFLICT_REPLACE, FC_CONFLICT_SKIP, FC_CONFLICT_BOTH, FC_CONFLICT_CANCEL, FC_CONFLICT_EACH } FcConflictChoice;
typedef struct FcProgress FcProgress;
typedef FcConflictChoice (*FcConflictFn)(FcProgress *prog, int count, PCWSTR example);
// Some files failed: `list` has a "path, newline, indented reason" block per file.
// TRUE = retry just those, FALSE = give up on them.
typedef BOOL (*FcErrorFn)(FcProgress *prog, int count, PCWSTR list);

// Shared between the engine (writer) and the progress window (reader). Zero-initialize.
struct FcProgress {
    volatile LONG64 bytesDone;
    volatile LONG64 bytesTotal;  // known after planning; moves count their verify pass too
    volatile LONG filesDone, filesTotal;
    volatile LONG phase;
    volatile LONG cancel, paused;  // set by the UI
    FcConflictFn onConflict;       // NULL: any existing target falls back to Windows
    FcErrorFn onError;             // NULL: failed files are reported in `failed`, no retry
    SRWLOCK lock;                  // guards `current`
    WCHAR current[MAX_PATH];
    // Conflict hand-off between the engine thread (asks, then polls conflictChoice) and the UI (answers).
    HANDLE conflictEvent;  // signalled by the engine when it starts asking; optional
    volatile LONG conflictPending, conflictChoice, conflictCount;  // choice: -1 until answered, else FcConflictChoice
    // The same hand-off also carries the "some files failed" question: askError is set while it is open and the
    // answer is 1 (skip) or 2 (retry); the text to show is in `failed`.
    volatile LONG askError;
    // The last copy round stopped early on a device-level error (unplugged, asleep, disk full...): the
    // question is then "stopped at N %: retry or cancel" instead of "these files failed: retry or skip".
    volatile LONG errorHalted;
    WCHAR conflictExample[MAX_PATH];
    // While a question is open: the conflicting files (source, existing target) and, for an EACH answer,
    // where to write one FcConflictChoice per file. Owned by the engine; valid until the answer is read.
    PCWSTR *conflictSrc, *conflictDst;
    volatile LONG *conflictPick;
    // Files that still failed at the end (the rest of the operation carried on): their count, and
    // the same "path / reason" text the onError callback gets (truncated to fit).
    volatile LONG filesFailed;
    WCHAR failed[2048];
};

typedef enum {
    FC_OK,         // everything copied/moved
    FC_FALLBACK,   // nothing was written; let Windows do it (conflicts, virtual paths, reparse points...)
    FC_ERROR,      // some files failed (see prog->filesFailed); the others were done, and a move only deletes the sources it verified
    FC_CANCELLED   // user cancelled: finished files stay, moved files are only removed from the source once verified
} FcResult;

FcResult FcRun(const FcItem *items, int n, FcProgress *prog);

void FcLog(const wchar_t *fmt, ...);
