#pragma once
// ===========================================================================
// TestMaterial.h — placeholder material data for bringing up a Hele-Shaw 2.5D
// (mid-plane) injection-filling analysis.
//
// SCOPE: this is a TEST file, not a material library. The values below are
// representative, order-of-magnitude-correct generic figures gathered to let
// the filling solver be developed and sanity-checked end to end. They are NOT
// certified datasheet data and must be replaced with grade-specific rheometry /
// pvT / thermal data (and a real, parsed material database) before any result
// is trusted. When the proper libraries land, these structs are a reasonable
// in-memory shape to target.
//
// WHAT A HELE-SHAW 2.5D FILL NEEDS (and where each input is used):
//   Pressure field:   ∇·(S ∇p) = 0,  S = ∫_0^{b} (z²/η) dz   (gap-wise flow
//                     conductance over the half-gap b). η is the melt viscosity.
//   Viscosity:        Cross-WLF  η(γ̇, T, p)  — the polymer `viscosity` block.
//   Energy / freeze:  ρ c_p (∂T/∂t + u·∇T) = k ∂²T/∂z² + η γ̇²   — needs the
//                     polymer ρ, c_p, k, and the no-flow temperature that sets
//                     the frozen-layer boundary (where η → ∞ / flow stops).
//   Wall boundary:    the mould material's ρ, c_p, k (effusivity) set how fast
//                     heat leaves the melt into the steel/aluminium wall.
//   Packing/shrink:   the 2-domain Tait pvT (optional for pure filling; needed
//                     once packing is added).
//
// UNITS: strict SI. Temperatures inside the Cross-WLF and Tait models are
// ABSOLUTE (kelvin). Human-facing transition/process temperatures are given in
// °C and clearly named `...C`; convert with kZeroC when feeding the models.
// ===========================================================================

#include <cmath>

namespace TestMaterial
{
    constexpr double kZeroC = 273.15;   // °C -> K offset

    // -----------------------------------------------------------------------
    // Cross-WLF shear-thinning viscosity (the Moldflow-standard filling model):
    //
    //   η(γ̇, T, p) = η0 / ( 1 + ( η0 · γ̇ / tauStar )^(1 - n) )
    //   η0(T, p)    = D1 · exp[ -A1 · (T - Tstar) / ( A2 + (T - Tstar) ) ]
    //   Tstar       = D2 + D3 · p
    //   A2          = A2tilde + D3 · p
    //
    // γ̇ = shear rate [1/s], T = melt temperature [K], p = pressure [Pa].
    // -----------------------------------------------------------------------
    struct CrossWLF
    {
        double n;         // power-law index in the shear-thinning region  [-]
        double tauStar;   // critical shear stress (onset of shear thinning) [Pa]
        double D1;        // zero-shear viscosity scale                     [Pa·s]
        double D2;        // reference (glass-like) temperature             [K]
        double D3;        // pressure sensitivity of Tstar (0 => none)      [K/Pa]
        double A1;        // WLF slope constant                             [-]
        double A2tilde;   // WLF constant (A2 at p = 0)                     [K]

        // Zero-shear viscosity η0 at temperature T [K], pressure p [Pa].
        double eta0(double T_K, double p_Pa = 0.0) const
        {
            const double Tstar = D2 + D3 * p_Pa;
            const double A2    = A2tilde + D3 * p_Pa;
            const double dT    = T_K - Tstar;
            return D1 * std::exp(-A1 * dT / (A2 + dT));
        }

        // Apparent viscosity at shear rate γ̇ [1/s], T [K], p [Pa]  -> [Pa·s].
        double eta(double gammaDot, double T_K, double p_Pa = 0.0) const
        {
            const double e0 = eta0(T_K, p_Pa);
            if (gammaDot <= 0.0) return e0;
            return e0 / (1.0 + std::pow(e0 * gammaDot / tauStar, 1.0 - n));
        }
    };

    // -----------------------------------------------------------------------
    // 2-domain modified Tait pvT: specific volume v(T,p) [m³/kg]. Used for
    // packing/shrinkage; not required for a first filling-only solve.
    //
    //   v(T,p) = v0(T) · [ 1 - C · ln(1 + p / B(T)) ]              (+ vt, solid)
    //   v0(T)  = b1 + b2 · (T - b5)      B(T) = b3 · exp(-b4 · (T - b5))
    //   Domain split at the transition temperature Tt(p) = b5 + b6 · p:
    //     T > Tt  -> melt (…m) coefficients;  T <= Tt -> solid (…s) coefficients.
    // -----------------------------------------------------------------------
    struct TaitPVT
    {
        double b1m, b2m, b3m, b4m;  // melt (upper-temperature) domain
        double b1s, b2s, b3s, b4s;  // solid (lower-temperature) domain
        double b5;                  // transition temperature at p = 0   [K]
        double b6;                  // dTt/dp                            [K/Pa]
        double C;                   // Tait universal constant (~0.0894) [-]

        double specificVolume(double T_K, double p_Pa) const
        {
            const double Tt = b5 + b6 * p_Pa;
            const bool   melt = (T_K > Tt);
            const double b1 = melt ? b1m : b1s;
            const double b2 = melt ? b2m : b2s;
            const double b3 = melt ? b3m : b3s;
            const double b4 = melt ? b4m : b4s;
            const double dT = T_K - b5;
            const double v0 = b1 + b2 * dT;
            const double B  = b3 * std::exp(-b4 * dT);
            return v0 * (1.0 - C * std::log(1.0 + p_Pa / B));
        }
    };

