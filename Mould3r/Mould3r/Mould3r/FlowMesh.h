#pragma once
// ===========================================================================
// FlowMesh — the cavity flow mesh for the Hele-Shaw 2.5D solver (P1).
//
// Dual-domain approach: rather than collapse the shot to a true mid-surface,
// we solve on the shot's own surface mesh and pair each facet with the facet
// on the OPPOSITE wall to recover the local wall thickness. Each facet stores
// its half-gap (thickness/2) — the gap-wise integral limit the flow
// conductance S = integral(z^2/eta) dz needs later.
//
// This file is the geometry front end only: it builds the surface soup with
// per-facet thickness + wall pairing and a paired/unpaired quality flag. No
// pressure solve, no wx/GL — pure compute, like DesignChecks / MeshBoolean.
//
// Pairing: from each facet centroid, cast a ray along the INWARD normal
// (-outward, into the plastic) against the shot's own mesh and take the
// nearest hit whose triangle normal is roughly ANTI-parallel (the true
// opposing wall, not a perpendicular rib or a same-wall graze). The hit
// distance is the local wall thickness. Facets with no valid opposing wall
// (open edges, ribs, chunky blobs where the ray exits through a side) are
// flagged unpaired — a quality metric we surface, never a hard failure.
// ===========================================================================

#include <glm/glm.hpp>
#include <vector>

#include "FlowSolver.h"   // Flow::SolveMesh (the mid-surface build target)

namespace Flow
{
    struct FlowMeshParams
    {
        // Ray start offset off the origin facet, so the cast doesn't
        // immediately re-hit its own triangle. mm.
        float rayEpsilon = 1.0e-3f;

        // Reject a candidate opposing wall unless its outward normal is at
        // least this anti-parallel to the origin facet's outward normal:
        // accept when dot(hitOutward, originOutward) <= -minOpposeDot. 0.35
        // (~110 deg or more apart) tolerates draft and gentle curvature while
        // still rejecting ribs / perpendicular faces.
        float minOpposeDot = 0.35f;

        // Discard a pairing whose thickness exceeds this (mm); such a ray has
        // almost certainly shot across a hollow or out through an opening
        // rather than to the true opposite wall. <= 0 means auto (a fraction
        // of the mesh's bounding-box diagonal, set in BuildFlowMesh).
        float maxThicknessMm = 0.0f;

        // Ignore hits closer than this (mm) as self/neighbour grazes.
        float minThicknessMm = 1.0e-4f;
    };

    struct FlowMeshStats
    {
        int   totalFacets    = 0;
        int   pairedFacets   = 0;   // facets with a valid opposing wall
        int   unpairedFacets = 0;
        float minThicknessMm  = 0.0f;   // over paired facets
        float maxThicknessMm  = 0.0f;
        float meanThicknessMm = 0.0f;
        float pairedFraction  = 0.0f;   // pairedFacets / totalFacets
    };

    // The built flow mesh. `posNorm` and `indices` mirror the input soup 1:1
    // (6 floats/vertex, 3 indices/triangle), so per-facet arrays are indexed
    // by triangle number and the caller can render the same soup with a
    // scalar->colour overlay. `thicknessMm`/`halfGapMm`/`paired` all have one
    // entry per triangle; for an unpaired facet thickness/half-gap are 0.
    struct FlowMesh
    {
        std::vector<float>        posNorm;      // xyz + nxyz per vertex
        std::vector<unsigned int> indices;      // 3 per triangle
        std::vector<float>        thicknessMm;  // per triangle (0 if unpaired)
        std::vector<float>        halfGapMm;    // thickness/2 (0 if unpaired)
        std::vector<unsigned char> paired;      // per triangle: 1 = paired
        std::vector<int>          opposite;     // per triangle: paired tri, -1 none

        bool  empty() const { return indices.size() < 3; }
        size_t triCount() const { return indices.size() / 3; }
    };

    // Build the flow mesh from a shot surface soup (posNorm 6 floats/vertex,
    // index buffer). Returns true if a mesh was produced (even with unpaired
    // facets); false only for an empty/degenerate input. `stats`, if non-null,
    // receives the pairing/thickness summary.
    bool BuildFlowMesh(const std::vector<float>& posNorm,
                       const std::vector<unsigned int>& indices,
                       const FlowMeshParams& params,
                       FlowMesh& out,
                       FlowMeshStats* stats = nullptr);

    struct SolveMeshStats
    {
        int   nodes        = 0;
        int   tris         = 0;
        int   sourceFacets = 0;   // paired facets in the flow mesh
        int   usedFacets   = 0;   // representatives kept (one per wall pair)
        int   droppedUnpaired = 0;
        float minGapMm     = 0.0f;
        float maxGapMm     = 0.0f;
        int   gateNode     = -1;  // resolved gate node (first gate)
        int   gateCount    = 0;
    };

    // Collapse the dual-domain FlowMesh into a single-wall MID-SURFACE SolveMesh
    // the flow solver runs on: weld the shot shell into shared nodes, keep one
    // wall of each paired facet (the +draw side), and offset each welded node
    // inward by its local half-gap so the kept wall lands on the cavity mid-
    // plane — carrying the half-gap through as the solver's per-node gap. The
    // offset is applied once per welded node, so shared nodes stay shared (no
    // cracks). Unpaired facets (ribs / open edges / chunky blobs) are dropped
    // from the flow domain. Each point in `gatePoints` (world space, the gate /
    // sprue cavity-entry positions) snaps to its nearest mid-surface node and
    // becomes an injection node; an empty list falls back to the extreme +draw
    // node. Returns false for an empty/degenerate flow mesh.
    bool BuildSolveMesh(const FlowMesh& fm,
                        const std::vector<glm::vec3>& gatePoints,
                        const glm::vec3& drawAxis,
                        SolveMesh& out,
                        SolveMeshStats* stats = nullptr);

} // namespace Flow
