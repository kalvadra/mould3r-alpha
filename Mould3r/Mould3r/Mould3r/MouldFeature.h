#pragma once

#include <glad/glad.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <cstdint>
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

// UV sphere, positioned at `center` with the given `radius`. Unlike
// GLCanvas::BuildSphereGPU (a shared UNIT sphere at the origin, scaled
// per-instance for marker rendering), this bakes the position and true
// radius into the vertex data — the same convention BuildCylinderMesh uses
// — so it can stand in as an ordinary per-feature SolidMesh (see
// IndexerFeature below).
SolidMesh BuildSphereMesh(const glm::vec3& center, float radius,
    int stacks = 16, int slices = 24);

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

// ---------------------------------------------------------------------------
// MakeTwoNodePath - turn a derived straight a->b route into the equivalent
// 2-node Complex path.
//
// Every feature path is Complex now: PathKind::Simple survives only as the
// transient state a path occupies between being derived and being promoted
// (and as what an old project file loads as, until the first rebuild promotes
// it). A 2-node Complex path samples to the same two stations a Simple one
// did, so the swept preview and the OCC cut are unchanged.
//
// CALLERS MUST ONLY PROMOTE A RESOLVED PATH. A path that could not be derived
// yet - a runner placed before the sprue exists, a gate with no feed to snap
// to - has to stay Simple so the next rebuild re-derives it; promoting an
// unresolved path freezes the placeholder forever, because the Complex branch
// of every Compute*Path preserves what it finds instead of re-deriving.
// ---------------------------------------------------------------------------
void MakeTwoNodePath(FeaturePath& path, const glm::vec3& a, const glm::vec3& b);

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
    int segments = 32, float overrunStart = 0.0f, float overrunEnd = 0.0f,
    bool sphereAtStart = false, bool sphereAtEnd = false);

// ---------------------------------------------------------------------------
// Gate types (Gates card "Gate type" dropdown).
//
//   TaperedCylinder — the original drafted cone (Diameter / Draft angle).
//   Tab             — a rectangular block, Width x Height x Length, from the
//                     gate point toward the feed; the sub-runner starts at its
//                     far end.
//   Fan             — the Tab with a per-side Taper angle on its width: Width is
//                     measured at the PART side (the placed point, like the
//                     cone's Diameter) and the block narrows toward the
//                     sub-runner. Height is constant.
//
// The underlying type is fixed so MainFrame.h can forward-declare it.
// ---------------------------------------------------------------------------
enum class GateType : int { TaperedCylinder = 0, Tab = 1, Fan = 2 };

// Dropdown / file label <-> type. Unknown labels read as TaperedCylinder so
// older files and fixtures keep their historical gate.
const char* GateTypeLabel(GateType t);
GateType    GateTypeFromLabel(const char* label);

// Which mould half a Tab / Fan gate is cut into. A is the top half (+Y), B the
// bottom (-Y). Both = the block straddles the parting surface, Height split
// evenly (like a vent); A / B = the whole Height sits in that one half, its
// near face on the parting surface. The sub-runner stays full-round either way.
enum class GateSide : int { Both = 0, A = 1, B = 2 };
const char* GateSideLabel(GateSide s);          // "Both", "A side", "B side"
GateSide    GateSideFromLabel(const char* label); // unknown -> Both

// Vertical extent of the block for a cut side: the height to build and the
// offset of its centre along upAxis (+ = toward A). `crossEps` lets a one-sided
// block reach that far past the parting surface into the OTHER half, so the cut
// never leaves the block's face coplanar with the half's own parting face; it
// is only safe because the mould cut applies a one-sided block to its own half
// alone. The preview, shot and auto-embed pass 0 (the exact block).
void GateBlockVertical(GateSide side, float height, float crossEps,
                       float& outHeight, float& outUpOffset);

// Narrowest the Fan's sub-runner end may get (mm). A taper steep enough to
// close the fan before Length is flattened to land here instead.
constexpr float kGateFanMinFrontWidth = 0.1f;

