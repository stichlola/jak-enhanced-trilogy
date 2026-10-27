; Jak Enhanced Trilogy: the Windows installer (Inno Setup 6).
;
;   ISCC /DMyVersion=v0.4 /DSrc=<release folder> /O<output folder> scripts\enhanced\installer.iss
;
; make_release.py --installer builds it from the release folder it lays out. Pages: where to install,
; the player's own disc images (one, two or all three), the HD textures for each game (with the time
; they take). Then it copies the programs (no game data inside) and starts the launcher, which
; extracts the discs (iso-paths.txt), makes the HD textures that were picked (data\hd-textures-jakN.txt)
; and plays.

#ifndef MyVersion
  #define MyVersion "dev"
#endif
#ifndef Src
  #define Src "..\..\out\release\Jak-Enhanced-Trilogy"
#endif

[Setup]
AppId={{5C7E2A51-3B8F-4E0A-9D4C-8A1F6E2B7C90}
AppName=Jak Enhanced Trilogy
AppVersion={#MyVersion}
AppPublisher=stichlola
AppPublisherURL=https://github.com/stichlola/jak-enhanced-trilogy
DefaultDirName={localappdata}\Programs\Jak Enhanced Trilogy
DefaultGroupName=Jak Enhanced Trilogy
PrivilegesRequired=lowest
DisableProgramGroupPage=yes
OutputBaseFilename=Setup-Jak-Enhanced-Trilogy-{#MyVersion}
UninstallDisplayIcon={app}\gk.exe
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
; the extracted discs: about 30 GB a game
ExtraDiskSpaceRequired=32212254720
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible

[Languages]
Name: "en"; MessagesFile: "compiler:Default.isl"
Name: "it"; MessagesFile: "compiler:Languages\Italian.isl"

[CustomMessages]
en.DiscsTitle=Your discs
en.DiscsSub=Pick your own disc images (.iso) of the PS2 games.
en.DiscsText=One, two or all three games (PAL, NTSC or NTSC-J). No game data comes with Jak Enhanced Trilogy: everything is made from your discs, 20-40 minutes a game the first time.
en.Disc1=Jak and Daxter: The Precursor Legacy:
en.Disc2=Jak II:
en.Disc3=Jak 3:
en.NoDisc=Pick at least one disc image.
en.Missing=File not found:
en.HdTitle=HD textures
en.HdSub=Every texture redrawn 4x by an AI (Real-ESRGAN) on your graphics card.
en.HdText=Optional: the games work the same without them, and the launcher asks again for the games you skip now. They are made on this computer from your discs after the extraction and take a while (it depends on the graphics card), plus 1-4 GB a game:
en.Hd1=Jak 1 (about 30 minutes)
en.Hd2=Jak II (about 1-2 hours)
en.Hd3=Jak 3 (about 1-2 hours)
en.PlayNow=Extract the discs and play now
en.Desktop=Create a desktop shortcut
it.DiscsTitle=I tuoi dischi
it.DiscsSub=Scegli le immagini (.iso) dei tuoi dischi PS2.
it.DiscsText=Uno, due o tutti e tre i giochi (PAL, NTSC o NTSC-J). Jak Enhanced Trilogy non contiene dati dei giochi: tutto si ricava dai tuoi dischi, 20-40 minuti a gioco la prima volta.
it.Disc1=Jak and Daxter: The Precursor Legacy:
it.Disc2=Jak II:
it.Disc3=Jak 3:
it.NoDisc=Scegli almeno un'immagine disco.
it.Missing=File non trovato:
it.HdTitle=Texture HD
it.HdSub=Ogni texture ridisegnata 4x da un'IA (Real-ESRGAN) sulla tua scheda video.
it.HdText=Facoltative: i giochi funzionano uguale senza, e per i giochi che salti ora il launcher lo richiede. Si fanno su questo computer dai tuoi dischi dopo l'estrazione e richiedono tempo (dipende dalla scheda video), più 1-4 GB a gioco:
it.Hd1=Jak 1 (circa 30 minuti)
it.Hd2=Jak II (circa 1-2 ore)
it.Hd3=Jak 3 (circa 1-2 ore)
it.PlayNow=Estrai i dischi e gioca ora
it.Desktop=Crea un collegamento sul desktop

[Tasks]
Name: "desktopicon"; Description: "{cm:Desktop}"

[Files]
Source: "{#Src}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\Jak Enhanced Trilogy"; Filename: "{app}\Jak Enhanced Trilogy.bat"; WorkingDir: "{app}"; IconFilename: "{app}\gk.exe"
Name: "{group}\{cm:UninstallProgram,Jak Enhanced Trilogy}"; Filename: "{uninstallexe}"
Name: "{userdesktop}\Jak Enhanced Trilogy"; Filename: "{app}\Jak Enhanced Trilogy.bat"; WorkingDir: "{app}"; IconFilename: "{app}\gk.exe"; Tasks: desktopicon

[Run]
Filename: "{app}\Jak Enhanced Trilogy.bat"; WorkingDir: "{app}"; Description: "{cm:PlayNow}"; Flags: postinstall shellexec nowait skipifsilent

[UninstallDelete]
; what the launcher made: the extracted discs, the compiled games, the HD textures (saves and settings
; stay in %APPDATA%\OpenGOAL, disc images in ISO stay)
Type: filesandordirs; Name: "{app}\data"
Type: files; Name: "{app}\ISO\iso-paths.txt"

[Code]
var
  DiscPage: TInputFileWizardPage;
  HdPage: TInputOptionWizardPage;

procedure InitializeWizard;
var
  i: Integer;
begin
  DiscPage := CreateInputFilePage(wpSelectDir, CustomMessage('DiscsTitle'), CustomMessage('DiscsSub'),
    CustomMessage('DiscsText'));
  for i := 1 to 3 do
    DiscPage.Add(CustomMessage('Disc' + IntToStr(i)), 'Disc images (*.iso)|*.iso|All files|*.*', '.iso');
  HdPage := CreateInputOptionPage(DiscPage.ID, CustomMessage('HdTitle'), CustomMessage('HdSub'),
    CustomMessage('HdText'), False, False);
  for i := 1 to 3 do
    HdPage.Add(CustomMessage('Hd' + IntToStr(i)));
end;

function NextButtonClick(CurPageID: Integer): Boolean;
var
  i, count: Integer;
begin
  Result := True;
  // a silent install (/VERYSILENT) leaves the discs to the ISO folder
  if (CurPageID = DiscPage.ID) and not WizardSilent then begin
    count := 0;
    for i := 0 to 2 do
      if Trim(DiscPage.Values[i]) <> '' then begin
        if not FileExists(DiscPage.Values[i]) then begin
          MsgBox(CustomMessage('Missing') + #13#10 + DiscPage.Values[i], mbError, MB_OK);
          Result := False;
          exit;
        end;
        count := count + 1;
      end;
    if count = 0 then begin
      MsgBox(CustomMessage('NoDisc'), mbError, MB_OK);
      Result := False;
    end;
  end;
end;

function ShouldSkipPage(PageID: Integer): Boolean;
begin
  // the HD page only for the games that have a disc
  Result := False;
  if PageID = HdPage.ID then begin
    HdPage.CheckListBox.ItemEnabled[0] := Trim(DiscPage.Values[0]) <> '';
    HdPage.CheckListBox.ItemEnabled[1] := Trim(DiscPage.Values[1]) <> '';
    HdPage.CheckListBox.ItemEnabled[2] := Trim(DiscPage.Values[2]) <> '';
  end;
end;

procedure CurStepChanged(CurStep: TSetupStep);
var
  i: Integer;
  paths: String;
  answer: String;
begin
  if CurStep = ssPostInstall then begin
    paths := '';
    for i := 0 to 2 do
      if Trim(DiscPage.Values[i]) <> '' then
        paths := paths + DiscPage.Values[i] + #13#10;
    ForceDirectories(ExpandConstant('{app}\ISO'));
    SaveStringToFile(ExpandConstant('{app}\ISO\iso-paths.txt'), paths, False);
    for i := 0 to 2 do
      if Trim(DiscPage.Values[i]) <> '' then begin
        if HdPage.Values[i] then answer := 'y' else answer := 'n';
        SaveStringToFile(ExpandConstant('{app}\data\hd-textures-jak' + IntToStr(i + 1) + '.txt'),
          answer, False);
      end;
  end;
end;
