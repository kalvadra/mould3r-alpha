#pragma once
// ===========================================================================
// MaterialResolve.h — turn a library material (MaterialFile.h, every value
// optional) into the complete property set the simulations consume
// (TestMaterial::PolymerMaterial / MouldMaterial).
//
// Every quantity is resolved in the same order:
//
//   1. an ADDITIONAL value (measured / study data), when present;
//   2. DERIVED from the datasheet values (each derivation is listed below);
//   3. FALLBACK — the generic Polypropylene / P20 steel figure — with a
//      warning naming what was missing.
//
// Steps 2 and 3 each leave a MaterialNote. Notes are tagged with the parts of
// a simulation that consume the value (MaterialUse bits) so a run only
// reports the values it actually used — an isothermal fill doesn't warn about
// a missing thermal conductivity.
//
// DERIVATIONS (injection):
//   Melt density     Tait pvT at the recommended melt temperature, else
//                    MFR / MVR (same test conditions), else solid density
//                    x 0.83 (semi-crystalline) / 0.89 (amorphous) / 0.86.
//   Cross-WLF        single-point fit to the melt-flow-rate test (ISO 1133
//                    die 2.095 x 8 mm, 9.55 mm barrel): wall shear stress
//                    from the load, flow from MVR (or MFR / melt density),
//                    Rabinowitsch-corrected wall shear rate -> the zero-shear
//                    viscosity at the test temperature -> D1. n, tau*, A1, A2,
//                    D2, D3 come from any Cross-WLF values given, else D2 = Tg
//                    with the universal WLF A1 = 17.44 / A2 = 51.6, else the
//                    generic PP constants. One point fixes the viscosity
//                    LEVEL, not the shear-thinning shape.
//   Melt c_p, k      the datasheet (usually solid-state) figure.
//   No-flow temp     Tm - 25 °C (semi-crystalline) or Tg + 30 °C (amorphous;
//                    Vicat stands in for a missing Tg).
//   Ejection temp    HDT @ 0.45 MPa, else HDT @ 1.80 MPa.
//   Melt/mould temps recommended <- middle of the range <- the one bound
//                    given (melt also <- the MFR test temperature); a
//                    missing bound = recommended -/+ 30 °C (melt) / 20 °C (mould).
//   Tait pvT         the generic PP surface with its volumes scaled to this
//                    material's melt and solid densities (and its transition
//                    moved to the no-flow temperature when that is known).
//   Modulus          flexural modulus when no tensile modulus is given.
// DERIVATIONS (mould):
//   Conductivity     thermal diffusivity x density x c_p.
//   Specific heat    conductivity / (density x diffusivity).
// ===========================================================================

#include <string>
#include <vector>

#include "MaterialFile.h"
#include "TestMaterial.h"

// Which parts of a simulation consume a value (bit flags).
enum MaterialUse : unsigned
{
    MatUseFill    = 1u << 0,   // any flow run (viscosity, melt temperature window, shear limit)
    MatUseThermal = 1u << 1,   // thermal fill (melt / wall thermal properties, no-flow)
    MatUsePack    = 1u << 2,   // pack / hold / cool (pvT, ejection temperature)
    MatUseWarp    = 1u << 3,   // warpage (modulus, Poisson's ratio)
};

struct MaterialNote
{
    enum class Level { Derived, Fallback };
    Level       level = Level::Derived;
    unsigned    uses = 0;          // MaterialUse bits
    std::string quantity;          // "Melt density" (UTF-8)
    std::string message;           // what was used and why (UTF-8)
    // Fallback notes: the input fields ("section.key", as in the material
    // file) whose absence caused it — where the Add Material dialog puts its
    // warning star. The most direct fix first; empty for Derived notes.
    std::vector<std::string> fields;
};

// NOTE: props.name points at a static placeholder — use `name` for display.
// (PolymerMaterial carries a const char*; pointing it into our own std::string
// would dangle as soon as the struct is copied.)
struct ResolvedInjectionMaterial
{
    std::string                  name;
    TestMaterial::PolymerMaterial props{};
    std::vector<MaterialNote>    notes;
};

struct ResolvedMouldMaterial
{
    std::string                name;
    TestMaterial::MouldMaterial props{};
    std::vector<MaterialNote>  notes;
};

ResolvedInjectionMaterial ResolveInjectionMaterial(const InjectionMaterialData& d);
ResolvedMouldMaterial     ResolveMouldMaterial(const MouldMaterialData& d);

// The simulation(s) behind each MaterialUse bit, one line per set bit, for
// "this missing value affects ..." messages. Add a line here when a new
// simulation starts consuming material data.
std::vector<std::string> MaterialUseDescriptions(unsigned uses);

// The notes whose `uses` overlap `activeUses` (MaterialUse bits).
std::vector<const MaterialNote*> MaterialNotesFor(const std::vector<MaterialNote>& notes,
                                                  unsigned activeUses);
