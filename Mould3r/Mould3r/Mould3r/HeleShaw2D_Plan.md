# Hele-Shaw 2.5D Flow — implementation plan

A plan for a new **"Hele-Shaw 2.5D Flow"** simulation in the Preview perspective:
model the melt filling the cavity, starting at the **sprue entry**, through the
**runners**, **gates**, into the **cavity**, with **vents** as pressure-relief
(outflow) boundaries. This is a design doc, not code — it sets the architecture,
phases, and the decisions to lock before we build.

---

## 1. Modeling approach

Injection filling is a thin-cavity, pressure-driven creeping flow. The 2.5D
Hele-Shaw model is the standard reduction: solve a **2D pressure field over the
mid-surface** with a gap-wise (through-thickness) treatment of viscosity and
temperature, rather than a full 3D Navier–Stokes solve.

Governing equation over the cavity mid-surface (Ω):

```
∇·( S ∇p ) = 0        (mass conservation, creeping flow)

S(x,y) = ∫_0^{b} (z² / η(γ̇,T,z)) dz      "flow conductance" over the half-gap b
```

- `p` — pressure field (the unknown we solve for).
- `b` — local half-gap (half the wall thickness).
- `η` — melt viscosity from **Cross-WLF** (`TestMaterial::CrossWLF`), a function
  of shear rate, temperature, and pressure. Temperature varies through the gap,
  so `S` is a gap-wise integral, recomputed as the temperature field evolves.
- Gap-wise velocity is recovered from `∇p` and `η(z)`; the **frozen layer**
  (where `T < noFlowTemp`) contributes ~zero to `S`, so as the part skins over,
  `S` drops and pressure climbs — the physically important coupling.

### Cavity representation: **Dual-domain (recommended)** vs true mid-plane

The hard part of any Hele-Shaw code is getting a mid-surface + a thickness field
from a solid. Two options:

- **True mid-plane extraction** — collapse the solid to a single mid-surface mesh
  with a thickness attribute. Cleanest math, but robust mid-surface extraction
  (medial-axis-like) is a research-grade problem on its own.
