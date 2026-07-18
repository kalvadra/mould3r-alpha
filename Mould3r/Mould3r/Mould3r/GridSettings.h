// GridSettings.h
#pragma once

// ---------------------------------------------------------------------------
// Grid configuration model.
//
// Shared between the MainFrame (which owns the authoritative settings and the
// Grid menu that edits them) and the small Grid dialogs. Kept in its own tiny
// header so neither the dialogs nor the renderer need to pull in MainFrame.h.
//
// All lengths are stored in MILLIMETERS internally, matching the rest of the
// app (the UI converts to/from inches for display only). Authored via the
// consolidated Grid Settings dialog and pushed to the live GridRenderer.
// ---------------------------------------------------------------------------

enum class GridShape
{
    Rectangular,   // bounded X x Y grid (maps to world X / world Z extents)
    Circular       // bounded disc of the given radius
};

struct GridSettings
{
    GridShape shape = GridShape::Rectangular;

    // Rectangular full extents (mm). sizeY is the world-Z extent; the grid
    // lives in the y=0 (XZ) ground plane, so the authored "Y" is world Z.
    float sizeX = 200.0f;
    float sizeY = 200.0f;

    // Circular radius (mm).
    float radius = 100.0f;

    // Circular radial spokes: number of minor radial divisions (e.g. 12 =>
    // a spoke every 30 deg). Unitless; circular grids only. The 4 cardinal
    // axes are always drawn as major spokes regardless of this count.
    int spokes = 12;

    // Minor line spacing (mm).
    float spacing = 10.0f;

    // Major line (bold) every N minor divisions. Unitless. The major step is
    // derived as spacing * majorEvery (so the default 10mm * 5 = 50mm matches
    // the grid's original look).
    int majorEvery = 5;
};
