[Setup]
AppName=123NET TalQ
AppVersion=0.17.3
AppPublisher=123 NET CPT (PTY) LTD
AppPublisherURL=https://123net.link
DefaultDirName={localappdata}\Programs\123NET TalQ
PrivilegesRequired=lowest
DefaultGroupName=123NET TalQ
OutputDir=..\dist
OutputBaseFilename=123NET-TalQ-v0.17.3-Setup
SetupIconFile=..\resources\talq.ico
UninstallDisplayIcon={app}\talq.exe
WizardImageFile=..\resources\123net-wizard.bmp
WizardSmallImageFile=..\resources\123net-wizard-small.bmp
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
Name: "autostart"; Description: "Start 123NET TalQ when Windows starts"; GroupDescription: "System:"; Flags: checkedonce

[Files]
Source: "..\dist\TalQ-v0.17.3-win64-123net\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\123NET TalQ"; Filename: "{app}\talq.exe"; Tasks: startmenuicon
Name: "{group}\Uninstall 123NET TalQ"; Filename: "{uninstallexe}"; Tasks: startmenuicon
Name: "{autodesktop}\123NET TalQ"; Filename: "{app}\talq.exe"; Tasks: desktopicon

[Registry]
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueName: "123NET TalQ"; ValueType: string; ValueData: """{app}\talq.exe"""; Flags: uninsdeletevalue; Tasks: autostart

[Run]
Filename: "{app}\talq.exe"; Description: "Launch 123NET TalQ"; Flags: nowait postinstall skipifsilent
