// Update check for fc_agent: once a day, looks at the newest GitHub release (pre-releases too).
// If it is newer than this build, asks; on yes downloads RelevoFC.zip, checks it against the SHA-256
// GitHub publishes for the asset, and runs its install.ps1 (which asks for UAC and restarts Explorer).
// Off with [updates] check=0 in %APPDATA%\RelevoFC\settings.ini.
#include <windows.h>
#include <winhttp.h>
#include <bcrypt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../engine/engine.h"
#include "update.h"

#define W2(x) L##x
#define W(x) W2(x)
#ifndef RELEVOFC_VERSION
#define RELEVOFC_VERSION "v0.0.0"
#endif
#define REPO "Daniboy2116/RelevoFC"
#define DOWNLOAD_PREFIX "https://github.com/" REPO "/releases/download/"
#define KEY L"Software\\RelevoFC"
#define DAY_SECONDS (24 * 3600)

PCWSTR FcVersion(void) { return W(RELEVOFC_VERSION); }

// "v1.2.3" (anything after the numbers ignored) -> comparable number.
static ULONGLONG VersionKey(PCSTR s)
{
    unsigned a = 0, b = 0, c = 0;
    if (*s == 'v' || *s == 'V')
        s++;
    sscanf_s(s, "%u.%u.%u", &a, &b, &c);
    return ((ULONGLONG)a << 40) | ((ULONGLONG)b << 20) | c;
}

// GET over HTTPS (redirects followed), at most `cap` bytes. Caller frees *out (NUL-terminated).
static BOOL HttpGet(PCSTR url, BYTE **out, DWORD *len, DWORD cap)
{
    WCHAR wurl[2048], host[256], path[1800];
    MultiByteToWideChar(CP_UTF8, 0, url, -1, wurl, 2048);
    URL_COMPONENTS uc = { sizeof(uc) };
    uc.lpszHostName = host, uc.dwHostNameLength = 256;
    uc.lpszUrlPath = path, uc.dwUrlPathLength = 1800;
    if (!WinHttpCrackUrl(wurl, 0, 0, &uc) || uc.nScheme != INTERNET_SCHEME_HTTPS)
        return FALSE;
    HINTERNET s = WinHttpOpen(L"RelevoFC-updater", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, NULL, NULL, 0);
    HINTERNET c = s ? WinHttpConnect(s, host, uc.nPort, 0) : NULL;
    HINTERNET r = c ? WinHttpOpenRequest(c, L"GET", path, NULL, NULL, NULL, WINHTTP_FLAG_SECURE) : NULL;
    BOOL ok = FALSE;
    DWORD status = 0, sz = sizeof(status);
    *out = NULL;
    *len = 0;
    if (r && WinHttpSendRequest(r, L"Accept: application/vnd.github+json\r\n", (DWORD)-1, NULL, 0, 0, 0) &&
        WinHttpReceiveResponse(r, NULL) &&
        WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, NULL, &status, &sz, NULL) && status == 200) {
        DWORD got;
        ok = TRUE;
        for (;;) {
            BYTE *grown = realloc(*out, *len + 65536 + 1);
            if (!grown || *len > cap || !WinHttpReadData(r, (*out = grown) + *len, 65536, &got)) {
                ok = FALSE;
                break;
            }
            if (!got)
                break;
            *len += got;
        }
        if (ok)
            (*out)[*len] = 0;
    }
    if (!ok) {
        FcLog(L"update: GET failed (%lu, http %lu) %s", GetLastError(), status, wurl);
        free(*out);
        *out = NULL;
    }
    if (r)
        WinHttpCloseHandle(r);
    if (c)
        WinHttpCloseHandle(c);
    if (s)
        WinHttpCloseHandle(s);
    return ok;
}

// Copies the JSON string value after `key` (e.g. "\"tag_name\":\"") at or after `from`. GitHub's JSON has no spaces.
static BOOL JsonStr(PCSTR from, PCSTR key, PSTR out, size_t cap)
{
    PCSTR p = from ? strstr(from, key) : NULL, e;
    if (!p || !(e = strchr(p += strlen(key), '"')) || (size_t)(e - p) >= cap)
        return FALSE;
    memcpy(out, p, e - p);
    out[e - p] = 0;
    return TRUE;
}

