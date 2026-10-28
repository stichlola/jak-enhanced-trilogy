# Downloads the players' reports not fetched yet from the worker's KV namespace into
# local\reports\<id>\ (unzipped, with note.txt and meta.json), through the wrangler login (no
# ADMIN_KEY needed). Prints one line a new report. local\ is outside git: reports hold players' data.
#   powershell -File tools\report-worker\fetch_reports.ps1 [-Out <folder>]
param([string]$Out = '')
$ErrorActionPreference = 'Stop'
# (run through [scriptblock]::Create there is no $PSScriptRoot: then the current folder is the worker's)
$here = if ($PSScriptRoot) { $PSScriptRoot } else { (Get-Location).Path }
if (-not $Out) { $Out = Join-Path $here '..\..\local\reports' }
Push-Location $here
try {
  New-Item -ItemType Directory -Force $Out | Out-Null
  $keys = (npx wrangler kv key list --binding REPORTS --remote --prefix 'zip/' 2>$null | Out-String) | ConvertFrom-Json
  $new = 0
  foreach ($k in $keys) {
    $id = $k.name.Substring(4)
    $dir = Join-Path $Out $id
    if (Test-Path $dir) { continue }
    New-Item -ItemType Directory -Force $dir | Out-Null
    $zip = Join-Path $dir 'report.zip'
    # (through cmd's redirection: a PowerShell pipe would mangle the ZIP's bytes)
    cmd /c "npx wrangler kv key get zip/$id --binding REPORTS --remote > `"$zip`" 2>nul"
    cmd /c "npx wrangler kv key get note/$id --binding REPORTS --remote --text > `"$(Join-Path $dir 'note.txt')`" 2>nul"
    $k.metadata | ConvertTo-Json | Set-Content -Encoding UTF8 (Join-Path $dir 'meta.json')
    try { Expand-Archive $zip -DestinationPath (Join-Path $dir 'files') -Force } catch {}
    $m = $k.metadata
    Write-Host ("{0}  {1} {2} {3}  {4}" -f $id, $m.version, $m.game, $m.country, $m.note)
    $new++
  }
  Write-Host ("{0} new, {1} in all, in {2}" -f $new, @($keys).Count, (Resolve-Path $Out))
} finally {
  Pop-Location
}
