# Jak Enhanced Trilogy: the installer's window (WPF), in place of the Inno Setup wizard pages.
#   installer_ui.ps1 -Dir <folder>
# installer.iss runs it from its temporary folder at the start (InitializeSetup) and waits. <folder>\ui.txt
# has what it needs (key=value, UTF-8): Setup (the Setup .exe), Lang, DefaultDir, ProgMB, Icon, Update
# (update.ps1), Art (the donate-*.bmp folder) and the installer's own texts (S.<name>, its CustomMessages,
# so they stay in installer.iss in every language).
# Steps: the discs (found on this PC, Browse, or dropped on the window) > where to install and the HD
# textures > the build > play. The build is the same Setup .exe run again silently:
#   /VERYSILENT /SUPPRESSMSGBOXES /DIR=... /MERGETASKS=... /JEUI=<folder>
# which reads <folder>\choices.ini (the discs, the HD textures) and writes <folder>\status.txt:
#   @@pct <0-100>        progress of the step       @@step <id>        a step began (copy, x1-3, hd1-3)
#   @@line <text>        its last output line       @@note python      installing Python
#   @@fail <id> <code>   a step failed              @@done             all done
# Exit code: 0 done or cancelled; 3 this window failed (the installer shows its classic wizard instead).
param([Parameter(Mandatory = $true)][string]$Dir)
$ErrorActionPreference = 'Stop'
$errorFile = Join-Path $Dir 'ui-error.txt'
try {
Add-Type -AssemblyName PresentationFramework, PresentationCore, WindowsBase, System.Windows.Forms

# ---- what the installer passed -------------------------------------------------------------------
$cfg = @{}
foreach ($l in [IO.File]::ReadAllLines((Join-Path $Dir 'ui.txt'), [Text.Encoding]::UTF8)) {
  $i = $l.IndexOf('=')
  if ($i -gt 0) { $cfg[$l.Substring(0, $i)] = $l.Substring($i + 1).Replace('\n', "`n") }
}
function T([string]$k) { if ($cfg.ContainsKey("S.$k")) { $cfg["S.$k"] } else { $k } }
function F([string]$k) { $s = T $k; for ($i = 0; $i -lt $args.Count; $i++) { $s = $s.Replace('%' + ($i + 1), [string]$args[$i]) }; $s }
$ProgGB = [double]$cfg.ProgMB / 1024
$Culture = if ($cfg.Lang -eq 'it') { [Globalization.CultureInfo]'it-IT' } else { [Globalization.CultureInfo]::InvariantCulture }
$GameGB = 30; $HdGB = 4
$ExtractMin = 30; $HdMin = @{ 1 = 30; 2 = 90; 3 = 90 }
$GameName = @{ 1 = 'Jak and Daxter'; 2 = 'Jak II'; 3 = 'Jak 3' }
$ShortName = @{ 1 = 'Jak 1'; 2 = 'Jak II'; 3 = 'Jak 3' }
$Status = Join-Path $Dir 'status.txt'

# ---- which game a disc image is: the serial on its boot line, as the launcher does -----------------
$IdentifyCode = @'
function Get-Disc([string]$path) {
  try {
    $fs = [IO.File]::Open($path, 'Open', 'Read', 'ReadWrite')
    try {
      $n = [Math]::Min([long]8MB, $fs.Length); $buf = New-Object byte[] $n
      $got = 0; while ($got -lt $n) { $r = $fs.Read($buf, $got, $n - $got); if ($r -le 0) { break }; $got += $r }
      $size = $fs.Length
    } finally { $fs.Close() }
  } catch { return $null }
  $text = [Text.Encoding]::ASCII.GetString($buf, 0, $got)
  $at = $text.IndexOf('cdrom0:\')
  if ($at -lt 0) { return $null }
  $id = $text.Substring($at + 8, [Math]::Min(11, $text.Length - $at - 8))
  $game = switch ($id) {
    { $_ -in 'SCUS_971.24', 'SCES_503.61', 'SCPS_150.21' } { 1 }
    { $_ -in 'SCUS_972.65', 'SCES_516.08', 'SCPS_150.57' } { 2 }
    { $_ -in 'SCUS_973.30', 'SCES_524.60', 'SCPS_150.93' } { 3 }
    default { 0 }
  }
  if (-not $game) { return $null }
  $region = switch ($id.Substring(0, 4)) { 'SCUS' { 'NTSC' } 'SCES' { 'PAL' } default { 'NTSC-J' } }
  [pscustomobject]@{ Path = [IO.Path]::GetFullPath($path); Game = $game; Region = $region; Serial = $id; Size = $size }
}
'@
. ([scriptblock]::Create($IdentifyCode))

# ---- the window --------------------------------------------------------------------------------
[xml]$xaml = @'
<Window xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation"
        xmlns:x="http://schemas.microsoft.com/winfx/2006/xaml"
        Title="Jak Enhanced Trilogy Setup" Width="880" Height="620" WindowStyle="None" AllowsTransparency="True"
        Background="Transparent" ResizeMode="CanMinimize" WindowStartupLocation="CenterScreen" AllowDrop="True"
        FontFamily="Segoe UI Variable Text, Segoe UI" FontSize="13" Foreground="#FFFFFF" UseLayoutRounding="True"
        TextOptions.TextFormattingMode="Display">
  <Window.Resources>
    <Style x:Key="Primary" TargetType="Button">
      <Setter Property="Foreground" Value="#3A1A00"/><Setter Property="FontWeight" Value="Bold"/>
      <Setter Property="FontSize" Value="14"/><Setter Property="Cursor" Value="Hand"/>
      <Setter Property="Template"><Setter.Value><ControlTemplate TargetType="Button">
        <Border x:Name="b" CornerRadius="10" Background="#FF8A1E" BorderBrush="#FFC266" BorderThickness="0,1,0,0" Padding="24,11">
          <ContentPresenter HorizontalAlignment="Center" VerticalAlignment="Center"/></Border>
        <ControlTemplate.Triggers>
          <Trigger Property="IsMouseOver" Value="True"><Setter TargetName="b" Property="Background" Value="#FFA040"/></Trigger>
          <Trigger Property="IsPressed" Value="True"><Setter TargetName="b" Property="Background" Value="#E06E0A"/></Trigger>
          <Trigger Property="IsEnabled" Value="False"><Setter TargetName="b" Property="Opacity" Value="0.4"/></Trigger>
        </ControlTemplate.Triggers></ControlTemplate></Setter.Value></Setter>
    </Style>
    <Style x:Key="Ghost" TargetType="Button">
      <Setter Property="Foreground" Value="#F2F7FF"/><Setter Property="FontSize" Value="13"/><Setter Property="Cursor" Value="Hand"/>
      <Setter Property="Template"><Setter.Value><ControlTemplate TargetType="Button">
        <Border x:Name="b" CornerRadius="8" Background="#11497A" BorderBrush="#2A6EA0" BorderThickness="1" Padding="16,8">
          <ContentPresenter HorizontalAlignment="Center" VerticalAlignment="Center"/></Border>
        <ControlTemplate.Triggers>
          <Trigger Property="IsMouseOver" Value="True"><Setter TargetName="b" Property="Background" Value="#185A8E"/></Trigger>
          <Trigger Property="IsEnabled" Value="False"><Setter TargetName="b" Property="Opacity" Value="0.4"/></Trigger>
        </ControlTemplate.Triggers></ControlTemplate></Setter.Value></Setter>
    </Style>
    <Style x:Key="Glyph" TargetType="Button">
      <Setter Property="Foreground" Value="#97B9D6"/><Setter Property="FontFamily" Value="Segoe MDL2 Assets"/>
      <Setter Property="FontSize" Value="10"/><Setter Property="Width" Value="34"/><Setter Property="Height" Value="30"/>
      <Setter Property="Template"><Setter.Value><ControlTemplate TargetType="Button">
        <Border x:Name="b" CornerRadius="6" Background="Transparent">
          <ContentPresenter HorizontalAlignment="Center" VerticalAlignment="Center"/></Border>
        <ControlTemplate.Triggers>
          <Trigger Property="IsMouseOver" Value="True"><Setter TargetName="b" Property="Background" Value="#1FFFFFFF"/>
            <Setter Property="Foreground" Value="White"/></Trigger>
        </ControlTemplate.Triggers></ControlTemplate></Setter.Value></Setter>
    </Style>
    <Style x:Key="Link" TargetType="Button">
      <Setter Property="Foreground" Value="#6FF0FF"/><Setter Property="Cursor" Value="Hand"/>
      <Setter Property="Template"><Setter.Value><ControlTemplate TargetType="Button">
        <TextBlock x:Name="t" Text="{TemplateBinding Content}"/>
        <ControlTemplate.Triggers>
          <Trigger Property="IsMouseOver" Value="True"><Setter TargetName="t" Property="TextDecorations" Value="Underline"/></Trigger>
        </ControlTemplate.Triggers></ControlTemplate></Setter.Value></Setter>
    </Style>
    <Style TargetType="CheckBox">
      <Setter Property="Foreground" Value="#F2F7FF"/><Setter Property="Cursor" Value="Hand"/>
      <Setter Property="Template"><Setter.Value><ControlTemplate TargetType="CheckBox">
        <Grid Background="Transparent"><Grid.ColumnDefinitions><ColumnDefinition Width="Auto"/><ColumnDefinition/></Grid.ColumnDefinitions>
          <Border x:Name="box" Width="18" Height="18" CornerRadius="5" BorderThickness="1.5" BorderBrush="#4A86B5"
                  Background="#0A3157" VerticalAlignment="Center">
            <Path x:Name="tick" Data="M3.5,9 L7.5,13 L14.5,5" Stroke="White" StrokeThickness="2" Visibility="Collapsed"
                  StrokeStartLineCap="Round" StrokeEndLineCap="Round" StrokeLineJoin="Round" Margin="-1.5,-1.5,0,0"/></Border>
          <ContentPresenter Grid.Column="1" Margin="10,0,0,0" VerticalAlignment="Center"/></Grid>
        <ControlTemplate.Triggers>
          <Trigger Property="IsChecked" Value="True"><Setter TargetName="tick" Property="Visibility" Value="Visible"/>
            <Setter TargetName="box" Property="Background" Value="#FF8A1E"/><Setter TargetName="box" Property="BorderBrush" Value="#FF8A1E"/></Trigger>
          <Trigger Property="IsEnabled" Value="False"><Setter Property="Opacity" Value="0.4"/></Trigger>
        </ControlTemplate.Triggers></ControlTemplate></Setter.Value></Setter>
    </Style>
    <Style TargetType="ScrollBar">
      <Setter Property="Width" Value="10"/>
      <Setter Property="Template"><Setter.Value><ControlTemplate TargetType="ScrollBar">
        <Track x:Name="PART_Track" IsDirectionReversed="True"><Track.Thumb><Thumb><Thumb.Template><ControlTemplate>
          <Border CornerRadius="3" Background="#2E6C9C" Width="6" Margin="0,4"/></ControlTemplate></Thumb.Template></Thumb></Track.Thumb></Track>
      </ControlTemplate></Setter.Value></Setter>
    </Style>
    <Style x:Key="Label" TargetType="TextBlock">
      <Setter Property="Foreground" Value="#FFD54A"/><Setter Property="FontSize" Value="11"/>
      <Setter Property="FontWeight" Value="SemiBold"/><Setter Property="Margin" Value="0,0,0,8"/>
    </Style>
    <Style x:Key="Panel" TargetType="Border">
      <Setter Property="Background" Value="#0D3C66"/><Setter Property="BorderBrush" Value="#1F5C8C"/>
      <Setter Property="BorderThickness" Value="1"/><Setter Property="CornerRadius" Value="10"/>
    </Style>
  </Window.Resources>

  <Grid x:Name="Root">
    <Grid.Clip><RectangleGeometry Rect="0,0,880,620" RadiusX="16" RadiusY="16"/></Grid.Clip>

    <!-- the card -->
    <!-- bright island colors: ocean blue, orange and sand trim, glowing eco orbs (all drawn here, no art) -->
    <Border x:Name="Card" CornerRadius="16" BorderThickness="1.5">
      <Border.Background><LinearGradientBrush StartPoint="0,0" EndPoint="0.4,1">
        <GradientStop Color="#0F5A92" Offset="0"/><GradientStop Color="#0A3E6E" Offset="0.55"/><GradientStop Color="#082C52" Offset="1"/></LinearGradientBrush></Border.Background>
      <Border.BorderBrush><LinearGradientBrush StartPoint="0,0" EndPoint="1,1">
        <GradientStop Color="#FFD54A" Offset="0"/><GradientStop Color="#FF7A1A" Offset="0.5"/><GradientStop Color="#FFB02E" Offset="1"/></LinearGradientBrush></Border.BorderBrush>
      <Grid>
        <!-- glow in the corners and a carved band under the header -->
        <Ellipse Width="520" Height="420" HorizontalAlignment="Right" VerticalAlignment="Top" Margin="0,-220,-200,0" IsHitTestVisible="False">
          <Ellipse.Fill><RadialGradientBrush><GradientStop Color="#333EE0F0" Offset="0"/><GradientStop Color="#003EE0F0" Offset="1"/></RadialGradientBrush></Ellipse.Fill></Ellipse>
        <Ellipse Width="560" Height="380" HorizontalAlignment="Left" VerticalAlignment="Bottom" Margin="-240,0,0,-220" IsHitTestVisible="False">
          <Ellipse.Fill><RadialGradientBrush><GradientStop Color="#33FF8A1E" Offset="0"/><GradientStop Color="#00FF8A1E" Offset="1"/></RadialGradientBrush></Ellipse.Fill></Ellipse>
        <Border Height="2" VerticalAlignment="Top" Margin="22,64,22,0" IsHitTestVisible="False">
          <Border.Background><LinearGradientBrush StartPoint="0,0" EndPoint="1,0">
            <GradientStop Color="#00FFD54A" Offset="0"/><GradientStop Color="#99FFD54A" Offset="0.5"/><GradientStop Color="#00FFD54A" Offset="1"/></LinearGradientBrush></Border.Background></Border>
      <Grid>
        <Grid.RowDefinitions><RowDefinition Height="64"/><RowDefinition/><RowDefinition Height="Auto"/></Grid.RowDefinitions>
        <Grid x:Name="Head" Background="Transparent" Margin="22,0,14,0">
          <StackPanel Orientation="Horizontal" VerticalAlignment="Center" HorizontalAlignment="Left">
            <Image x:Name="AppIcon" Width="26" Height="26" RenderOptions.BitmapScalingMode="HighQuality"/>
            <TextBlock Text="Jak Enhanced" FontWeight="Black" FontSize="16" Margin="10,0,0,1" VerticalAlignment="Center" Foreground="#FFD54A"/>
            <StackPanel Orientation="Horizontal" VerticalAlignment="Center" Margin="10,1,0,0">
              <Ellipse Width="9" Height="9" Margin="0,0,4,0"><Ellipse.Fill><RadialGradientBrush GradientOrigin="0.35,0.3"><GradientStop Color="#E8FBFF" Offset="0"/><GradientStop Color="#3EE0F0" Offset="0.6"/><GradientStop Color="#1676A8" Offset="1"/></RadialGradientBrush></Ellipse.Fill>
                <Ellipse.Effect><DropShadowEffect Color="#3EE0F0" BlurRadius="10" ShadowDepth="0" Opacity="0.9"/></Ellipse.Effect></Ellipse>
              <Ellipse Width="9" Height="9" Margin="0,0,4,0"><Ellipse.Fill><RadialGradientBrush GradientOrigin="0.35,0.3"><GradientStop Color="#F2FFE0" Offset="0"/><GradientStop Color="#4CD964" Offset="0.6"/><GradientStop Color="#3C8A1E" Offset="1"/></RadialGradientBrush></Ellipse.Fill>
                <Ellipse.Effect><DropShadowEffect Color="#4CD964" BlurRadius="10" ShadowDepth="0" Opacity="0.9"/></Ellipse.Effect></Ellipse>
              <Ellipse Width="9" Height="9"><Ellipse.Fill><RadialGradientBrush GradientOrigin="0.35,0.3"><GradientStop Color="#FFF6D8" Offset="0"/><GradientStop Color="#F2C230" Offset="0.6"/><GradientStop Color="#B07A10" Offset="1"/></RadialGradientBrush></Ellipse.Fill>
                <Ellipse.Effect><DropShadowEffect Color="#F2C230" BlurRadius="10" ShadowDepth="0" Opacity="0.9"/></Ellipse.Effect></Ellipse>
            </StackPanel>
          </StackPanel>
          <Border CornerRadius="18" Background="#0B3A63" BorderBrush="#134C7C" BorderThickness="1" Padding="4"
                  HorizontalAlignment="Center" VerticalAlignment="Center">
            <StackPanel x:Name="Steps" Orientation="Horizontal"/></Border>
          <StackPanel Orientation="Horizontal" HorizontalAlignment="Right" VerticalAlignment="Center">
            <Button x:Name="MinBtn" Style="{StaticResource Glyph}" Content="&#xE921;"/>
            <Button x:Name="CloseBtn" Style="{StaticResource Glyph}" Content="&#xE8BB;"/>
          </StackPanel>
        </Grid>

        <Grid Grid.Row="1" Margin="56,22,56,0">
          <!-- 0: the discs -->
          <DockPanel x:Name="Page0">
            <StackPanel DockPanel.Dock="Top">
              <TextBlock x:Name="DiscTitle" FontSize="32" FontWeight="Black" FontFamily="Segoe UI Black, Segoe UI" Foreground="#FFFFFF"/>
              <TextBlock x:Name="DiscSub" Foreground="#A9C8E2" FontSize="14" Margin="0,6,0,0"/>
              <TextBlock x:Name="FoundLabel" Style="{StaticResource Label}" Margin="0,28,0,8"/>
            </StackPanel>
            <StackPanel DockPanel.Dock="Bottom">
              <StackPanel Orientation="Horizontal" Margin="2,10,0,0">
                <Button x:Name="BrowseBtn" Style="{StaticResource Link}"/>
                <TextBlock Text="  ·  " Foreground="#5A84A8"/>
                <TextBlock x:Name="DropHint" Foreground="#88ACCB"/>
              </StackPanel>
              <TextBlock x:Name="Note0" Foreground="#FF7A7A" TextWrapping="Wrap" Margin="2,8,0,0" Visibility="Collapsed"/>
              <Grid Margin="0,16,0,0"><Grid.ColumnDefinitions><ColumnDefinition Width="Auto"/><ColumnDefinition/></Grid.ColumnDefinitions>
                <Border Width="18" Height="18" CornerRadius="9" BorderBrush="#6C93B5" BorderThickness="1.3" VerticalAlignment="Top" Margin="0,1,10,0">
                  <TextBlock Text="i" FontSize="11" FontWeight="Bold" Foreground="#A9C8E2" HorizontalAlignment="Center" Margin="0,-1,0,0"/></Border>
                <TextBlock x:Name="DiscInfo" Grid.Column="1" Foreground="#97B9D6" FontSize="12" TextWrapping="Wrap" LineHeight="18"/>
              </Grid>
            </StackPanel>
            <Border Style="{StaticResource Panel}">
              <Grid>
                <ScrollViewer VerticalScrollBarVisibility="Auto"><StackPanel x:Name="DiscList"/></ScrollViewer>
                <TextBlock x:Name="DiscHint" Foreground="#88ACCB" HorizontalAlignment="Center" VerticalAlignment="Center"
                           TextAlignment="Center" TextWrapping="Wrap" Margin="30,0"/>
              </Grid>
            </Border>
          </DockPanel>

          <!-- 1: where, the HD textures, the shortcuts -->
          <StackPanel x:Name="Page1" Visibility="Collapsed">
            <TextBlock x:Name="OptTitle" FontSize="32" FontWeight="Black" FontFamily="Segoe UI Black, Segoe UI" Foreground="#FFFFFF"/>
            <TextBlock x:Name="OptSub" Foreground="#A9C8E2" FontSize="14" Margin="0,6,0,0"/>
            <TextBlock x:Name="DirLabel" Style="{StaticResource Label}" Margin="0,26,0,8"/>
            <Border Style="{StaticResource Panel}" Padding="14,4,5,4">
              <Grid><Grid.ColumnDefinitions><ColumnDefinition/><ColumnDefinition Width="Auto"/></Grid.ColumnDefinitions>
                <TextBox x:Name="DirBox" Background="Transparent" BorderThickness="0" Foreground="#F2F7FF" CaretBrush="White"
                         FontFamily="Cascadia Mono, Consolas" FontSize="13" VerticalAlignment="Center"/>
                <Button x:Name="ChangeBtn" Grid.Column="1" Style="{StaticResource Ghost}"/>
              </Grid>
            </Border>
            <TextBlock x:Name="SizeNote" Foreground="#88ACCB" FontSize="12" Margin="2,8,0,0" TextWrapping="Wrap"/>
            <TextBlock x:Name="HdLabel" Style="{StaticResource Label}" Margin="0,22,0,6"/>
            <TextBlock x:Name="HdSub" Foreground="#A9C8E2" FontSize="12" Margin="0,0,0,10" TextWrapping="Wrap"/>
            <Border Style="{StaticResource Panel}" Padding="16,4"><WrapPanel x:Name="HdList"/></Border>
            <WrapPanel Margin="2,16,0,0">
              <CheckBox x:Name="DesktopChk" IsChecked="True" Margin="0,0,28,0"/>
              <CheckBox x:Name="SteamChk"/>
            </WrapPanel>
            <TextBlock x:Name="Note1" Foreground="#FF7A7A" TextWrapping="Wrap" Margin="2,10,0,0" Visibility="Collapsed"/>
            <Grid Margin="0,14,0,0"><Grid.ColumnDefinitions><ColumnDefinition Width="Auto"/><ColumnDefinition/></Grid.ColumnDefinitions>
              <Border Width="18" Height="18" CornerRadius="9" BorderBrush="#6C93B5" BorderThickness="1.3" VerticalAlignment="Top" Margin="0,1,10,0">
                <TextBlock Text="i" FontSize="11" FontWeight="Bold" Foreground="#A9C8E2" HorizontalAlignment="Center" Margin="0,-1,0,0"/></Border>
              <TextBlock x:Name="HdInfo" Grid.Column="1" Foreground="#97B9D6" FontSize="12" TextWrapping="Wrap" LineHeight="18"/>
            </Grid>
          </StackPanel>

          <!-- 2: the build -->
          <StackPanel x:Name="Page2" Visibility="Collapsed">
            <TextBlock x:Name="WorkTitle" FontSize="32" FontWeight="Black" FontFamily="Segoe UI Black, Segoe UI" Foreground="#FFFFFF"/>
            <TextBlock x:Name="WorkSub" Foreground="#A9C8E2" FontSize="14" Margin="0,6,0,0" TextWrapping="Wrap"/>
            <TextBlock x:Name="StepsLabel" Style="{StaticResource Label}" Margin="0,26,0,8"/>
            <Border Style="{StaticResource Panel}" Padding="18,6"><StackPanel x:Name="WorkList"/></Border>
            <TextBlock x:Name="NowText" FontSize="14" FontWeight="SemiBold" Margin="0,18,0,10" TextTrimming="CharacterEllipsis"/>
            <Border x:Name="BarBack" Height="6" CornerRadius="3" Background="#134C7C" ClipToBounds="True">
              <Border x:Name="BarFill" HorizontalAlignment="Left" Width="0" CornerRadius="3" Background="#FF8A1E"/></Border>
            <TextBlock x:Name="DetailText" Foreground="#88ACCB" FontFamily="Cascadia Mono, Consolas" FontSize="11.5"
                       Margin="0,10,0,0" TextTrimming="CharacterEllipsis"/>
          </StackPanel>

          <!-- 3: play -->
          <StackPanel x:Name="Page3" Visibility="Collapsed">
            <TextBlock x:Name="DoneTitle" FontSize="32" FontWeight="Black" FontFamily="Segoe UI Black, Segoe UI" Foreground="#FFFFFF"/>
            <TextBlock x:Name="DoneSub" Foreground="#A9C8E2" FontSize="14" Margin="0,6,0,0" TextWrapping="Wrap"/>
            <StackPanel x:Name="FailBox" Visibility="Collapsed" Margin="0,22,0,0">
              <Border Background="#2A1A1D" BorderBrush="#4A262B" BorderThickness="1" CornerRadius="10" Padding="16,12">
                <StackPanel><StackPanel x:Name="FailList"/>
                  <Button x:Name="LogBtn" Style="{StaticResource Link}" Margin="0,8,0,0" HorizontalAlignment="Left"/></StackPanel></Border>
            </StackPanel>
            <TextBlock x:Name="DonateLabel" Style="{StaticResource Label}" Margin="0,30,0,8"/>
            <TextBlock x:Name="FreeText" Foreground="#A9C8E2" FontSize="13" TextWrapping="Wrap" LineHeight="20"/>
            <WrapPanel x:Name="DonateRow" Margin="0,16,0,0"/>
            <Grid Margin="0,14,0,0"><Grid.ColumnDefinitions><ColumnDefinition Width="Auto"/><ColumnDefinition/></Grid.ColumnDefinitions>
              <Image x:Name="LockIcon" Width="12" Height="12" VerticalAlignment="Top" Margin="2,3,10,0"/>
              <TextBlock x:Name="TrustText" Grid.Column="1" Foreground="#88ACCB" FontSize="12" TextWrapping="Wrap" LineHeight="18"/>
            </Grid>
          </StackPanel>
        </Grid>

        <!-- the bottom row: disk, time, buttons -->
        <Grid Grid.Row="2" Margin="56,18,56,30">
          <Grid.ColumnDefinitions><ColumnDefinition Width="Auto"/><ColumnDefinition/><ColumnDefinition Width="Auto"/></Grid.ColumnDefinitions>
          <StackPanel x:Name="DiskBox" VerticalAlignment="Center">
            <Border Width="240" Height="4" CornerRadius="2" Background="#134C7C" HorizontalAlignment="Left">
              <Border x:Name="DiskFill" HorizontalAlignment="Left" Width="0" CornerRadius="2" Background="#4CD964"/></Border>
            <TextBlock x:Name="DiskText" Foreground="#A9C8E2" FontSize="12" Margin="0,8,0,0"/>
          </StackPanel>
          <TextBlock x:Name="TimeText" Grid.Column="1" Foreground="#88ACCB" FontSize="12" HorizontalAlignment="Center" VerticalAlignment="Center"/>
          <StackPanel Grid.Column="2" Orientation="Horizontal">
            <Button x:Name="BackBtn" Style="{StaticResource Ghost}" Margin="0,0,10,0" Padding="18,11"/>
            <Button x:Name="NextBtn" Style="{StaticResource Primary}"/>
          </StackPanel>
        </Grid>
      </Grid>
      </Grid>
    </Border>
  </Grid>
</Window>
'@
$win = [Windows.Markup.XamlReader]::Load((New-Object Xml.XmlNodeReader $xaml))
$ui = @{}
$xaml.SelectNodes('//*[@*[local-name()="Name"]]') | ForEach-Object {
  $n = $_.GetAttribute('Name', 'http://schemas.microsoft.com/winfx/2006/xaml'); if ($n) { $ui[$n] = $win.FindName($n) }
}
$bc = New-Object Windows.Media.BrushConverter
function Br([string]$hex) { $bc.ConvertFromString($hex) }
function New-Text([string]$text, [double]$size = 13, [string]$color = '#FFFFFF', [string]$weight = 'Normal') {
  $t = New-Object Windows.Controls.TextBlock
  $t.Text = $text; $t.FontSize = $size; $t.Foreground = Br $color; $t.FontWeight = $weight
  $t.VerticalAlignment = 'Center'; $t
}
function Load-Image([string]$path) {
  try { $b = New-Object Windows.Media.Imaging.BitmapImage; $b.BeginInit(); $b.UriSource = New-Object Uri $path
        $b.CacheOption = 'OnLoad'; $b.EndInit(); $b } catch { $null }
}
if ($cfg.Icon -and (Test-Path -LiteralPath $cfg.Icon)) {
  $ico = Load-Image $cfg.Icon; if ($ico) { $ui.AppIcon.Source = $ico; $win.Icon = $ico }
}
function Format-Size([double]$bytes) { if ($bytes -ge 1GB) { ($bytes / 1GB).ToString('0.0', $Culture) + ' GB' } else { ($bytes / 1MB).ToString('0', $Culture) + ' MB' } }
function Format-Time([int]$min) {
  if ($min -lt 60) { "$min min" } elseif ($min % 60) { '{0} h {1} min' -f [Math]::Floor($min / 60), ($min % 60) } else { '{0} h' -f ($min / 60) }
}

# ---- texts ---------------------------------------------------------------------------------------
$ui.DiscTitle.Text = T 'DiscsTitle'; $ui.DiscSub.Text = T 'DiscsSub'
$ui.FoundLabel.Text = (T 'UiFound').ToUpper(); $ui.DiscInfo.Text = T 'DiscsText'
$ui.BrowseBtn.Content = T 'UiBrowse'; $ui.DropHint.Text = T 'UiDrop'; $ui.DiscHint.Text = T 'UiSearching'
$ui.OptTitle.Text = T 'UiOptTitle'; $ui.OptSub.Text = T 'UiOptSub'
$ui.DirLabel.Text = (T 'UiInstallTo').ToUpper(); $ui.ChangeBtn.Content = T 'UiChange'
$ui.HdLabel.Text = (T 'HdTitle').ToUpper(); $ui.HdSub.Text = T 'HdSub'; $ui.HdInfo.Text = T 'HdText'
$ui.DesktopChk.Content = T 'Desktop'; $ui.SteamChk.Content = T 'Steam'
$ui.WorkTitle.Text = T 'WorkTitle'; $ui.WorkSub.Text = T 'WorkSub'; $ui.StepsLabel.Text = (T 'UiSteps').ToUpper()
$ui.DoneTitle.Text = T 'UiDoneTitle'; $ui.DonateLabel.Text = (T 'DonateHead').ToUpper()
$ui.FreeText.Text = T 'Free'; $ui.TrustText.Text = T 'Trust'; $ui.LogBtn.Content = T 'UiOpenLog'
$ui.BackBtn.Content = T 'UiBack'
$ui.DirBox.Text = $cfg.DefaultDir

# the step bar on top
$StepNames = @((T 'UiStepDiscs'), (T 'UiStepOptions'), (T 'UiStepBuild'), (T 'UiStepPlay'))
function Show-Steps([int]$cur) {
  $ui.Steps.Children.Clear()
  for ($i = 0; $i -lt $StepNames.Count; $i++) {
    if ($i) { $c = New-Text ([char]0xE76C) 9 '#5A84A8'; $c.FontFamily = 'Segoe MDL2 Assets'; $c.Margin = '6,0'; [void]$ui.Steps.Children.Add($c) }
    $pill = New-Object Windows.Controls.Border
    $pill.CornerRadius = 14; $pill.Padding = '14,5'
    $t = New-Text $StepNames[$i] 12.5 $(if ($i -eq $cur) { '#3A1A00' } elseif ($i -lt $cur) { '#D8E8F5' } else { '#88ACCB' }) $(if ($i -eq $cur) { 'Bold' } else { 'Normal' })
    if ($i -eq $cur) { $pill.Background = Br '#FF8A1E' }
    $pill.Child = $t
    [void]$ui.Steps.Children.Add($pill)
  }
}

# ---- page 0: the discs ---------------------------------------------------------------------------
$discs = New-Object Collections.ArrayList   # @{ Info; Check }
function Refresh-Discs {
  $ui.DiscList.Children.Clear()
  $i = 0
  foreach ($d in $discs) {
    $row = New-Object Windows.Controls.Grid
    $row.Margin = '16,0'; $row.Height = 58
    foreach ($w in 'Auto', '*', 'Auto', 'Auto') {
      $cd = New-Object Windows.Controls.ColumnDefinition
      $cd.Width = if ($w -eq '*') { New-Object Windows.GridLength 1, 'Star' } else { [Windows.GridLength]::Auto }
      $row.ColumnDefinitions.Add($cd)
    }
    [Windows.Controls.Grid]::SetColumn($d.Check, 0); $d.Check.Margin = '0,0,4,0'
    if ($d.Check.Parent) { $d.Check.Parent.Children.Remove($d.Check) }
    [void]$row.Children.Add($d.Check)
    $mid = New-Object Windows.Controls.StackPanel; $mid.VerticalAlignment = 'Center'
    $top = New-Object Windows.Controls.StackPanel; $top.Orientation = 'Horizontal'
    [void]$top.Children.Add((New-Text $GameName[$d.Info.Game] 14 '#FFFFFF' 'SemiBold'))
    $tag = New-Object Windows.Controls.Border; $tag.CornerRadius = 4; $tag.Background = Br '#134C7C'; $tag.Padding = '6,1'; $tag.Margin = '10,0,0,0'
    $tag.Child = New-Text ('{0}  {1}' -f $d.Info.Region, $d.Info.Serial) 10.5 '#A9C8E2'
    [void]$top.Children.Add($tag)
    [void]$mid.Children.Add($top)
    $p = New-Text $d.Info.Path 11.5 '#88ACCB'; $p.FontFamily = 'Cascadia Mono, Consolas'; $p.TextTrimming = 'CharacterEllipsis'; $p.Margin = '0,3,0,0'
    $p.ToolTip = $d.Info.Path
    [void]$mid.Children.Add($p)
    $mid.Margin = '12,0,16,0'
    [Windows.Controls.Grid]::SetColumn($mid, 1); [void]$row.Children.Add($mid)
    $sz = New-Text (Format-Size $d.Info.Size) 12 '#88ACCB'; [Windows.Controls.Grid]::SetColumn($sz, 2); [void]$row.Children.Add($sz)
    $x = New-Object Windows.Controls.Button; $x.Style = $win.Resources['Glyph']; $x.Content = [string][char]0xE711; $x.Margin = '8,0,-8,0'
    $x.Tag = $d; $x.ToolTip = T 'UiRemove'
    $x.Add_Click({ param($s) [void]$discs.Remove($s.Tag); Refresh-Discs; Update-Footer })
    [Windows.Controls.Grid]::SetColumn($x, 3); [void]$row.Children.Add($x)
    if ($i) {
      $line = New-Object Windows.Controls.Border; $line.Height = 1; $line.Background = Br '#1A5585'; $line.Margin = '16,0'
      [void]$ui.DiscList.Children.Add($line)
    }
    [void]$ui.DiscList.Children.Add($row)
    $i++
  }
  $ui.DiscHint.Visibility = if ($discs.Count) { 'Collapsed' } else { 'Visible' }
}
function Add-Disc($info, [bool]$check) {
  if (-not $info) { return }
  # the same disc twice (a copy in another folder): one row is enough
  foreach ($d in $discs) { if ($d.Info.Path -eq $info.Path -or ($d.Info.Game -eq $info.Game -and $d.Info.Serial -eq $info.Serial -and $d.Info.Size -eq $info.Size)) { if ($check) { $d.Check.IsChecked = $true }; return } }
  $cb = New-Object Windows.Controls.CheckBox
  $cb.VerticalAlignment = 'Center'
  $d = [pscustomobject]@{ Info = $info; Check = $cb }
  $cb.Tag = $d
  # one disc a game: checking one unchecks the others of the same game
  $cb.Add_Checked({ param($s) foreach ($o in $discs) { if ($o -ne $s.Tag -and $o.Info.Game -eq $s.Tag.Info.Game) { $o.Check.IsChecked = $false } }; Update-Footer })
  $cb.Add_Unchecked({ Update-Footer })
  [void]$discs.Add($d)
  $cb.IsChecked = $check -or -not ($discs | Where-Object { $_ -ne $d -and $_.Info.Game -eq $info.Game -and $_.Check.IsChecked })
  $sorted = @($discs | Sort-Object { $_.Info.Game }, { $_.Info.Path }); $discs.Clear(); $discs.AddRange($sorted)
  Refresh-Discs
}
function Show-Note($tb, [string]$text) {
  $tb.Text = $text; $tb.Visibility = if ($text) { 'Visible' } else { 'Collapsed' }
}
function Add-File([string]$path) {
  if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { Show-Note $ui.Note0 ((T 'Missing') + ' ' + $path); return }
  $info = Get-Disc $path
  if (-not $info) { Show-Note $ui.Note0 ((T 'Unknown') + ' ' + $path); return }
  Show-Note $ui.Note0 ''
  Add-Disc $info $true
  Update-Footer
}
$ui.BrowseBtn.Add_Click({
  $dlg = New-Object Microsoft.Win32.OpenFileDialog
  $dlg.Filter = 'Disc images (*.iso)|*.iso|All files|*.*'; $dlg.Multiselect = $true
  if ($dlg.ShowDialog($win)) { foreach ($f in $dlg.FileNames) { Add-File $f } }
})
$win.Add_DragOver({ param($s, $e)
  $e.Effects = if ($e.Data.GetDataPresent([Windows.DataFormats]::FileDrop)) { 'Copy' } else { 'None' }; $e.Handled = $true })
$win.Add_Drop({ param($s, $e)
  if ($e.Data.GetDataPresent([Windows.DataFormats]::FileDrop)) {
    foreach ($f in $e.Data.GetData([Windows.DataFormats]::FileDrop)) { Add-File $f }
    if ($script:page -ne 0) { Set-Page 0 }
  } })

# the search: the last install's list, then the usual folders and the drives, in the background
$roots = New-Object Collections.ArrayList
$prev = Join-Path $cfg.DefaultDir 'ISO\iso-paths.txt'
$known = @(); if (Test-Path -LiteralPath $prev) { $known = @(Get-Content -LiteralPath $prev | Where-Object { $_.Trim() }) }
$known += @(Join-Path $cfg.DefaultDir 'ISO')
foreach ($f in 'Desktop', 'Downloads', 'Documents', 'Videos') { [void]$roots.Add(@((Join-Path $env:USERPROFILE $f), 3)) }
[void]$roots.Add(@($env:USERPROFILE, 0))
foreach ($dr in [IO.DriveInfo]::GetDrives()) {
  if ($dr.DriveType -eq 'Fixed' -and $dr.IsReady) { [void]$roots.Add(@($dr.RootDirectory.FullName, $(if ($dr.Name -like 'C*') { 1 } else { 2 }))) }
}
$scan = [PowerShell]::Create()
[void]$scan.AddScript("param(`$known, `$roots)`n" + $IdentifyCode + @'
$seen = @{}
$files = New-Object Collections.ArrayList
foreach ($k in $known) {
  if (Test-Path -LiteralPath $k -PathType Leaf) { [void]$files.Add($k) }
  elseif (Test-Path -LiteralPath $k -PathType Container) { Get-ChildItem -LiteralPath $k -Filter *.iso -File -ErrorAction SilentlyContinue | % { [void]$files.Add($_.FullName) } }
}
$clock = [Diagnostics.Stopwatch]::StartNew()
foreach ($r in $roots) {
  if ($clock.Elapsed.TotalSeconds -gt 12) { break }
  if (-not (Test-Path -LiteralPath $r[0])) { continue }
  Get-ChildItem -LiteralPath $r[0] -Filter *.iso -File -Recurse -Depth $r[1] -ErrorAction SilentlyContinue |
    Where-Object { $_.Length -gt 100MB } | % { [void]$files.Add($_.FullName) }
}
foreach ($f in $files) {
  $key = $f.ToLowerInvariant(); if ($seen[$key]) { continue }; $seen[$key] = 1
  $d = Get-Disc $f; if ($d) { $d }
}
'@).AddArgument($known).AddArgument($roots)
$scanAsync = $scan.BeginInvoke()

# ---- page 1: where, HD -----------------------------------------------------------------------------
$hdChecks = @{}
function Chosen { @($discs | Where-Object { $_.Check.IsChecked } | Sort-Object { $_.Info.Game }) }
function Refresh-Hd {
  $ui.HdList.Children.Clear()
  foreach ($d in (Chosen)) {
    $g = $d.Info.Game
    if (-not $hdChecks[$g]) { $c = New-Object Windows.Controls.CheckBox; $c.Margin = '0,10,32,10'; $c.Add_Checked({ Update-Footer }); $c.Add_Unchecked({ Update-Footer }); $hdChecks[$g] = $c }
    $hdChecks[$g].Content = T "Hd$g"
    if ($hdChecks[$g].Parent) { $hdChecks[$g].Parent.Children.Remove($hdChecks[$g]) }
    [void]$ui.HdList.Children.Add($hdChecks[$g])
  }
}
function Wants-Hd([int]$g) { $hdChecks[$g] -and $hdChecks[$g].IsChecked -and ((Chosen) | Where-Object { $_.Info.Game -eq $g }) }
$ui.ChangeBtn.Add_Click({
  $dlg = New-Object Windows.Forms.FolderBrowserDialog
  $dlg.SelectedPath = Split-Path $ui.DirBox.Text -Parent
  if ($dlg.ShowDialog() -eq 'OK') {
    $p = $dlg.SelectedPath.TrimEnd('\')
    if ((Split-Path $p -Leaf) -ne 'Jak Enhanced Trilogy') { $p = Join-Path $p 'Jak Enhanced Trilogy' }
    $ui.DirBox.Text = $p
  }
})
$ui.DirBox.Add_TextChanged({ Update-Footer })

# ---- the bottom row ------------------------------------------------------------------------------
$script:needGB = 0; $script:freeGB = -1
function Update-Footer {
  $games = Chosen
  $need = $ProgGB + $GameGB * $games.Count; $min = $ExtractMin * $games.Count
  foreach ($d in $games) { if (Wants-Hd $d.Info.Game) { $need += $HdGB; $min += $HdMin[$d.Info.Game] } }
  $script:needGB = [Math]::Ceiling($need)
  $drive = ''; $script:freeGB = -1
  try { $drive = [IO.Path]::GetPathRoot($ui.DirBox.Text).TrimEnd('\'); $script:freeGB = [Math]::Floor((New-Object IO.DriveInfo $drive).AvailableFreeSpace / 1GB) } catch { }
  if ($script:freeGB -ge 0) {
    $ui.DiskText.Text = F 'UiNeeds' $script:needGB $drive $script:freeGB
    $frac = if ($script:freeGB -gt 0) { [Math]::Min(1, $script:needGB / $script:freeGB) } else { 1 }
    $ui.DiskFill.Width = [Math]::Max(4, 240 * $frac)
    $ui.DiskFill.Background = Br $(if ($script:needGB -gt $script:freeGB) { '#FF5A5F' } else { '#4CD964' })
  } else { $ui.DiskText.Text = F 'UiNeedsOnly' $script:needGB; $ui.DiskFill.Width = 0 }
  $ui.TimeText.Text = if ($games.Count) { F 'UiAbout' (Format-Time $min) } else { '' }
  if ($script:page -eq 0) { $ui.NextBtn.IsEnabled = $games.Count -gt 0 }
}

# ---- page 2: the build ---------------------------------------------------------------------------
$work = [ordered]@{}   # id -> @{ Text; Icon; Time; State; Start }
function Add-WorkRow([string]$id, [string]$text) {
  $row = New-Object Windows.Controls.Grid; $row.Height = 34
  foreach ($w in 'Auto', '*', 'Auto') { $cd = New-Object Windows.Controls.ColumnDefinition
    $cd.Width = if ($w -eq '*') { New-Object Windows.GridLength 1, 'Star' } else { [Windows.GridLength]::Auto }; $row.ColumnDefinitions.Add($cd) }
  $icon = New-Object Windows.Controls.Border; $icon.Width = 20; $icon.Height = 20; $icon.CornerRadius = 10; $icon.BorderThickness = 1.5
  $icon.BorderBrush = Br '#4A86B5'; $icon.Margin = '0,0,14,0'
  $glyph = New-Text '' 9 '#FFFFFF' 'Bold'; $glyph.FontFamily = 'Segoe MDL2 Assets'; $glyph.HorizontalAlignment = 'Center'; $icon.Child = $glyph
  [void]$row.Children.Add($icon)
  $t = New-Text $text 13 '#A9C8E2'; [Windows.Controls.Grid]::SetColumn($t, 1); [void]$row.Children.Add($t)
  $tm = New-Text '' 12 '#88ACCB'; [Windows.Controls.Grid]::SetColumn($tm, 2); [void]$row.Children.Add($tm)
  [void]$ui.WorkList.Children.Add($row)
  $work[$id] = @{ Text = $t; Icon = $icon; Glyph = $glyph; Time = $tm; State = 'wait'; Start = $null }
}
function Set-Work([string]$id, [string]$state) {
  $w = $work[$id]; if (-not $w) { return }
  if ($w.State -eq 'fail') { return }
  $w.State = $state
  switch ($state) {
    'run' { $w.Start = Get-Date; $w.Icon.BorderBrush = Br '#FF8A1E'; $w.Icon.Background = Br '#33FF8A1E'; $w.Text.Foreground = Br '#FFFFFF' }
    'done' { $w.Icon.BorderBrush = Br '#4CD964'; $w.Icon.Background = Br '#4CD964'; $w.Glyph.Text = [string][char]0xE73E; $w.Text.Foreground = Br '#D8E8F5' }
    'fail' { $w.Icon.BorderBrush = Br '#FF5A5F'; $w.Icon.Background = Br '#FF5A5F'; $w.Glyph.Text = [string][char]0xE8BB; $w.Text.Foreground = Br '#FF9A9A' }
  }
}
$script:inner = $null; $script:statusPos = 0; $script:running = $null; $script:failures = New-Object Collections.ArrayList
$script:pct = -1; $script:t0 = $null; $script:marquee = 0; $script:finished = $false
$timer = New-Object Windows.Threading.DispatcherTimer
$timer.Interval = [TimeSpan]::FromMilliseconds(200)
function Step-Text([string]$id) {
  switch -regex ($id) { '^copy$' { T 'UiCopy' } '^x(\d)$' { F 'Extract' $Matches[1] } '^hd(\d)$' { F 'Hd' $Matches[1] } default { $id } }
}
function Read-Status {
  if (-not (Test-Path -LiteralPath $Status)) { return }
  $fs = [IO.File]::Open($Status, 'Open', 'Read', 'ReadWrite')
  try { [void]$fs.Seek($script:statusPos, 'Begin'); $sr = New-Object IO.StreamReader $fs, ([Text.Encoding]::Default); $txt = $sr.ReadToEnd() } finally { $fs.Close() }
  $cut = $txt.LastIndexOf("`n"); if ($cut -lt 0) { return }
  $script:statusPos += [Text.Encoding]::Default.GetByteCount($txt.Substring(0, $cut + 1))
  foreach ($l in ($txt.Substring(0, $cut) -split "`r?`n")) {
    if ($l -notmatch '^@@(\w+) ?(.*)$') { continue }
    $a = $Matches[2]
    switch ($Matches[1]) {
      'step' { foreach ($k in @($work.Keys)) { if ($k -eq $a) { break }; if ($work[$k].State -ne 'fail') { Set-Work $k 'done' } }
               $script:running = $a; Set-Work $a 'run'
               $ui.NowText.Text = Step-Text $a; $ui.DetailText.Text = ''; $script:pct = -1 }
      'note' { if ($a -eq 'python') { $ui.NowText.Text = T 'Python' } }
      'pct' { $script:pct = [int]$a }
      'line' { $ui.DetailText.Text = $a }
      'fail' { $p = $a -split ' ', 2; Set-Work $p[0] 'fail'
               $n = if ($p[0] -match '\d') { $Matches[0] } else { '?' }
               if ($p[0] -eq 'prepare') { [void]$script:failures.Add((T 'GameStuck')) }
               else { [void]$script:failures.Add((F 'Failed' $n $p[1] (Join-Path $ui.DirBox.Text 'install.log'))) } }
      'done' { if ($script:running) { Set-Work $script:running 'done' }; $script:running = $null; $script:finished = $true }
    }
  }
}
$timer.Add_Tick({
  try { Read-Status } catch { }
  $el = (Get-Date) - $script:t0
  $ui.TimeText.Text = F 'UiElapsed' ('{0}:{1:00}:{2:00}' -f [int][Math]::Floor($el.TotalHours), $el.Minutes, $el.Seconds)
  if ($script:running -and $work[$script:running].Start) {
    $e = (Get-Date) - $work[$script:running].Start; $work[$script:running].Time.Text = '{0}:{1:00}' -f [int][Math]::Floor($e.TotalMinutes), $e.Seconds
  }
  $full = $ui.BarBack.ActualWidth
  if ($script:pct -ge 0) { $ui.BarFill.Margin = '0'; $ui.BarFill.Width = $full * $script:pct / 100 }
  else { $script:marquee = ($script:marquee + 0.025) % 1.3; $ui.BarFill.Width = $full * 0.22; $ui.BarFill.Margin = New-Object Windows.Thickness (($script:marquee - 0.22) * $full), 0, 0, 0 }
  if ($script:inner.HasExited) {
    Read-Status
    $timer.Stop()
    if (-not $script:finished -and -not $script:failures.Count) {
      [void]$script:failures.Add((F 'UiSetupFailed' $script:inner.ExitCode (Join-Path $Dir 'setup.log')))
      if ($script:running) { Set-Work $script:running 'fail' }
    }
    Set-Page 3
  }
})
function Start-Build {
  $dest = $ui.DirBox.Text.Trim().TrimEnd('\')
  if (-not $dest -or -not [IO.Path]::IsPathRooted($dest)) { Show-Note $ui.Note1 (T 'UiBadDir'); return }
  if ($script:freeGB -ge 0 -and $script:needGB -gt $script:freeGB) { Show-Note $ui.Note1 (F 'UiNoSpace' $script:needGB $script:freeGB); return }
  # a game open from that folder: asked here (the silent setup closes it without asking)
  if ((Test-Path -LiteralPath $dest) -and $cfg.Update) {
    $cmd = "& ([scriptblock]::Create([IO.File]::ReadAllText('{0}'))) -Root '{1}' -CheckOnly" -f $cfg.Update.Replace("'", "''"), $dest.Replace("'", "''")
    $p = Start-Process powershell.exe -ArgumentList '-NoProfile', '-Command', ('"' + $cmd + '"') -WindowStyle Hidden -PassThru -Wait
    if ($p.ExitCode -eq 10) {
      if ([Windows.MessageBox]::Show($win, (T 'GameOpen'), 'Jak Enhanced Trilogy', 'YesNo', 'Question') -ne 'Yes') { Show-Note $ui.Note1 (T 'GameStays'); return }
    }
  }
  Show-Note $ui.Note1 ''
  $ini = @('[discs]')
  foreach ($d in (Chosen)) { $ini += 'jak{0}={1}' -f $d.Info.Game, $d.Info.Path }
  $ini += '[hd]'
  foreach ($g in 1..3) { $ini += 'jak{0}={1}' -f $g, $(if (Wants-Hd $g) { 1 } else { 0 }) }
  [IO.File]::WriteAllLines((Join-Path $Dir 'choices.ini'), $ini, [Text.Encoding]::Default)
  Remove-Item -LiteralPath $Status -ErrorAction SilentlyContinue
  $tasks = @($(if ($ui.DesktopChk.IsChecked) { 'desktopicon' } else { '!desktopicon' }), $(if ($ui.SteamChk.IsChecked) { 'steam' } else { '!steam' })) -join ','
  $argList = '/VERYSILENT /SUPPRESSMSGBOXES /NORESTART /LANG={0} /DIR="{1}" /MERGETASKS="{2}" /LOG="{3}" /JEUI="{4}"' -f
             $cfg.Lang, $dest, $tasks, (Join-Path $Dir 'setup.log'), $Dir
  $ui.WorkList.Children.Clear(); $work.Clear()
  Add-WorkRow 'copy' (T 'UiCopyRow')
  foreach ($d in (Chosen)) {
    $g = $d.Info.Game
    Add-WorkRow "x$g" (F 'UiExtractRow' $ShortName[$g])
    if (Wants-Hd $g) { Add-WorkRow "hd$g" (F 'UiHdRow' $ShortName[$g]) }
  }
  $ui.NowText.Text = T 'UiCopy'; $ui.DetailText.Text = ''
  $script:statusPos = 0; $script:running = $null; $script:failures.Clear(); $script:finished = $false; $script:pct = -1
  $script:t0 = Get-Date
  Set-Work 'copy' 'run'; $script:running = 'copy'
  $script:installDir = $dest
  $script:started = $true
  $script:inner = Start-Process -FilePath $cfg.Setup -ArgumentList $argList -PassThru
  Set-Page 2
  $timer.Start()
}

# ---- page 3: play --------------------------------------------------------------------------------
$donateUrl = 'https://paypal.me/mymusicminis'
$art = @{ 1 = 'sweet'; 2 = 'coffee'; 3 = 'heart' }
foreach ($i in 1..3) {
  $b = New-Object Windows.Controls.Button; $b.Cursor = 'Hand'; $b.Margin = '0,0,10,8'; $b.Tag = $i
  $b.Template = [Windows.Markup.XamlReader]::Parse('<ControlTemplate xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation" TargetType="Button"><Border x:Name="b" xmlns:x="http://schemas.microsoft.com/winfx/2006/xaml" CornerRadius="10" Background="#0070BA" Padding="14,9"><ContentPresenter/></Border><ControlTemplate.Triggers><Trigger Property="IsMouseOver" Value="True"><Setter TargetName="b" Property="Background" Value="#1A85CC"/></Trigger></ControlTemplate.Triggers></ControlTemplate>')
  $sp = New-Object Windows.Controls.StackPanel; $sp.Orientation = 'Horizontal'
  $img = New-Object Windows.Controls.Image; $img.Width = 16; $img.Height = 16; $img.Margin = '0,0,9,0'
  $bmp = Join-Path $cfg.Art ('donate-' + $art[$i] + '.bmp'); if (Test-Path -LiteralPath $bmp) { $img.Source = Load-Image $bmp }
  [void]$sp.Children.Add($img); [void]$sp.Children.Add((New-Text (T "Donate$i") 13 '#FFFFFF' 'SemiBold'))
  $b.Content = $sp
  $b.Add_Click({ param($s) $u = switch ($s.Tag) { 1 { "$donateUrl/1EUR" } 2 { "$donateUrl/3EUR" } default { $donateUrl } }; Start-Process $u })
  [void]$ui.DonateRow.Children.Add($b)
}
$lock = Join-Path $cfg.Art 'donate-lock.bmp'; if (Test-Path -LiteralPath $lock) { $ui.LockIcon.Source = Load-Image $lock }
$ui.LogBtn.Add_Click({
  $log = Join-Path $script:installDir 'install.log'; if (-not (Test-Path -LiteralPath $log)) { $log = Join-Path $Dir 'setup.log' }
  if (Test-Path -LiteralPath $log) { Start-Process notepad.exe -ArgumentList ('"' + $log + '"') } })

# ---- pages ---------------------------------------------------------------------------------------
$script:page = 0
function Set-Page([int]$n) {
  $script:page = $n
  foreach ($i in 0..3) { $ui["Page$i"].Visibility = if ($i -eq $n) { 'Visible' } else { 'Collapsed' } }
  Show-Steps $n
  $ui.BackBtn.Visibility = if ($n -eq 1) { 'Visible' } else { 'Collapsed' }
  $ui.NextBtn.Visibility = 'Visible'; $ui.NextBtn.IsEnabled = $true
  $ui.DiskBox.Visibility = if ($n -le 1) { 'Visible' } else { 'Collapsed' }
  switch ($n) {
    0 { $ui.NextBtn.Content = T 'UiNext'; Update-Footer }
    1 { Refresh-Hd; $ui.NextBtn.Content = T 'UiInstall'; Update-Footer
        $ui.SizeNote.Text = F 'UiSizeNote' ($ProgGB.ToString('0.0', $Culture)) $GameGB }
    2 { $ui.NextBtn.Visibility = 'Collapsed'; $ui.BackBtn.Visibility = 'Collapsed' }
    3 { $ok = -not $script:failures.Count
        $ui.DoneTitle.Text = T $(if ($ok) { 'UiDoneTitle' } else { 'UiDoneFailTitle' })
        $ui.DoneSub.Text = T $(if ($ok) { 'UiDoneSub' } else { 'UiDoneFailSub' })
        $ui.FailList.Children.Clear()
        foreach ($f in $script:failures) { $t = New-Text $f 12.5 '#FFB4B4'; $t.TextWrapping = 'Wrap'; $t.Margin = '0,2'; [void]$ui.FailList.Children.Add($t) }
        $ui.FailBox.Visibility = if ($ok) { 'Collapsed' } else { 'Visible' }
        $ui.NextBtn.Content = T 'PlayNow'
        $ui.NextBtn.IsEnabled = Test-Path -LiteralPath (Join-Path $script:installDir 'Jak Enhanced Trilogy.bat')
        $ui.BackBtn.Content = T 'UiFinish'; $ui.BackBtn.Visibility = 'Visible' }
  }
}
$ui.NextBtn.Add_Click({
  switch ($script:page) {
    0 { Set-Page 1 }
    1 { try { Start-Build } catch { Show-Note $ui.Note1 $_.Exception.Message } }
    3 { Start-Process -FilePath (Join-Path $script:installDir 'Jak Enhanced Trilogy.bat') -WorkingDirectory $script:installDir -WindowStyle Minimized
        $win.Close() }
  }
})
$ui.BackBtn.Add_Click({ if ($script:page -eq 3) { $win.Close() } else { Show-Note $ui.Note1 ''; Set-Page 0 } })
$ui.MinBtn.Add_Click({ $win.WindowState = 'Minimized' })
$ui.CloseBtn.Add_Click({ $win.Close() })
$win.Add_Closing({ param($s, $e)
  if ($script:page -eq 2 -and $script:inner -and -not $script:inner.HasExited) {
    if ([Windows.MessageBox]::Show($win, (T 'UiCancelAsk'), 'Jak Enhanced Trilogy', 'YesNo', 'Warning') -ne 'Yes') { $e.Cancel = $true; return }
    $timer.Stop()
    & taskkill.exe /T /F /PID $script:inner.Id | Out-Null
  }
})
foreach ($h in $ui.Head, $ui.Root) { $h.Add_MouseLeftButtonDown({ param($s, $e) if ($e.OriginalSource -is [Windows.Controls.Panel] -or $e.OriginalSource -is [Windows.Shapes.Shape] -or $e.OriginalSource -is [Windows.Controls.Border]) { try { $win.DragMove() } catch { } } }) }

# the search's results, when it ends
$scanTimer = New-Object Windows.Threading.DispatcherTimer
$scanTimer.Interval = [TimeSpan]::FromMilliseconds(250)
$scanTimer.Add_Tick({
  if (-not $scanAsync.IsCompleted) { return }
  $scanTimer.Stop()
  try { foreach ($d in $scan.EndInvoke($scanAsync)) { Add-Disc $d $false } } catch { }
  $scan.Dispose()
  $ui.DiscHint.Text = T 'UiNoneFound'
  Refresh-Discs; Update-Footer
})
$scanTimer.Start()

# bare bones: the long explanations stay out of the window
foreach ($e in $ui.DiscInfo.Parent, $ui.HdInfo.Parent, $ui.SizeNote, $ui.DropHint, $ui.DropHint.Parent.Children[1]) { $e.Visibility = 'Collapsed' }

Set-Page 0
Refresh-Discs
[void]$win.ShowDialog()
exit 0
} catch {
  try { [IO.File]::WriteAllText($errorFile, ($_ | Out-String)) } catch { }
  if ($script:started) { exit 0 }  # (the build runs on: no second wizard)
  exit 3
}