typedef struct {
    char tag[64], url[512], sha[65];
} Release;

static BOOL LatestRelease(Release *rel)
{
    BYTE *json;
    DWORD len;
    if (!HttpGet("https://api.github.com/repos/" REPO "/releases?per_page=1", &json, &len, 4 << 20))
        return FALSE;
    PCSTR asset = strstr((PCSTR)json, "\"name\":\"RelevoFC.zip\"");
    BOOL ok = JsonStr((PCSTR)json, "\"tag_name\":\"", rel->tag, sizeof(rel->tag)) &&
              JsonStr(asset, "\"digest\":\"sha256:", rel->sha, sizeof(rel->sha)) && strlen(rel->sha) == 64 &&
              JsonStr(asset, "\"browser_download_url\":\"", rel->url, sizeof(rel->url)) &&
              !strncmp(rel->url, DOWNLOAD_PREFIX, strlen(DOWNLOAD_PREFIX));  // only our own release assets
    if (!ok)
        FcLog(L"update: no usable release in the GitHub answer");
    free(json);
    return ok;
}

static BOOL HashMatches(const BYTE *data, DWORD len, PCSTR hex)
{
    BYTE h[32];
    char got[65];
    if (BCryptHash(BCRYPT_SHA256_ALG_HANDLE, NULL, 0, (PUCHAR)data, len, h, 32))
        return FALSE;
    for (int i = 0; i < 32; i++)
        sprintf_s(got + 2 * i, 3, "%02x", h[i]);
    return !_stricmp(got, hex);
}

// PowerShell single-quoted literal: ' doubles.
static void Quote(PWSTR out, size_t cap, PCWSTR s)
{
    size_t n = 0;
    out[n++] = L'\'';
    for (; *s && n + 3 < cap; s++) {
        if (*s == L'\'')
            out[n++] = L'\'';
        out[n++] = *s;
    }
    out[n++] = L'\'';
    out[n] = 0;
}

