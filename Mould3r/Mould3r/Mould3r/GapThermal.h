#pragma once
// ===========================================================================
// GapThermal — the through-thickness ("gap-wise") pieces of the thermal
// Hele-Shaw fill: a layered temperature profile at every control volume, the
// melt fluidity across those layers, and the implicit conduction / viscous-
// heating / convection updates. Used by CoupledFill; kept separate so each
// kernel can be validated on its own.
//
// Geometry. Every control volume is either
//   Slab      a gap of thickness 2b (part midplanes, rectangular runners /
//             gates / vents across their thin side) — symmetric about the
//             centre plane, so only the half-gap 0..b is stored;
//   Cylinder  a round channel of radius R (round sprue / runners / gates) —
//             axisymmetric, radius 0..R.
// Both use the same normalised layer grid x in [0,1] (0 = centre / axis,
// 1 = wall), graded finer towards the wall where the frozen layer grows.
//
// Physics per layer (the classic Hieber-Shen thermal Hele-Shaw model):
//   rho c (dT/dt + u.grad T) = k d2T/dz2 + eta gammaDot^2
//   wall: T = Tw (mould contact temperature), centre / axis: symmetry
//   shear stress  tau(z) = z |grad p|  (slab),  (r/2) |dp/dx|  (cylinder)
//   fluidity      phi = 1/eta(gammaDot, T),  0 (floor) below the no-flow temp
//   conductance   slab  S = 2 int_0^b z^2 phi dz         -> h^3/(12 eta)
//                 pipe  C = (pi/2) int_0^R r^3 phi dr    -> pi R^4/(8 eta)
// Latent heat and the pressure dependence of viscosity are not modelled.
//
// Units: lengths in mm where they meet the pressure solve (S, C, tau), SI
// (metres, K, W) inside the conduction step. Temperatures in kelvin.
// ===========================================================================

#include "TestMaterial.h"

#include <cstdint>
#include <vector>

namespace Flow
{
    enum class GapGeom : uint8_t { Slab, Cylinder };

    // Normalised layer grid (0 = centre / axis, 1 = wall), n layers.
    struct LayerGrid
    {
        int n = 0;
        std::vector<double> xe;        // n+1 layer edges
        std::vector<double> xc;        // n layer centres
        std::vector<double> vf[2];     // volume fraction per layer   [Slab, Cylinder]
        std::vector<double> mom[2];    // conductance moment per layer: (x+^3-x-^3)/3, (x+^4-x-^4)/4

        // grading: layer thickness at the wall relative to the mean (0.5 -> the
        // wall layer is a third of the centre layer).
        void build(int layers, double grading = 0.5);
    };

    // Cross-WLF melt viscosity with ln(eta0(T)) tabulated (no exp per call) and
    // a no-flow cut-off: below it the melt is frozen and its fluidity drops to a
    // tiny floor (keeps the pressure matrix factorable).
    class MeltViscosity
    {
    public:
        void init(const TestMaterial::CrossWLF& cw, double noFlowK, double tMinK, double tMaxK);
        double phi(double gammaDot, double TK) const;     // 1/eta [1/(Pa s)]
        double eta(double gammaDot, double TK) const { return 1.0 / phi(gammaDot, TK); }
        bool   frozen(double TK) const { return TK < m_noFlowK; }
        double noFlowK() const { return m_noFlowK; }
        double frozenPhi() const { return m_phiFloor; }
    private:
        double lnEta0(double TK) const;
        TestMaterial::CrossWLF m_cw{};
        double m_noFlowK = 0.0, m_t0 = 0.0, m_dT = 0.1, m_oneMinusN = 0.75, m_lnTau = 0.0;
        double m_phiFloor = 0.0;
        std::vector<double> m_lnEta0;
    };

    // Shear stress at each layer centre for a pressure gradient |grad p| [Pa/mm]
    // over a channel of half-gap / radius sizeMm.  [Pa]
    inline double LayerStress(GapGeom g, double sizeMm, double xc, double gradP)
    {
        return (g == GapGeom::Slab ? 1.0 : 0.5) * sizeMm * xc * gradP;
    }

