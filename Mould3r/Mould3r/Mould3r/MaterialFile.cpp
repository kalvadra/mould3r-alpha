// wx FIRST: wx/platform.h defines _CRT_SECURE_NO_DEPRECATE before the CRT
// headers are pulled in. If <fstream>/<filesystem> come first, the CRT is
// already declared with deprecation attributes and wx/filefn.h's _wopen
// trips C4996 (an error under /sdl). Same order as the rest of the tree.
#include <wx/stdpaths.h>

#include "MaterialFile.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>

namespace fs = std::filesystem;

// UTF-8 pieces for units/labels (escaped: keeps this file ASCII-safe for
// MSVC's source charset, and each escape is its own literal so a following
// hex-looking character can't extend it).
#define U_DEGC  "\xC2\xB0" "C"
#define U_DOT   "\xC2\xB7"
#define U_SUP2  "\xC2\xB2"
#define U_SUP3  "\xC2\xB3"
#define U_DASH  "\xE2\x80\x94"

namespace
{
    using G = MaterialFieldGroup;
    using IF = MaterialFieldSpec<InjectionMaterialData>;
    using MF = MaterialFieldSpec<MouldMaterialData>;
    using I = InjectionMaterialData;
    using M = MouldMaterialData;

    std::string Trim(const std::string& s)
    {
        const auto start = s.find_first_not_of(" \t\r\n");
        if (start == std::string::npos) return "";
        const auto end = s.find_last_not_of(" \t\r\n");
        return s.substr(start, end - start + 1);
    }

    std::string ToLowerAscii(std::string s)
    {
        for (char& c : s)
            if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
        return s;
    }

    // Locale-independent, shortest round-trip number formatting / parsing.
    // (A wxLocale with a comma decimal separator must never change the file.)
    std::string FormatNumber(double v)
    {
        char buf[64];
        const auto r = std::to_chars(buf, buf + sizeof(buf), v);
        return (r.ec == std::errc()) ? std::string(buf, r.ptr) : std::string();
    }

    bool ParseNumber(const std::string& s, double& out)
    {
        const char* first = s.data();
        const char* last = s.data() + s.size();
        if (first != last && *first == '+') ++first;   // from_chars rejects '+'
        double v = 0.0;
        const auto r = std::from_chars(first, last, v);
        if (r.ec != std::errc() || r.ptr != last || !std::isfinite(v)) return false;
        out = v;
        return true;
    }

    // Text values are single-line on disk: newline <-> "\n", backslash <-> "\\".
    std::string EscapeText(const std::string& s)
    {
        std::string o;
        o.reserve(s.size());
        for (char c : s)
        {
            if (c == '\\') o += "\\\\";
            else if (c == '\n') o += "\\n";
            else if (c == '\r') continue;
            else o += c;
        }
        return o;
    }

    std::string UnescapeText(const std::string& s)
    {
        std::string o;
        o.reserve(s.size());
        for (size_t i = 0; i < s.size(); ++i)
        {
            if (s[i] == '\\' && i + 1 < s.size())
            {
                if (s[i + 1] == 'n') { o += '\n'; ++i; continue; }
                if (s[i + 1] == '\\') { o += '\\'; ++i; continue; }
            }
            o += s[i];
        }
        return o;
    }

    // Write a (possibly multi-line) comment block, one "# " per line.
    void WriteComment(std::ostream& os, const std::string& text)
    {
        std::istringstream in(text);
        std::string line;
        while (std::getline(in, line))
            os << (line.empty() ? "#" : "# " + line) << "\n";
    }

    // ---- Generic reader / writer over a field table ------------------------

