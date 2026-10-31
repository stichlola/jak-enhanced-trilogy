# Jak Enhanced Trilogy: installs a newer release over this one, then starts the launcher again.
#   update.ps1 -Root <the release folder> -Tag v0.6.2
#   update.ps1 -Root <the release folder> -CheckOnly    exit code 10 if the game is open from there, else 0
#   update.ps1 -Root <the release folder> -CloseOnly    closes it without asking; 0 closed, 11 couldn't
# The launcher runs it (and quits) when the player says yes to a new version; the installer uses the
# last two (it asks in its own window). The release ZIP holds only the programs and data\: the
# extracted discs (data\out), the ISO folder and the HD textures are not in it, so copying it over the
# folder keeps them, and what the player can edit (the main world) comes in data\shipped, for the
# launcher to apply file by file (apply_shipped.ps1); saves and settings live in %APPDATA%\OpenGOAL.
param([Parameter(Mandatory = $true)][string]$Root, [string]$Tag, [switch]$CheckOnly, [switch]$CloseOnly)
$ErrorActionPreference = 'Stop'
$Root = $Root.TrimEnd('\')

# what runs from this install: every program inside the folder, and the launcher's windows and the
# hidden PowerShell that waits for the game to open the report window, which name it on their command line
function Get-InstallProcesses {
  $inside = $Root + '\'
  $programs = @(); $helpers = @()
  foreach ($p in Get-CimInstance Win32_Process) {
    if ($p.ProcessId -eq $PID) { continue }
    if ($p.ExecutablePath -and $p.ExecutablePath.StartsWith($inside, [StringComparison]::OrdinalIgnoreCase)) {
      $programs += $p
    } elseif ($p.Name -in 'cmd.exe', 'powershell.exe' -and $p.CommandLine) {
      # (the launcher writes an apostrophe of the path doubled)
      $line = $p.CommandLine.Replace("''", "'")
      if ($line.IndexOf($inside, [StringComparison]::OrdinalIgnoreCase) -ge 0 -and
          $line -match 'Jak Enhanced Trilogy\.bat|Report a problem\.bat|report\.ps1|gk\.exe') {
        $helpers += $p
      }
    }
  }
  return @{ Programs = $programs; Helpers = $helpers }
}

function Wait-Gone([int[]]$ids, [int]$seconds) {
  $until = (Get-Date).AddSeconds($seconds)
  while ((Get-Date) -lt $until) {
    if (-not (Get-Process -Id $ids -ErrorAction SilentlyContinue)) { return $true }
    Start-Sleep -Milliseconds 300
  }
  return -not (Get-Process -Id $ids -ErrorAction SilentlyContinue)
}

# closes all of it: true when nothing is left. First what would answer the game's exit with a window of
# its own, then the game by asking its window to close, and by force only if it doesn't
function Close-Install {
  $found = Get-InstallProcesses
  foreach ($h in $found.Helpers) { Stop-Process -Id $h.ProcessId -Force -ErrorAction SilentlyContinue }
  $ids = @($found.Programs | ForEach-Object { [int]$_.ProcessId })
  if ($ids.Count) {
    foreach ($id in $ids) {
      $proc = Get-Process -Id $id -ErrorAction SilentlyContinue
      if ($proc -and $proc.MainWindowHandle -ne [IntPtr]::Zero) { [void]$proc.CloseMainWindow() }
    }
    if (-not (Wait-Gone $ids 15)) {
      Stop-Process -Id $ids -Force -ErrorAction SilentlyContinue
      [void](Wait-Gone $ids 10)
    }
  }
  $left = Get-InstallProcesses
  return ($left.Programs.Count + $left.Helpers.Count) -eq 0
}

if ($CheckOnly) {
  $found = Get-InstallProcesses
  if ($found.Programs.Count + $found.Helpers.Count) { exit 10 }
  exit 0
}
if ($CloseOnly) {
  if (Close-Install) { exit 0 }
  exit 11
}
if (-not $Tag) { throw 'update.ps1: -Tag is needed' }

function Write-Line([string]$text) { Write-Host ("`r" + $text.PadRight(70)) -NoNewline }

function Test-Room([string]$path, [long]$bytes, [string]$what) {
  try { $free = (New-Object IO.DriveInfo ([IO.Path]::GetPathRoot($path))).AvailableFreeSpace } catch { return }
  if ($free -lt $bytes) {
    throw ("not enough room for {0} on {1}: it takes {2} MB, there are {3} MB free" -f
           $what, [IO.Path]::GetPathRoot($path), [int]($bytes / 1MB), [int]($free / 1MB))
  }
}

# with its progress; a connection that ends early is an error here, not a broken ZIP later
function Get-Download([string]$url, [string]$file) {
  $request = [Net.HttpWebRequest]::Create($url)
  $request.UserAgent = 'jak-enhanced-update'
  try {
    $response = $request.GetResponse()
  } catch {
    $web = $_.Exception.InnerException
    if ($web -and $web.Response) { throw "the server doesn't have this version's file (HTTP $([int]$web.Response.StatusCode))" }
    throw "couldn't reach the server: check your connection"
  }
  try {
    $total = $response.ContentLength
    if ($total -gt 0) {
      Test-Room $file ($total * 3) 'the download and its unpacked files'
      Test-Room $Root ($total * 2) 'the new files'
      Write-Host ("Downloading Jak Enhanced Trilogy {0} ({1} MB)..." -f $Tag, [int]($total / 1MB))
    } else {
      Write-Host "Downloading Jak Enhanced Trilogy $Tag..."
    }
    $in = $response.GetResponseStream()
    $out = [IO.File]::Create($file)
    try {
      $buffer = New-Object byte[] (1MB)
      $done = [long]0
      $clock = [Diagnostics.Stopwatch]::StartNew()
      $shown = 0
      while (($n = $in.Read($buffer, 0, $buffer.Length)) -gt 0) {
        $out.Write($buffer, 0, $n)
        $done += $n
        if ($clock.ElapsedMilliseconds - $shown -ge 500) {
          $shown = $clock.ElapsedMilliseconds
          $speed = $done / 1MB / [Math]::Max($clock.Elapsed.TotalSeconds, 0.1)
          if ($total -gt 0) {
            Write-Line ("  {0} of {1} MB ({2}%), {3:0.0} MB/s" -f [int]($done / 1MB), [int]($total / 1MB), [int](100 * $done / $total), $speed)
          } else {
            Write-Line ("  {0} MB, {1:0.0} MB/s" -f [int]($done / 1MB), $speed)
          }
        }
      }
    } finally {
      $out.Close()
      $in.Close()
    }
    Write-Host ''
    if ($total -gt 0 -and $done -ne $total) {
      throw ("the download stopped early: {0} of {1} MB" -f [int]($done / 1MB), [int]($total / 1MB))
    }
  } finally {
    $response.Close()
  }
}

function Expand-WithProgress([string]$zipFile, [string]$destination) {
  Add-Type -AssemblyName System.IO.Compression.FileSystem
  try {
    $zip = [IO.Compression.ZipFile]::OpenRead($zipFile)
  } catch {
    throw "the downloaded file is damaged"
  }
  try {
    $bytes = [long]0
    foreach ($e in $zip.Entries) { $bytes += $e.Length }
    Test-Room $destination $bytes 'the unpacked files'
    $base = [IO.Path]::GetFullPath($destination) + '\'
    $count = $zip.Entries.Count
    $i = 0
    foreach ($e in $zip.Entries) {
      $i++
      $target = [IO.Path]::GetFullPath((Join-Path $base $e.FullName))
      if (-not $target.StartsWith($base, [StringComparison]::OrdinalIgnoreCase)) { throw "a bad path in the ZIP: $($e.FullName)" }
      if ($e.FullName.EndsWith('/')) {
        [void][IO.Directory]::CreateDirectory($target)
      } else {
        [void][IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($target))
        [IO.Compression.ZipFileExtensions]::ExtractToFile($e, $target, $true)
      }
      if ($i % 100 -eq 0 -or $i -eq $count) { Write-Line ("  {0} of {1} files" -f $i, $count) }
    }
    Write-Host ''
  } finally {
    $zip.Dispose()
  }
}

# robocopy's codes below 8 are success
function Invoke-Copy([string]$from, [string]$to) {
  $count = 0
  # /E every folder, files replaced, nothing of the player's deleted; data\VERSION comes last, below
  & robocopy $from $to /E /XF VERSION /R:3 /W:2 /NDL /NJH /NJS /NP | ForEach-Object {
    $count++
    if ($count % 50 -eq 0) { Write-Line ("  {0} files" -f $count) }
  }
  $code = $LASTEXITCODE
  Write-Line ("  {0} files" -f $count)
  Write-Host ''
  if ($code -ge 8) { throw "copying the new files failed (robocopy $code)" }
}

$copying = $false
$cancelled = $false
$work = Join-Path $env:TEMP 'jak-enhanced-update'
$name = 'Jak-Enhanced-Trilogy'
$launcher = Join-Path $Root 'Jak Enhanced Trilogy.bat'
try {
  try { $Host.UI.RawUI.WindowTitle = "Jak Enhanced Trilogy: updating to $Tag" } catch { }
  # GitHub only answers over TLS 1.2 or newer
  [Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12
  # the launcher has to be gone before its own file is replaced (cmd reads a .bat as it runs it)
  Start-Sleep -Seconds 2
  if (Test-Path -LiteralPath $work) { Remove-Item -LiteralPath $work -Recurse -Force }
  [void](New-Item -ItemType Directory $work)
  $zip = Join-Path $work "$name-$Tag.zip"
  $url = "https://github.com/stichlola/jak-enhanced-trilogy/releases/download/$Tag/$name-$Tag.zip"
  Get-Download $url $zip
  Write-Host 'Unpacking...'
  $unpacked = Join-Path $work 'new'
  Expand-WithProgress $zip $unpacked
  $new = Join-Path $unpacked $name
  if (-not (Test-Path -LiteralPath (Join-Path $new 'gk.exe'))) { throw 'the download has no gk.exe' }

  # nothing may run from the folder while it is replaced
  $found = Get-InstallProcesses
  if ($found.Programs.Count + $found.Helpers.Count) {
    $names = @($found.Programs | ForEach-Object { $_.Name }) + @($found.Helpers | ForEach-Object { 'the launcher' }) | Select-Object -Unique
    Write-Host ''
    Write-Host "Still open: $($names -join ', ')."
    Write-Host 'They have to close before the update. Save your game first: what is not saved is lost.'
    $answer = Read-Host 'Press Enter to close them and update, or type N and Enter to cancel'
    if ($answer -match '^\s*[nN]') {
      $cancelled = $true
      throw 'you cancelled it, the game stays open'
    }
    Write-Host 'Closing...'
    if (-not (Close-Install)) { throw 'the game could not be closed: close it by hand' }
  }
  # (a program's file can still be held for a moment, or by something else: before anything is copied)
  foreach ($exe in Get-ChildItem -LiteralPath $Root -Filter *.exe) {
    try {
      ([IO.File]::Open($exe.FullName, 'Open', 'ReadWrite', 'None')).Close()
    } catch {
      throw "$($exe.Name) is in use by another program: close it and try again"
    }
  }

  Write-Host "Installing over $Root..."
  $copying = $true
  Invoke-Copy $new $Root
  # data\VERSION last: an update that stops before this is offered again
  Copy-Item -LiteralPath (Join-Path $new 'data\VERSION') -Destination (Join-Path $Root 'data\VERSION') -Force
  Write-Host "Updated to $Tag."
} catch {
  Write-Host ''
  Write-Host "The update didn't work: $($_.Exception.Message)"
  if ($copying) {
    Write-Host 'It stopped while copying: some files are new, others are still the old ones. Your extracted'
    Write-Host 'games, saves and settings are fine. Start the launcher again and press U to finish it, or'
  } else {
    Write-Host 'Nothing was changed: the game is as it was. You can try again at the next start, or'
  }
  Write-Host 'download the new version by hand from https://github.com/stichlola/jak-enhanced-trilogy/releases/latest'
  Read-Host 'Press Enter to carry on'
} finally {
  # (a failed download doesn't stay on the disk)
  if (Test-Path -LiteralPath $work) { Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue }
}
# (the game the player chose to keep open isn't started a second time)
if (-not $cancelled) { Start-Process -FilePath $launcher -WorkingDirectory $Root }
