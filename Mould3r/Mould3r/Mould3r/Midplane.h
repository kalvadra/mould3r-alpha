#pragma once
// ===========================================================================
// Midplane — the planform ("pull-axis") midplane of a moulded part for the
// Hele-Shaw flow solve.
//
// Mould3r always draws along Y, so a part's flow domain is built in PLAN VIEW:
//   1. Footprint: every source triangle is projected onto the parting plane
//      (XZ) and the projections are unioned (Clipper2) into the part outline,
//      holes included. Works on any triangle soup — no watertight BREP needed.
//   2. 2D mesh at a target triangle area: the outline is resampled at the
//      matching edge length (sharp corners kept exactly), the interior filled
//      with an equilateral lattice, and the lot triangulated with the outline
//      as constraints (CDT). A safe Laplacian pass evens out the boundary band,
//      then any triangle over the target area is split along its longest edge
//      until none remain — so every triangle is at most the target area.
//   3. Thickness + midplane height: at each node a line along Y is intersected
//      with the part's own surface. The material interval gives the vertical
//      thickness and its middle the node's midplane height, so the midplane
//      follows steps and gentle curvature instead of being one flat slice.
//      The Hele-Shaw gap is that thickness corrected for surface inclination
//      (h * mean |n_y| of the two walls), exact for a uniformly thick inclined
//      sheet.
//
// For a straight-pull (demouldable) part every Y column crosses exactly one
// material interval, so the planform midplane is well defined everywhere. Its
// weakness is tall walls running along the pull: there the footprint is thin
// but the column is tall and the midplane facets tilt steeply. Those facets
// (tilt beyond MidplaneParams::steepTiltDeg) are flagged rather than trusted,
// as are multi-interval columns (an undercut) and nodes whose column missed.
//
// The triangle-area target applies to the actual midplane surface (3D area),
// except on flagged steep facets, which are held to it in plan view only.
//
// Pure compute — no wx, no GL. Built per part in the Preview perspective from
// the PartSurface snapshot taken at Generate Mould.
// ===========================================================================

#include <glm/glm.hpp>
#include <vector>
#include <string>

#include "FlowSolver.h"   // Flow::SolveMesh (the fill solver's input)

namespace Flow
{
    // A moulded part's own surface in world space, captured at Generate Mould
    // (GLCanvas::BuildPartSurfaces). One per imported object.
    struct PartSurface
    {
        int                       objectIndex = -1;   // GLCanvas::m_objects index
        std::string               label;
        std::vector<float>        xyz;                // 3 floats / vertex (mm)
        std::vector<unsigned int> indices;            // 3 / triangle
    };

    struct MidplaneParams
    {
        float targetAreaMm2    = 1.0f;    // max triangle area on the midplane
        float steepTiltDeg     = 60.0f;   // facets tilted past this are flagged
        int   smoothIterations = 2;       // safe Laplacian passes on the 2D mesh
        int   maxRefinePasses  = 8;       // split-to-target passes
        int   maxTriangles     = 2000000; // refuse beyond this (time / memory)
    };

    // Per-node flags (bitwise).
    enum MidplaneNodeFlag : unsigned char
    {
        MidNodeBoundary   = 1,   // on the footprint outline
        MidNodeFilled     = 2,   // column missed; thickness filled from neighbours
        MidNodeMultiLayer = 4,   // column crossed >1 material interval (undercut)
        MidNodeSteep      = 8    // touches a steep (flagged) facet
    };

    struct MidplaneStats
    {
        bool        ok = false;
        std::string message;

        int    nodes = 0, tris = 0;
        double footprintAreaMm2 = 0.0;   // plan-view area of the outline
        double midplaneAreaMm2  = 0.0;   // 3D area of the midplane surface

        // Triangle size / shape on the midplane surface (non-steep facets).
        float minAreaMm2 = 0.0f, meanAreaMm2 = 0.0f, maxAreaMm2 = 0.0f;
        float minAngleDeg = 0.0f;
        float meanQuality = 0.0f;        // 4*sqrt(3)*A / sum(l^2): 1 = equilateral
        float worstQuality = 0.0f;

        // Gap (tilt-corrected thickness) over sampled nodes.
        float minThicknessMm = 0.0f, meanThicknessMm = 0.0f, maxThicknessMm = 0.0f;

        // Volume check: the part's own (closed-surface) volume vs the integral
        // of column length over the footprint. They agree to discretisation
        // error when every column is one material interval.
        double sourceVolumeMm3   = 0.0;
        double midplaneVolumeMm3 = 0.0;

        int   filledNodes = 0, multiLayerNodes = 0, steepTris = 0;
        float steepAreaPct = 0.0f;       // of the footprint
        int   refinePasses = 0;
        float latticeScale = 1.0f;       // < 1 when the lattice was tightened for inclined regions
    };

    struct MidplaneMesh
    {
        int         objectIndex = -1;
        std::string label;

        std::vector<glm::vec3>     nodes;               // 3D midplane positions (mm)
        std::vector<glm::ivec3>    tris;
        std::vector<float>         thicknessMm;         // per node: Hele-Shaw gap (tilt-corrected)
        std::vector<float>         verticalThicknessMm; // per node: raw column length along Y
        std::vector<unsigned char> nodeFlags;           // MidplaneNodeFlag bits
        std::vector<unsigned char> triSteep;            // per tri: 1 = flagged steep

        MidplaneStats stats;

        bool empty() const { return tris.empty(); }
    };

    // Build the planform midplane of one part. Returns stats.ok; on failure
    // stats.message says why (empty surface, no footprint, too many triangles).
    bool BuildPlanformMidplane(const PartSurface& part, const MidplaneParams& params,
                               MidplaneMesh& out);

    // The fill solver's input for a midplane (half-gap = thickness / 2). Gate
    // nodes are left empty for the caller to assign.
    SolveMesh MidplaneToSolveMesh(const MidplaneMesh& m);

    // Nearest midplane node to a world point (plan-view distance, ties broken in
    // 3D); -1 for an empty mesh. `outDistMm` receives the 3D distance.
    int NearestMidplaneNode(const MidplaneMesh& m, const glm::vec3& p, float* outDistMm = nullptr);

} // namespace Flow