    template <class Data>
    bool LoadImpl(const std::string& path, Data& out, MaterialKind kind,
                  const std::vector<MaterialFieldSpec<Data>>& fields,
                  std::string& error, std::vector<std::string>* warnings)
    {
        // Fresh struct: a reused destination must not inherit values the new
        // file is silent on (same reasoning as FixtureFile::Load).
        out = Data{};

        std::ifstream file(path);
        if (!file.is_open())
        {
            error = "Could not open material file: " + path;
            return false;
        }

        auto warn = [warnings](int lineNo, const std::string& msg)
        {
            if (!warnings) return;
            warnings->push_back(lineNo > 0 ? "Line " + std::to_string(lineNo) + ": " + msg : msg);
        };

        const char* wantKind = MaterialFile::KindKeyword(kind);
        const char* otherKind = MaterialFile::KindKeyword(
            kind == MaterialKind::Injection ? MaterialKind::Mould : MaterialKind::Injection);

        std::string section;
        std::string line;
        int lineNo = 0;
        bool sawKind = false;
        while (std::getline(file, line))
        {
            ++lineNo;
            if (lineNo == 1 && line.rfind("\xEF\xBB\xBF", 0) == 0)
                line.erase(0, 3);                               // UTF-8 BOM
            line = Trim(line);
            if (line.empty() || line[0] == '#' || line[0] == ';') continue;

            if (line[0] == '[')
            {
                if (line.back() != ']')
                {
                    warn(lineNo, "malformed section header \"" + line + "\" " U_DASH " ignored");
                    continue;
                }
                section = ToLowerAscii(Trim(line.substr(1, line.size() - 2)));
                continue;
            }

            const auto eq = line.find('=');
            if (eq == std::string::npos)
            {
                warn(lineNo, "expected \"key = value\", got \"" + line + "\" " U_DASH " ignored");
                continue;
            }
            const std::string key = ToLowerAscii(Trim(line.substr(0, eq)));
            const std::string value = Trim(line.substr(eq + 1));

            if (section == "material" && key == "format_version")
            {
                double v = 0.0;
                if (!value.empty() && ParseNumber(value, v) && v > kMaterialFormatVersion)
                    warn(lineNo, "written by a newer Mould3r (format " + value +
                                 "); values this version doesn't know are kept but not used");
                continue;
            }
            if (section == "material" && key == "kind")
            {
                std::string k = ToLowerAscii(value);
                if (k == "mold") k = "mould";                   // accept US spelling
                if (k.empty()) continue;
                if (k == wantKind) { sawKind = true; continue; }
                if (k == otherKind)
                {
                    error = std::string("This is a ") + otherKind +
                            " material file, not an " + wantKind + " material: " + path;
                    if (kind == MaterialKind::Mould)
                        error = std::string("This is an ") + otherKind +
                                " material file, not a " + wantKind + " material: " + path;
                    return false;
                }
                warn(lineNo, "unknown kind \"" + value + "\" " U_DASH " read as " + wantKind);
                continue;
            }

            // Retired keys: kept verbatim for the round trip, without the
            // "unrecognised" warning (files written before they were dropped).
            if (section == "material" && key == "source")
            {
                out.extras.push_back({ section, key, value });
                continue;
            }

            const MaterialFieldSpec<Data>* spec = nullptr;
            for (const auto& f : fields)
                if (section == f.section && key == f.key) { spec = &f; break; }

            if (!spec)
            {
                out.extras.push_back({ section, key, value });
                warn(lineNo, "unrecognised value [" + section + "] " + key +
                             " " U_DASH " kept in the file but not used");
                continue;
            }

            if (spec->text)
            {
                out.*(spec->text) = UnescapeText(value);
            }
            else if (!value.empty())
            {
                double v = 0.0;
                if (ParseNumber(value, v))
                    out.*(spec->number) = v;
                else
                    warn(lineNo, "[" + section + "] " + key + " = \"" + value +
                                 "\" is not a number " U_DASH " ignored");
            }
        }

        if (!sawKind)
            warn(0, std::string("no \"kind\" line " U_DASH " read as ") + wantKind + " material");
        return true;
    }