- **Dual-domain (Moldflow's approach)** — solve on the **boundary (surface)
  mesh** directly, pairing each surface facet with the facet on the opposite wall
  to recover the local thickness; the two matched walls carry a shared pressure.
  Less exact on thick/chunky geometry, but robust and mesh-cheap.

**Recommendation: dual-domain**, because we already compute most of the hard
inputs it needs:

- The Draft Angle Checks already **split the shot at the parting plane and assign
  each facet an owning half** (`m_faceDraftSamples`, `BuildFaceDraftSamples`).
  Opposite-wall pairing is essentially "for each A-side facet, find the B-side
  facet along the wall normal" — the same ray-into-the-other-side machinery.
- `SplitMeshByPlane` + the ownership pass are the natural front end for building
  the flow mesh.

We keep the mid-plane path in mind as a later upgrade for chunky parts, but
dual-domain gets us a validated solver fastest with maximum reuse.

---

## 2. Domain build (the groundwork — the hardest ~40% of the work)

### 2a. Cavity flow mesh + thickness field
- Start from the shot/part **surface mesh** (already retained: `m_shotMesh`,
  and the split/ownership mesh `m_faceDraftPosNorm/Idx` + `m_faceDraftSamples`).
- For each facet, find the **opposing wall** (nearest facet whose normal is
  roughly anti-parallel, hit by a short ray along −normal). Distance = local
  **thickness**; store half-gap `b` per facet/node. Reuse the `TriGrid`
  accelerator + `RayNearestHit` already in `DesignChecks.cpp`.
- Pair matched facets so both walls share one pressure DOF (dual-domain). Nodes
  without a valid pair (edges, ribs, thick blobs) get flagged — a quality metric
  we surface, not a hard failure.
- Output: a **FlowMesh** — nodes with (position, half-gap, wall pairing), and
  triangles for the 2D pressure discretization.

### 2b. Feed system as a 1D channel network
The sprue/runner/gate features already exist with centerline paths and
cross-sections — no new geometry authoring needed:

- **Sprue** (`SprueFeature`) — the **injection entry**; its start node is the
  boundary condition inlet (velocity- or pressure-controlled).
- **Runners** (`RunnerFeature.path`) — sample the centerline with `SamplePath()`
  → a chain of **1D channel elements**, each with a hydraulic radius/area from
  the runner cross-section (`runnerRadius`, box vs tube).
- **Gates** (`GateFeature`, `subPath`) — short 1D elements connecting the feed
  network to the cavity at the **gate node(s)** on the FlowMesh.
- Each 1D element gets a **channel conductance** (Hagen–Poiseuille form for
  round/rectangular sections, same Cross-WLF η, temperature-coupled) assembled
  into the same global linear system as the 2D cavity — one coupled matrix.

### 2c. Vents → pressure-relief boundaries
- **Vents** (`VentInstance` / `VentPath`) mark where trapped air escapes. At vent
  locations, impose an **outflow BC** (p = ambient / 0 gauge) so the melt front
  can reach and push air out there without pressurizing.
- Regions that fill **last and are not adjacent to a vent** → **air-trap**
  candidates (a key output).

### 2d. Connectivity graph
`sprue start → runner elements → gate elements → cavity gate nodes → 2D field
→ vent outflow nodes`. One assembled system spanning 1D feed + 2D cavity DOFs.

---

## 3. Physics / solver

- **Pressure solve** — assemble `∇·(S∇p)=0` over the FlowMesh (CVFEM or linear
  FEM) + the 1D channel conductances → sparse SPD system → solve for `p`.
  Needs a **sparse linear solver** (see Decision D2).
- **Flow-front advance (filling)** — **Control-Volume FEM** with a per-node
  **fill factor** (0 = empty, 1 = full). Each pseudo-timestep: solve pressure on
  the currently-filled + front control volumes, compute nodal flow rates from
  `S∇p`, advance fill factors, march the front. Inlet is either **flow-rate
  controlled** (fill time / injection rate) or **pressure controlled** (machine
  max) — see Decision D3.
- **Thermal** — gap-wise 1D transient conduction across the thickness at each
  node: melt convection in-plane, conduction to the wall, **viscous (shear)
  heating** `η γ̇²`. Wall BC uses the **mould material effusivity**
  (`TestMaterial::MouldMaterial`). The **no-flow temperature** grows the frozen
  layer, which feeds back into `S` each step. Start with a **lumped/analytic
  frozen-layer model** (Stefan-like) for the MVP; upgrade to a few gap-wise
  nodes later.
- **Viscosity coupling** — `S` depends on `T` and `γ̇`, which depend on `p`;
  iterate a couple of Picard passes per step (cheap, converges fast for filling).

---

## 4. Inputs

- **Materials** — from `TestMaterial.h` via the Physical Setup dropdowns:
  Cross-WLF, ρ/c_p/k, no-flow temp (polymer); ρ/c_p/k → effusivity (mould).
- **Process** — melt temp, mould temp (default from the polymer's recommended
  values, editable), and **injection control**: fill time OR flow rate OR max
  pressure. These live in the sim card (and/or the Physical Setup bar later).
- **Geometry** — shot mesh (cavity), sprue/runner/gate features, vents. All
  already produced by the mould generation.

---

## 5. Outputs & visualization

Fields computed per node/facet, shown by reusing the existing debug-overlay path
(`SetShotDebugMesh` with a **scalar → colour ramp**; add a **time slider** for
animation, and a legend):

- **Fill-time contours** — when each region fills (the headline result).
- **Flow-front animation** — the melt front over time.
- **Pressure field** at end of fill; **max injection pressure**.
- **Weld/knit lines** — where separate flow fronts meet (front-collision nodes).
- **Air traps** — last-fill regions not adjacent to a vent.
- **Short shot** — regions unfilled when the inlet limit is hit.
- **Est. clamp tonnage** — ∫p dA over the projected area (a headline number).
- Results card: fill time, max pressure, clamp force, weld-line / air-trap counts,
  short-shot %.

---

## 6. Code structure (UI-agnostic core, like DesignChecks / MeshBoolean)

- `FlowMesh.h/.cpp` — build the cavity flow mesh + thickness pairing + 1D feed
  network from the shot mesh and feed features. (Depends on the `TriGrid`/split
  machinery; may factor those out of `DesignChecks.cpp` into a small shared
  geometry header.)
- `FlowSolver.h/.cpp` — the CVFEM fill + pressure + thermal solve. Pure compute,
  no wx/GL. Takes FlowMesh + `TestMaterial` + process params, returns a
  `FlowResult` (per-node fields + time series + summary scalars).
- `PreviewPanel` — new **"Hele-Shaw 2.5D Flow"** sim card (process fields +
  Start), a Results verdict/summary, a new **Debug View** result mode (fill time
  / pressure / flow front) with the time slider, and a worker-thread run with a
  progress dialog (the solve is heavy — mirror the old remesh worker pattern).
- Materials flow in from the Physical Setup dropdowns → `TestMaterial` constants.

---

## 7. Phasing (each phase builds + is demoable)

- **P0 — Scaffold (small, buildable now).** New sim card + Run stub + process
  fields (fill time, melt/mould temp) reading the Physical Setup materials +
  Results card placeholder + a "Flow" debug-view mode stub. No physics yet.
- **P1 — Flow mesh + thickness.** Build the cavity FlowMesh with the dual-domain
  thickness field; visualise thickness as a heat map. Validates the geometry
  front end before any solve.
- **P2 — Isothermal Newtonian fill.** Constant η, cavity only, single gate.
  CVFEM fill-factor march → fill-time + pressure + flow-front animation.
  **Validate** against the analytic centre-gated disk (radial fill pressure).
- **P3 — Cross-WLF + thermal.** Add shear/temperature-dependent viscosity and
  the gap-wise thermal + frozen-layer coupling. Validate viscous-heating and
  skin growth trends; spiral-flow length sanity check.
- **P4 — Feed network.** 1D sprue/runner/gate elements coupled to the cavity;
  injection begins at the sprue start. Multi-gate.
- **P5 — Vents + defect detection.** Vent outflow BCs, air-trap detection, weld
  lines, clamp-force estimate.
- **P6 — Packing (later).** Use the Tait pvT for the pack/hold phase + shrinkage.

Validation harness runs through P2–P3 (analytic disk, mass conservation, spiral
flow) so we can trust results before wiring defect outputs.

---

## 8. Decisions to lock before P1

- **D1 — Cavity model:** dual-domain (recommended, max reuse) vs true mid-plane.
- **D2 — Linear solver dependency:** the pressure solve needs a sparse solver.
  Add **Eigen** (header-only, easy via vcpkg) or hand-roll a
  preconditioned-CG? Eigen recommended.
- **D3 — Injection control for the MVP:** fill-time / flow-rate controlled
  (simplest, recommended) vs pressure-controlled vs a velocity→pressure switch.
- **D4 — Thermal fidelity for the MVP:** lumped analytic frozen layer
  (recommended for P3) vs a few gap-wise thermal nodes from the start.
- **D5 — Threading/perf:** confirm the solve runs on a worker thread with a
  progress + cancel dialog (assumed yes).

---

## 9. Reuse summary (why this is tractable here)

- Parting split + per-facet half **ownership** → opposite-wall pairing / thickness.
- `TriGrid` + `RayNearestHit` → the pairing ray casts.
- Feed features (`SprueFeature`, `RunnerFeature.path`, `GateFeature`, vents) +
  `SamplePath()` → the 1D network with cross-sections, no new authoring.
- `TestMaterial.h` → Cross-WLF + thermal + no-flow already in solver-ready shape.
- `SetShotDebugMesh` scalar overlay + Physical Setup bar → results viz + inputs.

---

## 10. Amendment — feed network + lumped parts (Sep 2026)

Running P2 on the **shot** mesh showed two problems: the shot includes the
sprue / runner / gate solids, so the mid-surface collapse mangled the feed
system into the cavity, and the display mesh is too coarse to pair walls
reliably (parts of the cavity dropped out). The approach is amended:

- **Feed system = 1D nodal network** (`FeedNetwork.h/.cpp`), snapshotted by
  `GLCanvas::BuildFeedNetwork()` at **Generate Mould** and handed to Preview via
  `ShotPreviewInput::feedNetwork`. Feature path ends are nodes (runners split
  where a gate attaches part-way); each edge carries its path length and the
  feature's specified section (sprue inlet dia, runner dia, gate orifice dia,
  sub-runner dia, vent width x depth) assumed over the whole edge. Gates are two
  edges (frustum @ orifice dia, then sub-runner), mirroring RebuildGateSolids.
