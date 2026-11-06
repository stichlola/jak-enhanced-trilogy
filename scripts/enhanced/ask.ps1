# Jak Enhanced Trilogy: the launcher's questions, in a window instead of the console.
#   ask.ps1 -Update -Latest v0.14.5 -Mine v0.14.0 -MB 19 [-Icon <the .ico>]   update now or later
#   ask.ps1 -Hd -Game 2 [-Icon <the .ico>]                                    the HD textures now?
#   ask.ps1 -ExportIcons <folder>     the donation icons as .bmp for the installer (game\assets\enhanced\art)
# Exit code 10 yes ("Update now", "Make them now"), 11 no ("Later", "Not now", the window closed),
# anything else the window couldn't be shown: the launcher then asks in the console, or takes no.
# The update question answers "Later" by itself after 15 seconds (as the console's did after 10), so
# the game still starts with nobody at the keyboard; the buy-me-a-coffee buttons under it stop the clock.
param([switch]$Update, [string]$Latest, [string]$Mine, [string]$MB,
      [switch]$Hd, [string]$Game, [string]$Icon, [string]$ExportIcons)
try { Add-Type -AssemblyName System.Windows.Forms, System.Drawing } catch { exit 2 }

# PayPal's colours, and the donation buttons: icon, amount, what it buys, the PayPal.Me page
$PayPal = [Drawing.Color]::FromArgb(0x00, 0x70, 0xBA)
$PayPalDark = [Drawing.Color]::FromArgb(0x00, 0x30, 0x87)
$Euro = [string][char]0x20AC
$Dash = [string][char]0x2014
$Donations = @(
  @('sweet', "1 $Euro $Dash a sweet", 'https://paypal.me/mymusicminis/1EUR'),
  @('coffee', "3 $Euro $Dash a coffee", 'https://paypal.me/mymusicminis/3EUR'),
  @('heart', 'Any amount', 'https://paypal.me/mymusicminis'))
$Trust = "Secure payment on PayPal's own page (paypal.me/mymusicminis), to Code & Craft Solutions, " +
         "the author's account. You can change the amount before paying."

# a small icon drawn here (emoji show as empty boxes in WinForms): white on the button, the lock grey;
# drawn on a 16x16 grid scaled to $size, on $back (transparent when not given)
function New-DonateIcon([string]$kind, [int]$size, $back = $null) {
  $bmp = New-Object Drawing.Bitmap $size, $size
  $g = [Drawing.Graphics]::FromImage($bmp)
  $g.SmoothingMode = 'AntiAlias'
  if ($back) { $g.Clear($back) } else { $g.Clear([Drawing.Color]::Transparent) }
  $g.ScaleTransform($size / 16.0, $size / 16.0)
  $white = [Drawing.Brushes]::White
  $pen = New-Object Drawing.Pen ([Drawing.Color]::White), 1.4
  $cup = {
    param([float]$x, [float]$y, [float]$k)
    $pts = [Drawing.PointF[]]@((New-Object Drawing.PointF ($x + 1 * $k), ($y + 5 * $k)),
                               (New-Object Drawing.PointF ($x + 9 * $k), ($y + 5 * $k)),
                               (New-Object Drawing.PointF ($x + 8 * $k), ($y + 13 * $k)),
                               (New-Object Drawing.PointF ($x + 2 * $k), ($y + 13 * $k)))
    $g.FillPolygon($white, $pts)
    $p = New-Object Drawing.Pen ([Drawing.Color]::White), (1.3 * $k)
    $g.DrawEllipse($p, $x + 7.3 * $k, $y + 6.3 * $k, 3.6 * $k, 3.6 * $k)
    $g.DrawBezier($p, $x + 3.5 * $k, $y + 3.8 * $k, $x + 2.5 * $k, $y + 2.5 * $k, $x + 4.5 * $k, $y + 1.5 * $k, $x + 3.5 * $k, $y + 0.2 * $k)
    $g.DrawBezier($p, $x + 6.5 * $k, $y + 3.8 * $k, $x + 5.5 * $k, $y + 2.5 * $k, $x + 7.5 * $k, $y + 1.5 * $k, $x + 6.5 * $k, $y + 0.2 * $k)
  }
  switch ($kind) {
    'sweet' {
      $g.FillEllipse($white, 4.5, 4.5, 7, 7)
      $g.FillPolygon($white, [Drawing.PointF[]]@((New-Object Drawing.PointF 5, 8), (New-Object Drawing.PointF 0.5, 4.5), (New-Object Drawing.PointF 0.5, 11.5)))
      $g.FillPolygon($white, [Drawing.PointF[]]@((New-Object Drawing.PointF 11, 8), (New-Object Drawing.PointF 15.5, 4.5), (New-Object Drawing.PointF 15.5, 11.5)))
      $stripe = New-Object Drawing.Pen ([Drawing.Color]::FromArgb(0xE0, 0x40, 0x70)), 1.2
      $g.DrawLine($stripe, 6.5, 10.5, 9.5, 5.5)
    }
    'coffee' { & $cup 2.5 1.5 1.0 }
    'heart' {
      $g.FillEllipse($white, 1.5, 3, 6.8, 6.8); $g.FillEllipse($white, 7.7, 3, 6.8, 6.8)
      $g.FillPolygon($white, [Drawing.PointF[]]@((New-Object Drawing.PointF 1.9, 7.8), (New-Object Drawing.PointF 14.1, 7.8), (New-Object Drawing.PointF 8, 14.5)))
    }
    'lock' {
      $grey = [Drawing.Color]::FromArgb(0x60, 0x60, 0x60)
      $g.DrawArc((New-Object Drawing.Pen $grey, 1.8), 4.5, 1.5, 7, 8, 180, 180)
      $g.DrawLine((New-Object Drawing.Pen $grey, 1.8), 4.5, 5.5, 4.5, 7.5); $g.DrawLine((New-Object Drawing.Pen $grey, 1.8), 11.5, 5.5, 11.5, 7.5)
      $g.FillRectangle((New-Object Drawing.SolidBrush $grey), 2.5, 7, 11, 8)
    }
  }
  $g.Dispose()
  return $bmp
}