// Per-side width change of a Fan, in mm of HALF-width per mm along the gate
// axis (= tan(taperDeg)), reduced if needed so the sub-runner end never gets
// narrower than kGateFanMinFrontWidth. 0 for a zero angle. The back-extension
// (overrun / auto-embed) continues at this same slope, so the side walls stay
// single planes from the extended back face to the sub-runner end.
float GateFanSlope(float width, float length, float taperDeg);

// The two axes perpendicular to a gate's first-leg direction: sideAxis (the
// Width direction, horizontal) and upAxis (the Height direction, as close to
// world Y as the leg allows — a gate sitting just off the parting plane can
// tilt its first leg slightly). The block is centred on the parting surface
// like the vent cross-section: Height splits evenly between the two halves.
void GateBlockAxes(const glm::vec3& dir, glm::vec3& sideAxis, glm::vec3& upAxis);

// ---------------------------------------------------------------------------
// GateBlock — the Tab / Fan gate body: a ruled solid between two rectangles
// perpendicular to the gate axis. All six faces are planar (Height is
// constant and the width changes symmetrically), so it cuts as exact
// geometry.
//
//   back[]  — the part-side face, pushed `backExt` behind the placed point
//             along -dir (overrun / auto-embed / cut epsilon). On a Fan it is
//             wider than Width: the fan keeps fanning back into the part.
//   front[] — the sub-runner face, `frontLen` along +dir from the placed point.
//   Corner order BL, BR, TR, TL (B/T along upAxis, L/R along sideAxis), the
//   same as VentCrossSection.
// ---------------------------------------------------------------------------
struct GateBlock
{
    bool      valid = false;
    glm::vec3 back[4] = {};
    glm::vec3 front[4] = {};
    glm::vec3 dir{ 0.0f }, sideAxis{ 0.0f }, upAxis{ 0.0f };
    float     backWidth = 0.0f, frontWidth = 0.0f, height = 0.0f;
    float     axialLen = 0.0f;   // backExt + frontLen
};

// `width` is the width AT `origin`; `slope` from GateFanSlope (0 for a Tab);
// `upOffset` shifts the block's centre along upAxis (GateBlockVertical).
// Invalid when height / width <= 0 or the total axial length is ~0.
GateBlock MakeGateBlock(const glm::vec3& origin, const glm::vec3& dir,
    float width, float height, float slope, float backExt, float frontLen,
    float upOffset = 0.0f);

// Preview mesh of a GateBlock (flat-shaded box / frustum-prism).
SolidMesh BuildGateBlockMesh(const GateBlock& block);

// ===========================================================================
// Channel profiles — runner / sub-runner cross-sections.
//
//   Cylinder    — full-round, centred on the parting surface (the original
//                 runner; still built by BuildTubeSweepMesh /
//                 BuildRunnerCutPieces, so its geometry is unchanged).
//   Parabolic   — cut into ONE half: a circle of `diameter` touching the
//                 parting plane at a single point, on the cut side, plus two
//                 straight walls tangent to it, leaning out at `draftDeg` off
//                 the parting direction until they reach the parting plane.
//                 Open width at the plane = D (1 + sin a) / cos a; depth = D.
//   Trapezoidal — cut into ONE half: `width` at the parting plane, `height`
//                 deep, side walls at `draftDeg` (so the floor is narrower).
//
// Profiles are described in 2D section coordinates: s along the station's
// side axis, u perpendicular to the parting plane INTO the cut half (u >= 0).
// The world mapping is pos + sideAxis * s + Y * (sign * u), sign = +1 for the
// A half (top, +Y), -1 for B.
// ===========================================================================
enum class ChannelType : int { Cylinder = 0, Parabolic = 1, Trapezoidal = 2 };

// "Cylinder" / "Cylindrical" (both historical spellings) / "Parabolic" /
// "Trapezoidal"; anything else reads as Cylinder.
ChannelType ChannelTypeFromLabel(const char* label);

constexpr float kChannelDefaultDraftDeg = 10.0f;

struct ChannelProfile
{
    ChannelType type = ChannelType::Cylinder;
    float diameter = 5.0f;              // Cylinder, Parabolic
    float width = 5.0f;                 // Trapezoidal: at the parting plane
    float height = 3.5f;                // Trapezoidal: depth into the half
    float draftDeg = kChannelDefaultDraftDeg;  // Parabolic, Trapezoidal
    GateSide side = GateSide::B;        // Parabolic, Trapezoidal: A or B only

