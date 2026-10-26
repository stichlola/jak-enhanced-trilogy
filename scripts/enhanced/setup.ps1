# Jak 3 Enhanced: one-command setup on Windows.
#
#   powershell -ExecutionPolicy Bypass -File scripts\enhanced\setup.ps1 -Iso "D:\Games\Jak 3.iso"
#
# 1. installs what's missing (Visual Studio 2022 Build Tools with Clang, CMake, Ninja, NASM) with winget
# 2. builds the game, the compiler and the extractor (Release, clang-cl)
# 3. extracts your ISO, decompiles its data and compiles the game (OpenGOAL's extractor: 20-40 min)
# 4. writes Play-Jak3-Enhanced.bat in the repository folder
#
# -SkipTools / -SkipBuild / -SkipExtract skip a step (to rebuild after a code change: -SkipTools -SkipExtract).

param(
  [string]$Iso = "",
  [switch]$SkipTools,
  [switch]$SkipBuild,
  [switch]$SkipExtract
)
$ErrorActionPreference = "Stop"
$root = (Resolve-Path "$PSScriptRoot\..\..").Path
Write-Host "Jak 3 Enhanced setup in $root"

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
  if (-not $Iso) { throw "Pass your Jak 3 ISO: -Iso `"<path to your own copy>`"" }
  Write-Host "Extracting, decompiling and compiling the game from $Iso (20-40 min)..."
  & "$root\out\build\Release\bin\extractor.exe" --game jak3 --proj-path "$root" -e -d -c "$Iso"
  if ($LASTEXITCODE -ne 0) { throw "Extraction failed (exit $LASTEXITCODE)" }
  # the REPL and `goalc --cmd` use the folder the extractor made (jak3, jak3_pal...)
  $folder = Get-ChildItem "$root\iso_data" -Directory | Where-Object { $_.Name -like "jak3*" } | Select-Object -First 1
  if ($folder) {
    New-Item -ItemType Directory -Force "$root\goal_src\user\player" | Out-Null
    Set-Content -Encoding ascii "$root\goal_src\user\user.txt" "player"
    Set-Content -Encoding ascii "$root\goal_src\user\player\repl-config.json" "{`"jak3`": {`"gameVersionFolder`": `"$($folder.Name)`"}}"
  }
}

$play = Join-Path $root "Play-Jak3-Enhanced.bat"
Set-Content -Encoding ascii $play @"
@echo off
rem Jak 3 Enhanced: the full game. Press \ in the game for the Enhanced menu.
cd /d "$root"
start "" out\build\Release\bin\gk.exe --game jak3 -- -boot -fakeiso
"@
Write-Host ""
Write-Host "Done. Play with: $play"