    // Conductance integral of a fluidity profile: slab 2 int z^2 phi dz
    // [mm^3/(Pa s)], cylinder (pi/2) int r^3 phi dr [mm^4/(Pa s)].
    double FluidityIntegral(const LayerGrid& g, GapGeom geom, double sizeMm, const double* phi);

    // Share of the volumetric flow carried by each layer (sums to 1) for a
    // fluidity profile (the velocity profile integrated over each layer).
    void LayerFlowFractions(const LayerGrid& g, GapGeom geom, const double* phi, double* frac);

    // One implicit (backward-Euler) step of gap-wise conduction with a volumetric
    // heat source: wall at Tw, symmetry at the centre / axis. T in/out [K],
    // heat [W/m^3] per layer (may be null), sizeM = half-gap / radius [m].
    //
    // Optional in-plane convection in conservative form, implicit in the same
    // solve. Per layer, as rates per unit volume of the whole control volume
    // [1/s]: inRate[k] = sum(q f_k)/V of the melt flowing in at the mixed
    // temperature inT[k], outRate[k] = sum(q f_k)/V flowing out at T[k]. Where
    // a layer's in- and outflow differ (the velocity profile changes along the
    // flow) the difference crosses to the neighbouring layers (upwinded), so
    // every layer keeps its mass and the step conserves energy.
    void ConductStep(const LayerGrid& g, GapGeom geom, double sizeM, double k, double rhoCp,
                     double dt, double Tw, const double* heat, double* T,
                     const double* inRate = nullptr, const double* inT = nullptr,
                     const double* outRate = nullptr);

    // Frozen share of the half-gap / radius (0..1): depth from the wall to where
    // the profile first rises through the no-flow temperature, interpolated
    // between the wall and the layer centres.
    double FrozenFraction(const LayerGrid& g, const double* T, double Tw, double noFlowK);

    // Volume-weighted mean temperature of a layered profile.
    double MeanTemperature(const LayerGrid& g, GapGeom geom, const double* T);

    // Contact temperature of two semi-infinite bodies (melt at Tm, mould at
    // Tmould) from their effusivities e = sqrt(k rho c): the wall temperature
    // the melt sees. Mould effusivity <= 0 -> the wall sits at Tmould.
    double ContactTemperature(double Tm, double eMelt, double Tmould, double eMould);

    // One thermal step over a set of control volumes: implicit first-order
    // upwind convection through the given directed fluxes, gap-wise conduction
    // to the wall and viscous heating, solved together per volume (so a volume
    // flushed many times per step still gets the heating / cooling of its short
    // residence time, not of the whole step). Volumes are processed in `order`,
    // which must put every upstream volume before those it feeds (descending
    // pressure does) — then the whole step is one sweep.
    //   T        [node * n + layer] in/out, K
    //   vol      control volume per node (any unit consistent with inQ / s)
    //   geom, sizeM   per node: layer geometry, half-gap / radius [m]
    //   inStart / inFrom / inQ / inFrac: incoming fluxes of each node in CSR
    //            form — source node, volumetric flux, pointer to its n layer shares
    //   outRate  [node * n + layer] outgoing volumetric flux per layer (sum q f_k,
    //            same unit as inQ) — to filled and to empty neighbours alike
    //   heat     [node * n + layer] W/m^3, may be null
    //   fixed    nodes held at their current profile (the inlet), may be null
    void ThermalSweep(const LayerGrid& g, const std::vector<int>& order, double dt,
                      const std::vector<double>& vol, const std::vector<GapGeom>& geom,
                      const std::vector<double>& sizeM, double k, double rhoCp, double Tw,
                      const std::vector<int>& inStart, const std::vector<int>& inFrom,
                      const std::vector<double>& inQ, const std::vector<const double*>& inFrac,
                      const std::vector<double>& outRate,
                      const double* heat, const std::vector<char>* fixed, std::vector<double>& T);

} // namespace Flow
