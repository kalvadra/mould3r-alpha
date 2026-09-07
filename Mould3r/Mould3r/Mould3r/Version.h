// Version.h
#ifndef MOULD3R_VERSION_H
#define MOULD3R_VERSION_H

// =============================================================================
// Mould3r — single source of version truth
//
// Everything that needs to name a version reads it from here: the About
// dialog, the Win32 VERSIONINFO resource block (app.rc), and — once the
// update checker lands — the comparison against the published manifest.
//
// TO SHIP A NEW VERSION: bump the three numbers below and nothing else.
// The only value that lives outside this file is Inno Setup's AppVersion,
// which has to be kept in step by hand (or driven from here by a build
// step — see the note at the bottom).
//
// Deliberately include-guarded rather than `#pragma once`: this header is
// #included by app.rc, and rc.exe's preprocessor doesn't reliably honour
// pragmas. Everything above the RC_INVOKED guard is plain preprocessor
// text so the resource compiler can consume it; the C++ constants live
// below, where rc.exe never sees them.
// =============================================================================

// ---- The numbers -----------------------------------------------------------
// Semantic versioning: MAJOR breaks compatibility, MINOR adds features,
// PATCH fixes bugs. The update checker's comparison is a straight
// major/minor/patch ordering, so these must only ever move forward.
#define MOULD3R_VERSION_MAJOR   0
#define MOULD3R_VERSION_MINOR   6
#define MOULD3R_VERSION_PATCH   0

// ---- Derived strings -------------------------------------------------------
// Two-step stringification so the macro *values* get pasted, not the macro
// names. (MOULD3R_STR(MOULD3R_VERSION_MAJOR) alone would yield the literal
// text "MOULD3R_VERSION_MAJOR".)
#define MOULD3R_STR_HELPER(x)   #x
#define MOULD3R_STR(x)          MOULD3R_STR_HELPER(x)

#define MOULD3R_VERSION_STRING                  \
    MOULD3R_STR(MOULD3R_VERSION_MAJOR) "."      \
    MOULD3R_STR(MOULD3R_VERSION_MINOR) "."      \
    MOULD3R_STR(MOULD3R_VERSION_PATCH)

// VERSIONINFO's FILEVERSION/PRODUCTVERSION fields want four comma-separated
// numbers. The fourth is the build number; left at 0 until there's a CI
// pipeline worth counting.
#define MOULD3R_VERSION_COMMAS                  \
    MOULD3R_VERSION_MAJOR, MOULD3R_VERSION_MINOR, MOULD3R_VERSION_PATCH, 0

// ---- Product identity ------------------------------------------------------
// These land in the EXE's Properties -> Details tab in Explorer, and in the
// About dialog. Windows SmartScreen and code-signing tooling both surface
// CompanyName, so it should match the subject name on the signing
// certificate once signing is in place.
#define MOULD3R_PRODUCT_NAME        "Mould3r"
#define MOULD3R_FILE_DESCRIPTION    "Mould3r - Injection Mould Design"
#define MOULD3R_COMPANY_NAME        "Clayton Stewart"
#define MOULD3R_COPYRIGHT           "Copyright (C) 2026 Clayton Stewart"
#define MOULD3R_WEBSITE             "https://mould3r.com"
#define MOULD3R_UPDATE_MANIFEST_URL "https://mould3r.com/updates/v1/stable.json"

#ifndef RC_INVOKED
// =============================================================================
// C++ view of the same values. Prefer these over the macros in application
// code — they're typed, namespaced, and don't leak into every translation
// unit that happens to include this header transitively.
// =============================================================================
namespace Mould3r
{
    namespace Version
    {
        inline constexpr int Major = MOULD3R_VERSION_MAJOR;
        inline constexpr int Minor = MOULD3R_VERSION_MINOR;
        inline constexpr int Patch = MOULD3R_VERSION_PATCH;

        // "0.1.0"
        inline constexpr const char* String = MOULD3R_VERSION_STRING;

        // Compile date/time of *this* translation unit. Only meaningful
        // because Version.h changes on every release, forcing a rebuild of
        // anything that includes it. Useful in bug reports for telling two
        // builds of the same version number apart.
        inline constexpr const char* BuildDate = __DATE__;
        inline constexpr const char* BuildTime = __TIME__;

        inline constexpr const char* ProductName = MOULD3R_PRODUCT_NAME;
        inline constexpr const char* CompanyName = MOULD3R_COMPANY_NAME;
        inline constexpr const char* Copyright = MOULD3R_COPYRIGHT;
        inline constexpr const char* Website = MOULD3R_WEBSITE;

        // The manifest URL every shipped binary polls. Compiled in, not
        // configurable: a config-file URL would let anything that can
        // write mould3r.cfg redirect update checks. Versioned path (/v1/)
        // so the schema can evolve without breaking old installs.
        inline constexpr const char* UpdateManifestUrl = MOULD3R_UPDATE_MANIFEST_URL;

        // Packed integer form: 0.1.0 -> 100, 1.2.3 -> 10203. Handy for
        // "is this at least version X" checks without string parsing.
        // Caps each component at 99, which is fine for a desktop app's
        // release cadence.
        inline constexpr int Packed = Major * 10000 + Minor * 100 + Patch;
    }
}
#endif  // RC_INVOKED

// -----------------------------------------------------------------------------
// Keeping the installer in step
//
// Inno Setup can read these values straight out of the built EXE instead of
// duplicating them, which removes the last place the version can drift:
//
//     #define AppVer GetVersionNumbersString("..\x64\Release\Mould3r.exe")
//     [Setup]
//     AppVersion={#AppVer}
//     VersionInfoVersion={#AppVer}
//
// That works because the VERSIONINFO block in app.rc is populated from this
// header — the EXE becomes the source of truth for the installer, and this
// header the source of truth for the EXE.
// -----------------------------------------------------------------------------

#endif  // MOULD3R_VERSION_H
