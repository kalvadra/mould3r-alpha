#pragma once
// ===========================================================================
// Flow3DFill — the 3D filling solve (Phase 3: isothermal).
//
// The melt is a generalized-Newtonian Stokes fluid (inertia-free, viscosity
// from the shear rate at the melt temperature) on the tetrahedral mesh:
//
//   - Velocity and pressure are both linear per tet (P1-P1), stabilized with
//     PSPG (Brezzi-Pitkaranta) so equal-order pressure is stable.
//   - Only the melt is solved: the active domain is every tet touching a
//     filled node. Nodes not yet filled (the flow front) hold p = 0 — the air
//     ahead of the melt, vented. Walls are no-slip.
//   - The melt enters through the boundary's inlet regions. Each inlet region
//     is one pressure unknown coupled to the melt by the flux through its
//     faces, and to a 1D feed network (the sprue / runners / gates as beam
//     elements, cavity-only meshes) or directly to the machine (full-shot
//     meshes, where the inlet is the sprue entry). The machine end delivers
//     the injection rate until the pressure limit is reached, then holds the
//     pressure limit.
//   - The front advances by control-volume filling: each node owns a quarter
//     of every tet around it; the fluxes into the front nodes (taken from the
//     same discrete continuity equation the solve satisfies, so they add up
//     to exactly the inflow) fill their volumes over each time step.
//
// The linear system per step is a saddle point (velocity | pressure +
// network), solved by FGMRES with AMGCL's Schur pressure-correction
// preconditioner (AMG on the velocity block).
//
// Thermal (Phase 4, FillSetup::thermal): the melt temperature is carried
// on the same nodes, advected with the melt (upwind, through the same
// conservative fluxes), conducted between filled nodes and heated by shear.
// The walls are handled by a sub-grid model, because at a few elements
// across a wall the thermal boundary layer (a few tenths of a mm during the
// fill) is much thinner than an element: each wall node loses heat as a
// semi-infinite melt against the wall's contact temperature since the melt
// arrived there (flux k dT / sqrt(pi alpha t)), and its frozen skin grows
// by the same solution (depth where the erf profile crosses the no-flow
// temperature). The skin thickens the melt in the tets along the wall
// (their viscosity scaled by the conductance they lose); melt below the
// no-flow temperature is frozen. With the machine at its pressure limit a
// fill that stalls is a short shot.
//
// Units: mm, s, Pa (so mm^3/s, Pa.s); results in MPa where marked.
// Pure compute: no wx / GL. Runs on a worker thread; `progress` is called
// from it.
// ===========================================================================

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "FeedNetwork.h"      // Flow::FeedSection (beam sections)
#include "TestMaterial.h"     // TestMaterial::CrossWLF (thermal viscosity)
#include "TetMeshJob.h"
#include "VolumeBoundary.h"

namespace Flow3D
{
    // A melt-carrying beam of the 1D feed network (cavity-only meshes).
    struct FeedBeam
    {
        int a = -1, b = -1;               // network node indices
        double lengthMm = 0.0;
        Flow::FeedSection section;        // conductance + shear-rate shape
        std::string label;
    };

    struct FillSetup
    {
        // 1D network: nodes 0..netNodes-1. `sourceNode` is the machine nozzle.
        // Every inlet region of the Boundary maps to a network node
        // (regionNetNode[region], -1 for vents); a full-shot mesh has a
        // one-node network whose node is both the source and the inlet.
        int                   netNodes = 0;
        std::vector<FeedBeam> beams;
        int                   sourceNode = -1;
        std::vector<int>      regionNetNode;
        std::vector<std::string> netNodeLabels;   // for reports (optional)

        // Melt viscosity [Pa.s] at a shear rate [1/s] (melt temperature fixed).
        std::function<double(double)> viscosity;

        double flowRateMm3s = 1000.0;     // injection rate (volume / fill time)
        double maxPressureMPa = 150.0;    // machine limit: then pressure control

        int    targetSteps = 120;         // ~ time steps over the fill
        int    snapshots = 60;            // pressure frames kept for playback
        double pspgAlpha = 1.0 / 12.0;    // PSPG tau = alpha h^2 / eta
        double solverTolerance = 1.0e-6;  // FGMRES relative residual
        int    solverMaxIterations = 400;

        // Thermal fill (off = isothermal at the viscosity function above).
        struct Thermal
        {
            bool   enabled = false;
            TestMaterial::CrossWLF viscosity{};   // eta(gammaDot, T)
            double meltK = 503.15;                // melt entering the inlets
            double wallK = 323.15;                // mould contact temperature
            double noFlowK = 411.15;              // frozen below this
            double rhoCp = 2.2e6;                 // melt rho * c_p [J/(m^3 K)]
            double k = 0.18;                      // melt conductivity [W/(m K)]
        } thermal;

