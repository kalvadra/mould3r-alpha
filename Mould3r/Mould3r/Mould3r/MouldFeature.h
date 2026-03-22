#pragma once

#include <glad/glad.h>
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

// ---------------------------------------------------------------------------
// VentCrossSection — the rectangular profile of a vent channel at its origin,
//                    in the plane perpendicular to the vent path direction.
//                    corners[0..3] are BL, BR, TR, TL in world space.
// ---------------------------------------------------------------------------
struct VentCrossSection
{
    glm::vec3 corners[4] = {};
    bool      valid = false;
};

// ---------------------------------------------------------------------------
// VentSolid — the swept 3D mesh of a vent channel, ready for GPU rendering
//             and eventually for boolean subtraction into the mould.
// ---------------------------------------------------------------------------
struct VentSolid
{
    // Owned GPU resources — call Destroy() before discarding
    GLuint  vao = 0;
    GLuint  vbo = 0;
    GLuint  ebo = 0;
    GLsizei indexCount = 0;
    bool    valid = false;

    void Destroy()
    {
        if (ebo) { glDeleteBuffers(1, &ebo);       ebo = 0; }
        if (vbo) { glDeleteBuffers(1, &vbo);       vbo = 0; }
        if (vao) { glDeleteVertexArrays(1, &vao);  vao = 0; }
        indexCount = 0;
        valid = false;
    }
};
