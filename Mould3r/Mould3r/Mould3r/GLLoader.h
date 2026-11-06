#pragma once

// =============================================================================
// GLLoader
//
// Shared GL setup helpers used by every viewport canvas in the app
// (currently MainFrame's GLCanvas and the FixtureEditor's FixtureCanvas).
// Centralised here so the wxGLCanvas pixel-format request and the platform-specific
// glad function-pointer loader stay in one place — adding a third canvas
// no longer means a third copy of either.
//
// Platform GL headers and dynamic-library handles stay in the implementation.
// =============================================================================
namespace GLLoader
{
    // GL pixel-format / context attributes for wxGLCanvas. 3.3-core,
    // double-buffered, 24-bit depth, 8-bit stencil, 4x MSAA. Two canvases
    // running these same attributes share function-pointer compatibility
    // on Windows, which is what lets glad's globals stay valid when both
    // canvases are alive.
    extern int glArgs[];

    // Resolve GL symbols from the platform library. Requires a current context.
    void* GetAnyGLFuncAddress(const char* name);
}