        // Packing, holding and cooling after a complete thermal fill (as the
        // 2.5D: Tait pvT melt, each node's mass conserved; while a node is full
        // its mass sets its pressure). The machine holds the pack pressure at
        // its nozzle; cooling draws more melt in through the gates until they
        // freeze; after the hold the nozzle drops to zero and the sealed melt
        // cools to the ejection temperature (a node that would go into tension
        // pulls away from the wall at zero pressure instead).
        struct Pack
        {
            bool   enabled = false;           // needs thermal and a complete fill
            double packFraction = 0.8;        // pack pressure = this x the fill's peak machine pressure
            double packPressureMPa = 0.0;     // > 0: use this instead
            double holdTimeS = 0.0;           // <= 0: hold until the gates freeze (at most holdMaxS)
            double holdMaxS = 60.0;
            double maxTimeS = 180.0;          // stop cooling this long after the fill at most
            double dtInitialS = 0.02, dtMaxS = 0.25;
            TestMaterial::TaitPVT pvt{};
            double ejectionTempC = 95.0;      // ejectable when every node is below this
            double roomTempC = 23.0;          // shrinkage is reported cooled to this, unloaded
            // Gates of the 1D feed (cavity-only meshes), per network node at a
            // gate mouth: the gate's radius, for its freezing (a cylinder of
            // melt cooling to the wall); 0 = not a gate.
            std::vector<double> gateRadiusMm;   // per network node
            int    frames = 50;               // playback frames over pack + cool
        } pack;

        // Called after each step with the progress (0..1) and the time;
        // return false to cancel.
        std::function<bool(double, double)> progress;
    };

    struct FillFrame
    {
        float timeS = 0.0f;
        float filledPct = 0.0f;
        float inletMPa = 0.0f;       // at the machine nozzle
        float flowMm3s = 0.0f;       // delivered
        int   snapshot = -1;         // index into framePressureMPa, -1 = none
        int   phase = 0;             // 0 filling, 1 packing (holding), 2 cooling
    };

    struct FillStats
    {
        int    steps = 0, solves = 0;
        double meanIterations = 0.0;
        int    maxIterations = 0;
        double worstResidual = 0.0;
        double solveSeconds = 0.0, totalSeconds = 0.0, thermalSeconds = 0.0;
        double backflowPct = 0.0;    // share of front flux that pointed backwards (dropped)
        size_t maxUnknowns = 0;
    };

    struct FillResult
    {
        bool ok = false;
        bool cancelled = false;
        bool pressureLimited = false;   // switched to pressure control at some point
        bool incomplete = false;        // some of the cavity was never reached
        bool thermal = false;           // ran with the thermal model
        bool stalled = false;           // the flow stopped (frozen off) before filling
        std::string message;
        std::vector<std::string> warnings;

        // Per mesh node.
        std::vector<float> fillTimeS;       // -1 = never filled
        std::vector<float> endPressureMPa;  // at the end of fill (-1 = not filled)
        std::vector<float> endSpeedMmS;     // melt speed at the end of fill
        std::vector<float> tetShearRate;    // per tet, end of fill [1/s] (0 outside)
        // Thermal runs (empty otherwise), per node.
        std::vector<float> endTempC;        // bulk melt temperature at the end (-1000 = not filled)
        std::vector<float> frontTempC;      // temperature of the melt as it arrived (-1000 = never)
        std::vector<float> endFrozenMm;     // frozen skin at the end (see frameFrozenMm)
        static constexpr float kNotFilledC = -1000.0f;
        static constexpr float kFrozenThrough = 1.0e6f;

        // Playback: every step's frame; pressure snapshots (per node, MPa, -1
        // where not filled) for some of them.
        std::vector<FillFrame> frames;
        std::vector<std::vector<float>> framePressureMPa;
        // Thermal runs, same snapshots: bulk melt temperature (degC, -1000
        // where not filled) and frozen skin (mm at wall nodes; kFrozenThrough
        // where the melt itself is below the no-flow temperature; 0 elsewhere).
        std::vector<std::vector<float>> frameTempC;
        std::vector<std::vector<float>> frameFrozenMm;

        // Per inlet region: flow at the end of fill and its share.
        std::vector<float> regionFlowMm3s;
        // Per network node: pressure at the end of fill (MPa).
        std::vector<float> netPressureMPa;

        double endTimeS = 0.0;          // time the last node filled
        double maxInletMPa = 0.0;
        double cavityVolumeMm3 = 0.0;
        double filledVolumeMm3 = 0.0;
        double maxShearRate = 0.0;
        double minFrontTempC = 0.0, maxTempC = 0.0;   // thermal: coldest arriving melt, hottest melt
        double maxFrozenMm = 0.0;                      // thermal: thickest frozen skin at a wall
        double wallTempC = 0.0;                        // thermal: the contact temperature used

        // Packing / cooling (pack.enabled and the fill completed).
        struct PackResult
        {
            bool   ran = false;
            std::string message;
            double packPressureMPa = 0.0;
            double holdTimeS = 0.0;            // pressure held this long after the fill
            bool   gatesFrozen = false;
            double gateFreezeS = -1.0;         // melt stopped entering (gates sealed), from injection start
            bool   ejectReached = false;
            double ejectS = -1.0;              // every node below the ejection temperature
            double endS = 0.0;                 // stopped here
            double massG = 0.0;                // the meshed volume's mass at the end
            double packedMassG = 0.0;          // of which added after the fill
            double meanShrinkPct = 0.0, minShrinkPct = 0.0, maxShrinkPct = 0.0;   // volume-weighted mean
            double massBalanceErrPct = 0.0;    // mass gained vs mass in through the inlets (solver check)
            int    steps = 0, solves = 0;
            std::vector<float> shrinkPct;      // per node: volumetric shrinkage cooled to room temperature (%)
            std::vector<float> ejectTimeS;     // per node: below the ejection temperature at (from injection start; < 0 never)
            std::vector<float> gateFreezePerRegion;   // per boundary region (inlets): sealed at (< 0 never)
        } pack;
        FillStats stats;
    };

    FillResult RunIsothermalFill(const TetMesh::Mesh& mesh, const std::vector<int32_t>& neighbours,
                                 const Boundary& boundary, const FillSetup& setup);
}