    bool  IsRound() const { return type == ChannelType::Cylinder; }
    // +1 when cut into A (+Y), -1 into B. (Both is never offered; read as B.)
    float SideSign() const { return side == GateSide::A ? 1.0f : -1.0f; }
};

// Half the open width where the channel meets the parting plane (round: the
// radius). Used as the "is this point on the channel" tolerance.
float ChannelHalfWidth(const ChannelProfile& p);

// Radius a Tapered Cylinder gate cone grows to before handing over to this
// channel as its sub-runner. Round: the radius (unchanged behaviour);
// Parabolic: D/2; Trapezoidal: height/2. The cone stays centred on the parting
// surface, so a one-sided sub-runner meets it with a step — they overlap
// between the plane and the cone's rim on the cut side.
float ChannelConeRadius(const ChannelProfile& p);

// Exact cross-section area and perimeter (crossEps = 0), mm^2 / mm.
void ChannelAreaPerimeter(const ChannelProfile& p, double& area, double& perimeter);

// One edge of a profile outline, exact: a straight line a->b, or a circular
// arc a->mid->b. Outlines are closed, convex and counter-clockwise in (s, u).
struct ChannelSeg
{
    bool      arc = false;
    glm::vec2 a{ 0.0f }, b{ 0.0f }, mid{ 0.0f };
};

// Exact outline of a one-sided profile. `crossEps` (>= 0) pushes the base edge
// that far past the parting plane (u = -crossEps) along the side walls, so a
// cut tool never has a face coplanar with the half's parting face — only safe
// because a one-sided channel is cut from its own half alone. Parabolic: base,
// right wall, top arc, left wall. Trapezoidal: base, right wall, floor, left
// wall. Empty for Cylinder.
std::vector<ChannelSeg> ChannelExactOutline(const ChannelProfile& p, float crossEps);

// Solid of revolution used as a channel JOINT (bends, and the round end a
// sub-runner starts in behind a Tab / Fan gate): the half-profile revolved
// about the vertical axis through the node, which meets every adjoining leg
// flush at any angle. Parabolic = cone frustum (base -> wall tangent points)
// + the sphere of the profile circle; Trapezoidal = one cone frustum. Values
// are along u from the parting plane (u0 = -crossEps).
struct ChannelJointShape
{
    float u0 = 0.0f, r0 = 0.0f;     // frustum base (at the parting plane side)
    float u1 = 0.0f, r1 = 0.0f;     // frustum top
    bool  sphere = false;           // Parabolic: plus a sphere ...
    float sphereU = 0.0f, sphereR = 0.0f;   // ... centred at u = sphereU
};
ChannelJointShape ChannelJoint(const ChannelProfile& p, float crossEps);

// Preview sweep of a channel along a path, the counterpart of
// BuildTubeSweepMesh for every profile. Round profiles go straight to
// BuildTubeSweepMesh (radius D/2) so they are unchanged. One-sided profiles:
// one swept run per leg (or one continuous run when smooth), flat end caps,
// a revolved joint (ChannelJoint) at every interior node, and optionally at
// the first / last station.
SolidMesh BuildChannelSweepMesh(const FeaturePath& path, const ChannelProfile& profile,
    bool jointAtStart = false, bool jointAtEnd = false);

