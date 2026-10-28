@echo off
rem Jak Enhanced Trilogy: puts what's needed to look into a problem in one ZIP on your Desktop (the
rem newest game logs, your graphics card, its driver, Windows, and the games' settings), to attach to
rem a message or a GitHub issue. Then it asks whether to send it to the developer (the project's own
rem report server, tools\report-worker: kept private); nothing is sent unless you say yes.
title Jak Enhanced Trilogy - problem report
cd /d "%~dp0"
set "REPORT_URL="
powershell -NoProfile -Command ^
  "$ErrorActionPreference = 'SilentlyContinue';" ^
  "$stamp = Get-Date -Format 'yyyy-MM-dd_HH-mm';" ^
  "$work = Join-Path $env:TEMP ('jak-enhanced-report-' + $stamp);" ^
  "New-Item -ItemType Directory -Force $work | Out-Null;" ^
  "Write-Host 'What went wrong? A line or two: what you were doing, what happened (Enter when done):';" ^
  "$note = Read-Host;" ^
  "$note | Set-Content -Encoding UTF8 (Join-Path $work 'what-happened.txt');" ^
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
  "Write-Host ''; Write-Host ('Report saved: ' + $zip);" ^
  "$ver = if (Test-Path 'data\VERSION') { Get-Content 'data\VERSION' -TotalCount 1 } else { 'source' };" ^
  "$sent = $false;" ^
  "if ($env:REPORT_URL) {" ^
  "  Write-Host 'It holds your game logs, PC details and settings (the logs show your Windows user name).';" ^
  "  $yes = Read-Host 'Send it to the developer now? (y/N)';" ^
  "  if ($yes -match '^[yY]') {" ^
  "    try {" ^
  "      [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12;" ^
  "      Add-Type -AssemblyName System.Net.Http;" ^
  "      $client = New-Object System.Net.Http.HttpClient; $client.Timeout = [TimeSpan]::FromSeconds(90);" ^
  "      $form = New-Object System.Net.Http.MultipartFormDataContent;" ^
  "      $q = [char]34;" ^
  "      function Part($c, $n, $f) { $d = 'form-data; name=' + $q + $n + $q; if ($f) { $d += '; filename=' + $q + $f + $q }; $c.Headers.ContentDisposition = [Net.Http.Headers.ContentDispositionHeaderValue]::Parse($d); $form.Add($c) };" ^
  "      Part (New-Object System.Net.Http.StringContent([string]$note)) 'note' $null;" ^
  "      Part (New-Object System.Net.Http.StringContent([string]$ver)) 'version' $null;" ^
  "      Part (New-Object System.Net.Http.ByteArrayContent(,[IO.File]::ReadAllBytes($zip))) 'report' 'report.zip';" ^
  "      $answer = $client.PostAsync($env:REPORT_URL, $form).Result;" ^
  "      Write-Host $answer.Content.ReadAsStringAsync().Result; $sent = $answer.IsSuccessStatusCode" ^
  "    } catch { Write-Host ('Sending failed: ' + $_.Exception.InnerException.Message) }" ^
  "  }" ^
  "};" ^
  "if (-not $sent) { Write-Host 'Attach it to your message or GitHub issue.' }"
echo.
pause
