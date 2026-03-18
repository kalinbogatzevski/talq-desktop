[Setup]
AppName=TalQ
AppVersion=0.3.0
AppPublisher=123NET
AppPublisherURL=https://123net.link
DefaultDirName={autopf}\TalQ
DefaultGroupName=TalQ
OutputDir=..\dist
OutputBaseFilename=TalQ-v0.3.0-Setup
SetupIconFile=..\resources\talq.ico
UninstallDisplayIcon={app}\talk-qt.exe
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

[Files]
Source: "..\dist\TalQ-v0.3.0-win64\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\TalQ"; Filename: "{app}\talk-qt.exe"; Tasks: startmenuicon
Name: "{group}\Uninstall TalQ"; Filename: "{uninstallexe}"; Tasks: startmenuicon
Name: "{autodesktop}\TalQ"; Filename: "{app}\talk-qt.exe"; Tasks: desktopicon

[Run]
Filename: "{app}\talk-qt.exe"; Description: "Launch TalQ"; Flags: nowait postinstall skipifsilent
