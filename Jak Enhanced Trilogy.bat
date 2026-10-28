@echo off
rem Jak Enhanced Trilogy: put your own disc images (.iso) in the ISO folder, then double-click this
rem file. The first time it extracts your discs (20-40 minutes a game); after that it starts the game
rem played last, on the trilogy's start menu to pick another.
rem In the release ZIP the programs are already built (gk.exe, extractor.exe next to this file, their
rem data in data\). In a copy of the source code it builds them first (Visual Studio, CMake: an hour).
setlocal EnableDelayedExpansion
title Jak Enhanced Trilogy
cd /d "%~dp0"
set "ROOT=%~dp0"
set "ROOT=%ROOT:~0,-1%"
if not exist "%ROOT%\ISO" mkdir "%ROOT%\ISO"

if exist "%ROOT%\gk.exe" (
  rem the release: the programs here, everything they read and write in data\
  set "BIN=%ROOT%"
  set "PROJ=%ROOT%\data"
) else (
  rem the source code: the OpenGOAL tools find the project through a folder named jak-project,
  rem linked next to this one when it has another name: a downloaded ZIP, a renamed clone
  for %%I in ("%ROOT%") do (
    set "LEAF=%%~nxI"
    set "PARENT=%%~dpI"
  )
  if /i "!LEAF!"=="jak-project" (
    set "PROJ=%ROOT%"
  ) else (
    set "PROJ=!PARENT!jak-project"
    if not exist "!PROJ!" mklink /J "!PROJ!" "%ROOT%" >nul
  )
  set "BIN=!PROJ!\out\build\Release\bin"
)

rem Which game each disc is: its serial, from the boot line of SYSTEM.CNF ("BOOT2 = cdrom0:\SCES_524.60;1")
rem in its first megabytes (the discs also list other games' serials)
set "JAK1_ISO="
set "JAK2_ISO="
set "JAK3_ISO="
rem the .iso files in the ISO folder, and the ones picked in the installer (ISO\iso-paths.txt, a path a line)
if not exist "%ROOT%\ISO\iso-paths.txt" type nul > "%ROOT%\ISO\iso-paths.txt"
for /f "usebackq delims=" %%F in (`dir /b /s "%ROOT%\ISO\*.iso" 2^>nul ^& type "%ROOT%\ISO\iso-paths.txt"`) do if exist "%%~fF" (
  set "GAME="
  for /f %%G in ('powershell -NoProfile -Command "$s=[IO.File]::OpenRead('%%~fF'); $b=New-Object byte[] 8MB; $n=$s.Read($b,0,$b.Length); $s.Close(); $t=[Text.Encoding]::ASCII.GetString($b,0,$n); if ($t -match 'cdrom0:\\(\w{4}_\d{3}\.\d{2})') { $id=$Matches[1]; if ($id -match 'SCUS_971\.24|SCES_503\.61|SCPS_150\.21') {'jak1'} elseif ($id -match 'SCUS_972\.65|SCES_516\.08|SCPS_150\.57') {'jak2'} elseif ($id -match 'SCUS_973\.30|SCES_524\.60|SCPS_150\.93') {'jak3'} }"') do set "GAME=%%G"
  if "!GAME!"=="jak1" if not defined JAK1_ISO set "JAK1_ISO=%%~fF" & echo Found Jak and Daxter: The Precursor Legacy: %%~nxF
  if "!GAME!"=="jak2" if not defined JAK2_ISO set "JAK2_ISO=%%~fF" & echo Found Jak II: %%~nxF
  if "!GAME!"=="jak3" if not defined JAK3_ISO set "JAK3_ISO=%%~fF" & echo Found Jak 3: %%~nxF
  if "!GAME!"=="" echo Not a Jak disc we know, skipped: %%~nxF
)

rem The release's version (data\VERSION): a game compiled by an older one is compiled again (an
rem update unzipped over the old folder keeps the extracted discs)
set "VER="
if exist "%PROJ%\VERSION" set /p VER=<"%PROJ%\VERSION"

rem A newer release on GitHub? (the release only; 4 seconds at most, nothing happens offline; the
rem player chooses: U installs it over this folder with scripts\enhanced\update.ps1, which keeps the
rem extracted discs and starts this launcher again; nothing is downloaded without asking)
if defined VER (
  set "LATEST="
  for /f "usebackq delims=" %%L in (`powershell -NoProfile -Command "try { (Invoke-RestMethod -TimeoutSec 4 -Uri 'https://api.github.com/repos/stichlola/jak-enhanced-trilogy/releases/latest').tag_name } catch {}"`) do set "LATEST=%%L"
  for /f "delims=-" %%V in ("!VER!") do set "MINE=%%V"
  if defined LATEST if /i not "!LATEST!"=="!MINE!" (
    powershell -NoProfile -Command "$v = { param($s) $p = @($s.TrimStart('v') -split '\.') + @('0','0','0'); [version]($p[0..2] -join '.') }; if ((& $v '!LATEST!') -gt (& $v '!MINE!')) { exit 1 }" || (
      echo.
      echo A new version is out: !LATEST! ^(you have !MINE!^).
      choice /c UC /t 10 /d C /m "U updates now, about 120 MB to download, your games are kept; C carries on"
      if not errorlevel 2 (
        start "Jak Enhanced Trilogy update" powershell -NoProfile -Command "& ([scriptblock]::Create([IO.File]::ReadAllText('%PROJ%\scripts\enhanced\update.ps1'))) -Root '%ROOT%' -Tag '!LATEST!'"
        exit /b 0
      )
    )
  )
)

