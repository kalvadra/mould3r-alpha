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

// ---------------------------------------------------------------------------
// Path model — shared by features that extrude a cross-section along a route.
//
// PathKind::Simple  — the historical behavior: a single straight channel from
//                     `start` to `end`. Features that never need more (e.g.
//                     ejectors) stay on this and ignore the node list.
// PathKind::Complex — a user-authored multi-node route on the parting plane.
//                     Nodes act like spline control points. When `smooth` is
//                     set they define a Bezier-style curve (using each node's
//                     symmetric tangent handle: `dir` + `handleLen`); when it
//                     is clear they connect as straight A->B->C segments and
//                     the handle fields are ignored.
//
// All path geometry is locked to the parting plane (constant Y) — so `pos` and
// `dir` are authored in the XZ plane and the swept frame needs no twist/Frenet
// handling.
//
// NOTE (Part 1 of the complex-path work): these types are introduced here but
// nothing populates the Complex branch yet. Every path is still Simple, so
// behavior is unchanged. `using VentPath = FeaturePath;` keeps all existing
// VentPath references compiling untouched.
// ---------------------------------------------------------------------------
enum class PathKind { Simple, Complex };

// One control point of a Complex path. For symmetric handles a single tangent
// direction + arm length describes both the incoming and outgoing handle.
struct PathNode
{
    glm::vec3 pos{ 0.0f, 0.0f, 0.0f };   // on the parting plane (Y locked)
    glm::vec3 dir{ 0.0f, 0.0f, 1.0f };   // unit tangent (auto basis for handles)
    float     handleLen = 0.0f;          // symmetric control-arm length (auto basis)

    // Part 6: explicit tangent handles as offsets from pos to the cubic-Bezier
    // control points (handleOut drives the outgoing CP = pos + handleOut;
    // handleIn the incoming CP = pos + handleIn). When linked (default) they
    // mirror: handleIn == -handleOut, giving a smooth (G1/C1) node; breaking the
    // link lets the in/out arms point and scale independently for a corner.
    // AutoComputeComplexHandles fills these from dir/handleLen for any node that
    // hasn't been hand-edited (handlesManual); manual nodes keep their offsets.
    glm::vec3 handleIn{ 0.0f };
    glm::vec3 handleOut{ 0.0f };
    bool      handlesLinked = true;
    bool      handlesManual = false;
};

struct FeaturePath
{
    PathKind  kind = PathKind::Simple;

    // Simple branch — straight channel (the current, derived vent path).
    glm::vec3 start{ 0.0f };
    glm::vec3 end{ 0.0f };

    // Complex branch — authored control points on the parting plane.
    std::vector<PathNode> nodes;
    bool      smooth = false;            // true: spline through nodes;
                                         // false: straight polyline

    bool      valid = false;

    float     overrunStart = 0.0f;
    float     overrunEnd = 0.0f;
};

// Back-compat alias: VentPath is the same type as FeaturePath, so every
// existing `VentPath` reference keeps working unchanged.
using VentPath = FeaturePath;

// ---------------------------------------------------------------------------
// PathStation — one sampled cross-section frame along a path. Produced by
// SamplePath and consumed by every sweeper (the GL preview mesh now; the OCC
// cut in Part 3) so preview and cut always agree on the route.
//
// Because paths are locked to the parting plane, the frame is trivial: `up` is
// always +Y, so a station only needs its position, the forward tangent, and
// the in-plane perpendicular (sideAxis). No Frenet/twist handling required.
// ---------------------------------------------------------------------------
struct PathStation
{
    glm::vec3 pos{ 0.0f };               // point on the path (parting plane)
    glm::vec3 tangent{ 0.0f, 0.0f, 1.0f };  // unit forward (sweep) direction
    glm::vec3 sideAxis{ 1.0f, 0.0f, 0.0f }; // unit in-plane perpendicular

    // Run boundary. A "run" is one contiguous swept piece: the consumer caps
    // the start of every run and the end of every run, and only connects walls
    // between stations WITHIN the same run. `startsRun` marks the first station
    // of a new run (index 0 is always an implicit run start, whether or not it
    // is flagged). Simple and smooth-complex paths are a single run (no station
    // after the first is flagged) so they sweep as one continuous tube exactly
    // as before. A straight (non-smooth) complex path emits one run PER SEGMENT
    // — two stations, both square to that segment — so every leg is its own
    // clean constant-width prism instead of a mitered chain. (The little gaps
    // that leaves at the outside of each corner are filled in a later step.)
    bool      startsRun = false;
};

