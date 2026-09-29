#pragma once
// ===========================================================================
// MaterialFile.h — the standard on-disk format for Mould3r's material library.
//
// Two material kinds share one file format (extension ".material"):
//
//   Injection material  — the polymer being moulded (melt side of every sim).
//   Mould material      — the tool the cavity is cut into (the wall heat sink).
//
// LIBRARY LAYOUT (next to the executable, beside fixtures/):
//
//   Materials/
//     Injection Materials/   *.material   (kind = injection)
//     Mould Materials/       *.material   (kind = mould)
//
// FILE SHAPE: INI-style and presence-driven, like .fixture files. Every value
// is optional — a blank value (or a missing key) means "not specified". The
// writer always emits the COMPLETE template (every section, every key, with a
// comment giving its label, unit and test standard), so any material file a
// user opens doubles as the blank template: fill in what the datasheet has,
// leave the rest empty.
//
//   [material]              identity (name, manufacturer, grade, family, ...)
//   [physical] [thermal]    DATASHEET VALUES — the figures a supplier's
//   [mechanical]            technical datasheet (TDS) / processing guide
//   [processing]            normally quotes, in the units it quotes them in.
//   [additional.*]          ADDITIONAL VALUES — measured or study-derived data
//                           (rheometry, pvT, melt-state thermal properties).
//                           When present they OVERRIDE whatever would be
//                           estimated from the datasheet values.
//
// UNITS: each key has ONE fixed unit — the one it is conventionally published
// in — so a value can be copied off a datasheet without conversion. Datasheet
// values therefore use °C, g/cm³, MPa, ...; the Cross-WLF and Tait blocks stay
// strictly SI (K, Pa, m³/kg) because that is how Moldflow / rheometry reports
// them. The unit is part of the field spec (below) and written into the
// file's comments. Conversion to the solver's SI happens when a material is
// resolved for a simulation, never in the file.
//
// RESOLUTION (next step — MaterialResolve, not in this file): a simulation
// never reads this struct directly. Each quantity it needs is resolved in
// order:  additional value  ->  derived from datasheet values  ->  fallback
// (the TestMaterial generic PP / P20 steel figures) + a WARNING naming the
// missing value. The chain for each solver input:
//
//   Injection (TestMaterial::PolymerMaterial field  <-  sources)
//     viscosity (Cross-WLF)  <- [additional.cross_wlf] (all 7)
//                            <- estimate from MFR + test temp/load + melt
//                               density (+ Tg for the WLF reference)
//     densityMelt            <- additional melt_density <- MFR / MVR
//                            <- Tait pvT at melt temp <- density x structure ratio
//     densitySolid           <- physical density
//     specificHeat (melt)    <- additional melt_specific_heat <- thermal specific_heat
//     thermalConductivity    <- additional melt_thermal_conductivity
//                            <- thermal thermal_conductivity
//     noFlowTempC            <- additional no_flow_temp <- Tm / Tg estimate
//     ejectionTempC          <- processing ejection_temp <- hdt_045
//     meltTempMin/Max/rec    <- processing melt_temp_* (rec <- mid-range)
//     recMouldTempC          <- processing mould_temp (<- mid-range)
//     pvt                    <- [additional.tait_pvt] (b1m..b6)
//     maxShearRate           <- processing max_shear_rate
//     elasticModulusMPa      <- mechanical tensile_modulus <- flexural_modulus
//     poissonRatio           <- mechanical poisson_ratio
//   Mould (TestMaterial::MouldMaterial)
//     density / specificHeat / thermalConductivity  <- datasheet
//     (thermalConductivity also <- additional thermal_diffusivity x rho x c_p)
//
// UNITS: the unit strings below are UTF-8 (escaped so the source file stays
// ASCII-safe for MSVC); wrap them in wxString::FromUTF8 for display.
// ===========================================================================

#include <optional>
#include <string>
#include <vector>

enum class MaterialKind { Injection, Mould };

// Current writer version. Readers are presence-driven, so this is
// informational: a file from a NEWER version still loads (its unrecognised
// keys are preserved on re-save) with a warning.
constexpr int kMaterialFormatVersion = 1;

