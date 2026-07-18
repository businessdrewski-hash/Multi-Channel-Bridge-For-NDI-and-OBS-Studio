#ifndef AppVersion
  #define AppVersion "0.5.0-alpha1-buildfix1"
#endif
#ifndef AppNumericVersion
  #define AppNumericVersion "0.5.0.0"
#endif
#ifndef SourceRoot
  #define SourceRoot "..\obs-install"
#endif
#ifndef OutputDir
  #define OutputDir "..\dist"
#endif

#define AppName "Multichannel Bridge for DistroAV"
#define RepoURL "https://github.com/businessdrewski-hash/Drewski-Multichannel-Bridge"

[Setup]
AppId={{E6286704-A58A-4D4B-86F7-CF31DA7BB6B9}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher=Andrew Carriker and contributors
AppPublisherURL={#RepoURL}
AppSupportURL={#RepoURL}/issues
AppUpdatesURL={#RepoURL}/releases
DefaultDirName={commonappdata}\Multichannel Bridge for DistroAV\Installer
DisableDirPage=yes
DisableProgramGroupPage=yes
PrivilegesRequired=admin
PrivilegesRequiredOverridesAllowed=commandline
ArchitecturesAllowed=x64
ArchitecturesInstallIn64BitMode=x64
MinVersion=10.0.17763
WizardStyle=modern
OutputDir={#OutputDir}
OutputBaseFilename=Multichannel-Bridge-for-DistroAV-Setup-v{#AppVersion}
Compression=lzma2/max
SolidCompression=yes
SetupLogging=yes
CloseApplications=no
RestartApplications=no
Uninstallable=yes
UninstallDisplayName={#AppName} {#AppVersion}
LicenseFile={#SourceRoot}\DISTROAV-GPL-LICENSE.txt
InfoAfterFile={#SourceRoot}\README.md
VersionInfoVersion={#AppNumericVersion}
VersionInfoCompany=Andrew Carriker and contributors
VersionInfoDescription=Multichannel audio bridge and upstream A/V timing guard for DistroAV
VersionInfoProductName={#AppName}
VersionInfoProductVersion={#AppNumericVersion}

[Files]
Source: "{#SourceRoot}\obs-plugins\*"; DestDir: "{app}\obs-plugins"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#SourceRoot}\data\*"; DestDir: "{app}\data"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#SourceRoot}\Install-MultichannelBridge.ps1"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourceRoot}\Uninstall-MultichannelBridge.ps1"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourceRoot}\README.md"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourceRoot}\INSTALL-BOTH-PCS.md"; DestDir: "{app}"; Flags: ignoreversion skipifsourcedoesntexist
Source: "{#SourceRoot}\RELEASE-NOTES.md"; DestDir: "{app}"; Flags: ignoreversion skipifsourcedoesntexist
Source: "{#SourceRoot}\AV-GOVERNOR.md"; DestDir: "{app}"; Flags: ignoreversion skipifsourcedoesntexist
Source: "{#SourceRoot}\UPSTREAM-NOTES.md"; DestDir: "{app}"; Flags: ignoreversion skipifsourcedoesntexist
Source: "{#SourceRoot}\Multichannel-Bridge-DistroAV-6.2.1.patch"; DestDir: "{app}"; Flags: ignoreversion skipifsourcedoesntexist
Source: "{#SourceRoot}\ROADMAP.md"; DestDir: "{app}"; Flags: ignoreversion skipifsourcedoesntexist
Source: "{#SourceRoot}\LICENSE-NOTICE.txt"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourceRoot}\DISTROAV-GPL-LICENSE.txt"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{autoprograms}\Multichannel Bridge for DistroAV\Readme"; Filename: "{app}\README.md"
Name: "{autoprograms}\Multichannel Bridge for DistroAV\Uninstall"; Filename: "{uninstallexe}"

[Code]
var
  ObsDirPage: TInputDirWizardPage;
  CleanLegacyCheck: TNewCheckBox;
  InstallScriptRan: Boolean;
  UninstallScriptRan: Boolean;

function PowerShellExe: String;
begin
  Result := ExpandConstant('{sys}\WindowsPowerShell\v1.0\powershell.exe');
end;

function DefaultObsDir: String;
var
  Candidate: String;
begin
  Candidate := ExpandConstant('{autopf}\obs-studio');
  if FileExists(AddBackslash(Candidate) + 'bin\64bit\obs64.exe') then
    Result := Candidate
  else
    Result := Candidate;
end;

function ObsPathLooksValid(const Path: String): Boolean;
begin
  Result := FileExists(AddBackslash(Path) + 'bin\64bit\obs64.exe') and
            DirExists(AddBackslash(Path) + 'obs-plugins');
end;

function IsObsRunning: Boolean;
var
  ResultCode: Integer;
  Parameters: String;
begin
  Parameters := '-NoProfile -NonInteractive -Command "if (Get-Process obs64 -ErrorAction SilentlyContinue) { exit 5 } else { exit 0 }"';
  if not Exec(PowerShellExe, Parameters, '', SW_HIDE, ewWaitUntilTerminated, ResultCode) then
    Result := False
  else
    Result := ResultCode = 5;
end;

procedure InitializeWizard;
begin
  ObsDirPage := CreateInputDirPage(
    wpSelectDir,
    'Select the OBS Studio installation',
    'Choose the OBS folder that should receive the modified DistroAV build.',
    'The same setup executable should be installed on both the gaming PC and stream PC.',
    False,
    '');
  ObsDirPage.Add('OBS Studio folder:');
  ObsDirPage.Values[0] := DefaultObsDir;

  CleanLegacyCheck := TNewCheckBox.Create(ObsDirPage.Surface);
  CleanLegacyCheck.Parent := ObsDirPage.Surface;
  CleanLegacyCheck.Left := ScaleX(0);
  CleanLegacyCheck.Top := ObsDirPage.Edits[0].Top + ObsDirPage.Edits[0].Height + ScaleY(16);
  CleanLegacyCheck.Width := ObsDirPage.Surface.Width;
  CleanLegacyCheck.Height := ScaleY(24);
  CleanLegacyCheck.Caption := 'Clean stale OBS Plugin Manager record for obsolete v0.2 bridge (recommended).';
  CleanLegacyCheck.Checked := True;
end;

function NextButtonClick(CurPageID: Integer): Boolean;
begin
  Result := True;
  if CurPageID = ObsDirPage.ID then begin
    if not ObsPathLooksValid(ObsDirPage.Values[0]) then begin
      MsgBox('That folder does not contain bin\64bit\obs64.exe. Select the root OBS Studio installation folder.', mbError, MB_OK);
      Result := False;
    end;
  end;
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  Result := '';
  if IsObsRunning then
    Result := 'OBS Studio is still running. Close OBS completely before continuing.';
end;

procedure RunInstallScript;
var
  Parameters: String;
  ResultCode: Integer;
begin
  if InstallScriptRan then
    Exit;
  InstallScriptRan := True;

  SaveStringToFile(ExpandConstant('{app}\selected-obs-path.txt'), ObsDirPage.Values[0], False);
  Parameters := '-NoProfile -NonInteractive -ExecutionPolicy Bypass -File ' +
    AddQuotes(ExpandConstant('{app}\Install-MultichannelBridge.ps1')) +
    ' -ObsPath ' + AddQuotes(ObsDirPage.Values[0]) +
    ' -LogPath ' + AddQuotes(ExpandConstant('{app}\install-script.log'));
  if CleanLegacyCheck.Checked then
    Parameters := Parameters + ' -CleanLegacyPluginManager';

  if not Exec(PowerShellExe, Parameters, ExpandConstant('{app}'), SW_SHOW, ewWaitUntilTerminated, ResultCode) then
    RaiseException('Windows could not start the installation helper.');
  if ResultCode <> 0 then
    RaiseException('The installation helper failed. See install-script.log in ' + ExpandConstant('{app}') + '.');
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssPostInstall then
    RunInstallScript;
end;

function InitializeUninstall: Boolean;
begin
  Result := True;
  if IsObsRunning then begin
    MsgBox('OBS Studio is still running. Close OBS completely, then start the uninstaller again.', mbError, MB_OK);
    Result := False;
  end;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  Parameters: String;
  ResultCode: Integer;
begin
  if (CurUninstallStep = usUninstall) and (not UninstallScriptRan) then begin
    UninstallScriptRan := True;
    Parameters := '-NoProfile -NonInteractive -ExecutionPolicy Bypass -File ' +
      AddQuotes(ExpandConstant('{app}\Uninstall-MultichannelBridge.ps1')) +
      ' -LogPath ' + AddQuotes(ExpandConstant('{app}\uninstall-script.log'));
    if not Exec(PowerShellExe, Parameters, ExpandConstant('{app}'), SW_SHOW, ewWaitUntilTerminated, ResultCode) then begin
      MsgBox('Windows could not start the uninstallation helper.', mbError, MB_OK);
      Abort;
    end;
    if ResultCode <> 0 then begin
      MsgBox('The uninstallation helper failed. The bridge files were not intentionally removed. Review uninstall-script.log in ' + ExpandConstant('{app}') + '.', mbError, MB_OK);
      Abort;
    end;
  end;
end;
