#pragma once
// ===========================================================================
// FlowSolver — the Hele-Shaw 2.5D filling solve (P2: isothermal Newtonian).
//
// Pure compute: no wx, no GL, no app types (like DesignChecks / MeshBoolean /
// FlowMesh). It takes a mid-surface triangle mesh with a per-node gap, a gate,
// and process settings, and returns per-node fill time + pressure plus summary
// scalars. Eigen (the sparse SPD solve) is an implementation detail of the .cpp;
// this header stays dependency-light so the rest of the app need not see Eigen.
//
// Model (P2): incompressible creeping flow in a thin cavity,
//     div( S grad p ) = 0,   S = h^3 / (12 eta)      (Newtonian, isothermal)
// with h the local full gap (= 2 * half-gap) and eta a constant viscosity
// (evaluate the Cross-WLF model at the melt temperature once, upstream). Filling
// is a CVFEM fill-factor march: each pseudo-step solves the pressure field on
// the currently-filled control volumes with the gate as a flow-rate source and
// the melt front held at ambient (p = 0), then advances the front. Flow-rate
// (fill-time) controlled injection — Decision D3.
//
// Cross-WLF, gap-wise thermal, the frozen layer, the 1D feed network and vent
// BCs arrive in P3-P5; this phase is the validatable core (analytic
// centre-gated disk).
// ===========================================================================

#include <glm/glm.hpp>
#include <vector>
#include <string>

namespace Flow
{
    // The mid-surface solve mesh: a welded (shared-node) triangle mesh with one
    // gap value per node. Positions are in millimetres (the solver converts to
    // SI internally). Built from the FlowMesh's dual-domain thickness field by a
    // separate mesh step; the solver is agnostic to how it was produced.
    struct SolveMesh
    {
        std::vector<glm::vec3>  nodes;     // node positions (mm)
        std::vector<glm::ivec3> tris;      // node indices per triangle (welded)
        std::vector<float>      halfGapMm; // per node: half the local wall thickness
        std::vector<int>        gateNodes; // injection node(s); melt enters here

        bool empty() const { return nodes.empty() || tris.empty(); }
    };

    struct FlowSolverParams
    {
        double viscosityPaS = 8000.0;   // constant Newtonian viscosity (Pa.s)
        double fillTimeS     = 1.0;      // target fill time -> inlet flow rate
        double maxPressureMPa = 0.0;     // machine limit; <=0 disables (no short-shot cut)
        int    maxSteps      = 500000;   // safety cap on the fill march
    };

    struct FlowResult
    {
        bool ok = false;
        std::string message;

        // Per node (indexed like SolveMesh::nodes).
        std::vector<float> fillTimeS;    // when the node filled; <0 = never (short shot)
        std::vector<float> pressureMPa;  // gauge pressure at end of fill

        // Summary scalars.
        float fillTimeTotalS = 0.0f;     // last node's fill time
        float maxPressureMPa = 0.0f;     // peak (at the gate, end of fill)
        float clampForceTonne = 0.0f;    // integral(p dA) over the projected area
        float cavityVolumeMm3 = 0.0f;
        float projectedAreaMm2 = 0.0f;
        int   totalNodes = 0;
        int   filledNodes = 0;
        float shortShotPct = 0.0f;       // 100 * (1 - filled/total volume)
        int   steps = 0;                 // fill-march iterations taken
    };

    // Solve an isothermal Newtonian fill. Returns ok=false (with `message`) for
    // an empty/degenerate mesh, no gate, or a solver breakdown. The projected
    // area used for the clamp-force estimate is taken normal to `projectAxis`
    // (default +Y, the mould's pull axis).
    FlowResult SolveIsothermalFill(
        const SolveMesh& mesh,
        const FlowSolverParams& params,
        const glm::vec3& projectAxis = glm::vec3(0.0f, 1.0f, 0.0f));

} // namespace Flow