- **Each moulded object is one PART node** (feed on one side via gate mouths /
  direct-injection sprue, vents on the other). Vents are air-side only.
- **Run** now solves the steady feed network: inlet flow = shot volume / fill
  time, gate mouths at 0 gauge, Cross-WLF viscosity at melt temp iterated per
  edge (validated: rect-duct G vs Shah & London, analytic power-law parallel
  split). Reports feed pressure drop + melt split per gate / part.
- **Debug View "Flow network"** draws the node tree on top of the scene.
- The shot-mesh fill-time / pressure views are retired from the UI.
  `FlowSolver` (disk-validated) and `FlowMesh`/`BuildSolveMesh` are kept as the
  library for the next step.

**Next:** build each part's cavity mesh from its **source geometry** (not the
shot), remeshed finer, then the dual-domain / mid-surface representation of
just the cavity; couple it to the network at the gate-mouth nodes and run the
fill through feed + cavity together.

---

## 11. Part midplane — planform (pull-axis) approach (Sep 2026)

A general midplane (medial surface / face-pair midsurfacing) is not reliably
extractable for arbitrary solids, and a single y-slice only fits flat parts.
Mould3r always pulls along Y, so the cavity is modelled in **plan view**
(`Midplane.h/.cpp`, `Flow::BuildPlanformMidplane`):

