#pragma once
// ===========================================================================
// CoupledFill — one filling solve through the whole shot: sprue, runners and
// gates (the 1D FeedNetwork) plus every fed part's planform midplane (the 2D
// MidplaneMesh), as a single control-volume system.
//
// Coupling: each cavity entry of the network (a gate mouth, or a direct-
// injection sprue end) is merged with the nearest node of its part's midplane,
// so feed and cavity share that pressure. Melt is injected at the sprue inlet
// at a constant rate Q = (feed + cavity volume) / fill time.
//
// Elements
//   1D feed edge   conductance G / (eta L) (G from the edge's section), control
//                  volume A L split to its two ends.
//   2D midplane    Hele-Shaw: cotangent stiffness * h^3 / (12 eta) with h the
//                  local gap, control volume area * h / 3 to each corner.
// Viscosity is shear-thinning (the caller's eta(gammaDot), e.g. Cross-WLF at
// the melt temperature): per element at its apparent wall shear rate — the
// section's pipe/slit formula on 1D edges, h |grad p| / (2 eta) on 2D ones —
// lagged from the previous step and refined by a few Picard passes.
//
// Filling (CVFEM): each step solves the pressure on the filled control volumes
// (inlet = flow source, the melt front held at 0 gauge), reads the inflow into
// every front volume, and advances by the time the fastest `frontFractionPer
// Step` of the front needs to top out. Volume that overflows a full node spills
// into its unfilled neighbours, so mass is conserved; each node's fill time is
// interpolated inside the step.
//
// Thermal (params.thermal.enabled): every control volume carries a layered
// temperature profile through its gap (GapThermal.h) — slab layers on the
// midplanes and rectangular channels, radial layers in round ones. Each step
// the profiles are convected along the solved flow (implicit upwind, swept in
// pressure order), heated by viscous dissipation and cooled by conduction to
// the mould wall at the melt/mould contact temperature. Layers below the no-
// flow temperature are frozen and stop carrying flow, so the conductances
// come from the fluidity integrated across the gap (Cross-WLF at each layer's
// temperature and shear stress) rather than one viscosity per element. Melt
// reaching an empty volume arrives at its upstream cup-mixing temperature
// (fountain flow). Feed edges are split into short segments so the
// temperature — and freezing — varies along the sprue, runners and gates.
//
// Isothermal (thermal off): the caller's eta(gammaDot) at the apparent wall
// shear rate per element; no frozen layer, so thin, long flows read optimistic.
//
// Units: mm, Pa, s throughout (dP = eta L Q / G is in Pa with Q in mm^3/s).
// Pure compute — no wx, no GL.
// ===========================================================================

#include "FeedNetwork.h"
#include "Midplane.h"
#include "TestMaterial.h"

#include <functional>
#include <string>
#include <vector>

namespace Flow
{
    struct FillThermalParams
    {
        bool   enabled = false;
        TestMaterial::CrossWLF viscosity{};           // replaces CoupledFillParams::viscosity
        double meltTempC = 230.0;                     // at the sprue inlet (nozzle)
        double mouldTempC = 40.0;                     // mould surface set point
        double noFlowTempC = 138.0;                   // below this a layer is frozen
        double meltDensity = 750.0;                   // kg/m^3
        double meltSpecificHeat = 2900.0;             // J/(kg K)
        double meltConductivity = 0.18;               // W/(m K)
        // Mould effusivity sqrt(k rho c) [W s^0.5/(m^2 K)]: the melt sees the
        // contact temperature between melt and mould. <= 0: wall at mould temp.
        double mouldEffusivity = 0.0;
        int    layers = 10;                           // per half-gap / radius
        bool   viscousHeating = true;
    };

    struct CoupledFillParams
    {
        double fillTimeS = 1.0;                       // flow-rate controlled
        std::function<double(double)> viscosity;      // eta [Pa.s] at wall shear rate [1/s] (isothermal)
        FillThermalParams thermal;                    // gap-wise thermal model (off = isothermal)
        double feedSegmentMm = 5.0;                   // feed edges split into segments <= this (>= 2 each)
        double frontFractionPerStep = 0.3;            // share of the front topped out per step
        int    maxSteps  = 20000;
        // Viscosity passes per step (the first step always runs >= 10). One
        // lagged pass per step matches three to within 0.1% on injection pressure
        // and ~1% on fill times at a third of the cost (see the validation notes).
        int    maxPicard = 1;
        double picardTol = 0.05;                      // max relative eta change to stop
        // Beyond maxPicard, keep iterating (up to maxPicardAdaptive passes) while
        // the inlet pressure still moves by more than this fraction between
        // passes — catches abrupt flow changes (a cavity closing and diverting
        // the flow) without paying for extra passes in the smooth phases.
        double picardTolAdaptive = 0.01;
        int    maxPicardAdaptive = 8;
        // Machine pressure limit. When the inlet would exceed it the machine
        // switches to pressure control: the flow rate drops so the inlet sits at
        // the limit and the fill takes longer than the target. <= 0 = none.
        double maxInjectionPressureMPa = 0.0;
        // Pressure-limited filling slower than this multiple of the target fill
        // time is reported as a short shot (freeze-off: with the thermal model
        // the frozen layer closes the flow path and the fill stalls).
        double shortShotTimeFactor = 10.0;
        // Velocity-to-pressure switchover: the pressure field, inlet pressure and
        // clamp force are reported when this share of the shot volume is in —
        // while the front is still a full line (the industry "end of fill" read).
        double vpSwitchFraction = 0.98;
        // Called every few steps with the filled fraction (0..1); return false
        // to cancel. Optional.
        std::function<bool(double)> progress;
    };