rem What still needs setting up: the build once, then each new disc
set "NEED_BUILD="
if not exist "%BIN%\gk.exe" set "NEED_BUILD=1"
set "NEED_EXTRACT="
for %%G in (1 2 3) do if defined JAK%%G_ISO if not exist "%PROJ%\out\jak%%G\iso" set "NEED_EXTRACT=1"
if defined NEED_BUILD if not defined JAK1_ISO if not defined JAK2_ISO if not defined JAK3_ISO (
  echo.
  echo Put your Jak disc images ^(.iso^) in the ISO folder, then start Jak Enhanced Trilogy again.
  start "" explorer "%ROOT%\ISO"
  pause
  exit /b 1
)
if defined NEED_BUILD (
  echo.
  echo This is the source code: installing the tools and building the game first ^(about an hour^).
  echo To skip this, download the ready-made ZIP from the Releases page instead.
  call :tools || goto :failed
  call :build || goto :failed
)
if defined NEED_EXTRACT (
  for %%G in (1 2 3) do if defined JAK%%G_ISO if not exist "%PROJ%\out\jak%%G\iso" (
    echo.
    echo First start of Jak %%G: extracting your disc and compiling the game ^(20-40 minutes,
    echo only this once^). You can leave it running.
    "%BIN%\extractor.exe" --game jak%%G --proj-path "%PROJ%" -e -d -c "!JAK%%G_ISO!" || goto :failed
    if defined VER echo !VER!>"%PROJ%\out\jak%%G\built-version.txt"
  )
)

rem The optional HD textures, made here from the player's own disc (scripts\enhanced\hd_setup.py,
rem Python installed with winget when missing): asked once per game, unless the installer already
rem asked (hd-textures-jakN.txt in data\: y or n)
for %%G in (1 2 3) do if defined JAK%%G_ISO if exist "%PROJ%\out\jak%%G\iso" if not exist "%PROJ%\out\jak%%G\hd-textures.asked" (
  set "HD="
  if exist "%PROJ%\hd-textures-jak%%G.txt" set /p HD=<"%PROJ%\hd-textures-jak%%G.txt"
  if not defined HD (
    echo.
    echo HD textures for Jak %%G: every texture of the game redrawn 4x by an AI ^(Real-ESRGAN^) on your
    echo graphics card, from your own disc. It takes a while: about 30 minutes for Jak 1, 1-2 hours for
    echo Jak II or Jak 3 ^(depends on the graphics card^), and 1-4 GB more on disk. The game works the
    echo same without them. To be asked again, delete out\jak%%G\hd-textures.asked.
    set "HD=n"
    set /p "HD=Make the HD textures for Jak %%G now? (y/N): "
  )
  echo asked> "%PROJ%\out\jak%%G\hd-textures.asked"
  if /i "!HD!"=="y" (
    where py >nul 2>nul || winget install --id Python.Python.3.12 -e --accept-package-agreements --accept-source-agreements
    set "PATH=%LOCALAPPDATA%\Programs\Python\Launcher;%PATH%"
    py -3 "%PROJ%\scripts\enhanced\hd_setup.py" --games jak%%G --bin "%BIN%" || goto :failed
  )
)

rem An update: the games compiled by an older version are compiled again (a minute or two each)
if defined VER for %%G in (1 2 3) do if defined JAK%%G_ISO if exist "%PROJ%\out\jak%%G\iso" (
  set "BUILT="
  if exist "%PROJ%\out\jak%%G\built-version.txt" set /p BUILT=<"%PROJ%\out\jak%%G\built-version.txt"
  if not "!BUILT!"=="!VER!" (
    echo.
    echo Updating Jak %%G to !VER!: compiling the game again ^(a minute or two, only this once^).
    rem the game's text too: it is rebuilt only when its own files change, not when the text
    rem encoder does ^(the buttons drawn in the messages^)
    del /q "%PROJ%\out\jak%%G\iso\*COMMON.TXT" 2>nul
    "%BIN%\extractor.exe" --game jak%%G --proj-path "%PROJ%" -c "!JAK%%G_ISO!" || goto :failed
    echo !VER!>"%PROJ%\out\jak%%G\built-version.txt"
  )
)