1. **Footprint** — every triangle of the part's own surface (world space,
   `GLCanvas::BuildPartSurfaces` at Generate Mould — not the shot) projected
   onto XZ, winding-normalised, and unioned with Clipper2 (chunked spatial
   batches merged as a tree: 640k triangles in ~1 s vs ~18 s for one union).
2. **Regular 2D mesh at the target triangle area** — outline resampled at the
   lattice spacing with corners kept, equilateral interior lattice, constrained
   Delaunay (CDT, header-only), safe Laplacian smoothing. If inclined regions
   make the midplane larger than its plan view, the lattice is tightened once
   (90th-percentile area ratio); remaining over-target facets get centroid
   insertion (the sqrt(3)-subdivision on a lattice, so it stays equilateral).
   Every non-wall facet ends at or below the target area on the actual
   midplane surface.
3. **Thickness + midplane height** — a Y column at each node through the part
   surface: the material interval gives the vertical thickness, its middle the
   midplane height. The Hele-Shaw gap is corrected for inclination
   (h * mean |n_y| of the two walls) — exact for uniformly thick inclined sheets.
4. **Flags** — facets that are steep AND jump by a large share of the column
   (tall walls along the pull, where planform is not valid) are marked; thickness
   steps are not. Multi-interval columns (undercuts) and missed columns are
   counted. A volume check compares the integral of column length with the part's
   own closed-surface volume.

Straight-pull (draft-passing) parts have exactly one material interval per
column, so the planform midplane is well defined everywhere except tall walls.

UI: "Mesh tri area" on the Hele-Shaw card; Debug View **"Flow midplane"** (gap
heat map, wall facets muted red, network on top); the Run report lists per-part
mesh stats, the volume check and where each gate mouth lands on its midplane.

Validated (synthetic parts): plate / stepped plate with hole / disk give exact
gaps and footprints with volume error <= 0.12%; a 30 deg inclined plate gives
the true gap and area ratio 1/cos 30; walls of an open tray are flagged;
mean regularity 0.95-0.99, min angle >= 23 deg, every facet <= target.

**Next:** couple the midplane to the network at the gate-mouth nodes
(`MidplaneToSolveMesh` + `NearestMidplaneNode`) and run the disk-validated fill
solver through feed + cavity together.

