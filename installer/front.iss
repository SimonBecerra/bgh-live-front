; BGH LIVE Front installer. GitHub builds it (see .github/workflows/build.yml).
; It puts the plug-in where OBS 28 and later look for plug-ins that aren't part of OBS itself:
;   C:\ProgramData\obs-studio\plugins\bgh-live-front\bin\64bit\bgh-live-front.dll
;   C:\ProgramData\obs-studio\plugins\bgh-live-front\data\...
; and lets phones on the church Wi-Fi reach the remote through Windows Firewall (TCP port 8650).

#define AppVersion GetEnv("FRONT_VERSION")
#if AppVersion == ""
  #define AppVersion "1.0.0"
#endif

[Setup]
AppId={{8C5B2E1A-6D4F-4B7A-9E21-B6F0A1C3D7E9}
AppName=BGH LIVE Front
AppVersion={#AppVersion}
AppVerName=BGH LIVE Front {#AppVersion}
AppPublisher=Barrington Gospel Hall
AppPublisherURL=https://github.com/SimonBecerra/bgh-live-front
DefaultDirName={commonappdata}\obs-studio\plugins\bgh-live-front
DisableDirPage=yes
DisableProgramGroupPage=yes
DisableReadyPage=yes
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
OutputDir=..\dist
OutputBaseFilename=BGH-LIVE-Front-Setup
SetupIconFile=icon.ico
UninstallDisplayIcon={app}\icon.ico
UninstallDisplayName=BGH LIVE Front (OBS plug-in)
WizardStyle=modern
Compression=lzma2
SolidCompression=yes
CloseApplications=no

[Tasks]
Name: "startobs"; Description: "Open OBS when Windows starts (for everyone who signs in to this laptop)"; Check: ObsInstalled

[Icons]
; A shortcut in Windows' Startup folder. It starts OBS from its own folder, which OBS needs to find its files.
; --disable-shutdown-check: if the laptop was switched off without closing OBS, OBS opens normally instead of
; stopping to ask about Safe Mode, so nobody has to be at the laptop.
Name: "{commonstartup}\OBS Studio"; Filename: "{code:ObsDir}\bin\64bit\obs64.exe"; WorkingDir: "{code:ObsDir}\bin\64bit"; Parameters: "--disable-shutdown-check"; Comment: "Opens OBS, and with it the BGH LIVE Front phone remote"; Tasks: startobs

[InstallDelete]
; Unticked when installing again: take the Startup shortcut away
Type: files; Name: "{commonstartup}\OBS Studio.lnk"; Tasks: not startobs

[Files]
Source: "..\release\RelWithDebInfo\bgh-live-front\*"; DestDir: "{app}"; Excludes: "*.pdb"; Flags: recursesubdirs createallsubdirs ignoreversion
Source: "icon.ico"; DestDir: "{app}"; Flags: ignoreversion

[Run]
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall delete rule name=""BGH LIVE Front"""; Flags: runhidden; StatusMsg: "Letting phones on the Wi-Fi reach the remote..."
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall add rule name=""BGH LIVE Front"" dir=in action=allow protocol=TCP localport=8650 profile=any"; Flags: runhidden

[UninstallRun]
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall delete rule name=""BGH LIVE Front"""; Flags: runhidden; RunOnceId: "RemoveFirewallRule"

[Messages]
WelcomeLabel2=This installs BGH LIVE Front, the phone remote for the laptop at the front of the auditorium.%n%nIt adds a BGH LIVE Front panel to OBS with a QR code for pairing phones.
FinishedHeadingLabel=BGH LIVE Front is installed
FinishedLabel=Open OBS. The BGH LIVE Front panel shows a QR code: scan it with a phone on the church Wi-Fi.%n%nIf you don't see the panel, open Tools > BGH LIVE Front in OBS.

[Code]
// Where OBS is installed: OBS's installer records it in the registry
function ObsDir(Param: String): String;
begin
  if not RegQueryStringValue(HKLM64, 'SOFTWARE\OBS Studio', '', Result) or (Result = '') then
    Result := ExpandConstant('{commonpf64}\obs-studio');
end;

function ObsInstalled(): Boolean;
begin
  Result := FileExists(ObsDir('') + '\bin\64bit\obs64.exe');
end;

function ObsRunning(): Boolean;
var
  Code: Integer;
begin
  Result := Exec(ExpandConstant('{cmd}'), '/C tasklist /FI "IMAGENAME eq obs64.exe" /NH | find /I "obs64.exe" >nul', '', SW_HIDE, ewWaitUntilTerminated, Code) and (Code = 0);
end;

function InitializeSetup(): Boolean;
begin
  Result := True;
  while ObsRunning() do
  begin
    if MsgBox('OBS is open. Close OBS, then click OK to carry on installing BGH LIVE Front.', mbInformation, MB_OKCANCEL) = IDCANCEL then
    begin
      Result := False;
      Exit;
    end;
  end;
end;

function InitializeUninstall(): Boolean;
begin
  Result := True;
  while ObsRunning() do
  begin
    if MsgBox('OBS is open. Close OBS, then click OK to remove BGH LIVE Front.', mbInformation, MB_OKCANCEL) = IDCANCEL then
    begin
      Result := False;
      Exit;
    end;
  end;
end;
