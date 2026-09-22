#define AppId "4678f42c-c6a2-4df9-bc2a-dddbd2613045"
#define AppName "RegKit"
#define AppExeName "regkit.exe"
#define AppPublisher "nohuto"
#define AppCopyright "(C) 2026 nohuto"
#define AppURL "https://github.com/nohuto/regkit"
#ifndef Arch
  #define Arch "x64"
#endif
#if Arch == "x86"
  #define BuildDir "..\\build32\\Release"
#else
  #define BuildDir "..\\build\\Release"
#endif
#define AppVersion GetVersionNumbersString(AddBackslash(BuildDir) + AppExeName)

[Setup]
AppId={#AppId}
AppName={#AppName}
AppVersion={#AppVersion}
AppPublisher={#AppPublisher}
AppPublisherURL={#AppURL}
AppSupportURL={#AppURL}
AppUpdatesURL={#AppURL}
VersionInfoVersion={#AppVersion}
VersionInfoCopyright={#AppCopyright}
DefaultDirName={code:GetDefaultDir}
DefaultGroupName=RegKit
CreateAppDir=yes
DisableDirPage=no
DisableProgramGroupPage=yes
LicenseFile=..\LICENSE
SetupIconFile=..\assets\icons\regkit.ico
UninstallDisplayIcon={app}\{#AppExeName}
SignTool=regkit
WizardSmallImageFile=images\small-55.png,images\small-69.png,images\small-83.png,images\small-97.png,images\small-110.png,images\small-138.png
WizardImageFile=images\large-100.png,images\large-125.png,images\large-150.png,images\large-175.png,images\large-200.png,images\large-250.png
Compression=lzma2
SolidCompression=yes
ChangesAssociations=yes
#if Arch == "x64"
ArchitecturesAllowed=x64os
ArchitecturesInstallIn64BitMode=x64os
#endif
PrivilegesRequired=admin
PrivilegesRequiredOverridesAllowed=dialog
WizardStyle=modern
OutputDir=dist
OutputBaseFilename=RegKit-Setup-{#AppVersion}-{#Arch}

[Messages]
PrivilegesRequiredOverrideTitle=Install Mode
PrivilegesRequiredOverrideInstruction=Select install mode
PrivilegesRequiredOverrideText1=Install %1 for:
PrivilegesRequiredOverrideAllUsersRecommended=&All users
PrivilegesRequiredOverrideCurrentUser=&Only me

[Types]
Name: "full"; Description: "Full installation"
Name: "minimal"; Description: "Minimal installation"
Name: "custom"; Description: "Custom installation"; Flags: iscustom

[Components]
Name: "main"; Description: "RegKit"; Types: full minimal custom; Flags: fixed
Name: "icons"; Description: "Classic toolbar icon set"; Types: full
Name: "comments"; Description: "Default comments"; Types: full
Name: "traces"; Description: "Trace files for the Trace menu"; Types: full
Name: "bitfields"; Description: "Bitfield definitions"; Types: full
Name: "defaults"; Description: "Registry exports for the Default menu"; Types: full

[Tasks]
Name: "startmenu"; Description: "Start Menu shortcut"; GroupDescription: "Shortcuts:"; Flags: checkedonce
Name: "desktopicon"; Description: "Desktop shortcut"; GroupDescription: "Shortcuts:"
Name: "replace_regedit"; Description: "Replace RegEdit"; GroupDescription: "Integration:"; Check: IsAdminInstallMode
Name: "edit_context_menu"; Description: "Add ""Edit"" Context Menu"; GroupDescription: "Integration:"; Flags: checkedonce

[Files]
Source: "{#BuildDir}\{#AppExeName}"; DestDir: "{app}"; Flags: ignoreversion; Components: main
Source: "{#BuildDir}\offreg.dll"; DestDir: "{app}"; Flags: ignoreversion; Components: main
Source: "..\redist\pcre2\LICENCE.md"; DestDir: "{app}\licences"; DestName: "PCRE2-LICENCE.md"; Flags: ignoreversion; Components: main
Source: "{#BuildDir}\assets\icons\classic\*"; DestDir: "{app}\assets\icons\classic"; Flags: ignoreversion; Components: icons
Source: "{#BuildDir}\assets\comments\*"; DestDir: "{app}\assets\comments"; Flags: ignoreversion recursesubdirs createallsubdirs; Components: comments
Source: "{#BuildDir}\assets\records\*"; DestDir: "{app}\assets\records"; Flags: ignoreversion recursesubdirs createallsubdirs; Components: traces
Source: "{#BuildDir}\assets\bitfields\*"; DestDir: "{app}\assets\bitfields"; Flags: ignoreversion recursesubdirs createallsubdirs; Components: bitfields
Source: "{#BuildDir}\assets\defaults\*"; DestDir: "{app}\assets\defaults"; Flags: ignoreversion recursesubdirs createallsubdirs; Components: defaults

[Icons]
Name: "{autoprograms}\RegKit\RegKit"; Filename: "{app}\{#AppExeName}"; WorkingDir: "{app}"; Tasks: startmenu
Name: "{autodesktop}\RegKit"; Filename: "{app}\{#AppExeName}"; WorkingDir: "{app}"; Tasks: desktopicon

[Registry]
Root: HKA; Subkey: "Software\Classes\Applications\{#AppExeName}"; ValueType: string; ValueName: "FriendlyAppName"; ValueData: "{#AppName}"; Flags: uninsdeletekey
Root: HKA; Subkey: "Software\Classes\Applications\{#AppExeName}\DefaultIcon"; ValueType: string; ValueData: """{app}\{#AppExeName}"",0"
Root: HKA; Subkey: "Software\Classes\Applications\{#AppExeName}\SupportedTypes"; ValueType: string; ValueName: ".reg"; ValueData: ""
Root: HKA; Subkey: "Software\Classes\Applications\{#AppExeName}\shell\open\command"; ValueType: string; ValueData: """{app}\{#AppExeName}"" ""%1"""
[Code]
procedure InstallRegEditReplacement;
var
  ResultCode: Integer;
begin
  if not WizardIsTaskSelected('replace_regedit') then
    exit;
  if not Exec(ExpandConstant('{app}\{#AppExeName}'), '--install-regedit-replacement', '', SW_HIDE, ewWaitUntilTerminated, ResultCode) or (ResultCode <> 0) then begin
    RaiseException('RegEdit replacement couldn''t be installed. Another program may already own its Debugger entry, or non administrators can modify the install folder.');
  end;
end;

procedure RemoveRegEditReplacement;
var
  ResultCode: Integer;
begin
  if not Exec(ExpandConstant('{app}\{#AppExeName}'), '--uninstall-regedit-replacement', '', SW_HIDE, ewWaitUntilTerminated, ResultCode) or (ResultCode <> 0) then begin
    RaiseException('RegEdit replacement couldn''t be removed. Uninstallation was stopped to avoid leaving RegEdit redirected to a deleted file.');
  end;
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssPostInstall then begin
    InstallRegEditReplacement;
  end;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep = usUninstall then begin
    RemoveRegEditReplacement;
  end;
end;

function GetDefaultDir(Param: string): string;
begin
  if IsAdminInstallMode then begin
    Result := ExpandConstant('{autopf}\Noverse\RegKit');
  end else begin
    Result := ExpandConstant('{localappdata}\Noverse\RegKit');
  end;
end;

[Run]
Filename: "{app}\{#AppExeName}"; Parameters: "--install-edit-context-menu"; Flags: runhidden runasoriginaluser; Tasks: edit_context_menu
Filename: "{app}\{#AppExeName}"; Description: "Launch RegKit"; Flags: nowait postinstall skipifsilent

[UninstallRun]
Filename: "{app}\{#AppExeName}"; Parameters: "--uninstall-edit-context-menu"; RunOnceId: "RemoveEditContextMenu"; Flags: runhidden

[UninstallDelete]
Type: filesandordirs; Name: "{localappdata}\Noverse\RegKit"
