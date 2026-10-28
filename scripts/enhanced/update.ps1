# Jak Enhanced Trilogy: installs a newer release over this one, then starts the launcher again.
#   update.ps1 -Root <the release folder> -Tag v0.6.2
# The launcher runs it (and quits) when the player says yes to a new version. The release ZIP holds
# only the programs and data\: the extracted discs (data\out), the ISO folder and the HD textures are
# not in it, so copying it over the folder keeps them; saves and settings live in %APPDATA%\OpenGOAL.
param([Parameter(Mandatory = $true)][string]$Root, [Parameter(Mandatory = $true)][string]$Tag)
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'  # (Windows PowerShell's progress bar slows downloads tenfold)
$Host.UI.RawUI.WindowTitle = "Jak Enhanced Trilogy: updating to $Tag"
$name = 'Jak-Enhanced-Trilogy'
$work = Join-Path $env:TEMP "jak-enhanced-update"
$launcher = Join-Path $Root 'Jak Enhanced Trilogy.bat'
try {
  # the launcher has to be gone before its own file is replaced (cmd reads a .bat as it runs it)
  Start-Sleep -Seconds 2
  if (Test-Path $work) { Remove-Item $work -Recurse -Force }
  New-Item -ItemType Directory $work | Out-Null
  $zip = Join-Path $work "$name-$Tag.zip"
  $url = "https://github.com/stichlola/jak-enhanced-trilogy/releases/download/$Tag/$name-$Tag.zip"
  Write-Host "Downloading Jak Enhanced Trilogy $Tag (about 120 MB)..."
  Invoke-WebRequest -Uri $url -OutFile $zip -UseBasicParsing
  Write-Host "Unpacking..."
  Expand-Archive -Path $zip -DestinationPath (Join-Path $work 'new') -Force
  $new = Join-Path $work "new\$name"
  if (-not (Test-Path (Join-Path $new 'gk.exe'))) { throw "the download has no gk.exe" }
  Write-Host "Installing over $Root..."
  # /E every folder, files replaced, nothing of the player's deleted; robocopy's codes below 8 are success
  & robocopy $new $Root /E /R:3 /W:2 /NFL /NDL /NJH /NJS /NP | Out-Null
  if ($LASTEXITCODE -ge 8) { throw "copying the new files failed (robocopy $LASTEXITCODE)" }
  Remove-Item $work -Recurse -Force -ErrorAction SilentlyContinue
  Write-Host "Updated to $Tag."
} catch {
  Write-Host ""
  Write-Host "The update didn't work: $($_.Exception.Message)"
  Write-Host "The game starts as it was. You can download the new version by hand from"
  Write-Host "https://github.com/stichlola/jak-enhanced-trilogy/releases/latest"
  Read-Host "Press Enter to carry on"
}
Start-Process -FilePath $launcher -WorkingDirectory $Root