// Sample a path into an ordered list of stations.
//   Simple            -> exactly two stations (start, end); one run.
//   Complex, !smooth  -> one independent run PER SEGMENT: two stations per leg
//                        (both square to that leg's direction), flagged with a
//                        run break so the consumer sweeps each leg as its own
//                        constant-width prism. No mitering, so thickness never
//                        balloons or pinches at a corner.
//   Complex, smooth    -> cubic Bezier per segment from the nodes' symmetric
//                        tangent handles (dir + handleLen), resampled at
//                        roughly `spacing` mm by arc length; one continuous run.
// Stations carry no overrun — the start/end overruns are a sweeper concern,
// applied by the consumer (BuildBoxSweepMesh / the OCC cut), not baked in here.
// Returns fewer than two stations for a degenerate path (caller treats as
// invalid).
std::vector<PathStation> SamplePath(const FeaturePath& path, float spacing = 1.5f);

// ---------------------------------------------------------------------------
// AutoComputeComplexHandles — fill every node's symmetric tangent handle
// (dir + handleLen) from the node positions using a Catmull-Rom rule, so a
// smooth complex path produces a clean interpolating spline WITHOUT the user
// having to author handles by hand. (Manual handle editing is Part 6; until
// then the authoring UI calls this whenever the node set changes.)
//
//   interior node i : dir = normalize(pos[i+1] - pos[i-1])
//   end nodes       : dir = chord direction toward the single neighbour
//   handleLen       : 1/3 of the shorter adjacent chord, so the curve stays
//                     taut and never balloons past its control polygon.
//
// No-op for Simple paths or fewer than two nodes. Operates in place.
// ---------------------------------------------------------------------------
void AutoComputeComplexHandles(FeaturePath& path);

// Swept rectangular cross-section along a path on the parting plane.
SolidMesh BuildBoxSweepMesh(const FeaturePath& path, float width, float depth,
    float overrunStart = 0.0f, float overrunEnd = 0.0f);

// Swept CIRCULAR cross-section along a path on the parting plane — the round
// analogue of BuildBoxSweepMesh, used for runners.  Driven by the same
// SamplePath stations, so a Simple path yields a plain straight cylinder
// (visually identical to BuildCylinderMesh), a non-smooth complex path yields
// one independent constant-radius cylinder per leg with a SPHERE joint at each
// interior node (the tube-radius sphere passes through each adjoining leg's end
// rim, so it mates flush at any bend angle — the circular counterpart of the
// vent's revolved-rectangle cylinder joint), and a smooth complex path yields a
// single continuous swept tube.  Un-drafted (constant radius).
SolidMesh BuildTubeSweepMesh(const FeaturePath& path, float radius,
    int segments = 32, float overrunStart = 0.0f, float overrunEnd = 0.0f);

// ---------------------------------------------------------------------------
// VentPoint  — a user-placed point on the parting surface of an object.
// ---------------------------------------------------------------------------
struct VentPoint
{
    glm::vec3 worldPos{ 0.0f, 0.0f, 0.0f };
    glm::vec3 worldNormal{ 0.0f, 1.0f, 0.0f };
};

// ---------------------------------------------------------------------------
// (VentPath is now an alias of FeaturePath, defined above with the path model.)
// ---------------------------------------------------------------------------

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

    // Part 7 (R1): the runner's route from the shared sprue feed point to
    // `point`.  Populated as a Simple path (start = sprue parting point, end =
    // point) by GLCanvas::ComputeRunnerPath, so it is byte-identical to the
    // historical straight cylinder.  Later steps let the user author a Complex
    // multi-node route in its place; until then nothing consumes this field —
    // the preview and cut are still driven directly by point + sprue feed
    // point — so populating it changes nothing that is drawn or cut.  Kept as
    // the LAST data member so the RunnerFeature{ point, {}, {} } aggregate
    // initializers keep meaning point / solid / coldPlugSolid, with path
    // value-initialized from its in-class defaults (Simple, invalid).
    FeaturePath path;

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
// EjectorFeature — placement point for an ejector pin plus its preview
// geometry.
//
// Geometry: a straight cylinder extruded in the -Y direction (toward the B
// mould half) starting at `point`, with diameter and length read from the
// MainFrame UI at rebuild time. Built in RebuildEjectorSolids on GLCanvas.
//
// Snapping target sources (handled in GLCanvas, not here): sprue parting
// point, any runner segment, any gate segment, or any object surface. The
// chosen world-space hit lands in `point`. There is no normal because the
// snap surfaces don't share a normal concept — a runner segment is a line,
// a gate path is a line, an object face has a normal but the sprue parting
// point has none. For now the geometry is always extruded -Y regardless of
// what surface was snapped; surface-aligned ejectors can come later if
// needed.
// ---------------------------------------------------------------------------
struct EjectorFeature
{
    glm::vec3 point{ 0.0f };
    SolidMesh solid;

    void Destroy() { solid.Destroy(); }
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
