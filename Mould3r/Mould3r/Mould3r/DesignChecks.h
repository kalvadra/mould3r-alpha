#pragma once

#include <glm/glm.hpp>
#include <string>
#include <vector>
#include <functional>

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

    // One per-facet draft sample produced by the face-by-face check. No geometry
    // is retained, so the array is small and cheap to reduce over.
    struct DraftSample
    {
        float signedDraftDeg = 90.0f; // asin(n . pull) in deg; + releases, - back-draft
        float area           = 0.0f;  // triangle area (mm^2) - the weight
        int   objectId       = -1;    // reserved (per-cavity); unused, always -1
        int   faceId         = -1;    // overlay key: split-triangle index + 1
        int   half           = -1;    // owning mould half: 0 = +drawAxis, 1 = -drawAxis
        bool  trapped        = false; // reserved; unused (trapped check omitted)
    };
    // ======================================================================
    // Face-by-face draft check (ray-assigned mould-half ownership)
    //
    // A lighter, remesh-free alternative to the area-weighted score above. It
    // runs directly on the shot's display mesh and, per triangle:
    //   1. Orients the facet normal outward (via the interpolated vertex normal).
    //   2. Assigns the OWNING mould half by casting a ray from just outside the
    //      facet along its outward normal into the supplied half meshes — the
    //      nearest half hit is the half whose steel forms that facet. The half
    //      meshes must already be the post-cut, orphan-resolved solids (the
    //      generator's ResolveOrphanVolumes has run), so a facet formed by the
    //      opposite half's steel (an overhang) is owned correctly rather than by
    //      the crude "which side of y=0 the centroid sits on".
    //   3. Measures signed draft relative to the OWNING half's pull direction.
    // A facet whose ray hits no half falls back to the parting-plane side of its
    // centroid. Trapped/undercut testing is intentionally omitted here.
    // ======================================================================
    struct FaceDraftParams
    {
        glm::vec3 drawAxis      = glm::vec3(0.0f, 1.0f, 0.0f); // halves part +/-
        float     failDraftDeg  = 1.0f;   // signed draft below this => fail
        float     warnDraftDeg  = 3.0f;   // ... below this (>= fail) => warning
        float     rayEpsilon    = 1.0e-3f;// ray start offset off the origin facet
        float     partingOffset = 0.0f;   // parting-plane position along drawAxis
        // Facets within this of vertical read as zero-draft, not back-draft: a
        // signed draft in [-backdraftEpsDeg, 0) is a near-vertical wall, not a
        // true negative-draft (undercut) face. Only affects the back-draft tally
        // and the back-draft overlay band, never the fail/warn thresholds.
        float     backdraftEpsDeg = 0.1f;

        // Minimum significant area: a flagged band (fail, or warn) counts toward
        // the verdict only once its total facet area reaches this threshold, so
        // isolated mesh-artifact facets don't fail an otherwise-good part. The
        // threshold is either a percentage of the shot's total surface area or an
        // absolute area in mm^2, per significanceByPercent. Zero disables it
        // (any flagged area counts). The per-facet overlay colours are unaffected
        // — suppression changes only the overall verdict.
        bool      significanceByPercent = true;  // true: value is % of surface area
        float     significanceValue     = 0.0f;  // percent (0..100) or absolute mm^2
    };

    // Split a shot mesh soup (posNorm, 6 floats/vertex + index buffer) by the
    // parting plane dot(p, planeNormal) == planeOffset. Any triangle straddling
    // the plane is cut so no output triangle crosses it; the new edge vertices
    // land exactly on the plane, with linearly interpolated (renormalised)
    // normals, and are welded so the parting line is a shared ring of vertices.
    // Non-straddling triangles pass through unchanged. This is step 1 of the
    // face-by-face draft method (clean per-half ownership at the parting line).
    void SplitMeshByPlane(
        const std::vector<float>& posNorm,
        const std::vector<unsigned int>& indices,
        const glm::vec3& planeNormal,
        float planeOffset,
        std::vector<float>& outPosNorm,
        std::vector<unsigned int>& outIndices,
        float onPlaneEps = 1.0e-5f);

    struct FaceDraftStats
    {
        Severity overall = Severity::Pass; // verdict after the significance filter
        int  totalFaces    = 0;
        int  passCount     = 0;
        int  warnCount     = 0;            // >= fail, < warn
        int  failCount     = 0;            // < fail (includes back-draft)
        int  backdraftCount= 0;            // < -backdraftEpsDeg (subset of fail)
        int  fallbackCount = 0;            // owner from parting-side fallback
        float minDraftDeg  = 90.0f;        // smallest signed draft over the shot

        // Areas (mm^2) behind the verdict, and the resolved significance gate.
        float failAreaMm2      = 0.0f;     // area of fail-band facets
        float warnAreaMm2      = 0.0f;     // area of warn-band facets
        float totalAreaMm2     = 0.0f;     // whole shot surface area scored
        float significanceMm2  = 0.0f;     // resolved absolute threshold applied
        bool  failSuppressed   = false;    // fail facets existed but below threshold
        bool  warnSuppressed   = false;    // warn facets existed but below threshold
    };

    // Build one sample per shot facet with ray-assigned half ownership. `posNorm`
    // is the shot display mesh (6 floats/vertex). `halfVerts`/`halfIndices` are
    // the per-half surface soups (xyz per vertex + index buffer) the ownership
    // ray is cast against; each half's +/- side is inferred from its centroid.
    // `triFaceId` (one entry per shot triangle) sets each sample's faceId key so
    // the caller's overlay can map it back (BREP face id on a BREP scene); pass
    // empty to key by 1-based triangle index. `outFallbackCount`, when non-null,
    // receives the number of facets that fell back to the parting-plane side.
    std::vector<DraftSample> BuildFaceDraftSamples(
        const std::vector<float>& posNorm,
        const std::vector<unsigned int>& indices,
        const std::vector<std::vector<float>>& halfVerts,
        const std::vector<std::vector<unsigned int>>& halfIndices,
        const std::vector<int>& triFaceId = {},
        const FaceDraftParams& params = FaceDraftParams{},
        int* outFallbackCount = nullptr);

    // Per-face reduction: tally each sample against the fail/warn thresholds.
    // (One sample == one facet, so these are true per-face counts.)
    FaceDraftStats ClassifyFaceDraft(
        const std::vector<DraftSample>& samples,
        const FaceDraftParams& params = FaceDraftParams{});

}  // namespace DesignChecks