    // -----------------------------------------------------------------------
    // Injection (polymer) material — everything the melt side of the solve needs.
    // -----------------------------------------------------------------------
    struct PolymerMaterial
    {
        const char* name;

        CrossWLF viscosity;         // shear/temperature-dependent melt viscosity

        // Thermal (melt-state values unless the field name says otherwise).
        double densityMelt;         // melt density                     [kg/m³]
        double densitySolid;        // solid (room-T) density           [kg/m³]
        double specificHeat;        // melt specific heat c_p           [J/(kg·K)]
        double thermalConductivity; // melt thermal conductivity k      [W/(m·K)]

        // Transition temperatures (°C for readability; convert with kZeroC).
        double noFlowTempC;         // flow stops here (frozen-layer edge) [°C]
        double ejectionTempC;       // safe-to-eject temperature           [°C]
        double meltTempMinC;        // recommended melt-temperature window  [°C]
        double meltTempMaxC;        //   ""                                  [°C]

        // Convenience process defaults (PROCESS, not intrinsic material data —
        // a starting point for a run, easily overridden by the Physical Setup).
        double recMeltTempC;        // recommended barrel/melt temperature [°C]
        double recMouldTempC;       // recommended mould-wall temperature  [°C]

        TaitPVT pvt;                // packing/shrinkage (optional for filling)

        // Processing guideline: wall shear rate above which the melt risks
        // degradation / surface defects (gates are the usual offender)  [1/s].
        double maxShearRate = 100000.0;

        // Melt thermal diffusivity α = k / (ρ c_p)  [m²/s].
        double thermalDiffusivity() const
        { return thermalConductivity / (densityMelt * specificHeat); }
    };

    // -----------------------------------------------------------------------
    // Mould material — thermal properties for the cavity-wall heat sink. The
    // effusivity e = √(k ρ c_p) governs the melt/wall contact temperature and
    // how quickly the frozen layer grows against this wall.
    // -----------------------------------------------------------------------
    struct MouldMaterial
    {
        const char* name;
        double density;             // [kg/m³]
        double specificHeat;        // c_p [J/(kg·K)]
        double thermalConductivity; // k   [W/(m·K)]

        double thermalDiffusivity() const   // α = k / (ρ c_p)  [m²/s]
        { return thermalConductivity / (density * specificHeat); }
        double effusivity() const           // e = √(k ρ c_p)  [W·s^½/(m²·K)]
        { return std::sqrt(thermalConductivity * density * specificHeat); }
    };

    // =======================================================================
    // Instances (representative generic values — see the scope note above).
    // =======================================================================

    // Generic homopolymer polypropylene. Cross-WLF η0 ≈ 8·10³ Pa·s at 230 °C,
    // shear-thinning past ~10² 1/s; α ≈ 8·10⁻⁸ m²/s.
    inline const PolymerMaterial kGenericPolypropylene = {
        "Generic Polypropylene",
        // Cross-WLF:      n     tauStar[Pa]  D1[Pa·s]   D2[K]    D3[K/Pa] A1     A2tilde[K]
        /* viscosity */ { 0.25, 30000.0,     3.6e13,    263.15,  0.0,     27.0,  51.6 },
        /* densityMelt        */ 750.0,      // kg/m³ (melt, ~230 °C)
        /* densitySolid       */ 905.0,      // kg/m³ (solid, ~23 °C)
        /* specificHeat       */ 2900.0,     // J/(kg·K) (melt)
        /* thermalConductivity*/ 0.18,       // W/(m·K)  (melt)
        /* noFlowTempC        */ 138.0,      // °C (near crystallisation)
        /* ejectionTempC      */ 95.0,       // °C
        /* meltTempMinC       */ 200.0,      // °C
        /* meltTempMaxC       */ 280.0,      // °C
        /* recMeltTempC       */ 230.0,      // °C
        /* recMouldTempC      */ 40.0,       // °C
        // 2-domain Tait (representative):
        //           b1m       b2m       b3m     b4m      b1s       b2s       b3s     b4s      b5[K]  b6[K/Pa]  C
        /* pvt */ { 1.250e-3, 8.0e-7,   1.0e8,  4.5e-3,  1.180e-3, 2.5e-7,   1.5e8,  2.0e-3,  400.0, 2.5e-7,   0.0894 }
    };

    // Generic P20-class mould steel.
    inline const MouldMaterial kMouldSteel = {
        "Steel (P20-class)",
        /* density             */ 7800.0,    // kg/m³
        /* specificHeat        */ 460.0,     // J/(kg·K)
        /* thermalConductivity */ 29.0       // W/(m·K)
    };

    // Generic high-conductivity mould aluminium (7075 / QC-10 class).
    inline const MouldMaterial kMouldAluminum = {
        "Aluminum (7075-class)",
        /* density             */ 2810.0,    // kg/m³
        /* specificHeat        */ 960.0,     // J/(kg·K)
        /* thermalConductivity */ 130.0      // W/(m·K)
    };

} // namespace TestMaterial
