// Times one copy with the real engine: bench.exe <threads> <source> <destination folder> [verify].
// The settings go through a temporary settings file (RELEVOFC_INI), like a user's settings.ini.
#include <windows.h>
#include <stdio.h>
#include "../src/engine/engine.h"

int wmain(int argc, wchar_t **argv)
{
    if (argc < 4) {
        wprintf(L"uso: bench.exe <hilos> <origen> <carpeta destino> [verify] [lane=0|1]\n");
        return 2;
    }
    WCHAR ini[MAX_PATH];
    GetTempPathW(MAX_PATH, ini);
    wcscat_s(ini, MAX_PATH, L"relevofc_bench.ini");
    DeleteFileW(ini);
    WritePrivateProfileStringW(L"engine", L"threads", argv[1], ini);
    for (int i = 4; i < argc; i++) {
        if (!wcscmp(argv[i], L"verify"))
            WritePrivateProfileStringW(L"engine", L"verify_copies", L"1", ini);
        else if (!wcsncmp(argv[i], L"lane=", 5))
            WritePrivateProfileStringW(L"engine", L"big_files_one_at_a_time", argv[i] + 5, ini);
    }
    SetEnvironmentVariableW(L"RELEVOFC_INI", ini);

    FcItem item = { FALSE, argv[2], argv[3], NULL };
    FcProgress pr = { 0 };
    ULONGLONG t0 = GetTickCount64();
    FcResult r = FcRun(&item, 1, &pr);
    ULONGLONG ms = GetTickCount64() - t0;
    DeleteFileW(ini);
    wprintf(L"hilos=%s resultado=%d archivos=%ld %llu ms %.1f MB/s\n", argv[1], r, pr.filesDone, ms,
            ms ? pr.bytesDone / 1048576.0 / (ms / 1000.0) : 0.0);
    return r != FC_OK;
}
