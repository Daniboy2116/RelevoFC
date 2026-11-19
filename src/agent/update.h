#pragma once
#include <windows.h>

PCWSTR FcVersion(void);           // this build, e.g. L"v0.5.0" (from git or package.bat)
void FcCheckForUpdate(BOOL force);  // force: ignore the once-a-day rule, the setting and a skipped version
void FcStartUpdateChecks(void);     // background thread: a minute after start, then daily
