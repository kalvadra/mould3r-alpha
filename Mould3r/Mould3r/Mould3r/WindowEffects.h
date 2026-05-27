#pragma once

class wxWindow;

// =============================================================================
// WindowEffects — small per-platform window-decoration helpers
//
// Currently exposes one function: ApplyRoundedCorners, which asks the OS
// compositor to round the corners of a top-level window. On Windows 11
// this routes through the DWM (Desktop Window Manager) corner-preference
// attribute and produces anti-aliased corners at the compositor level.
// On Windows 10 the underlying call silently fails (the attribute didn't
// exist) and the window stays square — no fallback rendering, no error.
//
// Call from a wxDialog / wxFrame constructor, after the underlying HWND
// exists (i.e. after the wxWidgets constructor body has started). One
// line per window:
//
//     WindowEffects::ApplyRoundedCorners(this);
//
// The DWM attribute selects between several preset radii — there's no
// pixel-radius parameter. The "round" preset is roughly 8px, which
// matches what Microsoft uses for native Win11 dialogs.
// =============================================================================
namespace WindowEffects
{
    void ApplyRoundedCorners(wxWindow* window);
}