    template <class Data>
    bool SaveImpl(const std::string& path, const Data& d, MaterialKind kind,
                  const std::vector<MaterialSectionSpec>& sections,
                  const std::vector<MaterialFieldSpec<Data>>& fields,
                  std::string& error)
    {
        const bool inj = (kind == MaterialKind::Injection);
        std::ostringstream os;

        WriteComment(os, std::string("Mould3r material file " U_DASH " ") +
                         (inj ? "injection material" : "mould material") + "\n"
            "\n"
            "Every value is optional: leave it blank if you don't have it.\n"
            "Each value has one fixed unit, shown in its comment " U_DASH " the unit a\n"
            "datasheet normally quotes it in, so it can be copied across as-is.\n"
            "DATASHEET values are the figures a supplier's datasheet quotes.\n"
            "ADDITIONAL values are measured / study data; when present they override\n"
            "the estimates Mould3r derives from the datasheet values. If a value a\n"
            "simulation needs is neither given nor derivable, a generic fallback is\n"
            "used and the simulation warns which value was missing.");
        os << "\n";

        G lastGroup = G::Identity;
        for (const MaterialSectionSpec& s : sections)
        {
            if (s.group != lastGroup)
            {
                const char* bar =
                    "=============================================================================";
                os << "# " << bar << "\n";
                if (s.group == G::Datasheet)
                    WriteComment(os, "DATASHEET VALUES\n"
                        "The figures a supplier's technical datasheet / processing guide quotes.");
                else
                    WriteComment(os, "ADDITIONAL VALUES\n"
                        "Measured or study-derived data. Each one overrides the estimate\n"
                        "Mould3r would otherwise derive from the datasheet values.");
                os << "# " << bar << "\n\n";
                lastGroup = s.group;
            }

            WriteComment(os, s.comment);
            os << "[" << s.name << "]\n";

            // Key column width for this section (fields + preserved extras).
            size_t w = 0;
            const bool isMaterial = (std::string(s.name) == "material");
            if (isMaterial) w = std::string("format_version").size();
            for (const auto& f : fields)
                if (std::string(f.section) == s.name) w = std::max(w, std::string(f.key).size());
            for (const auto& e : d.extras)
                if (e.section == s.name) w = std::max(w, e.key.size());
            auto keyCol = [w](const std::string& k) { return k + std::string(w - k.size(), ' '); };

            if (isMaterial)
            {
                os << keyCol("format_version") << " = " << kMaterialFormatVersion << "\n";
                os << keyCol("kind") << " = " << MaterialFile::KindKeyword(kind) << "\n";
            }

            for (const auto& f : fields)
            {
                if (std::string(f.section) != s.name) continue;
                std::string c = f.label;
                if (f.unit && *f.unit) c += std::string(" [") + f.unit + "]";
                if (f.help && *f.help) c += std::string(" " U_DASH " ") + f.help;
                WriteComment(os, c);

                std::string v;
                if (f.text) v = EscapeText(d.*(f.text));
                else if ((d.*(f.number)).has_value()) v = FormatNumber(*(d.*(f.number)));
                os << keyCol(f.key) << " =" << (v.empty() ? "" : " " + v) << "\n";
            }

            for (const auto& e : d.extras)
                if (e.section == s.name)
                    os << keyCol(e.key) << " =" << (e.value.empty() ? "" : " " + e.value) << "\n";
            os << "\n";
        }

        // Preserved keys from sections this version doesn't know.
        std::vector<std::string> foreign;
        for (const auto& e : d.extras)
        {
            bool known = false;
            for (const auto& s : sections) if (e.section == s.name) { known = true; break; }
            if (!known && std::find(foreign.begin(), foreign.end(), e.section) == foreign.end())
                foreign.push_back(e.section);
        }
        for (const std::string& sec : foreign)
        {
            os << "[" << sec << "]\n";
            for (const auto& e : d.extras)
                if (e.section == sec)
                    os << e.key << " =" << (e.value.empty() ? "" : " " + e.value) << "\n";
            os << "\n";
        }

        std::ofstream file(path);
        if (!file.is_open())
        {
            error = "Could not write material file: " + path;
            return false;
        }
        file << os.str();
        file.close();
        if (file.fail())
        {
            error = "Failed while writing material file: " + path;
            return false;
        }
        return true;
    }
} // namespace

// ===========================================================================
// Section + field tables
// ===========================================================================

