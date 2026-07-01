[Setup]
; Stable AppId so Inno Setup reliably detects an existing TalQ install and
; treats subsequent runs as upgrades (reuses the prior install dir + the
; previously chosen [Tasks] selections, no re-prompting). This is what
; replaces the old separate "Update" installer — one .exe, both modes.
AppId={{B7E2A6F4-3D14-4F2A-9B5E-0A1C8F4E2D31}}
AppName=TalQ
AppVersion=0.52.9
AppPublisher=TalQ
AppPublisherURL=https://github.com/kalinbogatzevski/talq-desktop
DefaultDirName={localappdata}\Programs\TalQ
PrivilegesRequired=lowest
DefaultGroupName=TalQ
OutputDir=..\dist
OutputBaseFilename=TalQ-v0.52.9-Setup
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

; [InstallDelete] — DELIBERATELY EMPTY. Do NOT add a blanket wipe here.
;
; A blanket pre-delete (e.g. Type: files; Name: "{app}\*.dll") bricks an
; in-place auto-update: [InstallDelete] runs BEFORE [Files], so it removes
; libglib-2.0-0.dll & co. first, and if ANYTHING then stops [Files] from
; recopying one of them (the running TalQ still holds a GStreamer DLL lock
; during the updater hand-off → reboot-pending replace; an AV scan; an
; interrupted copy), the app is left with the file DELETED and not yet
; restored → "libglib-2.0-0.dll was not found" = dead app. This is exactly
; what hit 0.51.12 in the field (manual install was fine — no running TalQ
; holding locks — but the auto-update path bricked it).
;
; [Files] with `ignoreversion` overwrites every shipped file in place and,
; for a locked file, schedules a replace-on-reboot WHILE LEAVING THE OLD
; WORKING COPY IN PLACE. So the worst case becomes a slightly-stale-until-
; reboot DLL — never a MISSING one. The invariant we must preserve: never
; delete a working file before its replacement is safely on disk.
;
; Stale-file cleanup (a file a NEW version no longer ships) is the only
; thing we lose. That is benign for us (an unused extra DLL/plugin just
; sits there). If a specific obsolete file ever genuinely must go, retire
; it with a TARGETED named entry for that exact file — it is safe precisely
; because [Files] won't recreate it and the new binary doesn't need it.
; Never reintroduce a wildcard delete of files this version still ships.

[Files]
Source: "..\dist\TalQ-v0.52.9-win64\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\TalQ"; Filename: "{app}\talq.exe"; Tasks: startmenuicon
Name: "{group}\Uninstall TalQ"; Filename: "{uninstallexe}"; Tasks: startmenuicon
Name: "{autodesktop}\TalQ"; Filename: "{app}\talq.exe"; Tasks: desktopicon

[Registry]
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueName: "TalQ"; ValueType: string; ValueData: """{app}\talq.exe"""; Flags: uninsdeletevalue; Tasks: autostart

[Run]
Filename: "{app}\talq.exe"; Description: "Launch TalQ"; Flags: nowait postinstall

[Code]
// Belt-and-suspenders to /CLOSEAPPLICATIONS. An auto-update launches this
// installer and then asks the running TalQ to quit so its files can be replaced.
// If that quit wedges (a teardown hang) OR the window's minimize-on-close
// swallows the Restart-Manager close, the old process keeps the program files
// locked and the update silently never installs (field: a box left on the old
// version with the new one already downloaded). Force-terminate any lingering
// talq.exe here, BEFORE the file copy, so a stuck old process can never block the
// update. The [Run] entry relaunches talq.exe afterwards; TalQ's single-instance
// guard collapses any duplicate launch. Runs for silent (auto-update) AND manual
// installs — closing the target app before an upgrade is expected either way.
function PrepareToInstall(var NeedsRestart: Boolean): String;
var
  ResultCode: Integer;
begin
  Exec('taskkill.exe', '/F /IM talq.exe', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
  Sleep(800);   // let the OS release the killed process's file handles
  Result := '';  // empty string = proceed with the install
end;
