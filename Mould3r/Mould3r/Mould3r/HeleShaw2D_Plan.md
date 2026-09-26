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
