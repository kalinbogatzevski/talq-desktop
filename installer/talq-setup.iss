[Setup]
; Stable AppId so Inno Setup reliably detects an existing TalQ install and
; treats subsequent runs as upgrades (reuses the prior install dir + the
; previously chosen [Tasks] selections, no re-prompting). This is what
; replaces the old separate "Update" installer — one .exe, both modes.
AppId={{B7E2A6F4-3D14-4F2A-9B5E-0A1C8F4E2D31}}
AppName=TalQ
AppVersion=0.51.3
AppPublisher=TalQ
AppPublisherURL=https://github.com/kalinbogatzevski/talq-desktop
DefaultDirName={localappdata}\Programs\TalQ
PrivilegesRequired=lowest
DefaultGroupName=TalQ
OutputDir=..\dist
OutputBaseFilename=TalQ-v0.51.3-Setup
SetupIconFile=..\resources\talq.ico
UninstallDisplayIcon={app}\talq.exe
WizardImageFile=..\resources\talq-wizard.bmp
WizardSmallImageFile=..\resources\talq-wizard-small.bmp
Compression=lzma2/ultra64
SolidCompression=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
WizardStyle=modern
DisableProgramGroupPage=yes
; Upgrade UX — defaults are already "yes" for these, but stating them
; explicitly so the policy isn't a hidden Inno default.
UsePreviousAppDir=yes
UsePreviousTasks=yes
; Auto-updater calls us with /VERYSILENT /CLOSEAPPLICATIONS /RESTARTAPPLICATIONS;
; honour those flags by allowing the kill-and-restart of a running TalQ.
CloseApplications=yes
RestartApplications=yes

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "Create a desktop shortcut"; GroupDescription: "Additional shortcuts:"
Name: "startmenuicon"; Description: "Create a Start Menu shortcut"; GroupDescription: "Additional shortcuts:"
Name: "autostart"; Description: "Start TalQ when Windows starts"; GroupDescription: "System:"

[InstallDelete]
; An update MUST leave the on-disk dependency set identical to the one
; this version ships — a stale or removed GStreamer plugin / runtime DLL
; left behind by a prior version is a dead app for a comms client (this
; is exactly what broke 0.29.5: a new plugin dependency missing on disk).
; Wipe the version-specific runtime here; [Files] below recopies the
; complete, current payload. ignoreversion alone only overwrites files
; that still exist in the new payload — it can never remove one.
Type: filesandordirs; Name: "{app}\gst-plugins"
Type: files; Name: "{app}\*.dll"

[Files]
Source: "..\dist\TalQ-v0.51.3-win64\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\TalQ"; Filename: "{app}\talq.exe"; Tasks: startmenuicon
Name: "{group}\Uninstall TalQ"; Filename: "{uninstallexe}"; Tasks: startmenuicon
Name: "{autodesktop}\TalQ"; Filename: "{app}\talq.exe"; Tasks: desktopicon

[Registry]
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueName: "TalQ"; ValueType: string; ValueData: """{app}\talq.exe"""; Flags: uninsdeletevalue; Tasks: autostart

[Run]
Filename: "{app}\talq.exe"; Description: "Launch TalQ"; Flags: nowait postinstall
