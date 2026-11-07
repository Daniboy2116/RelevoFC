# Removes RelevoFC (and anything left from when it was called FastCopy) and restarts Explorer so the hook is unloaded.
$dst = Join-Path $env:ProgramFiles 'RelevoFC'
$legacy = Join-Path $env:ProgramFiles 'FastCopy'

foreach ($name in 'RelevoFCAgent', 'FastCopyAgent') {
    Unregister-ScheduledTask -TaskName $name -Confirm:$false -ErrorAction SilentlyContinue
    Remove-ItemProperty 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run' $name -ErrorAction SilentlyContinue
}
Get-Process fc_agent -ErrorAction SilentlyContinue | Stop-Process -Force
Stop-Process -Name explorer -Force
Start-Sleep 3
if (-not (Get-Process explorer -ErrorAction SilentlyContinue)) { Start-Process explorer.exe }
# Elevated: also unregisters a legacy task that only an admin can remove.
$cmd = "Unregister-ScheduledTask -TaskName 'FastCopyAgent' -Confirm:`$false -ErrorAction SilentlyContinue; " +
       "Remove-Item '$dst', '$legacy' -Recurse -Force -ErrorAction SilentlyContinue"
Start-Process powershell -Verb RunAs -Wait -WindowStyle Hidden -ArgumentList '-NoProfile', '-Command', $cmd
Write-Host 'RelevoFC desinstalado; el Explorador vuelve a copiar de forma nativa.'
