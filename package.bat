@echo off
rem Builds and zips what a user needs: dist\RelevoFC.zip (publish it as a GitHub release).
setlocal
set ROOT=%~dp0
call "%ROOT%build.bat" || exit /b 1
if exist "%ROOT%dist" rmdir /s /q "%ROOT%dist"
mkdir "%ROOT%dist\RelevoFC"
for %%F in (bin\fc_hook.dll bin\fc_agent.exe install.ps1 uninstall.ps1 Instalar.bat Desinstalar.bat) do copy "%ROOT%%%F" "%ROOT%dist\RelevoFC\" >nul || exit /b 1
powershell -NoProfile -Command "Compress-Archive -Path '%ROOT%dist\RelevoFC\*' -DestinationPath '%ROOT%dist\RelevoFC.zip' -Force" || exit /b 1
echo ZIP OK: %ROOT%dist\RelevoFC.zip