// Library file extension (with the dot).
constexpr const char* kMaterialFileExtension = ".material";

// A key the reader did not recognise. Kept (in file order) and re-emitted at
// the end of its section on save, so a file written by a newer Mould3r — or a
// hand-added value — survives a round trip through an older one.
struct MaterialExtraEntry
{
    std::string section;
    std::string key;
    std::string value;
};

// ---------------------------------------------------------------------------
// Injection (polymer) material. Every numeric field is optional: unset means
// "not on the datasheet / not measured". Units are fixed per field — see the
// field table (MaterialFile::InjectionFields) for label, unit and meaning.
// ---------------------------------------------------------------------------
struct InjectionMaterialData
{
    // [material] — identity
    std::string name;            // display name in the dropdowns (required to save)
    std::string manufacturer;
    std::string grade;
    std::string family;          // PP, ABS, PA6, PC, ...
    std::string structure;       // "semi-crystalline" | "amorphous" | ""
    std::string filler;          // "none", "30% glass fibre", ...
    std::string notes;

    // [physical]
    std::optional<double> density;            // g/cm³, solid @ 23 °C
    std::optional<double> meltFlowRate;       // g/10 min
    std::optional<double> mfrTemperature;     // °C
    std::optional<double> mfrLoad;            // kg
    std::optional<double> meltVolumeRate;     // cm³/10 min
    std::optional<double> shrinkageFlow;      // %
    std::optional<double> shrinkageCross;     // %

    // [thermal]
    std::optional<double> meltingTemp;        // °C
    std::optional<double> glassTransitionTemp;// °C
    std::optional<double> vicatTemp;          // °C
    std::optional<double> hdt045;             // °C
    std::optional<double> hdt180;             // °C
    std::optional<double> thermalConductivity;// W/(m·K), as published (solid)
    std::optional<double> specificHeat;       // J/(kg·K), as published (solid)
    std::optional<double> clteFlow;           // 1e-6/K
    std::optional<double> clteCross;          // 1e-6/K

    // [mechanical]
    std::optional<double> tensileModulus;     // MPa
    std::optional<double> flexuralModulus;    // MPa
    std::optional<double> poissonRatio;       // -

    // [processing]
    std::optional<double> meltTempMin;        // °C
    std::optional<double> meltTempMax;        // °C
    std::optional<double> meltTemp;           // °C (recommended)
    std::optional<double> mouldTempMin;       // °C
    std::optional<double> mouldTempMax;       // °C
    std::optional<double> mouldTemp;          // °C (recommended)
    std::optional<double> ejectionTemp;       // °C
    std::optional<double> maxShearRate;       // 1/s
    std::optional<double> maxShearStress;     // MPa

    // [additional.thermal]
    std::optional<double> meltDensity;              // g/cm³
    std::optional<double> meltSpecificHeat;         // J/(kg·K)
    std::optional<double> meltThermalConductivity;  // W/(m·K)
    std::optional<double> noFlowTemp;               // °C

    // [additional.cross_wlf] — SI, as published
    std::optional<double> cwlfN;         // -
    std::optional<double> cwlfTauStar;   // Pa
    std::optional<double> cwlfD1;        // Pa·s
    std::optional<double> cwlfD2;        // K
    std::optional<double> cwlfD3;        // K/Pa
    std::optional<double> cwlfA1;        // -
    std::optional<double> cwlfA2;        // K (Ã2)

    // [additional.tait_pvt] — SI, as published (modified 2-domain Tait)
    std::optional<double> taitB1m, taitB2m, taitB3m, taitB4m;
    std::optional<double> taitB1s, taitB2s, taitB3s, taitB4s;
    std::optional<double> taitB5, taitB6;
    std::optional<double> taitB7, taitB8, taitB9;

    std::vector<MaterialExtraEntry> extras;
};

// ---------------------------------------------------------------------------
// Mould (tool) material.
// ---------------------------------------------------------------------------
struct MouldMaterialData
{
    // [material] — identity
    std::string name;
    std::string manufacturer;
    std::string grade;           // P20, H13, 7075-T6, QC-10, ...
    std::string family;          // tool steel, aluminium alloy, printed resin, ...
    std::string notes;

