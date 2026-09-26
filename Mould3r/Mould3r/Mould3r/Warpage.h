#pragma once
// ===========================================================================
// Warpage — how each part deforms once it shrinks free of the mould, from the
// volumetric shrinkage the packing solve leaves (PartFillResult::shrinkPct).
//
// The part midplane is a linear-elastic shell (small displacements, room-
// temperature modulus):
//   membrane   constant-strain triangles in each facet's own plane (3D), with
//              the gap as the shell thickness;
//   bending    rotation-free "hinges" across interior edges: stiffness
//              c D |e|^2 / (A1 + A2) on the change of the dihedral angle,
//              D = E h^3 / 12(1 - nu^2) (c = 1 reproduces plate bending on
//              the midplane's near-equilateral meshes within ~6%), so only the three translations per node are
//              unknowns and curved / walled parts work as well as flat ones.
// Load: the shrinkage as an initial (free) strain. Linear shrinkage s = S_v/3
// per node, isotropic in the plane, or split along / across the local flow
// direction (the fill-time gradient) by flowShrinkRatio at the same area
// change. The part is free: the six rigid motions are held by a tiny
// regularisation and then removed by a best rigid fit.
//
// Reported per node: the deflection with rigid motion removed (shrinkage and
// warp together), and the warp — what is left after also removing the best
// uniform shrink, i.e. the change of shape — plus its component along the pull
// axis (out of the parting plane: flatness).
//
// Limits of this model: shrinkage is uniform through the thickness (the
// thermal model is symmetric, so there is no bending from a hot and a cold
// mould half), linear (no buckling — a flat part only distorts in its plane),
// and the anisotropy is a single ratio rather than a fibre / crystal
// orientation model.
// ===========================================================================

#include "CoupledFill.h"
#include "Midplane.h"

#include <glm/glm.hpp>

#include <string>
#include <vector>

namespace Flow
{
    struct WarpParams
    {
        double elasticModulusMPa = 1340.0;   // solid, room temperature
        double poissonRatio = 0.40;
        double flowShrinkRatio = 1.0;        // in-plane shrinkage along the flow / across it (1 = isotropic)
        double bendingCoefficient = 1.0;     // hinge stiffness c: matches beam / plate bending within ~6%
                                             // on the midplane's lattice meshes (see the tests)
    };

    struct PartWarp
    {
        int   part = -1;                     // index into the midplanes / fill parts
        bool  ok = false;
        std::string message;
        std::vector<glm::vec3> disp;         // per node: deflection, rigid motion removed (mm)
        std::vector<float>     dispMm;       // |disp|
        std::vector<glm::vec3> warp;         // per node: shape change (uniform shrink removed too)
        std::vector<float>     warpMm;       // |warp|
        std::vector<float>     warpPullMm;   // warp along the pull axis (y): out of the parting plane
        float maxDispMm = 0.0f, maxWarpMm = 0.0f;
        float flatnessMm = 0.0f;             // range of warpPullMm
        float uniformShrinkPct = 0.0f;       // best-fit uniform linear shrink
        glm::vec3 maxWarpPos{ 0.0f };
        glm::vec3 sizeBefore{ 0.0f }, sizeAfter{ 0.0f };   // bounding boxes (mm)
    };

    struct WarpResult
    {
        bool ok = false;
        std::string message;
        std::vector<PartWarp> parts;         // one per midplane (ok = false where not computed)
    };

    // Needs a fill with packing (fill.pack.ran: per-node shrinkPct).
    WarpResult SolveWarpage(const std::vector<MidplaneMesh>& parts, const CoupledFillResult& fill,
                            const WarpParams& params = WarpParams());

    // ---- kernel (exposed for the tests) --------------------------------------------
    // Shell stiffness (3 translations per node, row-major triplets) and the
    // shrinkage load for per-node linear shrink strains `shrink` (> 0 = the
    // material contracts) and optional per-triangle unit flow directions
    // (empty = isotropic).
    struct ShellSystem
    {
        int n = 0;                           // nodes (3n unknowns)
        std::vector<int> row, col;
        std::vector<double> val;
        std::vector<double> load;            // 3n
    };
    ShellSystem AssembleShell(const MidplaneMesh& m, const std::vector<double>& shrink,
                              const std::vector<glm::dvec3>& triFlowDir, const WarpParams& params);

} // namespace Flow
