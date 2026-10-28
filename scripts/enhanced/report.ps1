# Jak Enhanced Trilogy: the problem report window. The launcher opens it by itself when gk.exe
# closes with an error (-Crash <exit code>); the Enhanced menu's "Send feedback" button and
# "Report a problem.bat" open it any time. One click sends the newest game logs, the PC's graphics
# card, driver and Windows, the games' settings and what the player wrote to the project's report
# server (tools/report-worker: private, only the developer reads them). Nothing is sent without
# the click; without a server (or offline) the ZIP lands on the Desktop to attach to a message.
# Run through [scriptblock]::Create like update.ps1, so it works whatever the execution policy.
param(
  [string]$Proj = (Get-Location).Path,  # the folder with log\ and VERSION (data\ in the release)
  [int]$Crash = 0                        # gk's exit code when it crashed, 0 for feedback
)
$ErrorActionPreference = 'SilentlyContinue'

# the report server (tools/report-worker: https://<worker>.workers.dev/report); empty until it is
# deployed (the ZIP goes to the Desktop instead); REPORT_URL in the environment overrides it
$ReportUrl = ''
if ($env:REPORT_URL) { $ReportUrl = $env:REPORT_URL }
$Issues = 'https://github.com/stichlola/jak-enhanced-trilogy/issues/new'

Add-Type -AssemblyName System.Windows.Forms, System.Drawing
[Windows.Forms.Application]::EnableVisualStyles()

$version = if (Test-Path (Join-Path $Proj 'VERSION')) { Get-Content (Join-Path $Proj 'VERSION') -TotalCount 1 } else { 'source' }
$logDir = Join-Path $Proj 'log'
$newest = Get-ChildItem $logDir -Filter 'jak*.log' | Sort-Object LastWriteTime -Descending | Select-Object -First 1
$game = if ($newest) { $newest.Name.Split('.')[0] } else { '' }

