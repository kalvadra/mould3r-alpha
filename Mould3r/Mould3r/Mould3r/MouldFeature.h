#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

// ---------------------------------------------------------------------------
// VentPoint  — a user-placed point on the parting surface of an object.
// ---------------------------------------------------------------------------
struct VentPoint
{
    glm::vec3 worldPos{ 0.0f, 0.0f, 0.0f };
    glm::vec3 worldNormal{ 0.0f, 1.0f, 0.0f };
};

// ---------------------------------------------------------------------------
// VentPath  — a straight channel on the parting plane from a VentPoint to
//             the nearest point on the fixture's outer parting boundary.
//             Rendered as a line in the viewport.
// ---------------------------------------------------------------------------
struct VentPath
{
    glm::vec3 start{ 0.0f };
    glm::vec3 end{ 0.0f };
    bool      valid = false;   // false if no fixture boundary was found
};
