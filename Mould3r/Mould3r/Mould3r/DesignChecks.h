#pragma once

#include <glm/glm.hpp>
#include <string>
#include <vector>

class TopoDS_Shape;   // analysed at BREP level (see .cpp for OCC includes)

// ===========================================================================
// DesignChecks — geometric suitability analysis of the shot body.
//
// Operates at the BREP (boundary-representation) level on the shot solid, so
// each logical face is judged as a unit with its analytic surface normal,
// rather than as thousands of tessellation triangles. By convention the whole
// shot is evaluated as one body, not individual objects.
//
// Demoldability (straight-pull, two-plate mould, parting plane at Y = 0,
// halves opening along +Y / -Y): can the shot be drawn out of the steel?
// Per face the analysis samples the analytic surface and assesses:
//   * Draft — angle of the surface relative to the pull axis. A planar wall is
//     uniform; a curved face varies, so it is sampled and judged on its worst
//     (smallest) draft.
//   * Accessibility — whether the surface, given the direction it must release
//     in, is blocked by another part of the shot. Tested with an analytic
//     ray/solid intersection (no tessellation tolerances). A blocked face is a
//     true undercut: it locks the body in the steel.
//
// Results are reported as 1-based face indices into a TopExp::MapShapes face
// map of the same shot shape. The caller builds the identical map (via the
// display tessellation's per-triangle face id) to colour the offending facets.
// ===========================================================================
namespace DesignChecks
{
    enum class Severity { Pass, Warning, Fail };

    struct Issue
    {
        Severity    severity = Severity::Pass;
        std::string description;
        glm::vec3   location{ 0.0f };  // representative world point
    };

    struct DemoldabilityResult
    {
        Severity           overall = Severity::Pass;
        std::vector<Issue> issues;

        // Offending faces by category, as 1-based indices into the shot's
        // TopExp::MapShapes(TopAbs_FACE) map. A face appears in at most one.
        std::vector<int> undercutFaces;   // blocked along the pull axis
        std::vector<int> failDraftFaces;  // worst draft below fail threshold
        std::vector<int> warnDraftFaces;  // worst draft below warn threshold

        // Summary figures.
        float minDraftDeg    = 90.0f;  // smallest face draft over the shot
        int   undercutCount  = 0;
        int   failDraftCount = 0;
        int   warnDraftCount = 0;
        int   totalFaces     = 0;
    };

    struct Params
    {
        glm::vec3 drawAxis = glm::vec3(0.0f, 1.0f, 0.0f);  // pull axis (+/-)

        float failDraftDeg = 1.0f;   // worst-draft below this => fail
        float warnDraftDeg = 3.0f;   // ... below this (but >= fail) => warning

        // When false, the accessibility (undercut) analysis is skipped entirely
        // — only the draft assessment runs. Used by the "Draft Angle Checks"
        // simulation, which is concerned solely with draft; the separate
        // demoulding (separation) test covers trapping/undercuts.
        bool checkUndercuts = true;

        // Ray start offset along the pull axis (world units) to skip the
        // originating surface when testing accessibility.
        float rayEpsilon = 1.0e-3f;

        // Linear deflection for the sampling tessellation. Coarser than the
        // display mesh — accessibility is analytic, so this only sets how
        // densely each face is probed.
        float sampleDeflection = 0.25f;
    };

    // One accessibility ray that registered a block (i.e. flagged its face as
    // an undercut): where it started on the shot surface, the unit pull
    // direction tested, and the nearest point where it struck the shot again.
    // Recorded for debugging/visualisation.
    struct UndercutRay
    {
        glm::vec3 origin;  // on the originating face
        glm::vec3 dir;     // unit pull direction tested
        glm::vec3 hit;     // nearest contact point on the shot
    };

    // Run the demoldability assessment on the shot BREP. Returns a Pass result
    // with no issues when the shape is null or has no analysable faces. When
    // `debugRays` is non-null it is filled with the triggering ray of each
    // undercut face (one per undercut face) for visualisation.
    DemoldabilityResult CheckDemoldability(
        const TopoDS_Shape& shot,
        const Params& params = Params{},
        std::vector<UndercutRay>* debugRays = nullptr);

    // ---- Debug: draft-sign classification ----------------------------------
    // Which way each face points relative to the pull axis, using the SAME
    // analytic normals the demoldability check uses — so it isolates whether
    // those normals are oriented as expected (a planar wall that clearly faces
    // up but lands in `downFaces` indicates an inverted normal). A curved face
    // whose samples disagree (e.g. a cylinder spanning the parting plane) is
    // reported as `mixed`. Face indices are 1-based into the shot's
    // TopExp::MapShapes(TopAbs_FACE) map, as for DemoldabilityResult.
    struct DraftSignResult
    {
        std::vector<int> upFaces;        // all samples face +drawAxis
        std::vector<int> downFaces;      // all samples face -drawAxis
        std::vector<int> verticalFaces;  // all samples ~parallel to pull
        std::vector<int> mixedFaces;     // samples disagree (up and down)
        int totalFaces = 0;
    };

    DraftSignResult ClassifyDraftSign(
        const TopoDS_Shape& shot,
        const Params& params = Params{});

    // ---- Alternative: separation (collision) demoldability -----------------
    // A physically-direct check: lift each mould half a small distance along
    // its draw direction and test for interference with the shot. Any overlap
    // is a true undercut (a region where steel drives into the body). This is
    // an independent cross-check of CheckDemoldability's analytic undercut
    // detection; note it only finds hard locks, not insufficient draft (a
    // vertical wall slides without colliding).
    struct SeparationParams
    {
        glm::vec3 drawAxis = glm::vec3(0.0f, 1.0f, 0.0f);  // halves part along +/-
        float     liftMm = 1.0f;        // separation distance
        double    volumeThreshold = 1.0e-3;  // ignore sub-this overlap (noise)
    };

    struct SeparationResult
    {
        Severity overall = Severity::Pass;
        int      halvesTested = 0;
        int      halvesCollided = 0;
        int      halvesFailedToEval = 0;   // the boolean could not be computed
        double   totalOverlapVolume = 0.0; // cubic mm

        // Aligned to the input halves vector.
        std::vector<double> perHalfVolume;
        std::vector<int>    perHalfStatus;  // 0 clear, 1 collision, 2 eval-failed
    };

    // Run the separation check. Each half is lifted along the side of drawAxis
    // its centroid lies on, then intersected with the shot. When `outOverlap`
    // is non-null it receives a compound of the interference regions (for
    // visualisation); it is null/empty when nothing collided.
    SeparationResult CheckSeparation(
        const TopoDS_Shape& shot,
        const std::vector<TopoDS_Shape>& halves,
        const SeparationParams& params = SeparationParams{},
        TopoDS_Shape* outOverlap = nullptr);

}  // namespace DesignChecks
