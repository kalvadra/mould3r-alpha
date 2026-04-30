#pragma once

#include <glad/glad.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <vector>

// ---------------------------------------------------------------------------
// SolidMesh — GPU mesh for any solid preview (cylinders, boxes, frustums).
//             Vertex layout: [pos(3), normal(3)], compatible with vsLit.
// ---------------------------------------------------------------------------
struct SolidMesh
{
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

// Keep the old name as an alias so any straggling references still compile.
using VentSolid = SolidMesh;

// ---------------------------------------------------------------------------
// Geometry builders — pure functions, no scene dependency.
// Both produce a SolidMesh with GPU resources uploaded immediately.
// ---------------------------------------------------------------------------

// Swept circular cross-section (cylinder / frustum).
// draftAngleDeg controls taper: radius at 'start', grows toward 'end'.
SolidMesh BuildCylinderMesh(const glm::vec3& start, const glm::vec3& end,
    float radius, float draftAngleDeg = 0.0f, int segments = 32);

// Swept rectangular cross-section along a path on the parting plane.
struct VentPath;   // forward-declared, defined below
SolidMesh BuildBoxSweepMesh(const VentPath& path, float width, float depth,
    float overrunStart = 0.0f, float overrunEnd = 0.0f);

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
// ---------------------------------------------------------------------------
struct VentPath
{
    glm::vec3 start{ 0.0f };
    glm::vec3 end{ 0.0f };
    bool      valid = false;

    float overrunStart = 0.0f;
    float overrunEnd = 0.0f;
};

// ---------------------------------------------------------------------------
// VentCrossSection — the rectangular profile of a vent channel at its origin.
//                    corners[0..3] are BL, BR, TR, TL in world space.
// ---------------------------------------------------------------------------
struct VentCrossSection
{
    glm::vec3 corners[4] = {};
    bool      valid = false;
};

// ---------------------------------------------------------------------------
// VentInstance — one vent: placement point + derived geometry, all in one
//               struct.  Replaces the four parallel vectors that were in
//               GLCanvas (m_ventPoints / m_ventPaths / m_ventCrossSections /
//               m_ventSolids).
// ---------------------------------------------------------------------------
struct VentInstance
{
    VentPoint        point;
    VentPath         path;
    VentCrossSection crossSection;
    SolidMesh        solid;

    // Parent-object association for sticky placement.
    //   -1                   — unparented (placed without an object snap, or
    //                          loaded from a pre-v2 project file).  World
    //                          position is the source of truth, transforms
    //                          on objects do not affect the vent.
    //   >= 0                 — index into GLCanvas::m_objects.  localPos
    //                          and localNormal are valid and ARE the source
    //                          of truth: world position is re-derived from
    //                          parent.BuildModelMatrix() * localPos whenever
    //                          the parent is transformed (move / rotate /
    //                          scale, mirroring during grid pattern, etc.).
    //                          Patterning clones the parented vent onto each
    //                          new clone object with the same local data.
    int       parentIndex = -1;
    glm::vec3 localPos{ 0.0f, 0.0f, 0.0f };
    glm::vec3 localNormal{ 0.0f, 0.0f, 1.0f };

    void Destroy() { solid.Destroy(); }
};

// ---------------------------------------------------------------------------
// RunnerFeature — one runner: a point on the parting plane plus the preview
//                 cylinder and cold-plug extension.  Replaces the three
//                 parallel vectors (m_runnerPoints / m_runnerSolids /
//                 m_runnerColdPlugSolids).
// ---------------------------------------------------------------------------
struct RunnerFeature
{
    glm::vec3 point{ 0.0f };
    SolidMesh solid;
    SolidMesh coldPlugSolid;

    void Destroy() { solid.Destroy(); coldPlugSolid.Destroy(); }
};

// ---------------------------------------------------------------------------
// GateFeature — one gate: a placement point on the parting surface, a tapered
//               frustum (the gate itself), and a straight cylinder (the
//               sub-runner) connecting it to the feed network.
// ---------------------------------------------------------------------------
struct GateFeature
{
    VentPoint point;          // worldPos + worldNormal on the parting surface
    SolidMesh solid;          // tapered gate frustum
    SolidMesh subRunnerSolid; // straight sub-runner cylinder

    // Path from the gate to the nearest feed point (sprue parting pt or runner pt)
    glm::vec3 pathEnd{ 0.0f };
    bool      hasPath = false;

    // Parent-object association — same semantics as VentInstance.
    int       parentIndex = -1;
    glm::vec3 localPos{ 0.0f, 0.0f, 0.0f };
    glm::vec3 localNormal{ 0.0f, 0.0f, 1.0f };

    void Destroy() { solid.Destroy(); subRunnerSolid.Destroy(); }
};

// ---------------------------------------------------------------------------
// SprueFeature — all sprue state consolidated into one struct.
//                Replaces ~15 scattered m_sprue* members in GLCanvas.
// ---------------------------------------------------------------------------
struct SprueFeature
{
    // Placement state
    glm::vec3 worldPos{ 0.0f };
    glm::vec3 pathStart{ 0.0f };
    glm::vec3 pathEnd{ 0.0f };
    glm::vec3 partingPos{ 0.0f };
    bool      hasPoint = false;
    bool      hasPartingPoint = false;
    bool      isDirectInjection = false;

    // Dimensions (read from UI at placement time)
    float     radius = 2.5f;
    float     draftAngleDeg = 1.0f;
    float     coldSlugDepth = 5.0f;

    // Preview solids
    SolidMesh solid;
    SolidMesh coldSlugSolid;

    // Path line GPU resources
    GLuint  pathVAO = 0;
    GLuint  pathVBO = 0;
    GLsizei pathVertexCount = 0;

    // Cross-section circle GPU resources
    GLuint  xsecVAO = 0;
    GLuint  xsecVBO = 0;
    GLsizei xsecVertexCount = 0;

    void Clear()
    {
        hasPoint = false;
        hasPartingPoint = false;
        isDirectInjection = false;
        solid.Destroy();
        coldSlugSolid.Destroy();
    }

    void DestroyGL()
    {
        solid.Destroy();
        coldSlugSolid.Destroy();
        if (pathVBO) { glDeleteBuffers(1, &pathVBO);         pathVBO = 0; }
        if (pathVAO) { glDeleteVertexArrays(1, &pathVAO);    pathVAO = 0; }
        if (xsecVBO) { glDeleteBuffers(1, &xsecVBO);         xsecVBO = 0; }
        if (xsecVAO) { glDeleteVertexArrays(1, &xsecVAO);    xsecVAO = 0; }
        pathVertexCount = 0;
        xsecVertexCount = 0;
    }
};
