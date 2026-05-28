; =============================================================================
; Mould3r — Inno Setup Installer Script
; =============================================================================
;
; HOW TO USE:
;
; 1. Install Inno Setup from https://jrsoftware.org/isinfo.php
;
; 2. Create a staging folder with your Release build output.  The expected
;    layout is described below under [Files].  Adjust "StagingDir" to point
;    at that folder.
;
; 3. Open this .iss file in Inno Setup Compiler and click Build → Compile.
;    The output installer will be written to the "Output" subfolder.
;
; STAGING FOLDER LAYOUT (adjust paths below if yours differs):
;
;   staging/
;   ├── Mould3r.exe
;   ├── *.dll                  ← all runtime DLLs (wxWidgets, OpenCascade, etc.)
;   ├── redist/
;   │   └── vc_redist.x64.exe ← Visual C++ Redistributable (see notes below)
;   ├── res/
;   │   └── icons/
;   │       ├── app-icon.svg
;   │       ├── logo.svg
;   │       └── ... other SVG icons
;   └── fixtures/
;       └── ... your fixture definition folders
;
; FINDING vc_redist.x64.exe:
;   It lives in your Visual Studio installation, typically at:
;   C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Redist\MSVC\<version>\vc_redist.x64.exe
;   Copy it into the staging/redist/ folder.
;
; FINDING YOUR DLLs:
;   After a Release build, run your .exe from a clean folder.  Windows will
;   tell you which DLLs are missing.  Alternatively, run:
;       dumpbin /dependents Mould3r.exe
;   Copy every non-system DLL into the staging folder next to your .exe.
;   Common ones from vcpkg: wxbase*.dll, wxmsw*.dll, TK*.dll (OpenCascade).
;
; =============================================================================

; ---- Point this at your staging folder --------------------------------------
#define StagingDir "C:\dev\staging"

; ---- App metadata -----------------------------------------------------------
#define MyAppName      "Mould3r"
#define MyAppVersion   "0.3.2"
#define MyAppPublisher "Clayton Stewart"
#define MyAppURL       "https://mould3r.com"
#define MyAppExeName   "Mould3r.exe"

[Setup]
AppId={{F600F327-0945-42BC-AB7B-1527E396945A}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
AppPublisherURL={#MyAppURL}
AppSupportURL={#MyAppURL}
AppUpdatesURL={#MyAppURL}

; Install location
DefaultDirName={autopf}\{#MyAppName}
DefaultGroupName={#MyAppName}

; Installer output
OutputDir=Output
OutputBaseFilename=Mould3r_Setup_{#MyAppVersion}
Compression=lzma2
SolidCompression=yes

; Require 64-bit Windows
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible

; Minimum Windows version (Windows 10)
MinVersion=10.0

; Uninstall info
UninstallDisplayName={#MyAppName}
; UninstallDisplayIcon={app}\{#MyAppExeName}   ← uncomment once you have an .ico

; Misc
AllowNoIcons=yes
; WizardStyle=modern                            ← uncomment for Inno 6+ modern look
PrivilegesRequired=lowest
SetupIconFile=logo-icon-nobackground.ico

; License file (optional — uncomment and point at your license)
; LicenseFile={#StagingDir}\LICENSE.txt

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

; =============================================================================
; Files to install
; =============================================================================
[Files]

; ---- Executable -------------------------------------------------------------
Source: "{#StagingDir}\{#MyAppExeName}"; DestDir: "{app}"; Flags: ignoreversion

; ---- Runtime DLLs (wxWidgets, OpenCascade, etc.) ----------------------------
; This copies every .dll in the staging root.  If you prefer to list them
; individually, replace this line with explicit Source entries.
Source: "{#StagingDir}\*.dll"; DestDir: "{app}"; Flags: ignoreversion

; ---- SVG icons and resources ------------------------------------------------
Source: "{#StagingDir}\res\*"; DestDir: "{app}\res"; Flags: ignoreversion recursesubdirs createallsubdirs

; ---- Fixtures ---------------------------------------------------------------
Source: "{#StagingDir}\fixtures\*"; DestDir: "{app}\fixtures"; Flags: ignoreversion recursesubdirs createallsubdirs

; ---- VC Redistributable (runs silently during install) ----------------------
Source: "{#StagingDir}\redist\vc_redist.x64.exe"; DestDir: "{tmp}"; Flags: deleteafterinstall

; =============================================================================
; Run VC Redistributable silently before the app is launched for the first time
; =============================================================================
[Run]
Filename: "{tmp}\vc_redist.x64.exe"; \
    Parameters: "/install /quiet /norestart"; \
    StatusMsg: "Installing Visual C++ Runtime..."; \
    Flags: waituntilterminated skipifsilent

; =============================================================================
; Shortcuts
; =============================================================================
[Icons]

; Start Menu shortcut
Name: "{group}\{#MyAppName}";        Filename: "{app}\{#MyAppExeName}"
Name: "{group}\Uninstall {#MyAppName}"; Filename: "{uninstallexe}"

; Desktop shortcut (user can opt out via the checkbox on the final page)
Name: "{autodesktop}\{#MyAppName}";  Filename: "{app}\{#MyAppExeName}"; Tasks: desktopicon

[Tasks]
Name: "desktopicon"; Description: "Create a &desktop shortcut"; GroupDescription: "Additional shortcuts:"

; =============================================================================
; Registry (optional — stores install path for your app to read if needed)
; =============================================================================
[Registry]
Root: HKCU; Subkey: "Software\{#MyAppName}"; ValueType: string; ValueName: "InstallPath"; ValueData: "{app}"; Flags: uninsdeletekey

; =============================================================================
; Uninstall — clean up everything
; =============================================================================
[UninstallDelete]
Type: filesandordirs; Name: "{app}"
