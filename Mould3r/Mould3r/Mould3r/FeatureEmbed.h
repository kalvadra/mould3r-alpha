#pragma once

#include <glm/glm.hpp>

#include <cstdint>
#include <limits>
#include <vector>

// ===========================================================================
// FeatureEmbed — "is this gate / vent mouth fully embedded in the part?"
//
// A gate or vent is placed at a point on the part's parting line and cut as a
// straight channel whose START cross-section (circle / rectangle) sits on the
// plane through that point, perpendicular to the channel's first direction.
// On a curved (or obliquely-approached) surface part of that cross-section is
// OUTSIDE the part, so a wedge of mould steel is left between the channel and
// the cavity over that part of the mouth. Today the only defence is a fixed
// back-extension (gate kCutEps + Overrun, vent Overrun Start).
//
// This module measures the gap directly. The start cross-section is sampled
// as a bundle of lines parallel to the channel axis. Each line is intersected
// with the part mesh(es) along its FULL length, so the in/out parity is exact
// and every line resolves to the part's inside intervals along it. Then, with
// t = distance travelled back from the mouth plane INTO the part:
//
//   * entry(line)   — where the line first enters the part at or behind the
//                     mouth. A back-extension of L fills that line's gap iff
//                     L >= entry.
//   * exit(line)    — where it leaves that same inside interval. Extending
//                     past exit punches through a thin wall into steel behind
//                     it, carving an unwanted groove — so exit is a CEILING.
//   * a line that never meets the part within `maxExtension` is UNREACHABLE:
//     the channel is locally bigger than the part (e.g. a 5 mm deep vent on a
//     2 mm thick flange). Extension cannot fix those rows, so they are
//     reported, not chased.
//
// The recommended extension is the smallest L that lands every reachable line
// inside the part (max entry + margin), clamped to the ceiling and the cap.
//
// Why lines along the axis rather than "closest distance to the body": the
// extension is an extrusion along -axis, so the gap that matters is measured
// along -axis. Closest-point distance under-reads it on any surface the
// channel meets obliquely (by 1/cos of the approach angle) and can point
// sideways at a feature the extrusion never reaches; it also can't see the
// exit / punch-through ceiling.
//
// Pure geometry: glm + std only, no GL / OCC / wx, so it can run anywhere
// (placement, edit-drag, Generate) and be unit-tested on its own. Works on the
// tessellated CPU mesh every SceneObject already carries, so BREP and mesh
// scenes are analysed identically. (For BREP parts the tessellation's chordal
// deviation is absorbed by `margin`.)
// ===========================================================================
namespace FeatureEmbed
{
    // One part body as it is stored on a SceneObject: object-space triangle
    // soup plus its model matrix. Pointers must outlive the Analyze call.
    struct BodyMesh
    {
        const std::vector<float>*    verts   = nullptr;  // 3 floats per vertex
        const std::vector<uint32_t>* indices = nullptr;  // 3 per triangle
        glm::mat4                    model   = glm::mat4(1.0f);
    };

    struct Params
    {
        // Upper bound on the back-extension (mm). The feature is never pushed
        // further than this into the part, whatever the gap.
        float maxExtension = 10.0f;

        // Added on top of the deepest measured entry so the mouth sits
        // properly inside the part rather than exactly on its skin. Also
        // absorbs BREP tessellation deviation. Subtracted from the ceiling.
        float margin = 0.1f;

        // When true, the recommended extension is clamped so it never runs
        // out the back of a thin wall (see Result::ceiling). When false the
        // ceiling is still measured and reported, but the extension goes to
        // the full depth needed to seat every reachable line — the mouth is
        // guaranteed to connect, at the risk of a groove behind a thin wall.
        // Mould3r currently passes false (thin-wall handling deferred to the
        // later out-of-bounds analysis).
        bool  respectCeiling = true;

        // Hits closer than this along a line are one crossing (a line through
        // a shared edge / vertex reports the same crossing from every
        // triangle that touches it).
        float mergeTol = 1.0e-4f;
    };

    enum class LineState
    {
        Embedded,     // mouth already inside the part on this line (entry <= 0)
        Gap,          // outside at the mouth; enters the part within the cap
        Unreachable,  // never enters the part within maxExtension
        Unreliable    // odd crossing count even after nudging (open mesh)
    };

    struct LineResult
    {
        glm::vec3 offset{ 0.0f };   // sample position relative to the origin
        LineState state = LineState::Unreliable;
        float     entry = 0.0f;     // valid for Embedded (<= 0) and Gap (> 0)
        float     exit  = 0.0f;     // end of that inside interval (> 0)
    };

    struct Result
    {
        bool  analysed = false;     // false: no geometry / no samples

        // The measured gap: the deepest entry over all reachable lines,
        // WITHOUT margin (0 when the mouth is already fully inside).
        float maxGap = 0.0f;

        // Punch-through ceiling: smallest exit over reachable lines, minus
        // margin, clamped to maxExtension.
        float ceiling = 0.0f;

        // Recommended back-extension from the mouth plane:
        //   min(maxGap + margin, ceiling)  — or 0 if nothing needs filling.
        //   When the two margins don't both fit but maxGap is still short of
        //   the thinnest exit, the midpoint between them is used instead.
        // Callers typically combine it with the user's own overrun, e.g.
        // max(userOverrun, extension).
        float extension = 0.0f;

        // True when `extension` lands EVERY sample line inside the part:
        // no unreachable / unreliable lines and the ceiling didn't bite.
        bool  fullyEmbedded = false;

        // Fraction (0..1) of sample lines that are inside the part at depth
        // `extension` — the share of the mouth that actually connects.
        float coverage = 0.0f;

        // True when no single depth reaches every line without another line
        // punching out the back of a thin wall (or the maxExtension cap bit):
        // extension was clamped below maxGap and some of the mouth stays
        // walled off. Trimming only the safety margin does NOT set this.
        bool  ceilingLimited = false;

        int   samples     = 0;
        int   embedded    = 0;      // already inside at the mouth
        int   gap         = 0;      // needed extension
        int   unreachable = 0;
        int   unreliable  = 0;

        // Sample offset with the deepest entry (for a viewport marker), and
        // the per-line detail for debugging / overlays.
        glm::vec3               worstOffset{ 0.0f };
        std::vector<LineResult> lines;
    };

    // Analyse a channel mouth.
    //   origin     — the placed point (on the parting line), world space
    //   intoPart   — unit direction pointing from the mouth INTO the part,
    //                i.e. minus the channel's first tangent
    //   offsets    — cross-section sample offsets from origin, world space,
    //                all perpendicular to intoPart (see *Samples below)
    //   bodies     — the part(s) forming the cavity; their inside intervals
    //                are unioned per line
    Result Analyze(const glm::vec3& origin,
                   const glm::vec3& intoPart,
                   const std::vector<glm::vec3>& offsets,
                   const std::vector<BodyMesh>& bodies,
                   const Params& params = Params());

    // Rectangular section (vents): an n x m grid spanning +-halfWidth along
    // sideAxis and +-halfDepth along upAxis, corners and edges included.
    std::vector<glm::vec3> RectSamples(const glm::vec3& sideAxis,
                                       const glm::vec3& upAxis,
                                       float halfWidth, float halfDepth,
                                       int n = 9, int m = 9);

    // Circular section (gates): the centre plus `rings` concentric rings out
    // to `radius`, `spokes` points per ring, in the plane perpendicular to
    // `axis`.
    std::vector<glm::vec3> DiscSamples(const glm::vec3& axis, float radius,
                                       int rings = 4, int spokes = 24);
}
