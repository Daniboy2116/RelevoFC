@echo off
rem Builds and zips what a user needs: dist\RelevoFC.zip (publish it as a GitHub release).
rem Usage: package.bat v0.5.0 -- the same tag as the release, or installed copies would keep offering it.
setlocal
set ROOT=%~dp0
if "%~1"=="" (echo Uso: package.bat vX.Y.Z  ^(la etiqueta de la release^) & exit /b 1)
set RELEVOFC_VERSION=%~1
call "%ROOT%build.bat" || exit /b 1
if exist "%ROOT%dist" rmdir /s /q "%ROOT%dist"
mkdir "%ROOT%dist\RelevoFC"
for %%F in (bin\fc_hook.dll bin\fc_agent.exe bin\RelevoFC.exe install.ps1 uninstall.ps1 Instalar.bat Desinstalar.bat) do copy "%ROOT%%%F" "%ROOT%dist\RelevoFC\" >nul || exit /b 1
powershell -NoProfile -Command "Compress-Archive -Path '%ROOT%dist\RelevoFC\*' -DestinationPath '%ROOT%dist\RelevoFC.zip' -Force" || exit /b 1
echo ZIP OK: %ROOT%dist\RelevoFC.zip