## 12. Coupled fill — feed network + part midplanes (Sep 2026)

`CoupledFill.h/.cpp` — `Flow::SolveCoupledFill(network, midplanes, params)`.

**One system.** 1D feed edges (conductance G = πR⁴/8 or the exact rectangle
series, ΔP = ηLQ/G) and 2D midplane triangles (cotangent stiffness with
S = h³/12η) are assembled into one sparse matrix. Each gate mouth
(`CavityIn` edge) is merged with its nearest midplane node
(`NearestMidplaneNode`), so the gate feeds the cavity directly. Parts the melt
can't reach are reported and left out.

**March.** CVFEM fill factors over feed nodes (A·L/2 per end) and midplane
nodes (area·h/3). Each step solves filled nodes with the front at p = 0 and a
constant flow rate Q = (feed + cavity volume) / fill time at the sprue inlet,
then advances by the time for ~30% of the front to fill. Overflow is spilled
to the nearest unfilled nodes, and inflow to the front is kept positive
(obtuse triangles), so mass is conserved to ~1e-4%.

**Viscosity.** Cross-WLF at melt temperature per element (feed: apparent wall
shear 32Q/πD³ or 6Q/wh²; midplane: h|∇p|/2η), one Picard pass per step with
more while the inlet pressure is still moving by >1%; newly wetted elements
start from their neighbours' viscosity.

**Outputs.** Fill time and pressure per node (feed + midplane); injection
pressure = the inlet peak up to the V/P switchover at 98% of the shot; clamp
force = switchover pressure over the projected area of the midplanes; per-part
arrival / full time / max pressure and balance spread (>5% flagged); inlet
pressure history. If a machine limit is set, the fill switches to pressure
control; filling past 10× the target time is a short shot. Cancelable
progress callback.

**Preview.** Run shows a progress dialog, then the report (fill section,
verdict "FILL t s p MPa", amber when unbalanced) and opens the Debug View
"Flow fill time"; "Flow pressure" shows the switchover pressure. Unfilled
regions are grey.

Validated (synthetic): centre-gated disk vs the analytic log law (exact, R²
1.00000) and r² fill-front law; balanced and unbalanced two-part layouts
(unbalanced finish spread matches a fully converged run); edge-gated plate;
pressure limit → pressure control and short shot; cancel; 23k triangles in
~5 s (Release build faster).

Isothermal: no frozen layer, so thin or long flows read optimistic.

**Next:** P3 thermal — per-node temperature through the gap, frozen-layer
growth reducing the effective gap h, and viscosity at the local temperature
(this is where mould material and mould temperature start to matter). Then
fill-front animation (time slider over `fillTimeS`), weld lines and air traps
at the vents, and a machine-pressure-limit field in the Flow card.

## 13. P3 — thermal fill: gap-wise temperature + frozen layer (Sep 2026)

Decision D4 resolved in favour of **gap-wise layers** (not the lumped analytic
frozen layer): 10 layers per half-gap / radius, graded finer at the wall. The
fill solver's cost left room for it, and it captures what a lumped model can't:
hot melt keeping the frozen skin thin near a gate, shear heating in gates, and
gates / thin sections freezing off.

`GapThermal.h/.cpp` — the kernels, each validated on its own:
- `LayerGrid` (slab for midplanes / rectangular channels, cylinder for round
  sprue / runners / gates), `MeltViscosity` (Cross-WLF with ln η0(T) tabulated,
  fluidity floor below the no-flow temperature), `FluidityIntegral`
  (S = 2∫z²/η dz, C = (π/2)∫r³/η dr — exact h³/12η, πR⁴/8η for uniform η),
  `LayerFlowFractions` (the velocity profile per layer),
  `ConductStep` / `ThermalSweep` (below), `FrozenFraction`,
  `ContactTemperature` (wall = melt/mould contact temperature from the two
  effusivities — this is where the mould material enters).

`CoupledFill` with `params.thermal.enabled`:
- Each control volume carries a layered temperature profile. Feed edges are
  split into ≤ 5 mm segments (≥ 2 each), so temperature and freezing vary along
  the sprue / runners / gates and every gate has its own thermal volume.