function Build-Zip([string]$note) {
  $stamp = Get-Date -Format 'yyyy-MM-dd_HH-mm-ss'
  $work = Join-Path $env:TEMP ('jak-enhanced-report-' + $stamp)
  New-Item -ItemType Directory -Force $work | Out-Null
  $note | Set-Content -Encoding UTF8 (Join-Path $work 'what-happened.txt')
  $os = Get-CimInstance Win32_OperatingSystem
  $info = @(
    'Jak Enhanced Trilogy report, ' + (Get-Date),
    'Version: ' + $version,
    $(if ($Crash) { 'Crash: gk.exe exit code ' + $Crash + ' (0x' + ('{0:X8}' -f $Crash) + ')' } else { 'Feedback (no crash)' }),
    'Windows: ' + $os.Caption + ' ' + $os.Version,
    'CPU: ' + (Get-CimInstance Win32_Processor | Select-Object -First 1).Name,
    'RAM: ' + [math]::Round((Get-CimInstance Win32_ComputerSystem).TotalPhysicalMemory / 1GB, 1) + ' GB'
  )
  foreach ($g in Get-CimInstance Win32_VideoController) { $info += 'GPU: ' + $g.Name + ', driver ' + $g.DriverVersion + ' (' + $g.DriverDate + ')' }
  $info | Set-Content -Encoding UTF8 (Join-Path $work 'system.txt')
  foreach ($g in 'jak1', 'jak2', 'jak3') {
    Get-ChildItem $logDir -Filter ($g + '.*.log') | Sort-Object LastWriteTime -Descending | Select-Object -First 2 | Copy-Item -Destination $work
    $s = Join-Path $env:APPDATA ('OpenGOAL\' + $g + '\settings')
    if (Test-Path $s) {
      $t = Join-Path $work ($g + '-settings')
      New-Item -ItemType Directory -Force $t | Out-Null
      Get-ChildItem $s -File | Where-Object { $_.Extension -ne '.bin' } | Copy-Item -Destination $t
    }
  }
  if ($Crash) {
    # Windows' own record of the crash: the faulting module and offset
    Get-WinEvent -FilterHashtable @{ LogName = 'Application'; Id = 1000; StartTime = (Get-Date).AddMinutes(-10) } |
      Where-Object { $_.Message -match 'gk\.exe' } | Select-Object -First 3 | ForEach-Object { $_.TimeCreated.ToString() + "`r`n" + $_.Message + "`r`n" } |
      Set-Content -Encoding UTF8 (Join-Path $work 'windows-crash.txt')
  }
  $zip = Join-Path $env:TEMP ('JakEnhanced-report-' + $stamp + '.zip')
  Compress-Archive -Path (Join-Path $work '*') -DestinationPath $zip -Force
  Remove-Item -Recurse -Force $work
  return $zip
}

function Send-Zip([string]$zip, [string]$note) {
  [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
  Add-Type -AssemblyName System.Net.Http
  $client = New-Object System.Net.Http.HttpClient
  $client.Timeout = [TimeSpan]::FromSeconds(60)
  $form = New-Object System.Net.Http.MultipartFormDataContent
  $form.Add((New-Object System.Net.Http.StringContent([string]$note)), 'note')
  $form.Add((New-Object System.Net.Http.StringContent([string]$version)), 'version')
  $form.Add((New-Object System.Net.Http.StringContent([string]$game)), 'game')
  $form.Add((New-Object System.Net.Http.ByteArrayContent(, [IO.File]::ReadAllBytes($zip))), 'report', 'report.zip')
  $answer = $client.PostAsync($ReportUrl, $form).Result
  return $answer.IsSuccessStatusCode
}

# the window
$f = New-Object Windows.Forms.Form
$f.Text = 'Jak Enhanced Trilogy'
$f.Size = New-Object Drawing.Size(500, 360)
$f.StartPosition = 'CenterScreen'
$f.FormBorderStyle = 'FixedDialog'
$f.MaximizeBox = $false
$f.TopMost = $true
$f.Font = New-Object Drawing.Font('Segoe UI', 10)

$title = New-Object Windows.Forms.Label
$title.Location = New-Object Drawing.Point(16, 14)
$title.Size = New-Object Drawing.Size(460, 52)
$title.Font = New-Object Drawing.Font('Segoe UI', 12, [Drawing.FontStyle]::Bold)
$title.Text = if ($Crash) { 'The game closed because of an error. Send a report so it can be fixed?' } else { 'Send feedback or report a problem' }
$f.Controls.Add($title)

$ask = New-Object Windows.Forms.Label
$ask.Location = New-Object Drawing.Point(16, 70)
$ask.Size = New-Object Drawing.Size(460, 22)
$ask.Text = if ($Crash) { 'What were you doing? (optional)' } else { 'What happened, or what would you like changed?' }
$f.Controls.Add($ask)

$box = New-Object Windows.Forms.TextBox
$box.Location = New-Object Drawing.Point(16, 94)
$box.Size = New-Object Drawing.Size(452, 120)
$box.Multiline = $true
$box.ScrollBars = 'Vertical'
$f.Controls.Add($box)

$small = New-Object Windows.Forms.Label
$small.Location = New-Object Drawing.Point(16, 220)
$small.Size = New-Object Drawing.Size(460, 40)
$small.Font = New-Object Drawing.Font('Segoe UI', 8.5)
$small.ForeColor = [Drawing.Color]::DimGray
$small.Text = 'Sent with it: the newest game logs (they show your Windows user name), graphics card, driver, Windows version and the games'' settings. Only the developer sees it.'
$f.Controls.Add($small)

$send = New-Object Windows.Forms.Button
$send.Location = New-Object Drawing.Point(262, 270)
$send.Size = New-Object Drawing.Size(100, 34)
$send.Text = 'Send'
$f.Controls.Add($send)
$f.AcceptButton = $null  # Enter makes a new line in the box

$no = New-Object Windows.Forms.Button
$no.Location = New-Object Drawing.Point(368, 270)
$no.Size = New-Object Drawing.Size(100, 34)
$no.Text = if ($Crash) { 'No thanks' } else { 'Cancel' }
$no.Add_Click({ $f.Close() })
$f.Controls.Add($no)
$f.CancelButton = $no

$send.Add_Click({
  if (-not $Crash -and -not $box.Text.Trim()) {
    $ask.ForeColor = [Drawing.Color]::Firebrick
    return
  }
  $send.Enabled = $false; $no.Enabled = $false; $box.Enabled = $false
  $title.Text = 'Sending...'
  $f.Refresh()
  $zip = Build-Zip $box.Text
  $ok = $false
  if ($ReportUrl) { try { $ok = Send-Zip $zip $box.Text } catch { $ok = $false } }
  if ($ok) {
    Remove-Item $zip
    [Windows.Forms.MessageBox]::Show($f, 'Thanks! The report was sent.', 'Jak Enhanced Trilogy') | Out-Null
  } else {
    # offline or no server: the ZIP goes on the Desktop, and the GitHub page opens to attach it
    $desk = Join-Path ([Environment]::GetFolderPath('Desktop')) (Split-Path $zip -Leaf)
    Move-Item $zip $desk -Force
    [Windows.Forms.MessageBox]::Show($f, "It couldn't be sent from here. The report is on your Desktop:`r`n" + (Split-Path $desk -Leaf) + "`r`n`r`nThe GitHub page opens now: drag the ZIP into a new issue.", 'Jak Enhanced Trilogy') | Out-Null
    Start-Process explorer.exe ('/select,"' + $desk + '"')
    Start-Process $Issues
  }
  $f.Close()
})

$f.Add_Shown({ $f.Activate(); $box.Focus() })
[void]$f.ShowDialog()
