#ifndef AppVersion
  #define AppVersion "0.1.0"
#endif
#ifndef BuildDir
  #define BuildDir "..\..\build"
#endif
[Setup]
AppId={{79F43E71-13B0-4C84-B83D-B9BB215E1AA0}
AppName=Aether Matrix
AppVersion={#AppVersion}
AppPublisher=AetherAudioLabs
AppPublisherURL=https://github.com/PelayoGarrido/Aether-Matrix
DefaultDirName={commoncf64}\VST3\Aether Matrix.vst3
DisableDirPage=yes
DisableProgramGroupPage=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequired=admin
OutputDir={#BuildDir}\installers
OutputBaseFilename=Aether_Matrix_Windows_Setup
Compression=lzma2
SolidCompression=yes
LicenseFile=..\..\LICENSE
WizardStyle=modern
UninstallDisplayName=Aether Matrix
[Files]
Source: "{#BuildDir}\Aether_Matrix_artefacts\Release\VST3\Aether Matrix.vst3\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "..\..\models\rave_decoder.onnx"; DestDir: "{commonappdata}\AetherAudioLabs\AetherMatrix"; Flags: ignoreversion
Source: "..\..\LICENSE"; DestDir: "{commonappdata}\AetherAudioLabs\AetherMatrix\licenses"; Flags: ignoreversion
Source: "..\..\THIRD_PARTY_NOTICES.md"; DestDir: "{commonappdata}\AetherAudioLabs\AetherMatrix\licenses"; Flags: ignoreversion
Source: "{#BuildDir}\_deps\onnxruntime_sdk-src\LICENSE"; DestDir: "{commonappdata}\AetherAudioLabs\AetherMatrix\licenses"; DestName: "ONNX-Runtime-LICENSE"; Flags: ignoreversion
Source: "{#BuildDir}\_deps\onnxruntime_sdk-src\ThirdPartyNotices.txt"; DestDir: "{commonappdata}\AetherAudioLabs\AetherMatrix\licenses"; Flags: ignoreversion
Source: "..\..\docs\licenses\*"; DestDir: "{commonappdata}\AetherAudioLabs\AetherMatrix\licenses"; Flags: ignoreversion
[Icons]
Name: "{group}\Uninstall Aether Matrix"; Filename: "{uninstallexe}"
