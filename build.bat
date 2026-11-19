@echo off
setlocal
set ROOT=%~dp0
set "PATH=%PATH%;C:\Program Files (x86)\Microsoft Visual Studio\Installer"
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
if not exist "%ROOT%bin" mkdir "%ROOT%bin"
if not exist "%ROOT%obj" mkdir "%ROOT%obj"
set MH=%ROOT%third_party\minhook
set ENGINE="%ROOT%src\engine\engine.c" "%ROOT%src\engine\log.c"
set LIBS=ole32.lib shell32.lib shlwapi.lib uuid.lib user32.lib gdi32.lib dwmapi.lib advapi32.lib msimg32.lib comctl32.lib

cl /nologo /utf-8 /O2 /W3 /MT /LD /Fo"%ROOT%obj\\" /Fe"%ROOT%bin\fc_hook.dll" /I"%MH%\include" ^
  "%ROOT%src\hook\fchook.c" "%ROOT%src\hook\ui.c" "%ROOT%src\hook\elevate.c" %ENGINE% "%MH%\src\hook.c" "%MH%\src\buffer.c" "%MH%\src\trampoline.c" "%MH%\src\hde\hde64.c" ^
  %LIBS% || exit /b 1

cl /nologo /utf-8 /O2 /W3 /MT /Fo"%ROOT%obj\\" /Fe"%ROOT%bin\engine_test.exe" "%ROOT%tests\engine_test.c" %ENGINE% %LIBS% || exit /b 1

cl /nologo /utf-8 /O2 /W3 /MT /Fo"%ROOT%obj\\" /Fe"%ROOT%bin\bench.exe" "%ROOT%tests\bench.c" %ENGINE% %LIBS% || exit /b 1

cl /nologo /utf-8 /O2 /W3 /MT /Fo"%ROOT%obj\\" /Fe"%ROOT%bin\ui_demo.exe" "%ROOT%tests\ui_demo.c" "%ROOT%src\hook\ui.c" "%ROOT%src\hook\elevate.c" %ENGINE% %LIBS% || exit /b 1

cl /nologo /utf-8 /O2 /W3 /MT /Fo"%ROOT%obj\\" /Fe"%ROOT%bin\RelevoFC.exe" "%ROOT%src\admin\admin.c" %ENGINE% %LIBS% ^
  /link /SUBSYSTEM:WINDOWS /MANIFEST:EMBED || exit /b 1

rem The agent's version decides when it offers an update: package.bat sets it, otherwise git (e.g. v0.4.0-6-g4d6b1d8).
if not defined RELEVOFC_VERSION for /f %%v in ('git -C "%ROOT%." describe --tags 2^>nul') do set RELEVOFC_VERSION=%%v
if not defined RELEVOFC_VERSION set RELEVOFC_VERSION=v0.0.0
cl /nologo /utf-8 /O2 /W3 /MT /DRELEVOFC_VERSION=\"%RELEVOFC_VERSION%\" /Fo"%ROOT%obj\\" /Fe"%ROOT%bin\fc_agent.exe" ^
  "%ROOT%src\agent\agent.c" "%ROOT%src\agent\update.c" "%ROOT%src\engine\log.c" ^
  shlwapi.lib winhttp.lib bcrypt.lib user32.lib advapi32.lib /link /SUBSYSTEM:WINDOWS || exit /b 1
echo BUILD OK
