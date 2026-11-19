#pragma once
#include "../engine/engine.h"

// Live channel between the progress window in Explorer and the elevated RelevoFC.exe, at the start
// of the unnamed mapping that also carries the items (right after this struct). The helper writes
// the progress and the questions; Explorer writes cancel, paused and the answers.
typedef struct {
    volatile LONG64 bytesDone, bytesTotal;
    volatile LONG filesDone, filesTotal, phase, filesFailed;
    volatile LONG cancel, paused;
    volatile LONG ask;        // FC_ASK_*, set by the helper once the fields of the question are filled in
    volatile LONG answer;     // -1 until Explorer answers: an FcConflictChoice, or 1 = retry the failed files
    volatile LONG count, errorHalted;
    volatile ULONGLONG list;  // conflict: handle, valid in Explorer, of a mapping with the file lists (0: none)
    WCHAR current[MAX_PATH], example[MAX_PATH], failed[2048];
} FcShared;

enum { FC_ASK_NONE, FC_ASK_CONFLICT, FC_ASK_ERROR };

// Conflict list mapping: DWORD count, LONG picks[count] (Explorer writes them for an EACH answer),
// then src and dst of each file as NUL-terminated strings.

// Items after the FcShared header: DWORD n, then per item DWORD move and three strings, each a DWORD
// length in WCHARs (0xFFFFFFFF = NULL) followed by the characters and a NUL.

// Explorer side: reruns the operation in RelevoFC.exe through UAC, mirroring its progress into `prog`
// and passing its questions to prog->onConflict / prog->onError.
FcResult RunElevated(const FcItem *items, int n, FcProgress *prog);
