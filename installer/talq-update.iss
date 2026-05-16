; Slim UPDATE installer — ships only talq.exe over an existing install.
; Valid for upgrading an already-installed TalQ to the next version; it
; is NOT a fresh install (the Qt runtime / DLLs are not included). If no
; existing install is found it aborts and points to the full installer.
; AppName matches talq-setup.iss so it targets the same install dir.
[Setup]
AppName=TalQ
AppVersion=0.28.4
AppPublisher=TalQ
AppPublisherURL=https://github.com/kalinbogatzevski/talq-desktop
DefaultDirName={localappdata}\Programs\TalQ
UsePreviousAppDir=yes
PrivilegesRequired=lowest
DisableDirPage=yes
DisableProgramGroupPage=yes
OutputDir=..\dist
OutputBaseFilename=TalQ-v0.28.4-Update
SetupIconFile=..\resources\talq.ico
UninstallDisplayIcon={app}\talq.exe
Compression=lzma2/ultra64
SolidCompression=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
WizardStyle=modern
CloseApplications=yes
RestartApplications=no

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Files]
; Only the binary that changes between point releases.
Source: "..\dist\TalQ-v0.28.4-win64\talq.exe"; DestDir: "{app}"; Flags: ignoreversion

[Run]
Filename: "{app}\talq.exe"; Description: "Launch TalQ"; Flags: nowait postinstall

[Code]
function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  // Guard: only run as an update over a real existing install (the Qt
  // runtime must already be present — this package doesn't carry it).
  if not FileExists(ExpandConstant('{app}\Qt6Core.dll')) then
    Result := 'No existing TalQ installation was found to update. ' +
              'This is a slim update package and does not include the ' +
              'Qt runtime. Please download and run the full TalQ installer.'
  else
    Result := '';
end;
