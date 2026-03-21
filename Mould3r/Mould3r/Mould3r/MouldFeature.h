#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

// ---------------------------------------------------------------------------
// VentPoint  — a user-placed point on the surface of an imported object.
//
// worldPos    : position in world space, set when the user clicks a surface
// worldNormal : outward surface normal at that point (used for vent direction)
//
// VentPoint is the raw placement anchor.  VentFeature (see VentFeature.h)
// builds on top of it by adding channel dimensions, depth, etc.
// ---------------------------------------------------------------------------
struct VentPoint
{
    glm::vec3 worldPos{ 0.0f, 0.0f, 0.0f };
    glm::vec3 worldNormal{ 0.0f, 1.0f, 0.0f };
};