const std::vector<MaterialSectionSpec>& MaterialFile::InjectionSections()
{
    static const std::vector<MaterialSectionSpec> s = {
        { "material",   G::Identity,
          "IDENTITY " U_DASH " name is what the material dropdowns show.", "Identity" },
        { "physical",   G::Datasheet,  "Physical properties.", "Physical" },
        { "thermal",    G::Datasheet,
          "Thermal properties, as published (datasheets usually quote solid,\n"
          "room-temperature values).", "Thermal" },
        { "mechanical", G::Datasheet,  "Mechanical properties (solid, 23 " U_DEGC ").", "Mechanical" },
        { "processing", G::Datasheet,
          "Processing guide. The recommended melt / mould temperatures seed the\n"
          "simulation process fields; the ranges bound them.", "Processing" },
        { "additional.thermal", G::Additional,
          "Melt-state thermal data and the no-flow temperature.", "Melt-state thermal" },
        { "additional.cross_wlf", G::Additional,
          "Cross-WLF viscosity model " U_DASH " SI, exactly as published (Moldflow /\n"
          "capillary rheometry):\n"
          "  eta  = eta0 / (1 + (eta0 * shear_rate / tau_star)^(1 - n))\n"
          "  eta0 = d1 * exp(-a1 (T - T*) / (a2 + (T - T*))),  T* = d2 + d3 p\n"
          "n, tau_star, d1, d2, a1 and a2 are needed for the set to be used;\n"
          "a blank d3 means 0 (no pressure dependence).", "Cross-WLF viscosity (SI)" },
        { "additional.tait_pvt", G::Additional,
          "Modified 2-domain Tait pvT " U_DASH " SI, exactly as published.\n"
          "b1m-b4m melt domain, b1s-b4s solid domain, b5-b6 transition,\n"
          "b7-b9 crystallisation term (blank = 0, e.g. amorphous materials).", "Tait pvT (SI)" },
    };
    return s;
}

const std::vector<MaterialSectionSpec>& MaterialFile::MouldSections()
{
    static const std::vector<MaterialSectionSpec> s = {
        { "material",   G::Identity,
          "IDENTITY " U_DASH " name is what the material dropdowns show.", "Identity" },
        { "physical",   G::Datasheet,  "Physical properties.", "Physical" },
        { "thermal",    G::Datasheet,
          "Thermal properties. Conductivity, specific heat and density set how\n"
          "quickly the cavity wall draws heat out of the melt.", "Thermal" },
        { "mechanical", G::Datasheet,  "Mechanical properties.", "Mechanical" },
        { "additional.thermal", G::Additional,
          "Measured thermal data.", "Measured thermal" },
    };
    return s;
}

