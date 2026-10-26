# Jak Enhanced Trilogy: one-command setup on Windows.
#
#   powershell -ExecutionPolicy Bypass -File scripts\enhanced\setup.ps1 -Jak1Iso "D:\Games\Jak 1.iso" `
#     -Jak2Iso "D:\Games\Jak 2.iso" -Jak3Iso "D:\Games\Jak 3.iso"
#
# Any of the three discs is enough (-Iso is Jak 3, as before).
# 1. installs what's missing (Visual Studio 2022 Build Tools with Clang, CMake, Ninja, NASM) with winget
# 2. builds the game, the compiler and the extractor (Release, clang-cl)
# 3. extracts your ISOs, decompiles their data and compiles the games (OpenGOAL's extractor: 20-40
#    min a game)
# 4. writes Play-Jak1/2/3-Enhanced.bat for each game, and Play-Jak-Enhanced-Trilogy.bat (starts from
#    the first game you have; F9 in the game switches to the next one)
#
# -SkipTools / -SkipBuild / -SkipExtract skip a step (to rebuild after a code change: -SkipTools -SkipExtract).

param(
  [string]$Iso = "",
  [string]$Jak1Iso = "",
  [string]$Jak2Iso = "",
  [string]$Jak3Iso = "",
  [switch]$SkipTools,
  [switch]$SkipBuild,
  [switch]$SkipExtract
)
$ErrorActionPreference = "Stop"
$root = (Resolve-Path "$PSScriptRoot\..\..").Path
Write-Host "Jak Enhanced Trilogy setup in $root"
if ($Iso -and -not $Jak3Iso) { $Jak3Iso = $Iso }
$isos = [ordered]@{ jak1 = $Jak1Iso; jak2 = $Jak2Iso; jak3 = $Jak3Iso }

# The OpenGOAL tools find the project by a folder named jak-project in their path
if ((Split-Path $root -Leaf) -ne "jak-project") {
  $link = Join-Path (Split-Path $root -Parent) "jak-project"
  if (-not (Test-Path $link)) {
    Write-Host "Linking $link -> $root (the tools look for a folder named jak-project)"
    cmd /c mklink /J "$link" "$root" | Out-Null
  }
  $root = $link
}

function Find-VsDevCmd {
  $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
  if (-not (Test-Path $vswhere)) { return $null }
  $vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Llvm.Clang -property installationPath
  if (-not $vs) { return $null }
  return Join-Path $vs "VC\Auxiliary\Build\vcvars64.bat"
}

if (-not $SkipTools) {
  if (-not (Find-VsDevCmd)) {
    Write-Host "Installing Visual Studio 2022 Build Tools with C++ and Clang (a few GB)..."
    winget install --id Microsoft.VisualStudio.2022.BuildTools -e --accept-package-agreements --accept-source-agreements `
      --override "--quiet --wait --norestart --add Microsoft.VisualStudio.Workload.VCTools --add Microsoft.VisualStudio.Component.VC.Llvm.Clang --add Microsoft.VisualStudio.Component.VC.Llvm.ClangToolset --includeRecommended"
  }
  foreach ($tool in @(@("cmake", "Kitware.CMake"), @("ninja", "Ninja-build.Ninja"), @("nasm", "NASM.NASM"), @("git", "Git.Git"))) {
    if (-not (Get-Command $tool[0] -ErrorAction SilentlyContinue)) {
      Write-Host "Installing $($tool[1])..."
      winget install --id $tool[1] -e --accept-package-agreements --accept-source-agreements
    }
  }
}

$vcvars = Find-VsDevCmd
if (-not $vcvars) { throw "Visual Studio with the Clang component not found: run without -SkipTools, or add 'C++ Clang tools' in the Visual Studio Installer." }
# NASM's winget package doesn't put itself on the PATH
$nasmDirs = @("$env:LOCALAPPDATA\bin\NASM", "$env:ProgramFiles\NASM") | Where-Object { Test-Path $_ }
$extraPath = ($nasmDirs -join ";")

if (-not $SkipBuild) {
  Write-Host "Building (10-20 min the first time)..."
  $cmd = "call `"$vcvars`" >nul && set `"PATH=$extraPath;%PATH%`" && cd /d `"$root`" && cmake --preset=Release-windows-clang && cmake --build out\build\Release --parallel"
  cmd /c $cmd
  if ($LASTEXITCODE -ne 0) { throw "Build failed (exit $LASTEXITCODE)" }
}

if (-not $SkipExtract) {
  if (-not ($isos.Values | Where-Object { $_ })) {
    throw "Pass at least one of your discs: -Jak1Iso / -Jak2Iso / -Jak3Iso `"<path to your own copy>`""
  }
  $repl = @{}
  foreach ($game in $isos.Keys) {
    $gameIso = $isos[$game]
    if (-not $gameIso) { continue }
    Write-Host "Extracting, decompiling and compiling $game from $gameIso (20-40 min)..."
    & "$root\out\build\Release\bin\extractor.exe" --game $game --proj-path "$root" -e -d -c "$gameIso"
    if ($LASTEXITCODE -ne 0) { throw "Extraction of $game failed (exit $LASTEXITCODE)" }
    # the REPL and `goalc --cmd` use the folder the extractor made (jak3, jak3_pal...)
    $folder = Get-ChildItem "$root\iso_data" -Directory | Where-Object { $_.Name -like "$game*" } | Select-Object -First 1
    if ($folder) { $repl[$game] = $folder.Name }
  }
  if ($repl.Count -gt 0) {
    New-Item -ItemType Directory -Force "$root\goal_src\user\player" | Out-Null
    Set-Content -Encoding ascii "$root\goal_src\user\user.txt" "player"
    $entries = ($repl.Keys | Sort-Object | ForEach-Object { "`"$_`": {`"gameVersionFolder`": `"$($repl[$_])`"}" }) -join ", "
    Set-Content -Encoding ascii "$root\goal_src\user\player\repl-config.json" "{$entries}"
  }
}

# a launcher for each game that's been extracted, and one for the trilogy
$have = @("jak1", "jak2", "jak3") | Where-Object { Test-Path "$root\out\$_\iso" }
foreach ($game in $have) {
  $n = $game.Substring(3)
  Set-Content -Encoding ascii (Join-Path $root "Play-Jak$n-Enhanced.bat") @"
@echo off
rem Jak Enhanced Trilogy: Jak $n. Press \ in the game for the Enhanced menu.
cd /d "$root"
start "" out\build\Release\bin\gk.exe --game $game -- -boot -fakeiso
"@
}
if ($have) {
  $play = Join-Path $root "Play-Jak-Enhanced-Trilogy.bat"
  Set-Content -Encoding ascii $play @"
@echo off
rem Jak Enhanced Trilogy: from the first game you have. F9 in the game switches to the next one.
cd /d "$root"
start "" out\build\Release\bin\gk.exe --game $($have[0]) -- -boot -fakeiso
"@
  Write-Host ""
  Write-Host "Done. Play with: $play (or Play-Jak1/2/3-Enhanced.bat)"
} else {
  Write-Host ""
  Write-Host "Built. No game extracted yet: run again with -Jak1Iso / -Jak2Iso / -Jak3Iso."
}
