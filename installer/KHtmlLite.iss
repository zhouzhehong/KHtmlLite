; KHtmlLite per-user installer.
; Build with: ISCC.exe installer\KHtmlLite.iss
; The desktop shortcut is created unconditionally after installation.

#define MyAppName "KHtmlLite"
#define MyAppVersion "1.0.0"
#define MyAppPublisher "KHtmlLite"
#define MyAppExeName "khtml_browser.exe"

[Setup]
AppId={{401BA94C-D5A0-48A7-BBBB-1555F2521AE3}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
DefaultDirName={autopf}\{#MyAppName}
DefaultGroupName={#MyAppName}
DisableProgramGroupPage=yes
OutputDir=..\release
OutputBaseFilename=KHtmlLite-Setup-{#MyAppVersion}
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
UninstallDisplayIcon={app}\{#MyAppExeName}
CloseApplications=yes
RestartApplications=no
LicenseFile=..\LICENSE
SetupLogging=yes

[Languages]
Name: "chinesesimplified"; MessagesFile: "compiler:Default.isl"

[Tasks]
; No optional shortcut task: the desktop shortcut is mandatory.
Name: "startmenu"; Description: "创建开始菜单快捷方式"; GroupDescription: "快捷方式:"; Flags: checkedonce

[Files]
; Ship the tested portable runtime.  KHTML's per-user cache and crash logs
; are runtime artifacts and must not be installed.
Source: "..\release\KHtmlLite-portable\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs; Excludes: "khtml_cache\*,khtml_renderer_crash.log"
Source: "..\LICENSE"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
; Requirement: the desktop shortcut is created automatically and points at
; the installed browser EXE, not at the installer or a launcher script.
Name: "{autodesktop}\KHtmlLite"; Filename: "{app}\{#MyAppExeName}"; WorkingDir: "{app}"; IconFilename: "{app}\{#MyAppExeName}"; Comment: "KHtmlLite 浏览器"
Name: "{autoprograms}\KHtmlLite"; Filename: "{app}\{#MyAppExeName}"; WorkingDir: "{app}"; IconFilename: "{app}\{#MyAppExeName}"; Tasks: startmenu

[Run]
; Keep installation non-intrusive: do not auto-start the browser.
Filename: "{app}\{#MyAppExeName}"; Description: "启动 KHtmlLite"; Flags: nowait postinstall skipifsilent
