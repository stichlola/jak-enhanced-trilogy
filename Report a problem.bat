@echo off
rem Jak Enhanced Trilogy: opens the report window (scripts\enhanced\report.ps1, the same as the
rem Enhanced menu's "Send feedback" button): write what happened, one click sends it with the newest
rem game logs, graphics card, driver and settings. Nothing is sent without the click; offline the
rem ZIP lands on the Desktop to attach to a message or a GitHub issue.
cd /d "%~dp0"
set "PROJ=%~dp0."
if exist "%~dp0gk.exe" set "PROJ=%~dp0data"
start "" powershell -NoProfile -WindowStyle Hidden -Command "& ([scriptblock]::Create([IO.File]::ReadAllText('%PROJ%\scripts\enhanced\report.ps1'))) -Proj '%PROJ%'"
