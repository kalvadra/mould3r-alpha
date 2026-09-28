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
; LICENSES (no staging needed):
;   The installer pulls these straight from the source tree via "ProjectDir"
;   below, so they always match the build you are packaging:
;     - LICENSE                  -> {app}\LICENSE.txt   (Mould3r, GPL-3.0)
;     - THIRD_PARTY_NOTICES.md   -> {app}\THIRD_PARTY_NOTICES.md
;     - vcpkg's per-library license files
;         vcpkg_installed\x64-windows\x64-windows\share\<port>\copyright
;                                -> {app}\licenses\<port>\copyright
;   The MIT / BSD / BSL / Apache / IJG licenses of the bundled DLLs require
;   their license text to travel with the binaries; this is how it does.
;   Compiling fails with a clear message if any of these are missing.
;
; =============================================================================

; ---- Point this at your staging folder --------------------------------------
#define StagingDir "C:\dev\staging"

; ---- Point this at the Mould3r source folder (where vcpkg.json lives) --------
#define ProjectDir "C:\dev\mould3r-alpha\Mould3r\Mould3r\Mould3r"

; vcpkg manifest mode under MSBuild installs into
; vcpkg_installed\<triplet>\<triplet>\ (the triplet really is doubled).
; If your tree only has one x64-windows level, drop the second one here.
#define VcpkgShareDir ProjectDir + "\vcpkg_installed\x64-windows\x64-windows\share"

#if !FileExists(ProjectDir + "\LICENSE")
  #error "LICENSE not found in ProjectDir - point ProjectDir at the Mould3r source folder."
#endif
#if !FileExists(ProjectDir + "\THIRD_PARTY_NOTICES.md")
  #error "THIRD_PARTY_NOTICES.md not found in ProjectDir."
#endif
#if !DirExists(VcpkgShareDir)
  #error "vcpkg share folder not found - build Release x64 first, or fix VcpkgShareDir."
#endif

; ---- App metadata -----------------------------------------------------------
#define MyAppName      "Mould3r"
#define MyAppVersion   "0.7.0"
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

; License page (optional). GPL-3.0 doesn't require users to accept it to
; install or run the program, so the text is installed to {app} (see [Files])
; rather than shown as an "I accept" page. To show it read-only before
; install instead, uncomment:
; InfoBeforeFile={#ProjectDir}\LICENSE

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

; ---- Licenses ---------------------------------------------------------------
; Mould3r's own license (GPL-3.0) and the third-party notices summary.
Source: "{#ProjectDir}\LICENSE"; DestDir: "{app}"; DestName: "LICENSE.txt"; Flags: ignoreversion
Source: "{#ProjectDir}\THIRD_PARTY_NOTICES.md"; DestDir: "{app}"; Flags: ignoreversion

; Every vcpkg port's license file, keeping one folder per port. recursesubdirs
; makes Inno look for files named "copyright" in each share\<port>\ folder.
; vcpkg's own build-helper ports (vcpkg-cmake etc.) never ship, so skip them.
Source: "{#VcpkgShareDir}\copyright"; DestDir: "{app}\licenses"; \
    Excludes: "vcpkg-*"; \
    Flags: ignoreversion recursesubdirs createallsubdirs

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
Name: "{group}\Licenses";            Filename: "{app}\licenses"
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