// Downloads, verifies and starts the installer. FALSE: nothing was started.
static BOOL Install(const Release *rel)
{
    BYTE *zip;
    DWORD len;
    WCHAR dir[MAX_PATH], file[MAX_PATH], wtag[64];
    MultiByteToWideChar(CP_UTF8, 0, rel->tag, -1, wtag, 64);
    if (!HttpGet(rel->url, &zip, &len, 64 << 20))
        return FALSE;
    if (!HashMatches(zip, len, rel->sha)) {
        FcLog(L"update: %s does not match its published SHA-256, not installed", wtag);
        free(zip);
        return FALSE;
    }
    GetTempPathW(MAX_PATH, dir);
    swprintf_s(dir + wcslen(dir), MAX_PATH - wcslen(dir), L"RelevoFC-%s", wtag);
    CreateDirectoryW(dir, NULL);
    swprintf_s(file, MAX_PATH, L"%s\\RelevoFC.zip", dir);
    HANDLE h = CreateFileW(file, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    DWORD w = 0;
    BOOL ok = h != INVALID_HANDLE_VALUE && WriteFile(h, zip, len, &w, NULL) && w == len;
    if (h != INVALID_HANDLE_VALUE)
        CloseHandle(h);
    free(zip);
    if (!ok)
        return FALSE;

    // install.ps1 stops this agent and starts the new one; on an error the window stays open to show it.
    WCHAR qzip[MAX_PATH * 2], qdir[MAX_PATH * 2], cmd[4 * MAX_PATH + 512];
    Quote(qzip, _countof(qzip), file);
    wcscat_s(dir, MAX_PATH, L"\\files");
    Quote(qdir, _countof(qdir), dir);
    swprintf_s(cmd, _countof(cmd),
               L"powershell.exe -NoProfile -ExecutionPolicy Bypass -Command \"try { $ErrorActionPreference = 'Stop'; "
               L"Expand-Archive -LiteralPath %s -DestinationPath %s -Force; & (Join-Path %s 'install.ps1') } "
               L"catch { Write-Host $_ -ForegroundColor Red; Read-Host 'Pulsa Enter para cerrar' }\"",
               qzip, qdir, qdir);
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi;
    if (!CreateProcessW(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi))
        return FALSE;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    FcLog(L"update: installing %s", wtag);
    return TRUE;
}

static BOOL Enabled(void)
{
    WCHAR ini[MAX_PATH];
    if (!GetEnvironmentVariableW(L"RELEVOFC_INI", ini, MAX_PATH)) {
        if (!GetEnvironmentVariableW(L"APPDATA", ini, MAX_PATH))
            ini[0] = 0;
        wcscat_s(ini, MAX_PATH, L"\\RelevoFC\\settings.ini");
    }
    return GetPrivateProfileIntW(L"updates", L"check", 1, ini) != 0;
}

static ULONGLONG NowSeconds(void)
{
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    return (((ULONGLONG)ft.dwHighDateTime << 32) | ft.dwLowDateTime) / 10000000;
}

void FcCheckForUpdate(BOOL force)
{
    ULONGLONG last = 0, now = NowSeconds();
    DWORD sz = sizeof(last);
    RegGetValueW(HKEY_CURRENT_USER, KEY, L"LastUpdateCheck", RRF_RT_REG_QWORD, NULL, &last, &sz);
    if (!force && (!Enabled() || now - last < DAY_SECONDS))
        return;
    RegSetKeyValueW(HKEY_CURRENT_USER, KEY, L"LastUpdateCheck", REG_QWORD, &now, sizeof(now));

    Release rel;
    char mine[64];
    WideCharToMultiByte(CP_UTF8, 0, FcVersion(), -1, mine, 64, NULL, NULL);
    if (!LatestRelease(&rel))
        return;
    WCHAR wtag[64], skipped[64] = L"", text[512];
    MultiByteToWideChar(CP_UTF8, 0, rel.tag, -1, wtag, 64);
    sz = sizeof(skipped);
    RegGetValueW(HKEY_CURRENT_USER, KEY, L"SkippedVersion", RRF_RT_REG_SZ, NULL, skipped, &sz);
    FcLog(L"update: installed %s, latest %s", FcVersion(), wtag);
    if (VersionKey(rel.tag) <= VersionKey(mine) || (!force && !wcscmp(skipped, wtag)))
        return;

    swprintf_s(text, _countof(text),
               L"Hay una versión nueva de RelevoFC: %s (tienes %s).\n\n"
               L"¿Descargarla e instalarla ahora? Windows pedirá permiso de administrador y el Explorador "
               L"se reiniciará (se cierran las carpetas abiertas).\n\n"
               L"Si eliges No, no volverá a avisar de esta versión.",
               wtag, FcVersion());
    int a = MessageBoxW(NULL, text, L"RelevoFC", MB_YESNO | MB_ICONINFORMATION | MB_SETFOREGROUND);
    if (a == IDNO) {
        RegSetKeyValueW(HKEY_CURRENT_USER, KEY, L"SkippedVersion", REG_SZ, wtag, (DWORD)(wcslen(wtag) + 1) * sizeof(WCHAR));
    } else if (a == IDYES && !Install(&rel)) {
        MessageBoxW(NULL, L"No se pudo descargar o comprobar la actualización. Se volverá a intentar mañana; "
                          L"también puedes descargarla a mano desde la página de Releases.",
                    L"RelevoFC", MB_OK | MB_ICONWARNING | MB_SETFOREGROUND);
    }
}

static DWORD WINAPI UpdateThread(LPVOID unused)
{
    (void)unused;
    Sleep(60 * 1000);  // let the logon settle first
    for (;;) {
        FcCheckForUpdate(FALSE);
        Sleep(3600 * 1000);  // the once-a-day rule lives in FcCheckForUpdate
    }
}

void FcStartUpdateChecks(void)
{
    HANDLE th = CreateThread(NULL, 0, UpdateThread, NULL, 0, NULL);
    if (th)
        CloseHandle(th);
}