- Conductances come from the fluidity integrated across the gap: per layer
  η(γ̇, T) with the lagged layer shear rate (flow-rate control keeps γ̇ ≈ fixed,
  so the Picard lag converges like the isothermal one); frozen layers carry
  (almost) nothing.
- Each step, after the pressure solve: viscous heating τ²/η per layer from the
  field that solve used; then **one implicit sweep in descending pressure**:
  per volume a tridiagonal solve of conduction to the wall + heating +
  in-plane convection in **conservative form with cross-layer exchange** (where
  a layer's in- and outflow differ because the profile changes along the flow,
  the difference crosses to the neighbouring layer, upwinded). Each volume's
  outflow is balanced to its inflow (obtuse-corner couplings otherwise leave
  small mismatches that would pump energy in proportion to absolute
  temperature).
- Melt reaching an empty volume arrives at the upstream cup-mixing
  temperature, uniform through the gap (fountain flow).
- Freeze-off: when the frozen layer chokes the path the fill goes pressure-
  limited, slows, and stops at 10x the target time as a short shot (the step
  is capped there; no runaway step). Without a machine limit a 400 MPa safety
  ceiling applies (with a warning).

Outputs: per node melt-front temperature and frozen share of the gap at end of
fill; per feed edge max wall shear rate, frozen share and arrival temperature;
cavity min/max front temperature, max frozen, max wall shear; energy check
(flow work vs heat gained).

Validated (kernels): slab frozen depth vs the exact series (±0.002 of the
half-gap), cylinder vs a 400-layer reference, Brinkman viscous heating,
Poiseuille / parabolic layer shares, Graetz cooling (Nu 7.54, decay length
within 1% at Courant 0.2 **and** 40), heated flow at any Courant, energy carried
across a velocity-profile change (exact).
Validated (solver): wall at melt temp + no heating + Newtonian == the
isothermal solver exactly; insulated wall: heat gained = flow work (0.25%);
2 mm PP plate 1 s: pressure +58% over isothermal, frozen 14% mean / 21% max,
front cooling along the flow, gate shear ~14,000 1/s; the pressure-vs-fill-
time **U-curve** (minimum near 3 s for that plate); aluminium freezes more
than steel, a hotter mould or melt lowers pressure, 1 mm freezes a larger
share than 3 mm; a 1 mm × 400 mm strip that fills isothermally short-shots at
~190 mm; 23k triangles thermal in ~6–10 s here (isothermal ~5.5 s; a Release
build is faster).

Not modelled: latent heat of crystallisation (PP freezes a little early), the
pressure dependence of viscosity (D3 = 0 for this PP anyway), mould-side
transient heating (the contact temperature is constant), packing.

Preview: the Flow card gains "Max inj. pressure" (default 150 MPa) and a
"Thermal (frozen layer)" toggle (on by default); the report adds wall
temperature, front-temperature range, per-part frozen share, per-gate wall
shear and freezing, and cautions (gate shear over the material guideline,
gate > 50% frozen, cold front within 20 °C of no-flow); Debug View adds
"Flow front temp" and "Flow frozen layer".

**Next:** fill-front animation / time slider, weld lines and air traps (vents),
then packing (Tait pvT) and shrinkage.

## 14. Fill animation + heat-map legend (Sep 2026)

**Solver.** `CoupledFillParams::animationFrames` (default 60): `SolveCoupledFill`
snapshots the per-node fields at evenly spaced times over the target fill —
pressure, and on thermal runs the gap-mean melt temperature and frozen share —
into `PartFillResult::framePressureMPa / frameBulkTempC / frameFrozenPct`, with
`CoupledFillResult::frames` holding each frame's time, filled share and
injection pressure. A node is filled at a frame when its fill time is at or
before the frame time. Fills that run long (pressure-limited) keep recording,
thinned to at most 3x the frame count; a stalled (freeze-off) fill records up
to the stall. End-of-fill melt temperature per node is `bulkTempC`. About
0.15 MB per frame per 12k nodes.

