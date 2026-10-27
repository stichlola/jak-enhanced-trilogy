@echo off
rem Jak Enhanced Trilogy: puts what's needed to look into a problem in one ZIP on your Desktop (the
rem newest game logs, your graphics card, its driver, Windows, and the games' settings), to attach to
rem a message or a GitHub issue. Nothing is sent anywhere: you choose whether to share the file.
title Jak Enhanced Trilogy - problem report
cd /d "%~dp0"
powershell -NoProfile -Command ^
  "$ErrorActionPreference = 'SilentlyContinue';" ^
  "$stamp = Get-Date -Format 'yyyy-MM-dd_HH-mm';" ^
  "$work = Join-Path $env:TEMP ('jak-enhanced-report-' + $stamp);" ^
  "New-Item -ItemType Directory -Force $work | Out-Null;" ^
  "$info = @();" ^
  "$info += 'Jak Enhanced Trilogy problem report, ' + (Get-Date);" ^
  "$info += 'Windows: ' + (Get-CimInstance Win32_OperatingSystem).Caption + ' ' + (Get-CimInstance Win32_OperatingSystem).Version;" ^
  "$info += 'CPU: ' + (Get-CimInstance Win32_Processor | Select-Object -First 1).Name;" ^
  "$info += 'RAM: ' + [math]::Round((Get-CimInstance Win32_ComputerSystem).TotalPhysicalMemory / 1GB, 1) + ' GB';" ^
  "foreach ($g in Get-CimInstance Win32_VideoController) { $info += 'GPU: ' + $g.Name + ', driver ' + $g.DriverVersion + ' (' + $g.DriverDate + ')' };" ^
  "if (Test-Path 'data\VERSION') { $info += 'Version: ' + (Get-Content 'data\VERSION' -TotalCount 1) } else { $info += 'Version: source copy' };" ^
  "$info | Set-Content -Encoding UTF8 (Join-Path $work 'system.txt');" ^
  "$logs = @('data\log', 'log') | Where-Object { Test-Path $_ };" ^
  "foreach ($d in $logs) { foreach ($game in 'jak1','jak2','jak3') { Get-ChildItem $d -Filter ($game + '.*.log') | Sort-Object LastWriteTime -Descending | Select-Object -First 2 | Copy-Item -Destination $work } };" ^
  "foreach ($game in 'jak1','jak2','jak3') { $s = Join-Path $env:APPDATA ('OpenGOAL\' + $game + '\settings'); if (Test-Path $s) { $t = Join-Path $work ($game + '-settings'); New-Item -ItemType Directory -Force $t | Out-Null; Get-ChildItem $s -File | Where-Object { $_.Extension -ne '.bin' } | Copy-Item -Destination $t } };" ^
  "$zip = Join-Path ([Environment]::GetFolderPath('Desktop')) ('JakEnhanced-report-' + $stamp + '.zip');" ^
  "Compress-Archive -Path (Join-Path $work '*') -DestinationPath $zip -Force;" ^
  "Remove-Item -Recurse -Force $work;" ^
  "Write-Host ''; Write-Host ('Report saved: ' + $zip); Write-Host 'Attach it to your message or GitHub issue.'"
echo.
pause
