; Instalador do Gestão de Equipamentos (Inno Setup 6.3 ou mais recente).
;
; Gera um único Setup.exe a partir da pasta preparada com windeployqt. Uso:
;   iscc /DAppVersion=0.1.0 /DSourceDir=..\dist\GestaoEquipamentos installer\GestaoEquipamentos.iss
; O workflow .github/workflows/windows-release.yml lê a versão de project() no CMakeLists.txt.
;
; O instalador não cria contas: a conta principal é criada pelo assistente de primeira configuração do
; próprio aplicativo. Os dados ficam em %LOCALAPPDATA% e não são apagados na desinstalação.

#ifndef AppVersion
  #define AppVersion "0.1.0"
#endif
#ifndef SourceDir
  #define SourceDir "..\dist\GestaoEquipamentos"
#endif
#ifndef OutputDir
  #define OutputDir "..\dist"
#endif

#define AppName "Gestão de Equipamentos"
#define AppExe "GestaoEquipamentos.exe"

[Setup]
; AppId fixo: novas versões atualizam a instalação existente no mesmo lugar. Não altere.
AppId={{E387CA1E-1E59-43FD-BBEB-8C58267B1AD9}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher=Virgilio Fuchs
AppPublisherURL=https://github.com/VirgilioFuchs/GestaoEquipamentos
AppSupportURL=https://github.com/VirgilioFuchs/GestaoEquipamentos/issues
AppUpdatesURL=https://github.com/VirgilioFuchs/GestaoEquipamentos/releases
VersionInfoVersion={#AppVersion}
DefaultDirName={autopf}\GestaoEquipamentos
DefaultGroupName={#AppName}
DisableProgramGroupPage=yes
; Por padrão instala só para o usuário atual, sem pedir administrador; o usuário pode escolher
; instalar para todos (com elevação) na primeira tela.
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
LicenseFile=..\LICENSE
OutputDir={#OutputDir}
OutputBaseFilename=GestaoEquipamentos-{#AppVersion}-Setup
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
UninstallDisplayIcon={app}\{#AppExe}
UninstallDisplayName={#AppName}
CloseApplications=yes
RestartApplications=no

[Languages]
Name: "brazilianportuguese"; MessagesFile: "compiler:Languages\BrazilianPortuguese.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
; Pasta gerada pelo windeployqt (Qt, plugins, SQLite, runtime MinGW, libsodium). O utilitário técnico em
; Python e o executável administrativo de linha de comando ficam fora do instalador.
Source: "{#SourceDir}\*"; DestDir: "{app}"; Excludes: "*.py,GestaoEquipamentosAdmin.exe"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "..\LICENSE"; DestDir: "{app}"; DestName: "LICENSE.txt"; Flags: ignoreversion

[Icons]
Name: "{autoprograms}\{#AppName}"; Filename: "{app}\{#AppExe}"
Name: "{autodesktop}\{#AppName}"; Filename: "{app}\{#AppExe}"; Tasks: desktopicon

[Run]
Filename: "{app}\{#AppExe}"; Description: "{cm:LaunchProgram,{#AppName}}"; Flags: nowait postinstall skipifsilent
