; ============================================================================
; Yuli Vault - Inno Setup installer
;
; Builds a Windows setup.exe from Release binaries, Qt runtime DLLs, plugins,
; and LICENSE.
;
; From the repo root:
;   cmake --build build --config Release
;   cmake --build build --target package_inno
;   iscc.exe packaging\yuli-vault.iss
;
; Output: build\package\yuli-vault-<version>-setup.exe
; ============================================================================

#define MyAppName "Yuli Vault"
#define MyAppVersion "4.0.0"
#define MyAppPublisher "Yuli Vault"
#define MyAppExeName "yuli-vault-ui.exe"
#define MyAppServiceName "yuli-vault-service.exe"

[Setup]
; New product identity (PwdVault used a different AppId).
AppId={{B3E1A9C4-7D52-4F18-9A6B-2C8E4F1D0A77}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
DefaultDirName={autopf}\YuliVault
DefaultGroupName={#MyAppName}
AllowNoIcons=yes
OutputDir=..\build\package
OutputBaseFilename=yuli-vault-{#MyAppVersion}-setup
Compression=lzma2
SolidCompression=yes
ArchitecturesAllowed=x64
ArchitecturesInstallIn64BitMode=x64
WizardStyle=modern
UninstallDisplayIcon={app}\bin\{#MyAppExeName}

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"
Name: "chinesesimp"; MessagesFile: "compiler:Languages\ChineseSimplified.isl"

[CustomMessages]
english.LaunchApp=Run %1
chinesesimp.LaunchApp=运行 %1

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
Source: "..\build\bin\Release\*.exe"; DestDir: "{app}\bin"; Flags: ignoreversion
Source: "..\build\bin\Release\*.dll"; DestDir: "{app}\bin"; Flags: ignoreversion
Source: "..\build\bin\Release\platforms\*"; DestDir: "{app}\bin\platforms"; Flags: ignoreversion recursesubdirs createallsubdirs skipifsourcedoesntexist
Source: "..\build\bin\Release\styles\*"; DestDir: "{app}\bin\styles"; Flags: ignoreversion recursesubdirs createallsubdirs skipifsourcedoesntexist
Source: "..\build\bin\Release\imageformats\*"; DestDir: "{app}\bin\imageformats"; Flags: ignoreversion recursesubdirs createallsubdirs skipifsourcedoesntexist
Source: "..\build\bin\Release\tls\*"; DestDir: "{app}\bin\tls"; Flags: ignoreversion recursesubdirs createallsubdirs skipifsourcedoesntexist
Source: "..\build\bin\Release\iconengines\*"; DestDir: "{app}\bin\iconengines"; Flags: ignoreversion recursesubdirs createallsubdirs skipifsourcedoesntexist
Source: "..\build\bin\Release\networkinformation\*"; DestDir: "{app}\bin\networkinformation"; Flags: ignoreversion recursesubdirs createallsubdirs skipifsourcedoesntexist
Source: "..\build\bin\Release\generic\*"; DestDir: "{app}\bin\generic"; Flags: ignoreversion recursesubdirs createallsubdirs skipifsourcedoesntexist
Source: "..\LICENSE"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{group}\{#MyAppName}"; Filename: "{app}\bin\{#MyAppExeName}"
Name: "{group}\{cm:UninstallProgram,{#MyAppName}}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\bin\{#MyAppExeName}"; Tasks: desktopicon

[Run]
Filename: "{app}\bin\{#MyAppExeName}"; Description: "{cm:LaunchApp,{#MyAppName}}"; Flags: nowait postinstall skipifsilent

[UninstallRun]
Filename: "{cmd}"; Parameters: "/C taskkill /IM {#MyAppServiceName} /F"; Flags: runhidden; RunOnceId: "StopService"
Filename: "{cmd}"; Parameters: "/C taskkill /IM {#MyAppExeName} /F"; Flags: runhidden; RunOnceId: "StopUI"