    struct PartFillResult
    {
        int         objectIndex = -1;
        std::string label;
        bool        fed = false;                      // reached by at least one entry
        std::vector<float> fillTimeS;                 // per midplane node; < 0 = unfilled
        std::vector<float> pressureMPa;               // per midplane node, at V/P switchover
        float fillStartS = -1.0f;                     // first node reached
        float fillEndS   = -1.0f;                     // last node filled
        float filledPct  = 0.0f;                      // of the part's volume
        float maxPressureMPa = 0.0f;                  // at switchover, in this part
        std::vector<int>   entryNodes;                // midplane nodes the entries feed
        std::vector<float> entryPlanDistMm;           // entry -> that node, plan view
        // Thermal only (empty otherwise), per midplane node:
        std::vector<float> frontTempC;                // melt temperature when the front arrived
        std::vector<float> frozenPct;                 // frozen share of the gap at end of fill (0..100; < 0 unfilled)
        float minFrontTempC = 0.0f, maxFrontTempC = 0.0f;
        float maxFrozenPct = 0.0f, meanFrozenPct = 0.0f;   // mean is volume-weighted
    };

    // Per feed-network edge (same order as FeedNetwork::edges).
    struct FeedEdgeFillResult
    {
        bool  modelled = false;                       // melt-carrying and on the melt path
        float maxWallShearRate = 0.0f;                // over the fill [1/s]
        float frozenPct = -1.0f;                      // thermal: most-frozen point along it at end of fill
        float frontTempC = 0.0f;                      // thermal: melt temperature arriving at its far end
    };

    struct CoupledFillResult
    {
        bool ok = false, complete = false, shortShot = false, cancelled = false;
        bool pressureLimited = false;                 // machine limit reached (slower fill)
        std::string message;

        std::vector<PartFillResult> parts;            // one per input midplane, same order
        std::vector<float> netFillTimeS;              // per network node; < 0 = not modelled / unfilled
        std::vector<float> netPressureMPa;            // per network node, at switchover
        std::vector<float> netFrontTempC;             // thermal: per network node, melt temp on arrival
        std::vector<float> netFrozenPct;              // thermal: per network node, at end of fill (< 0 = n/a)
        std::vector<FeedEdgeFillResult> feedEdges;    // per network edge

        bool  thermal = false;
        float meltTempC = 0.0f, wallTempC = 0.0f;     // wall = melt/mould contact temperature
        float minFrontTempC = 0.0f, maxFrontTempC = 0.0f;   // over the cavities
        float maxFrozenPct = 0.0f;                    // over the cavities at end of fill
        float maxCavityShearRate = 0.0f;              // wall shear over the fill [1/s]
        // Thermal energy check: inlet pressure work over the fill vs the heat the
        // melt gained relative to the inlet temperature (equal when the wall is
        // insulating — all flow work dissipates into the melt) [J].
        float pressureWorkJ = 0.0f, heatGainJ = 0.0f;

        float flowRateMm3s = 0.0f;
        float feedVolumeMm3 = 0.0f, cavityVolumeMm3 = 0.0f;
        float fillTimeS = 0.0f;                       // time the last node filled (or stop time)
        float peakInletPressureMPa = 0.0f;            // injection pressure needed (peak up to switchover)
        float switchoverInletPressureMPa = 0.0f;      // inlet pressure at V/P switchover
        float switchoverTimeS = 0.0f;
        float clampForceTonne = 0.0f;                 // at switchover, over the projected cavity
        float projectedAreaMm2 = 0.0f;
        float massErrorPct = 0.0f;                    // stored vs injected volume
        // Inlet (injection) pressure through the fill: (time s, pressure MPa)
        // per step, plus the filled share of the shot volume at that step.
        std::vector<float> historyTimeS, historyInletMPa, historyFilledFrac;
        int   steps = 0, dofs = 0, solves = 0;
        std::vector<std::string> warnings;
    };

    CoupledFillResult SolveCoupledFill(const FeedNetwork& net,
                                       const std::vector<MidplaneMesh>& parts,
                                       const CoupledFillParams& params);

} // namespace Flow