try {
  if ($ExportIcons) {
    # 32x32, on the button's blue (the lock on the installer's white page)
    foreach ($d in $Donations) {
      (New-DonateIcon $d[0] 32 $PayPal).Save((Join-Path $ExportIcons ('donate-' + $d[0] + '.bmp')), [Drawing.Imaging.ImageFormat]::Bmp)
    }
    (New-DonateIcon 'lock' 32 ([Drawing.Color]::White)).Save((Join-Path $ExportIcons 'donate-lock.bmp'), [Drawing.Imaging.ImageFormat]::Bmp)
    exit 0
  }
  [Windows.Forms.Application]::EnableVisualStyles()
  if ($Update) {
    $message = ("A new version of Jak Enhanced Trilogy is out: {0} (you have {1}).`n`n" +
                "Updating downloads {2} MB. Your games and saves are kept.") -f $Latest, $Mine, $MB
    $yesText = 'Update now'; $noText = 'Later'; $yesFirst = $false
  } elseif ($Hd) {
    $name = @{ '1' = 'Jak 1'; '2' = 'Jak II'; '3' = 'Jak 3' }[$Game]
    $message = ("HD textures for {0}: every texture of the game redrawn 4x by an AI (Real-ESRGAN) on your " +
                "graphics card, from your own disc.`n`nIt takes a while: about 30 minutes for Jak 1, 1-2 hours " +
                "for Jak II or Jak 3 (depends on the graphics card), and 1-4 GB more on disk. The game works " +
                "the same without them. To be asked again, delete data\out\jak{1}\hd-textures.asked.`n`n" +
                "Make the HD textures for {0} now?") -f $name, $Game
    $yesText = 'Make them now'; $noText = 'Not now'; $yesFirst = $false
  } else {
    exit 2
  }

  $form = New-Object Windows.Forms.Form
  $form.Text = 'Jak Enhanced Trilogy'
  $form.FormBorderStyle = 'FixedDialog'
  $form.MaximizeBox = $false
  $form.MinimizeBox = $false
  $form.StartPosition = 'CenterScreen'
  $form.TopMost = $true
  $form.AutoSize = $true
  $form.AutoSizeMode = 'GrowAndShrink'
  $form.Padding = New-Object Windows.Forms.Padding 12
  $form.Font = [Drawing.SystemFonts]::MessageBoxFont
  if ($Icon -and (Test-Path -LiteralPath $Icon)) { try { $form.Icon = New-Object Drawing.Icon $Icon } catch { } }

  $layout = New-Object Windows.Forms.TableLayoutPanel
  $layout.AutoSize = $true
  $layout.Location = New-Object Drawing.Point 12, 12
  $layout.ColumnCount = 1
  $text = New-Object Windows.Forms.Label
  $text.AutoSize = $true
  $text.MaximumSize = New-Object Drawing.Size 440, 0
  $text.Margin = New-Object Windows.Forms.Padding 0, 0, 0, 16
  $text.Text = $message
  $layout.Controls.Add($text)

  $buttons = New-Object Windows.Forms.FlowLayoutPanel
  $buttons.AutoSize = $true
  $buttons.FlowDirection = 'RightToLeft'
  $buttons.Dock = 'Fill'
  $buttons.Margin = New-Object Windows.Forms.Padding 0
  $no = New-Object Windows.Forms.Button
  $no.Text = $noText
  $no.AutoSize = $true
  $no.DialogResult = 'No'
  $yes = New-Object Windows.Forms.Button
  $yes.Text = $yesText
  $yes.AutoSize = $true
  $yes.DialogResult = 'Yes'
  $buttons.Controls.Add($no)
  $buttons.Controls.Add($yes)
  $layout.Controls.Add($buttons)

  $clock = @{ Left = 15; Timer = $null }
  if ($Update) {
    # Jak Enhanced is free: PayPal.Me buttons, one under the other (they only open the browser, the
    # question stays and its clock stops), and where the money goes
    $grey = [Drawing.SystemColors]::GrayText
    $head = New-Object Windows.Forms.Label
    $head.AutoSize = $true
    $head.Margin = New-Object Windows.Forms.Padding 0, 18, 0, 6
    $head.Font = New-Object Drawing.Font $form.Font, ([Drawing.FontStyle]::Bold)
    $head.Text = "Jak Enhanced is 100% free $Dash support is optional"
    $layout.Controls.Add($head)
    $iconSize = [int]($form.Font.Height * 1.25)
    foreach ($d in $Donations) {
      $b = New-Object Windows.Forms.Button
      $b.FlatStyle = 'Flat'
      $b.FlatAppearance.BorderSize = 0
      $b.FlatAppearance.MouseOverBackColor = $PayPalDark
      $b.FlatAppearance.MouseDownBackColor = $PayPalDark
      $b.BackColor = $PayPal
      $b.ForeColor = [Drawing.Color]::White
      $b.Font = New-Object Drawing.Font $form.Font, ([Drawing.FontStyle]::Bold)
      $b.Cursor = [Windows.Forms.Cursors]::Hand
      $b.TabStop = $false
      $b.Size = New-Object Drawing.Size ([int]($form.Font.Height * 14)), ([int]($form.Font.Height * 2.1))
      $b.Margin = New-Object Windows.Forms.Padding 0, 0, 0, 4
      $b.Padding = New-Object Windows.Forms.Padding 8, 0, 0, 0
      $b.Image = New-DonateIcon $d[0] $iconSize
      $b.ImageAlign = 'MiddleLeft'
      $b.TextAlign = 'MiddleLeft'
      $b.TextImageRelation = 'ImageBeforeText'
      $b.UseMnemonic = $false
      $b.Text = '  ' + $d[1]
      $b.Tag = $d[2]
      $b.Add_Click({
        param($s, $a)
        if ($clock.Timer) { $clock.Timer.Stop(); $no.Text = $noText }
        try { Start-Process $s.Tag } catch { }
        $no.Focus()
      })
      $layout.Controls.Add($b)
    }
    $trustRow = New-Object Windows.Forms.FlowLayoutPanel
    $trustRow.AutoSize = $true
    $trustRow.WrapContents = $false
    $trustRow.Margin = New-Object Windows.Forms.Padding 0, 4, 0, 0
    $lock = New-Object Windows.Forms.PictureBox
    $lock.Size = New-Object Drawing.Size $form.Font.Height, $form.Font.Height
    $lock.Image = New-DonateIcon 'lock' $form.Font.Height
    $lock.Margin = New-Object Windows.Forms.Padding 0, 1, 4, 0
    $trustText = New-Object Windows.Forms.Label
    $trustText.AutoSize = $true
    $trustText.UseMnemonic = $false
    $trustText.ForeColor = $grey
    $trustText.MaximumSize = New-Object Drawing.Size 420, 0
    $trustText.Margin = New-Object Windows.Forms.Padding 0
    $trustText.Text = $Trust
    $trustRow.Controls.AddRange(@($lock, $trustText))
    $layout.Controls.Add($trustRow)
  }
  $form.Controls.Add($layout)

  if ($Update) {
    # "Later (15)", "Later (14)"... then Later
    $no.Text = '{0} ({1})' -f $noText, $clock.Left
    $timer = New-Object Windows.Forms.Timer
    $timer.Interval = 1000
    $timer.Add_Tick({
      $clock.Left--
      if ($clock.Left -le 0) { $clock.Timer.Stop(); $form.DialogResult = 'No'; return }
      $no.Text = '{0} ({1})' -f $noText, $clock.Left
    })
    $clock.Timer = $timer
    $form.Add_Shown({ $clock.Timer.Start() })
  }
  # (Enter gives the answer the console gave by itself, after its 10 seconds: carry on, and no HD textures)
  $default = if ($yesFirst) { $yes } else { $no }
  $form.AcceptButton = $default
  $form.CancelButton = $no
  $form.Add_Shown({ $form.Activate(); $default.Focus() })
  $result = $form.ShowDialog()
  if ($clock.Timer) { $clock.Timer.Dispose() }
  $form.Dispose()
  if ($result -eq 'Yes') { exit 10 }
  exit 11
} catch {
  exit 2
}