const std::vector<IF>& MaterialFile::InjectionFields()
{
    static const std::vector<IF> f = {
        // ---- [material] ----------------------------------------------------
        { "material", "name",         G::Identity, "Name", "",
          "shown in the material dropdowns (required)", nullptr, &I::name },
        { "material", "manufacturer", G::Identity, "Manufacturer", "",
          "e.g. LyondellBasell", nullptr, &I::manufacturer },
        { "material", "grade",        G::Identity, "Grade", "",
          "trade name / grade, e.g. Moplen HP500N", nullptr, &I::grade },
        { "material", "family",       G::Identity, "Polymer family", "",
          "ISO 1043 abbreviation, e.g. PP, ABS, PA6, PC", nullptr, &I::family },
        { "material", "structure",    G::Identity, "Structure", "",
          "semi-crystalline or amorphous", nullptr, &I::structure },
        { "material", "filler",       G::Identity, "Filler / reinforcement", "",
          "\"none\" for an unfilled grade, e.g. 30% glass fibre, 20% talc", nullptr, &I::filler },
        { "material", "notes",        G::Identity, "Notes", "", "", nullptr, &I::notes },

        // ---- [physical] ----------------------------------------------------
        { "physical", "density",          G::Datasheet, "Density", "g/cm" U_SUP3,
          "solid, 23 " U_DEGC " (ISO 1183 / ASTM D792)", &I::density },
        { "physical", "melt_flow_rate",   G::Datasheet, "Melt flow rate (MFR)", "g/10 min",
          "ISO 1133 / ASTM D1238", &I::meltFlowRate },
        { "physical", "mfr_temperature",  G::Datasheet, "MFR test temperature", U_DEGC,
          "e.g. 230 for PP", &I::mfrTemperature },
        { "physical", "mfr_load",         G::Datasheet, "MFR test load", "kg",
          "e.g. 2.16", &I::mfrLoad },
        { "physical", "melt_volume_rate", G::Datasheet, "Melt volume rate (MVR)", "cm" U_SUP3 "/10 min",
          "ISO 1133, same test conditions as the MFR", &I::meltVolumeRate },
        { "physical", "shrinkage_flow",   G::Datasheet, "Moulding shrinkage, flow", "%",
          "parallel to flow (ISO 294-4)", &I::shrinkageFlow },
        { "physical", "shrinkage_cross",  G::Datasheet, "Moulding shrinkage, cross-flow", "%",
          "normal to flow (ISO 294-4)", &I::shrinkageCross },

        // ---- [thermal] -----------------------------------------------------
        { "thermal", "melting_temp",          G::Datasheet, "Melting temperature", U_DEGC,
          "DSC peak, 10 " U_DEGC "/min (ISO 11357-3); semi-crystalline only", &I::meltingTemp },
        { "thermal", "glass_transition_temp", G::Datasheet, "Glass transition temperature", U_DEGC,
          "DSC midpoint (ISO 11357-2)", &I::glassTransitionTemp },
        { "thermal", "vicat_temp",            G::Datasheet, "Vicat softening temperature", U_DEGC,
          "B50: 50 N, 50 " U_DEGC "/h (ISO 306)", &I::vicatTemp },
        { "thermal", "hdt_045",               G::Datasheet, "Heat deflection temp. @ 0.45 MPa", U_DEGC,
          "ISO 75-2/B", &I::hdt045 },
        { "thermal", "hdt_180",               G::Datasheet, "Heat deflection temp. @ 1.80 MPa", U_DEGC,
          "ISO 75-2/A", &I::hdt180 },
        { "thermal", "thermal_conductivity",  G::Datasheet, "Thermal conductivity", "W/(m" U_DOT "K)",
          "as published (usually solid)", &I::thermalConductivity },
        { "thermal", "specific_heat",         G::Datasheet, "Specific heat capacity", "J/(kg" U_DOT "K)",
          "as published (usually solid)", &I::specificHeat },
        { "thermal", "clte_flow",             G::Datasheet, "Thermal expansion (CLTE), flow", "1e-6/K",
          "ISO 11359-2, 23-55 " U_DEGC, &I::clteFlow },
        { "thermal", "clte_cross",            G::Datasheet, "Thermal expansion (CLTE), cross-flow", "1e-6/K",
          "ISO 11359-2, 23-55 " U_DEGC, &I::clteCross },

        // ---- [mechanical] --------------------------------------------------
        { "mechanical", "tensile_modulus",  G::Datasheet, "Tensile modulus", "MPa",
          "ISO 527-1/-2, 1 mm/min", &I::tensileModulus },
        { "mechanical", "flexural_modulus", G::Datasheet, "Flexural modulus", "MPa",
          "ISO 178 / ASTM D790 (used when no tensile modulus is given)", &I::flexuralModulus },
        { "mechanical", "poisson_ratio",    G::Datasheet, "Poisson's ratio", "",
          "", &I::poissonRatio },

        // ---- [processing] --------------------------------------------------
        { "processing", "melt_temp_min",    G::Datasheet, "Melt temperature, min", U_DEGC, "", &I::meltTempMin },
        { "processing", "melt_temp_max",    G::Datasheet, "Melt temperature, max", U_DEGC, "", &I::meltTempMax },
        { "processing", "melt_temp",        G::Datasheet, "Melt temperature, recommended", U_DEGC,
          "blank = middle of the range", &I::meltTemp },
        { "processing", "mould_temp_min",   G::Datasheet, "Mould temperature, min", U_DEGC, "", &I::mouldTempMin },
        { "processing", "mould_temp_max",   G::Datasheet, "Mould temperature, max", U_DEGC, "", &I::mouldTempMax },
        { "processing", "mould_temp",       G::Datasheet, "Mould temperature, recommended", U_DEGC,
          "blank = middle of the range", &I::mouldTemp },
        { "processing", "ejection_temp",    G::Datasheet, "Ejection temperature", U_DEGC,
          "safe-to-eject part temperature", &I::ejectionTemp },
        { "processing", "max_shear_rate",   G::Datasheet, "Maximum shear rate", "1/s",
          "above this the melt risks degradation / surface defects", &I::maxShearRate },
        { "processing", "max_shear_stress", G::Datasheet, "Maximum shear stress", "MPa",
          "", &I::maxShearStress },

        // ---- [additional.thermal] ------------------------------------------
        { "additional.thermal", "melt_density",              G::Additional, "Melt density", "g/cm" U_SUP3,
          "at processing temperature, zero pressure", &I::meltDensity },
        { "additional.thermal", "melt_specific_heat",        G::Additional, "Melt specific heat", "J/(kg" U_DOT "K)",
          "melt state", &I::meltSpecificHeat },
        { "additional.thermal", "melt_thermal_conductivity", G::Additional, "Melt thermal conductivity", "W/(m" U_DOT "K)",
          "melt state", &I::meltThermalConductivity },
        { "additional.thermal", "no_flow_temp",              G::Additional, "No-flow temperature", U_DEGC,
          "below this the melt stops flowing (frozen-layer edge)", &I::noFlowTemp },

        // ---- [additional.cross_wlf] ----------------------------------------
        { "additional.cross_wlf", "n",        G::Additional, "n",        "",       "power-law index", &I::cwlfN },
        { "additional.cross_wlf", "tau_star", G::Additional, "tau*",     "Pa",     "critical shear stress", &I::cwlfTauStar },
        { "additional.cross_wlf", "d1",       G::Additional, "D1",       "Pa" U_DOT "s", "zero-shear viscosity scale", &I::cwlfD1 },
        { "additional.cross_wlf", "d2",       G::Additional, "D2",       "K",      "reference temperature", &I::cwlfD2 },
        { "additional.cross_wlf", "d3",       G::Additional, "D3",       "K/Pa",   "pressure dependence of T*", &I::cwlfD3 },
        { "additional.cross_wlf", "a1",       G::Additional, "A1",       "",       "WLF constant", &I::cwlfA1 },
        { "additional.cross_wlf", "a2",       G::Additional, "A2 (\xC3\x83" "2)", "K", "WLF constant at zero pressure", &I::cwlfA2 },

        // ---- [additional.tait_pvt] -----------------------------------------
        { "additional.tait_pvt", "b1m", G::Additional, "b1m", "m" U_SUP3 "/kg",          "", &I::taitB1m },
        { "additional.tait_pvt", "b2m", G::Additional, "b2m", "m" U_SUP3 "/(kg" U_DOT "K)", "", &I::taitB2m },
        { "additional.tait_pvt", "b3m", G::Additional, "b3m", "Pa",                        "", &I::taitB3m },
        { "additional.tait_pvt", "b4m", G::Additional, "b4m", "1/K",                       "", &I::taitB4m },
        { "additional.tait_pvt", "b1s", G::Additional, "b1s", "m" U_SUP3 "/kg",          "", &I::taitB1s },
        { "additional.tait_pvt", "b2s", G::Additional, "b2s", "m" U_SUP3 "/(kg" U_DOT "K)", "", &I::taitB2s },
        { "additional.tait_pvt", "b3s", G::Additional, "b3s", "Pa",                        "", &I::taitB3s },
        { "additional.tait_pvt", "b4s", G::Additional, "b4s", "1/K",                       "", &I::taitB4s },
        { "additional.tait_pvt", "b5",  G::Additional, "b5",  "K",    "transition temperature at zero pressure", &I::taitB5 },
        { "additional.tait_pvt", "b6",  G::Additional, "b6",  "K/Pa", "pressure dependence of the transition", &I::taitB6 },
        { "additional.tait_pvt", "b7",  G::Additional, "b7",  "m" U_SUP3 "/kg", "", &I::taitB7 },
        { "additional.tait_pvt", "b8",  G::Additional, "b8",  "1/K",  "", &I::taitB8 },
        { "additional.tait_pvt", "b9",  G::Additional, "b9",  "1/Pa", "", &I::taitB9 },
    };
    return f;
}

