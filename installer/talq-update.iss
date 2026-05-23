; UPDATE installer — full-payload point-release upgrade.
;
; HISTORY: this used to ship ONLY talq.exe over an existing install. That
; broke 0.29.5: the release added new GStreamer plugin dependencies
; (decodebin3/playback, opusparse) the slim package did not carry, so
; anyone who upgraded via it ran a talq.exe whose required plugins were
; absent and the app died the instant a call connected.
;
; POLICY (do not regress): an update MUST always carry the COMPLETE
; dependency set and remove anything dropped since the prior version.
; This package now bundles the identical full payload as talq-setup.iss;
; the only difference is update-friendly UX (reuses the previous install
; dir, no dir/group pages). It is also a valid standalone install.
[Setup]
AppName=TalQ
AppVersion=0.37.1
AppPublisher=TalQ
AppPublisherURL=https://github.com/kalinbogatzevski/talq-desktop
DefaultDirName={localappdata}\Programs\TalQ
UsePreviousAppDir=yes
PrivilegesRequired=lowest
DisableDirPage=yes
DisableProgramGroupPage=yes
OutputDir=..\dist
OutputBaseFilename=TalQ-v0.37.1-Update
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

[InstallDelete]
; Identical to talq-setup.iss: wipe the version-specific runtime so a
; stale/removed plugin or DLL from the prior version can never linger
; (ignoreversion only overwrites files still present in the new payload,
; it can never delete one). [Files] below recopies the complete set.
Type: filesandordirs; Name: "{app}\gst-plugins"
Type: files; Name: "{app}\*.dll"

[Files]
; The COMPLETE payload — every DLL + the full GStreamer plugin set —
; exactly as the full installer. Never a talq.exe-only patch again.
Source: "..\dist\TalQ-v0.37.1-win64\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Run]
Filename: "{app}\talq.exe"; Description: "Launch TalQ"; Flags: nowait postinstall