**Results bar** (`FlowResultsBar.h/.cpp`), under the Preview canvas, shown for
the heat-map views (Flow thickness, Flow midplane, and the five fill views):
- Timeline (fill views): Play / Pause (~14 frames/s), a scrubber drawn over
  the fill time with the injection-pressure trace, a tick per frame and the
  played part highlighted (click / drag; Left / Right step, Home / End), and a
  readout (time, % filled, injection pressure; "End of fill" at the far end).
  The far end is the end-of-fill state (pressure at V/P switchover, frozen
  layer and melt temperature at end of fill) — the same fields as before.
- Legend: title and unit, the 12-band ramp exactly as the model draws it
  (`Heatmap::` is shared by both), five value labels, and swatches for the
  extra colours (Unfilled / Unpaired grey, wall-flagged red).
- Fill views use one legend range per fill, over the end state and every
  frame, so a colour means the same value on every frame.

**Views.** Fill time and melt-front temperature reveal the front as the
timeline advances; pressure, frozen layer and the new **Flow melt temp** (gap-
mean melt temperature) show the frame's field.

Validated: frame recording (count, order, filled set == fill times, frozen
layer growing, thinning, off switch); the Preview harness (legend titles /
ranges, fixed range while scrubbing, the front advancing, switchover field at
the end, bar hidden for other views, new fill resets the timeline);
FlowResultsBar and PreviewPanel.cpp compile clean against real wxWidgets 3.2 +
OpenCASCADE; the bar rendered and played under a virtual display.

**Next:** weld lines and air traps (vents) — done in section 15.

## 15. Weld lines, air traps, venting + Sim Viewer bar (Sep 2026)

`FillDefects.h/.cpp` — `Flow::DetectFillDefects(net, midplanes, fill)`, pure
post-processing of the fill-time field (~10 ms for 40k triangles):
- **Weld / meld lines.** Each triangle's flow direction is its fill-time
  gradient. Across an interior edge where both flows run into the edge
  (converging — not diverging, as around a gate) and meet at >= 75 degrees, two
  fronts joined. Edges join into lines through shared nodes; a line is a weld
  when >= 1/3 of its length meets at >= 135 degrees (head-on), else a meld.
  Per line: length, when the fronts met, max meeting angle and (thermal) the
  coldest front temperature — a weld formed more than 20 C below the melt is
  flagged weak. Limitation: only where fronts meet is traced; the meld line an
  obstacle leaves trailing downstream once the streams merge is not followed.
- **Air traps.** Air escapes at the part outline (parting line) and through
  vents (a vent mouth reaches midplane nodes within 3 mm + half its width).
  Walking the fill backwards (nodes in decreasing fill time, union-find), an
  unfilled region that is cut off from both while still holding air is a
  sealed pocket: reported with its air volume when it sealed, the time, and
  where it fills last (the air ends up there). Pockets under 3 nodes / 0.5 mm3
  are fill-time noise and dropped; nested pockets count once.
- **Last to fill.** Local fill-time maxima (latest within 5 mm) outside the
  pockets: where the air leaves last, i.e. where vents belong — each says
  whether a vent mouth reaches it.

Validated: centre gate — no lines, no traps, the four corners fill last; two
opposed gates — a weld line across the middle (head-on at the centre, ~110
degrees at the ends, as point gates give), no trap; plate with a hole — a line
just behind it, none upstream; race-tracking (3 mm rim, 1 mm centre) — a 249
mm3 air trap in the centre, sealed before it fills; a corner vent is
recognised as venting that corner only.

**Preview.** The Debug View card is gone: its dropdown, renamed **Sim Viewer
Select**, and the Wireframe toggle now live permanently at the left of the
results bar under the canvas; the timeline and legend appear beside them when
the selected view uses them. New view **Flow welds & air traps**: the filled
part in a neutral colour with weld lines (near-black), meld lines (purple),
air traps (red), fill-last points needing a vent (amber) and vented ones
(green) on top, with a key legend; with the timeline, lines appear when the
fronts meet and traps when they seal. The report lists them, and air traps
and weak welds turn the verdict amber.