    // [physical]
    std::optional<double> density;             // g/cm³

    // [thermal]
    std::optional<double> thermalConductivity; // W/(m·K)
    std::optional<double> specificHeat;        // J/(kg·K)
    std::optional<double> clte;                // 1e-6/K
    std::optional<double> maxServiceTemp;      // °C

    // [mechanical]
    std::optional<double> elasticModulus;      // GPa
    std::optional<double> poissonRatio;        // -
    std::optional<double> yieldStrength;       // MPa
    std::string           hardness;            // as published, e.g. "28-32 HRC"

    // [additional.thermal]
    std::optional<double> thermalDiffusivity;  // mm²/s (measured, e.g. laser flash)

    std::vector<MaterialExtraEntry> extras;
};

// ---------------------------------------------------------------------------
// Field table. ONE table per kind describes every field: which section/key it
// lives under, whether it is a datasheet or an additional value, its UI label,
// unit and one-line help. The reader, the writer (template comments) and —
// next — the Add Material dialog and the resolver's warnings all iterate the
// same table, so adding a field is a one-line change here plus the member.
// Exactly one of `number` / `text` is set.
// ---------------------------------------------------------------------------
enum class MaterialFieldGroup { Identity, Datasheet, Additional };

template <class Data>
struct MaterialFieldSpec
{
    const char*            section;   // "physical", "additional.cross_wlf", ...
    const char*            key;       // "density", "d1", ...
    MaterialFieldGroup     group;
    const char*            label;     // UI label (UTF-8)
    const char*            unit;      // UTF-8, "" for text / dimensionless
    const char*            help;      // one line: meaning / test standard (UTF-8)
    std::optional<double> Data::* number = nullptr;
    std::string           Data::* text   = nullptr;
};

// A section of the file, in write order, with the comment block written above
// it (UTF-8, may contain '\n' for several comment lines).
struct MaterialSectionSpec
{
    const char*        name;
    MaterialFieldGroup group;
    const char*        comment;
    const char*        title = "";   // short UI heading (UTF-8), e.g. "Cross-WLF viscosity"
};

class MaterialFile
{
public:
    // Load a material file. Returns false (with `error`) only when the file
    // cannot be read or is the wrong kind; malformed values, unknown keys and
    // a newer format version are reported through `warnings` (optional) and
    // otherwise skipped/preserved, so one typo never loses the whole file.
    static bool Load(const std::string& path, InjectionMaterialData& out,
                     std::string& error, std::vector<std::string>* warnings = nullptr);
    static bool Load(const std::string& path, MouldMaterialData& out,
                     std::string& error, std::vector<std::string>* warnings = nullptr);

    // Write the full commented template with the set values filled in.
    static bool Save(const std::string& path, const InjectionMaterialData& data,
                     std::string& error);
    static bool Save(const std::string& path, const MouldMaterialData& data,
                     std::string& error);

    static const std::vector<MaterialSectionSpec>& InjectionSections();
    static const std::vector<MaterialSectionSpec>& MouldSections();
    static const std::vector<MaterialFieldSpec<InjectionMaterialData>>& InjectionFields();
    static const std::vector<MaterialFieldSpec<MouldMaterialData>>& MouldFields();

    // ---- Library folders --------------------------------------------------
    // <exe dir>/Materials, and its per-kind subfolders.
    static std::string LibraryRoot();
    static std::string LibraryFolder(MaterialKind kind);

    // Create Materials/ and both subfolders if missing. Returns false (with
    // `error`) only if a folder could not be created.
    static bool EnsureLibraryFolders(std::string& error);

    static const char* KindKeyword(MaterialKind kind);   // "injection" / "mould"

    // The number syntax the file uses — '.' decimal point, optional exponent,
    // locale-independent — for UIs that must accept exactly what the reader
    // does. ParseValue trims surrounding whitespace; false on anything else.
    static bool        ParseValue(const std::string& text, double& out);
    static std::string FormatValue(double v);   // shortest round-trip form
};