rem Which game boots: the one played last (the game writes it), else the first ready. With more than
rem one ready, the game opens on the trilogy's start menu by itself to pick another
set "COUNT=0"
for %%G in (1 2 3) do if exist "%PROJ%\out\jak%%G\iso" (
  set /a COUNT+=1
  set "READY!COUNT!=%%G"
)
if %COUNT%==0 (
  echo No game is ready yet: put your disc images ^(.iso^) in the ISO folder and start again.
  pause
  exit /b 1
)
set "PICK=%READY1%"
set "LAST="
if exist "%APPDATA%\OpenGOAL\enhanced-last-game.txt" set /p LAST=<"%APPDATA%\OpenGOAL\enhanced-last-game.txt"
for %%G in (1 2 3) do if "!LAST!"=="%%G" if exist "%PROJ%\out\jak%%G\iso" set "PICK=%%G"
echo.
echo Starting Jak %PICK%.
echo   \  (the key left of 1)  opens the Enhanced menu: graphics, weather, time of day...
echo   Select / N             a nap: the hours go by while the world around runs fast
echo   F9                     switches to the next game of the trilogy
rem Jak 3 comes with a save in slot 4: the whole game finished, everything unlocked. Only into an
rem empty slot (delete or overwrite it in the game). The folder name has a "!": no delayed expansion.
setlocal DisableDelayedExpansion
set "SAVES=%APPDATA%\OpenGOAL\jak3\saves\BASCUS-97330AYBABTU!"
if exist "%PROJ%\game\assets\jak3\enhanced-save\bank6.bin" if not exist "%SAVES%\bank6.bin" if not exist "%SAVES%\bank7.bin" (
  if not exist "%SAVES%" mkdir "%SAVES%"
  copy /y "%PROJ%\game\assets\jak3\enhanced-save\bank6.bin" "%SAVES%\bank6.bin" >nul
)
endlocal
cd /d "%PROJ%"
rem a hidden PowerShell starts the game and waits for it: if gk.exe closes with an error it opens
rem the report window (scripts\enhanced\report.ps1: one click sends the logs, nothing without it)
start "" powershell -NoProfile -WindowStyle Hidden -Command "$p = Start-Process -FilePath '%BIN%\gk.exe' -ArgumentList '--game jak%PICK% -- -boot -fakeiso' -PassThru; $null = $p.Handle; $p.WaitForExit(); if ($p.ExitCode -ne 0) { & ([scriptblock]::Create([IO.File]::ReadAllText('%PROJ%\scripts\enhanced\report.ps1'))) -Proj '%PROJ%' -Crash $p.ExitCode }"
exit /b 0

:tools
rem Visual Studio 2022 Build Tools with Clang, CMake, Ninja, NASM, Git (winget, only what's missing)
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "VS="
if exist "%VSWHERE%" for /f "usebackq delims=" %%V in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Llvm.Clang -property installationPath`) do set "VS=%%V"
if not defined VS (
  echo Installing Visual Studio 2022 Build Tools with C++ and Clang ^(a few GB^)...
  winget install --id Microsoft.VisualStudio.2022.BuildTools -e --accept-package-agreements --accept-source-agreements --override "--quiet --wait --norestart --add Microsoft.VisualStudio.Workload.VCTools --add Microsoft.VisualStudio.Component.VC.Llvm.Clang --add Microsoft.VisualStudio.Component.VC.Llvm.ClangToolset --includeRecommended"
)
for %%T in ("cmake Kitware.CMake" "ninja Ninja-build.Ninja" "nasm NASM.NASM") do (
  for /f "tokens=1,2" %%A in (%%T) do (
    where %%A >nul 2>nul || (
      echo Installing %%B...
      winget install --id %%B -e --accept-package-agreements --accept-source-agreements
    )
  )
)
exit /b 0

:build
set "VS="
if exist "%VSWHERE%" for /f "usebackq delims=" %%V in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Llvm.Clang -property installationPath`) do set "VS=%%V"
if not defined VS (
  echo Visual Studio with the "C++ Clang tools" component wasn't found: add it in the Visual Studio Installer.
  exit /b 1
)
echo Building ^(10-20 minutes^)...
rem winget's NASM and CMake don't always put themselves on the PATH of this window
set "PATH=%LOCALAPPDATA%\bin\NASM;%ProgramFiles%\NASM;%ProgramFiles%\CMake\bin;%PATH%"
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
cd /d "%PROJ%"
cmake --preset=Release-windows-clang || exit /b 1
cmake --build out\build\Release --parallel || exit /b 1
exit /b 0

:failed
echo.
echo Something went wrong (see the messages above). Ask for help at
echo https://github.com/stichlola/jak-enhanced-trilogy/issues
pause
exit /b 1
