# Shadow copy of the build's bin folder, so a running gk/goalc never locks out\build\Release\bin
# and build.bat can always relink it. Prints the new bin folder (gk.exe, goalc.exe, the DLLs).
#
#   $bin = & ([scriptblock]::Create([IO.File]::ReadAllText('<repo>\scripts\enhanced\dev_bin.ps1'))) -Tag gk1
#
# Copies go to <main tree>\local\run\<stamp>-<tag>\bin (~63 MB, ~0.05 s). Old copies are pruned on
# every call: a run dir is deleted when no running gk/goalc was started from it and it is older than
# -MaxAgeMinutes, or when more than -Keep dirs are idle. A dir still in use is never touched.
# dev_launch.ps1 keeps a test game's config dir in the same run dir (<run dir>\cfg).
#   -Tag   short name for the dir (gk1, goalc1, user, ...)
#   -From  bin folder to copy (default: the main tree's out\build\Release\bin)
#   -PruneOnly  only prune, copy nothing
param(
  [string]$Tag = 'run',
  [string]$From = '',
  [string]$Repo = '',
  [int]$Keep = 5,
  [int]$MaxAgeMinutes = 60,
  [switch]$PruneOnly
)
$ErrorActionPreference = 'Stop'
if (-not $Repo) {
  $Repo = if ($PSScriptRoot) { Split-Path (Split-Path $PSScriptRoot) } else { (Get-Location).Path }
}
if (-not (Test-Path (Join-Path $Repo 'scripts\enhanced'))) { throw "-Repo: not a jak-enhanced tree: $Repo" }
$main = Split-Path (& git -C $Repo rev-parse --path-format=absolute --git-common-dir)
# a worktree with its own build runs its own gk (its C++ change), else the main tree's
if (-not $From) {
  $own = Join-Path $Repo 'out\build\Release\bin'
  $From = if (Test-Path (Join-Path $own 'gk.exe')) { $own } else { Join-Path $main 'out\build\Release\bin' }
}
$runRoot = Join-Path $main 'local\run'
New-Item -ItemType Directory -Force $runRoot | Out-Null

# --- prune --------------------------------------------------------------------------------
$inUse = @{}
foreach ($p in @(Get-Process gk, goalc -ErrorAction SilentlyContinue)) {
  try { if ($p.Path) { $inUse[(Split-Path (Split-Path $p.Path))] = $true } } catch {}
}
$dirs = @(Get-ChildItem $runRoot -Directory -ErrorAction SilentlyContinue |
  Where-Object { Test-Path (Join-Path $_.FullName 'bin') } | Sort-Object LastWriteTime -Descending)
$idle = 0
foreach ($d in $dirs) {
  if ($inUse.ContainsKey($d.FullName)) { continue }
  $idle++
  $old = $d.LastWriteTime -lt (Get-Date).AddMinutes(-$MaxAgeMinutes)
  if ($old -or $idle -gt $Keep) {
    try { Remove-Item -LiteralPath $d.FullName -Recurse -Force -ErrorAction Stop } catch {}
  }
}
if ($PruneOnly) { return }

# --- copy ---------------------------------------------------------------------------------
if (-not (Test-Path (Join-Path $From 'gk.exe'))) { throw "no gk.exe in $From" }
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss-fff'
$dst = Join-Path $runRoot "$stamp-$Tag\bin"
New-Item -ItemType Directory -Force $dst | Out-Null
$files = @(Get-ChildItem $From -File | Where-Object {
    ($_.Name -in 'gk.exe', 'goalc.exe') -or ($_.Extension -eq '.dll') })
foreach ($f in $files) { [IO.File]::Copy($f.FullName, (Join-Path $dst $f.Name), $true) }
$dst
