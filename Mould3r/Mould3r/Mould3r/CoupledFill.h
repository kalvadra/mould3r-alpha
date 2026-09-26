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
        // Packing / cooling (FillPackParams): specific volume v(T, p), the
        // ejection criterion and the reference temperature for shrinkage.
        TestMaterial::TaitPVT pvt{};
        double ejectionTempC = 95.0;                  // ejectable when the whole section is below this
        double roomTempC = 23.0;                      // shrinkage is reported cooled to this, unloaded
    };

    // Packing, holding and cooling after a complete thermal fill. The melt is
    // compressible (Tait pVT): each control volume holds a mass, and while it
    // is full that mass sets its pressure through v(T, p). The machine holds
    // the pack pressure at the sprue inlet; cooling shrinks the melt and draws
    // more in through the still-molten gates until they freeze, after which
    // the cavity pressure decays as the sealed melt cools (never below zero —
    // melt that would go into tension pulls away from the wall instead). The
    // volumetric shrinkage of each volume is then simply 1 - its mass / (room-
    // temperature density x its volume).
    struct FillPackParams
    {
        bool   enabled = false;                       // needs thermal.enabled and a complete fill
        double packPressureMPa = 0.0;                 // > 0: absolute pack pressure
        double packFraction = 0.8;                    // else this share of the fill's injection pressure
        double holdTimeS = 0.0;                       // <= 0: hold until every gate has frozen (at most holdMaxS)
        double holdMaxS = 60.0;
        double maxTimeS = 180.0;                      // stop cooling this long after the fill at most
        // Time step: starts small, grows 25% a step to dtMaxS (implicit Euler
        // cooling reads ~2.5% slow at 0.25 s on a 2 mm wall; 1 s would be ~10%).
        double dtInitialS = 0.02, dtMaxS = 0.25;
        int    frames = 60;                           // animation frames over pack + cool (fill has its own)
    };

    struct CoupledFillParams
    {
        double fillTimeS = 1.0;                       // flow-rate controlled
        std::function<double(double)> viscosity;      // eta [Pa.s] at wall shear rate [1/s] (isothermal)
        FillThermalParams thermal;                    // gap-wise thermal model (off = isothermal)
        FillPackParams pack;                          // packing + cooling after the fill (thermal only)
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
        // Animation frames: snapshots of the per-node fields (pressure, melt
        // temperature, frozen layer) at roughly this many even intervals over
        // the target fill time (a fill running longer keeps recording, thinned
        // to at most 3x as many). 0 = none.
        int    animationFrames = 60;
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
        std::vector<float> bulkTempC;                 // melt (gap-mean) temperature at end of fill
        float minFrontTempC = 0.0f, maxFrontTempC = 0.0f;
        float maxFrozenPct = 0.0f, meanFrozenPct = 0.0f;   // mean is volume-weighted

        // Animation frames (see CoupledFillResult::frames), per frame per
        // midplane node. A node is filled at frame f when 0 <= fillTimeS <= the
        // frame's time; unfilled nodes hold 0 (pressure) / -1 (thermal fields).
        std::vector<std::vector<float>> framePressureMPa;
        std::vector<std::vector<float>> frameBulkTempC;    // thermal only
        std::vector<std::vector<float>> frameFrozenPct;    // thermal only

        // Packing / cooling (when it ran), per midplane node:
        std::vector<float> shrinkPct;                 // volumetric shrinkage, cooled to room temp (%)
        std::vector<float> ejectTimeS;                // when its whole section got below the ejection
                                                      // temperature, from the start of injection (< 0: not reached)
        float meanShrinkPct = 0.0f, minShrinkPct = 0.0f, maxShrinkPct = 0.0f;   // mean volume-weighted
        float ejectS = -1.0f;                         // whole part below the ejection temperature
        float massG = 0.0f;                           // part mass at ejection
    };

    // One animation frame: when it was taken and the overall state then.
    struct FillFrame
    {
        float timeS = 0.0f;
        float filledFrac = 0.0f;                      // share of the shot volume in
        float inletMPa = 0.0f;                        // injection pressure
        int   phase = 0;                              // 0 filling, 1 packing (holding), 2 cooling
    };

    struct PackResult
    {
        bool  ran = false;
        std::string message;
        float packPressureMPa = 0.0f;
        float holdTimeS = 0.0f;                       // pressure held this long after the fill
        bool  gatesFrozen = false;
        float gateFreezeS = -1.0f;                    // last gate sealed (from the start of injection)
        bool  ejectReached = false;
        float ejectS = -1.0f;                         // every part below the ejection temperature
        float endS = 0.0f;                            // simulation stopped here
        float partsMassG = 0.0f;                      // all parts at ejection
        float packedMassG = 0.0f;                     // of which added after the fill
        float maxStateErrPct = 0.0f;                  // |mass - mass(T, p)| of full volumes (solver check)
        float massBalanceErrPct = 0.0f;               // mass gained vs mass through the inlet (solver check)
        std::vector<float> historyTimeS, historyInletMPa, historyCavityMaxMPa;
        int   steps = 0, solves = 0;
    };

    // Per feed-network edge (same order as FeedNetwork::edges).
    struct FeedEdgeFillResult
    {
        bool  modelled = false;                       // melt-carrying and on the melt path
        float maxWallShearRate = 0.0f;                // over the fill [1/s]
        float frozenPct = -1.0f;                      // thermal: most-frozen point along it at end of fill
        float frontTempC = 0.0f;                      // thermal: melt temperature arriving at its far end
        float freezeS = -1.0f;                        // packing: sealed (a fully frozen section) at, from injection start
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
        // Animation frames in time order (per-node values live in each
        // PartFillResult's frame* arrays, same indexing). The end-of-fill state
        // is the result's own fields, not a frame.
        std::vector<FillFrame> frames;
        PackResult pack;                              // packing + cooling (params.pack.enabled)
        int   steps = 0, dofs = 0, solves = 0;
        std::vector<std::string> warnings;
    };

    CoupledFillResult SolveCoupledFill(const FeedNetwork& net,
                                       const std::vector<MidplaneMesh>& parts,
                                       const CoupledFillParams& params);

} // namespace Flow