const std::vector<MF>& MaterialFile::MouldFields()
{
    static const std::vector<MF> f = {
        // ---- [material] ----------------------------------------------------
        { "material", "name",         G::Identity, "Name", "",
          "shown in the material dropdowns (required)", nullptr, &M::name },
        { "material", "manufacturer", G::Identity, "Manufacturer", "",
          "e.g. Uddeholm, Alcoa", nullptr, &M::manufacturer },
        { "material", "grade",        G::Identity, "Grade", "",
          "e.g. P20, H13, 420 stainless, 7075-T6, QC-10", nullptr, &M::grade },
        { "material", "family",       G::Identity, "Material family", "",
          "e.g. tool steel, stainless steel, aluminium alloy, copper alloy, printed resin",
          nullptr, &M::family },
        { "material", "notes",        G::Identity, "Notes", "", "", nullptr, &M::notes },

        // ---- [physical] ----------------------------------------------------
        { "physical", "density", G::Datasheet, "Density", "g/cm" U_SUP3, "", &M::density },

        // ---- [thermal] -----------------------------------------------------
        { "thermal", "thermal_conductivity", G::Datasheet, "Thermal conductivity", "W/(m" U_DOT "K)",
          "at or near room temperature", &M::thermalConductivity },
        { "thermal", "specific_heat",        G::Datasheet, "Specific heat capacity", "J/(kg" U_DOT "K)",
          "", &M::specificHeat },
        { "thermal", "clte",                 G::Datasheet, "Thermal expansion (CLTE)", "1e-6/K",
          "", &M::clte },
        { "thermal", "max_service_temp",     G::Datasheet, "Maximum service temperature", U_DEGC,
          "highest continuous working temperature (matters for printed / resin moulds)",
          &M::maxServiceTemp },

        // ---- [mechanical] --------------------------------------------------
        { "mechanical", "elastic_modulus", G::Datasheet, "Elastic modulus", "GPa", "", &M::elasticModulus },
        { "mechanical", "poisson_ratio",   G::Datasheet, "Poisson's ratio", "", "", &M::poissonRatio },
        { "mechanical", "yield_strength",  G::Datasheet, "Yield strength", "MPa",
          "0.2% proof stress", &M::yieldStrength },
        { "mechanical", "hardness",        G::Datasheet, "Hardness", "",
          "as published, e.g. 28-32 HRC", nullptr, &M::hardness },

        // ---- [additional.thermal] ------------------------------------------
        { "additional.thermal", "thermal_diffusivity", G::Additional, "Thermal diffusivity", "mm" U_SUP2 "/s",
          "measured (e.g. laser flash); fills in conductivity when that is blank",
          &M::thermalDiffusivity },
    };
    return f;
}

