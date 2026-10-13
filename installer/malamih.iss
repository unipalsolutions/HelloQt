; مثبّت ملامح: ملف exe واحد يثبّت البرنامج واختصاراته ويضيف إلغاء التثبيت
; يُبنى في CI بـ: iscc /DAppVersion=x.y.z /DSourceDir=dist\Malamih installer\malamih.iss

#ifndef AppVersion
  #define AppVersion "3.0.0"
#endif
#ifndef SourceDir
  #define SourceDir "..\dist\Malamih"
#endif

[Setup]
AppId={{6F1C2B8E-4D7A-4E59-9C3B-5A1D2E7F8B40}
AppName=ملامح
AppVersion={#AppVersion}
AppVerName=ملامح {#AppVersion}
AppPublisher=Unipal Solutions
DefaultDirName={autopf}\Malamih
DefaultGroupName=ملامح
UninstallDisplayName=ملامح
UninstallDisplayIcon={app}\Malamih.exe
SetupIconFile=..\resources\app.ico
OutputDir=..\dist
OutputBaseFilename=Malamih-Setup
Compression=lzma2/ultra64
SolidCompression=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
; يكفي حساب المستخدم، ويمكن التثبيت لكل المستخدمين عند الطلب
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
WizardStyle=modern
DisableProgramGroupPage=yes
CloseApplications=yes

[Languages]
#if FileExists(AddBackslash(CompilerPath) + "Languages\Arabic.isl")
Name: "ar"; MessagesFile: "compiler:Languages\Arabic.isl"
#endif
Name: "en"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"

[Files]
Source: "{#SourceDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\ملامح"; Filename: "{app}\Malamih.exe"
Name: "{group}\{cm:UninstallProgram,ملامح}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\ملامح"; Filename: "{app}\Malamih.exe"; Tasks: desktopicon

[Run]
Filename: "{app}\Malamih.exe"; Description: "{cm:LaunchProgram,ملامح}"; Flags: nowait postinstall skipifsilent