// CPU half of BuildChannelSweepMesh for one-sided profiles (no GL): appends
// [pos, normal] vertices and triangle indices. Exposed for testing.
void ChannelSweepGeometry(const FeaturePath& path, const ChannelProfile& profile,
    bool jointAtStart, bool jointAtEnd,
    std::vector<float>& verts, std::vector<uint32_t>& idx);

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

    // Auto-embed result: the back-extension (mm, from the placed point into
    // the part) that seats the WHOLE start cross-section inside the part.
    // Derived at Generate Mould by GLCanvas::RunAutoEmbedAnalysis and never
    // saved. 0 when Auto-embed is off, nothing needed extending, or the mouth
    // couldn't be embedded within the limit (the feature is then flagged and
    // cut with its plain overrun). Cut sites use max(overrun, embedExtension).
    float     embedExtension = 0.0f;

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

    // Gate step G1: the SUB-RUNNER's route from the gate origin (point.worldPos)
    // to the feed attach point.  Populated as a Simple path (start = gate origin,
    // end = pathEnd) by GLCanvas::ComputeGatePath, which runs at the top of
    // RebuildGateSolids.  The GATE FRUSTUM itself is never described by this path
    // — it stays a straight tapered cone driven purely by the gate-card fields
    // (diameter / draft / sub-runner diameter / overrun); only the sub-runner
    // section becomes a Complex multi-node route.  Until later steps consume it,
    // nothing reads this field — the frustum and sub-runner preview / cut are
    // still driven directly by point.worldPos + pathEnd — so populating it
    // changes nothing that is drawn or cut.  node[0] is pinned to the gate
    // origin; nodes.back() is the feed attach point (auto-snapped while Simple,
    // freely authored while Complex).
    FeaturePath subPath;

    // Auto-embed result: the back-extension (mm, from the placed point into
    // the part) that seats the WHOLE start cross-section inside the part.
    // Derived at Generate Mould by GLCanvas::RunAutoEmbedAnalysis and never
    // saved. 0 when Auto-embed is off, nothing needed extending, or the mouth
    // couldn't be embedded within the limit (the feature is then flagged and
    // cut with its plain overrun). Cut sites use max(overrun, embedExtension).
    float     embedExtension = 0.0f;

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
// IndexerFeature — placement point for an indexer (a spherical registration
// feature that lets the two mould halves fit tightly together) plus its
// preview geometry.
//
// Placement: a single point locked to the parting plane (y = 0), picked via
// GLCanvas::RayCastToPartingPlane — the same simple ray/plane intersection
// every other parting-plane point (runner nodes, gate sub-runner nodes, the
// free sprue terminus) uses. Unlike EjectorFeature there is no multi-source
// snapping; an indexer always sits exactly on the plane.
//
// Geometry: currently a preview sphere only (radius read from the MainFrame
// UI at rebuild time, in GLCanvas::RebuildIndexerSolids — mirrors
// RebuildEjectorSolids). The generation-time behaviour Clayton specced
// (Aug 2026): split the sphere at y = 0; fuse the upper (A-side) hemisphere
// into the A mould half as-is; scale the lower (B-side) hemisphere up about
// its center by the Extra Tolerance and fuse it into the B mould half;
// mirror both fuse operations into the Cast Shot Body so the casting bodies
// (top/bottom base) pick up the same registration feature when generated.
// That boolean step is NOT implemented yet — this struct only carries the
// placement point and the preview sphere, matching where EjectorFeature's
// cut geometry currently stands ("geometry TBD").
// ---------------------------------------------------------------------------
struct IndexerFeature
{
    glm::vec3 point{ 0.0f };   // always on y = 0 (the parting plane)
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

    // Radial endpoint override (Edit Sprue > Move). When the user drags the
    // endpoint of a RADIAL sprue, the dragged world position lands here and
    // hasEndpointOverride is set; PlaceSprue then uses it verbatim for pathEnd
    // instead of auto-deriving the terminus from the perimeter / an inward
    // ray. Only ever set for radial sprues — axial endpoints are not movable
    // (moving one would compromise demolding), so this stays false there.
    // Cleared by SetActiveInjectionPoint (a fresh injection-point pick starts
    // the sprue over) and by Clear(). The override lives on pathEnd once
    // applied, so save/load round-trips it through the existing pathEnd field
    // with no format change; this flag only matters while the sprue can still
    // be rebuilt in-session.
    bool      hasEndpointOverride = false;
    glm::vec3 endpointOverride{ 0.0f };

    // Whether the overridden radial endpoint sits ON a part (Edit Sprue > Move
    // snapped it to an object's parting-plane outline). When true the moved
    // sprue is a DIRECT-INJECTION sprue (ends at the model contact, no cold
    // slug); when false it's a free terminus with the usual cold slug. Only
    // meaningful while hasEndpointOverride is set; reset with it in Clear().
    bool      overrideDirectInjection = false;

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
        hasEndpointOverride = false;
        overrideDirectInjection = false;
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
