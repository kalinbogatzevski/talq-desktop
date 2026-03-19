[Setup]
AppName=123NET Talk
AppVersion=0.5.1
AppPublisher=123 NET CPT (PTY) LTD
AppPublisherURL=https://123net.link
DefaultDirName={autopf}\123NET Talk
DefaultGroupName=123NET Talk
OutputDir=..\dist
OutputBaseFilename=123NET-Talk-v0.5.1-Setup
SetupIconFile=..\resources\talq.ico
UninstallDisplayIcon={app}\talq.exe
Compression=lzma2/ultra64
SolidCompression=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
WizardStyle=modern
DisableProgramGroupPage=yes

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "Create a desktop shortcut"; GroupDescription: "Additional shortcuts:"
Name: "startmenuicon"; Description: "Create a Start Menu shortcut"; GroupDescription: "Additional shortcuts:"
Name: "autostart"; Description: "Start 123NET Talk when Windows starts"; GroupDescription: "System:"; Flags: checkedonce

[Files]
Source: "..\dist\123NET-Talk-v0.5.1-win64\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\123NET Talk"; Filename: "{app}\talq.exe"; Tasks: startmenuicon
Name: "{group}\Uninstall 123NET Talk"; Filename: "{uninstallexe}"; Tasks: startmenuicon
Name: "{autodesktop}\123NET Talk"; Filename: "{app}\talq.exe"; Tasks: desktopicon

[Registry]
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueName: "123NET Talk"; ValueType: string; ValueData: """{app}\talq.exe"""; Flags: uninsdeletevalue; Tasks: autostart

[Run]
Filename: "{app}\talq.exe"; Description: "Launch 123NET Talk"; Flags: nowait postinstall skipifsilent
