#ifndef MyAppVersion
#define MyAppVersion "0.1.0"
#endif
#ifndef SourceRoot
#define SourceRoot "..\obs-install"
#endif
#ifndef OutputRoot
#define OutputRoot "..\dist"
#endif

[Setup]
AppId={{5D03B79C-25CC-4AB6-86B7-1917D95E2312}
AppName=NDI Multichannel Bridge
AppVersion={#MyAppVersion}
DefaultDirName={autopf}\obs-studio
DisableProgramGroupPage=yes
OutputDir={#OutputRoot}
OutputBaseFilename=NDI-Multichannel-Bridge-Setup
Compression=lzma2
SolidCompression=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
Uninstallable=yes

[Files]
Source: "{#SourceRoot}\obs-plugins\64bit\ndi-multichannel-bridge.dll"; DestDir: "{app}\obs-plugins\64bit"; Flags: ignoreversion
Source: "{#SourceRoot}\data\obs-plugins\ndi-multichannel-bridge\locale\en-US.ini"; DestDir: "{app}\data\obs-plugins\ndi-multichannel-bridge\locale"; Flags: ignoreversion
Source: "{#SourceRoot}\README.md"; DestDir: "{app}\data\obs-plugins\ndi-multichannel-bridge"; Flags: ignoreversion
Source: "{#SourceRoot}\LICENSE"; DestDir: "{app}\data\obs-plugins\ndi-multichannel-bridge"; Flags: ignoreversion

[Code]
function InitializeSetup(): Boolean;
var
  ResultCode: Integer;
begin
  if CheckForMutexes('OBSStudio') then begin
    MsgBox('Close OBS Studio before installing NDI Multichannel Bridge.', mbError, MB_OK);
    Result := False;
  end else
    Result := True;
end;
