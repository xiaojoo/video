; Player 安装包：ISCC.exe playerui.iss
; 输入 = dist\Player-0.1.0-win64（windeployqt 补齐之后的自包含目录）
; 产物 = dist\Player-Setup-0.1.0.exe

#define MyAppName "Player"
#define MyAppVersion "0.1.0"
#define MyAppPublisher "hxttxiao"
#define MyAppExeName "appPlayerui.exe"
#define RepoRoot "..\..\"
#define StageDir "..\..\dist\Player-0.1.0-win64"

[Setup]
AppId={{5087C9E7-2DDD-407C-8347-9561B266C977}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppVerName={#MyAppName} {#MyAppVersion}
AppPublisher={#MyAppPublisher}
AppPublisherURL=https://github.com/xiaojoo/video
AppSupportURL=https://github.com/xiaojoo/video/issues
AppUpdatesURL=https://github.com/xiaojoo/video/releases/latest
DefaultDirName={autopf}\{#MyAppName}
DefaultGroupName={#MyAppName}
DisableProgramGroupPage=yes
DisableWelcomePage=no
LicenseFile={#RepoRoot}LICENSE
SetupIconFile={#RepoRoot}player\icons\app.ico
UninstallDisplayIcon={app}\{#MyAppExeName}
UninstallDisplayName={#MyAppName}
OutputDir={#RepoRoot}dist
OutputBaseFilename={#MyAppName}-Setup-{#MyAppVersion}
Compression=lzma2/ultra64
LZMAUseSeparateProcess=yes
SolidCompression=yes
WizardStyle=modern
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequired=lowest
MinVersion=10.0
VersionInfoVersion=0.1.0.0
VersionInfoCompany={#MyAppPublisher}
VersionInfoProductName={#MyAppName}
VersionInfoProductVersion={#MyAppVersion}
CloseApplications=yes
RestartApplications=no

[Languages]
Name: "zh"; MessagesFile: "lang\ChineseSimplified.isl"

[Tasks]
Name: "desktopicon"; Description: "创建桌面快捷方式"; GroupDescription: "附组件:"

[InstallDelete]
; Up to the first release the product was called Playerui. Renaming the Start Menu
; group and the default folder leaves the old pair owned by nobody - the new
; uninstaller does not know their names - so this run clears them.
Type: filesandordirs; Name: "{autoprograms}\Playerui"
Type: filesandordirs; Name: "{autopf}\Playerui"

[Files]
Source: "{#StageDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"
Name: "{group}\卸载 {#MyAppName}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Tasks: desktopicon

[Run]
Filename: "{app}\{#MyAppExeName}"; Description: "立即运行 {#MyAppName}"; Flags: nowait postinstall skipifsilent
