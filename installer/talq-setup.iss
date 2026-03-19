[Setup]
AppName=TalQ
AppVersion=0.5.1
AppPublisher=TalQ
AppPublisherURL=https://gitlab.123net.link/kalin/talk-desktop-qt
DefaultDirName={autopf}\TalQ
DefaultGroupName=TalQ
OutputDir=..\dist
OutputBaseFilename=TalQ-v0.5.1-Setup
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
Name: "autostart"; Description: "Start TalQ when Windows starts"; GroupDescription: "System:"

[Files]
Source: "..\dist\TalQ-v0.5.1-win64\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\TalQ"; Filename: "{app}\talq.exe"; Tasks: startmenuicon
Name: "{group}\Uninstall TalQ"; Filename: "{uninstallexe}"; Tasks: startmenuicon
Name: "{autodesktop}\TalQ"; Filename: "{app}\talq.exe"; Tasks: desktopicon

[Registry]
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueName: "TalQ"; ValueType: string; ValueData: """{app}\talq.exe"""; Flags: uninsdeletevalue; Tasks: autostart

[Run]
Filename: "{app}\talq.exe"; Description: "Launch TalQ"; Flags: nowait postinstall skipifsilent
