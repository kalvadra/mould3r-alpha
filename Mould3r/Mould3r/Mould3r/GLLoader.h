#pragma once

// =============================================================================
// GLLoader
//
// Shared GL setup helpers used by every viewport canvas in the app
// (currently MainFrame's GLCanvas and the FixtureEditor's FixtureCanvas).
// Centralised here so the wxGLCanvas pixel-format request and the Win32
// glad function-pointer loader stay in one place — adding a third canvas
// no longer means a third copy of either.
//
// Implementation pulls in <windows.h> for wglGetProcAddress and friends;
// keep that out of the header so this include doesn't macro-pollute every
// caller.
// =============================================================================
namespace GLLoader
{
    // GL pixel-format / context attributes for wxGLCanvas. 3.3-core,
    // double-buffered, 24-bit depth, 8-bit stencil, 4x MSAA. Two canvases
    // running these same attributes share function-pointer compatibility
    // on Windows, which is what lets glad's globals stay valid when both
    // canvases are alive.
    extern int glArgs[];

    // Win32 glad loader. Calls wglGetProcAddress first; falls back to
    // GetProcAddress on opengl32.dll for the legacy 1.1 entry points
    // wglGetProcAddress refuses to return.
    //
    // Caller must have a current GL context when this is called — otherwise
    // wglGetProcAddress returns nothing useful.
    void* GetAnyGLFuncAddress(const char* name);
}