"Flow melt temp" is the through-wall average including the frozen skin, so
at end of fill it is lowest near the gate (the melt there has been against
the wall longest) and rises along the flow — a residence-time effect, present
with viscous heating off too (heating adds only ~2-5 C on the test plate).

**Next:** packing (Tait pvT) and shrinkage; optionally stream tracking for
trailing meld lines.

## 16. Packing, holding and cooling — shrinkage (Sep 2026)

`SolveCoupledFill` with `params.pack.enabled` (thermal fills that complete)
continues past the fill (`FillPackParams`, results in `CoupledFillResult::pack`
and per part / per gate):
- **Compressible melt.** Each control volume holds a mass M. While full,
  M = V x mean over its layers of rho(T, p) from the 2-domain Tait pVT
  (`TestMaterial::TaitPVT`, now carried in `FillThermalParams::pvt`). Cooling
  at fixed mass lowers the pressure and draws melt in.
- **Each step:** temperatures (convection by the last flows + conduction, one
  implicit sweep; no viscous heating — the flows are slow), fluidities at the
  new temperatures (frozen layers carry nothing, so gates seal themselves),
  then the pressure implicitly, linearised in p:
  `c_i (p_i - p_i^n)/dt + sum rho_e G_e (p_i - p_j) = -(m_i(p^n, T) - M_i)/dt`
  with the inlet at the pack pressure while holding and 0 afterwards; then the
  masses from the solved fluxes (exactly conservative).
- **p >= 0, two-sided active set.** A volume is held at zero pressure only
  while it would hold less melt than fits at zero (the melt has pulled away
  from the wall); a held volume that receives more than fits is released.
  (The one-sided version left scattered over-packed volumes — fixed.)
- **Hold:** a set time, or (hold time 0) until every gate has a fully frozen
  section (capped). Seals: each gate's interior sections; with no gates, the
  cavity entries.
- **Ejection:** each cavity volume's time for its whole section to fall below
  the ejection temperature (interpolated inside the step); the run stops when
  every part is ejectable. Step: 0.02 s growing 25% a step to 0.25 s (implicit
  Euler cooling reads ~+3% at that on a 2 mm wall; 1 s steps read ~+14%).
- **Shrinkage:** volumetric shrinkage = 1 - M / (rho(room, 0) V) — directly
  from the mass each region ended up holding.
- **Animation:** pack / cool frames continue the fill's (phase 1 / 2), thinned
  to ~60.

Validated (2 mm PP plate, 1 s fill): mass balance 1e-13 %; full volumes match
their pVT state to 1e-4 %; packing adds ~7% mass; part mass = room density x
volume x (1 - shrinkage); shrinkage 1.7% at the gate to 5% at the far end
(3.9% mean — PP range); more pack pressure -> less shrinkage (6.3 / 4.6 / 4.0%
at 30 / 60 / 90%); the hold-time study falls until the gate freezes (~7.6 s
after the fill) and is flat after it; cooling vs the analytic slab time
h^2/(pi^2 a) ln(4/pi (Tm-Tw)/(Te-Tw)) +3% mean; 3 mm vs 1.5 mm cooling ratio
3.98 (theory 4); packing keeps the gate region hotter; ~0.6 s to solve.

**Preview.** Flow card: "Pack and cool" (on), "Pack pressure" (% of the fill's
injection pressure, 80), "Hold time" (s, 0 = until the gates freeze). The
report adds pack pressure and hold, each gate's freeze time, the ejection time
and a cycle estimate, part mass (and how much packing added), and per part the
volumetric shrinkage (mean, range, ~1/3 as a rough linear figure) with
cautions for > 8% (sink / void) and > 3 points of variation (warpage). Verdict:
"FILL t s p MPa EJECT t s". Sim Viewer adds "Pack: shrinkage" and "Cooling:
time to eject"; the timeline now runs through fill, pack and cool (frames
spaced evenly, phases shaded and named, readout per phase); after packing the
end of the timeline is the ejection state.

Not modelled: latent heat of crystallisation, mould-side heating, anisotropic
(flow-induced) shrinkage, so no warpage shape yet.

**Next:** warpage (differential shrinkage through the thickness and across the
part -> deflection), or cooling-channel design.
