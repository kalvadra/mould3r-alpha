#pragma once
#include "MouldFeature.h"

// ---------------------------------------------------------------------------
// VentFeature  — a vent channel defined by a placement point + dimensions.
// The point (and its surface normal) drives the position and direction;
// length/width/depth control the cut geometry during mould generation.
// ---------------------------------------------------------------------------
struct VentFeature
{
    VentPoint point;
    float length = 5.0f;   // mm along the surface
    float width = 2.0f;   // mm perpendicular to surface
    float depth = 1.0f;   // mm into the mould
};