// ===========================================================================
// Public API
// ===========================================================================

bool MaterialFile::ParseValue(const std::string& text, double& out)
{
    return ParseNumber(Trim(text), out);
}

std::string MaterialFile::FormatValue(double v)
{
    return FormatNumber(v);
}

const char* MaterialFile::KindKeyword(MaterialKind kind)
{
    return kind == MaterialKind::Injection ? "injection" : "mould";
}

bool MaterialFile::Load(const std::string& path, InjectionMaterialData& out,
                        std::string& error, std::vector<std::string>* warnings)
{
    return LoadImpl(path, out, MaterialKind::Injection, InjectionFields(), error, warnings);
}

bool MaterialFile::Load(const std::string& path, MouldMaterialData& out,
                        std::string& error, std::vector<std::string>* warnings)
{
    return LoadImpl(path, out, MaterialKind::Mould, MouldFields(), error, warnings);
}

bool MaterialFile::Save(const std::string& path, const InjectionMaterialData& data,
                        std::string& error)
{
    return SaveImpl(path, data, MaterialKind::Injection, InjectionSections(), InjectionFields(), error);
}

bool MaterialFile::Save(const std::string& path, const MouldMaterialData& data,
                        std::string& error)
{
    return SaveImpl(path, data, MaterialKind::Mould, MouldSections(), MouldFields(), error);
}

// Same anchor as the fixtures folder (FixtureEditor / StartupDialog): the
// directory holding the running executable.
std::string MaterialFile::LibraryRoot()
{
    return (fs::path(wxStandardPaths::Get().GetExecutablePath().ToStdString())
            .parent_path() / "Materials").string();
}

std::string MaterialFile::LibraryFolder(MaterialKind kind)
{
    return (fs::path(LibraryRoot()) /
            (kind == MaterialKind::Injection ? "Injection Materials" : "Mould Materials")).string();
}

bool MaterialFile::EnsureLibraryFolders(std::string& error)
{
    for (MaterialKind k : { MaterialKind::Injection, MaterialKind::Mould })
    {
        std::error_code ec;
        const std::string dir = LibraryFolder(k);
        fs::create_directories(dir, ec);
        if (ec)
        {
            error = "Could not create material library folder " + dir + ": " + ec.message();
            return false;
        }
    }
    return true;
}
