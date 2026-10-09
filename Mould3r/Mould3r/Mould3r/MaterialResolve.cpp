#include "MaterialResolve.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <optional>

// UTF-8 pieces (escaped, one literal each — see MaterialFile.cpp).
#define U_DEGC  "\xC2\xB0" "C"
#define U_DASH  "\xE2\x80\x94"
#define U_NDASH "\xE2\x80\x93"
#define U_TIMES "\xC3\x97"
#define U_ETA0  "\xCE\xB7" "0"
#define U_TAU   "\xCF\x84"
#define U_DOT   "\xC2\xB7"
#define U_SUP3  "\xC2\xB3"

namespace
{
    const TestMaterial::PolymerMaterial& kPP = TestMaterial::kGenericPolypropylene;
    const TestMaterial::MouldMaterial&   kSteel = TestMaterial::kMouldSteel;
    constexpr double kZeroC = TestMaterial::kZeroC;
    constexpr double kPi = 3.14159265358979323846;

    // Locale-independent number text for the notes.
    std::string Fx(double v, int decimals)
    {
        char b[64];
        const auto r = std::to_chars(b, b + sizeof(b), v, std::chars_format::fixed, decimals);
        return r.ec == std::errc() ? std::string(b, r.ptr) : std::string("?");
    }
    std::string Fg(double v, int sig = 3)
    {
        char b[64];
        const auto r = std::to_chars(b, b + sizeof(b), v, std::chars_format::general, sig);
        return r.ec == std::errc() ? std::string(b, r.ptr) : std::string("?");
    }
    std::string DegC(double c) { return Fx(c, 0) + " " U_DEGC; }

    enum class Structure { Unknown, SemiCrystalline, Amorphous };

    Structure ParseStructure(std::string s)
    {
        for (char& c : s)
            if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
        if (s.find("amorph") != std::string::npos) return Structure::Amorphous;
        if (s.find("semi") != std::string::npos || s.find("crystal") != std::string::npos)
            return Structure::SemiCrystalline;
        return Structure::Unknown;
    }

    bool Pos(const std::optional<double>& v) { return v.has_value() && *v > 0.0; }

    struct Notes
    {
        std::vector<MaterialNote>& out;
        std::vector<std::string> pending;   // fields for the next Fallback (see Fields)
        size_t Derived(unsigned uses, const std::string& qty, const std::string& msg)
        {
            out.push_back({ MaterialNote::Level::Derived, uses, qty, msg, {} });
            return out.size() - 1;
        }
        // N.Fields({...}).Fallback(...): the inputs whose absence caused it.
        Notes& Fields(std::vector<std::string> f) { pending = std::move(f); return *this; }
        size_t Fallback(unsigned uses, const std::string& qty, const std::string& msg)
        {
            out.push_back({ MaterialNote::Level::Fallback, uses, qty, msg, std::move(pending) });
            pending.clear();
            return out.size() - 1;
        }
    };

    const char* kGenericPP = " (generic Polypropylene)";

    // ISO 1133 melt-flow indexer: die 2.095 mm x 8.000 mm, barrel 9.550 mm.
    // One (load, flow) point + the Cross-WLF shape constants in `cw` -> D1.
    // Wall shear stress from the load (no entrance correction); wall shear
    // rate = apparent rate with the Rabinowitsch correction at the model's
    // local slope, iterated to consistency. Returns false if unsolvable.
    bool FitCrossWlfD1(double flowM3s, double loadKg, double testTempC,
                       TestMaterial::CrossWLF& cw, double& eta0AtTest)
    {
        const double R = 2.095e-3 / 2.0, L = 8.0e-3, Rb = 9.55e-3 / 2.0, g = 9.80665;
        if (!(flowM3s > 0.0) || !(loadKg > 0.0) || !(cw.n > 0.0 && cw.n < 1.0) || !(cw.tauStar > 0.0))
            return false;
        const double dP = loadKg * g / (kPi * Rb * Rb);          // Pa
        const double tauW = dP * R / (2.0 * L);                  // Pa
        const double gammaA = 4.0 * flowM3s / (kPi * R * R * R); // 1/s

        auto cross = [&](double e0, double gam)
        { return e0 / (1.0 + std::pow(e0 * gam / cw.tauStar, 1.0 - cw.n)); };

        double nEff = cw.n, eta0 = 0.0;
        for (int it = 0; it < 12; ++it)
        {
            const double gammaW = gammaA * (3.0 * nEff + 1.0) / (4.0 * nEff);
            const double etaW = tauW / gammaW;
            // cross(e0) is increasing in e0 and equals < etaW at e0 = etaW.
            double lo = etaW, hi = etaW * 2.0;
            int guard = 0;
            while (cross(hi, gammaW) < etaW && guard++ < 200) hi *= 2.0;
            if (guard >= 200) return false;
            for (int b = 0; b < 200; ++b)
            {
                const double mid = std::sqrt(lo * hi);
                (cross(mid, gammaW) < etaW ? lo : hi) = mid;
            }
            eta0 = std::sqrt(lo * hi);
            const double x = std::pow(eta0 * gammaW / cw.tauStar, 1.0 - cw.n);
            nEff = 1.0 - (1.0 - cw.n) * x / (1.0 + x);
        }

        const double T = testTempC + kZeroC;
        const double dT = T - cw.D2;
        const double den = cw.A2tilde + dT;
        if (!(den > 0.0) || !std::isfinite(eta0)) return false;
        cw.D1 = eta0 / std::exp(-cw.A1 * dT / den);
        eta0AtTest = eta0;
        return std::isfinite(cw.D1) && cw.D1 > 0.0;
    }
} // namespace

// ===========================================================================
// Injection material
// ===========================================================================
ResolvedInjectionMaterial ResolveInjectionMaterial(const InjectionMaterialData& d)
{
    ResolvedInjectionMaterial R;
    R.name = d.name.empty() ? std::string("Unnamed material") : d.name;
    TestMaterial::PolymerMaterial& P = R.props;
    P = kPP;          // every field starts at its fallback; overridden below
    P.name = "";      // see the header: use R.name
    Notes N{ R.notes, {} };

    // Structure: stated, else inferred from which transition is given.
    Structure st = ParseStructure(d.structure);
    if (st == Structure::Unknown)
    {
        if (d.meltingTemp) st = Structure::SemiCrystalline;
        else if (d.glassTransitionTemp) st = Structure::Amorphous;
    }

    const bool solidReal = Pos(d.density);
    if (solidReal) P.densitySolid = *d.density * 1000.0;

    // ---- Melt temperature: recommended, then the window ----------------------
    std::optional<double> recMelt;
    if (d.meltTemp) recMelt = *d.meltTemp;
    else if (d.meltTempMin && d.meltTempMax)
    {
        recMelt = 0.5 * (*d.meltTempMin + *d.meltTempMax);
        N.Derived(MatUseFill, "Recommended melt temperature",
                  DegC(*recMelt) + ", the middle of the " + Fx(*d.meltTempMin, 0) + U_NDASH +
                  DegC(*d.meltTempMax) + " range.");
    }
    else if (d.meltTempMin || d.meltTempMax)
    {
        recMelt = d.meltTempMin ? *d.meltTempMin : *d.meltTempMax;
        N.Derived(MatUseFill, "Recommended melt temperature",
                  DegC(*recMelt) + ", the only bound of the melt range given.");
    }
    else if (d.mfrTemperature)
    {
        recMelt = *d.mfrTemperature;
        N.Derived(MatUseFill, "Recommended melt temperature",
                  DegC(*recMelt) + ", the MFR test temperature (no processing temperatures given).");
    }
    if (recMelt) P.recMeltTempC = *recMelt;
    else
        N.Fields({"processing.melt_temp"}).Fallback(MatUseFill, "Recommended melt temperature",
                   "not given " U_DASH " using " + DegC(kPP.recMeltTempC) + kGenericPP + ".");

    if (d.meltTempMin && d.meltTempMax)
    {
        P.meltTempMinC = std::min(*d.meltTempMin, *d.meltTempMax);
        P.meltTempMaxC = std::max(*d.meltTempMin, *d.meltTempMax);
        if (*d.meltTempMin > *d.meltTempMax)
            N.Derived(MatUseFill, "Melt temperature range",
                      "min and max were given the wrong way round " U_DASH " read as " +
                      Fx(P.meltTempMinC, 0) + U_NDASH + DegC(P.meltTempMaxC) + ".");
    }
    else if (recMelt)
    {
        P.meltTempMinC = d.meltTempMin ? *d.meltTempMin : *recMelt - 30.0;
        P.meltTempMaxC = d.meltTempMax ? *d.meltTempMax : *recMelt + 30.0;
        N.Derived(MatUseFill, "Melt temperature range",
                  Fx(P.meltTempMinC, 0) + U_NDASH + DegC(P.meltTempMaxC) +
                  " (30 " U_DEGC " either side of the recommended temperature where a bound is missing).");
    }
    else
        N.Fields({"processing.melt_temp_min", "processing.melt_temp_max"}).Fallback(MatUseFill, "Melt temperature range",
                   "not given " U_DASH " using " + Fx(kPP.meltTempMinC, 0) + U_NDASH +
                   DegC(kPP.meltTempMaxC) + kGenericPP + ".");
    // Keep the recommended temperature inside the window (the Flow card clamps
    // the melt temperature to it).
    if (P.recMeltTempC < P.meltTempMinC || P.recMeltTempC > P.meltTempMaxC)
    {
        P.meltTempMinC = std::min(P.meltTempMinC, P.recMeltTempC);
        P.meltTempMaxC = std::max(P.meltTempMaxC, P.recMeltTempC);
        N.Derived(MatUseFill, "Melt temperature range",
                  "widened to " + Fx(P.meltTempMinC, 0) + U_NDASH + DegC(P.meltTempMaxC) +
                  " to include the recommended melt temperature.");
    }
    const double recMeltK = P.recMeltTempC + kZeroC;

    // ---- Mould temperature (seeds the Flow card field) -----------------------
    if (d.mouldTemp) P.recMouldTempC = *d.mouldTemp;
    else if (d.mouldTempMin && d.mouldTempMax)
    {
        P.recMouldTempC = 0.5 * (*d.mouldTempMin + *d.mouldTempMax);
        N.Derived(MatUseThermal, "Recommended mould temperature",
                  DegC(P.recMouldTempC) + ", the middle of the " + Fx(*d.mouldTempMin, 0) + U_NDASH +
                  DegC(*d.mouldTempMax) + " range.");
    }
    else if (d.mouldTempMin || d.mouldTempMax)
    {
        P.recMouldTempC = d.mouldTempMin ? *d.mouldTempMin : *d.mouldTempMax;
        N.Derived(MatUseThermal, "Recommended mould temperature",
                  DegC(P.recMouldTempC) + ", the only bound of the mould range given.");
    }
    else
        N.Fields({"processing.mould_temp"}).Fallback(MatUseThermal, "Recommended mould temperature",
                   "not given " U_DASH " using " + DegC(kPP.recMouldTempC) + kGenericPP + ".");

    // ---- Tait pvT (given set) --------------------------------------------------
    const std::optional<double>* tb[10] = {
        &d.taitB1m, &d.taitB2m, &d.taitB3m, &d.taitB4m,
        &d.taitB1s, &d.taitB2s, &d.taitB3s, &d.taitB4s, &d.taitB5, &d.taitB6 };
    const char* tbName[10] = { "b1m", "b2m", "b3m", "b4m", "b1s", "b2s", "b3s", "b4s", "b5", "b6" };
    int taitCount = 0;
    std::string taitMissing;
    for (int i = 0; i < 10; ++i)
    {
        if (tb[i]->has_value()) ++taitCount;
        else taitMissing += (taitMissing.empty() ? "" : ", ") + std::string(tbName[i]);
    }
    const bool taitComplete = (taitCount == 10);
    if (taitComplete)
    {
        P.pvt = { *d.taitB1m, *d.taitB2m, *d.taitB3m, *d.taitB4m,
                  *d.taitB1s, *d.taitB2s, *d.taitB3s, *d.taitB4s,
                  *d.taitB5, *d.taitB6, kPP.pvt.C };
        if ((d.taitB7 && *d.taitB7 != 0.0) || (d.taitB8 && *d.taitB8 != 0.0) || (d.taitB9 && *d.taitB9 != 0.0))
            N.Derived(MatUsePack, "Tait pvT",
                      "b7" U_NDASH "b9 (the crystallisation term) aren't modelled by the solver yet " U_DASH
                      " the pvT uses b1m" U_NDASH "b6 only.");
    }

    // ---- Melt density -----------------------------------------------------------
    bool meltReal = true;
    size_t meltNote = SIZE_MAX;
    if (Pos(d.meltDensity)) P.densityMelt = *d.meltDensity * 1000.0;
    else if (taitComplete && P.pvt.specificVolume(recMeltK, 0.0) > 0.0)
    {
        P.densityMelt = 1.0 / P.pvt.specificVolume(recMeltK, 0.0);
        meltNote = N.Derived(MatUseThermal, "Melt density",
                             Fx(P.densityMelt / 1000.0, 3) + " g/cm" U_SUP3 " from the Tait pvT at " +
                             DegC(P.recMeltTempC) + ".");
    }
    else if (Pos(d.meltFlowRate) && Pos(d.meltVolumeRate))
    {
        P.densityMelt = *d.meltFlowRate / *d.meltVolumeRate * 1000.0;
        meltNote = N.Derived(MatUseThermal, "Melt density",
                             Fx(P.densityMelt / 1000.0, 3) + " g/cm" U_SUP3 " = MFR / MVR" +
                             (d.mfrTemperature ? " (at " + DegC(*d.mfrTemperature) + ")" : std::string()) + ".");
    }
    else if (solidReal)
    {
        const double ratio = st == Structure::SemiCrystalline ? 0.83 : st == Structure::Amorphous ? 0.89 : 0.86;
        P.densityMelt = P.densitySolid * ratio;
        meltNote = N.Derived(MatUseThermal, "Melt density",
                             Fx(P.densityMelt / 1000.0, 3) + " g/cm" U_SUP3 " = solid density " U_TIMES " " +
                             Fx(ratio, 2) + " (typical for " +
                             (st == Structure::SemiCrystalline ? "semi-crystalline" :
                              st == Structure::Amorphous ? "amorphous" : "an unknown-structure") +
                             " polymers " U_DASH " a rough estimate).");
    }
    else
    {
        meltReal = false;
        meltNote = N.Fields({"physical.density"}).Fallback(MatUseThermal, "Melt density",
                              "not given and can't be derived (needs melt_density, the Tait set, MFR + MVR, "
                              "or the density) " U_DASH " using " + Fx(kPP.densityMelt / 1000.0, 3) +
                              " g/cm" U_SUP3 + kGenericPP + ".");
    }

    // ---- Viscosity (Cross-WLF) -----------------------------------------------------
    {
        const bool gN = d.cwlfN.has_value(), gT = d.cwlfTauStar.has_value(), gD1 = d.cwlfD1.has_value(),
                   gD2 = d.cwlfD2.has_value(), gA1 = d.cwlfA1.has_value(), gA2 = d.cwlfA2.has_value();
        const bool core = gN && gT && gD1 && gD2 && gA1 && gA2;
        const bool anyGiven = gN || gT || gD1 || gD2 || gA1 || gA2 || d.cwlfD3.has_value();
        std::string missing;
        auto miss = [&](bool g, const char* k) { if (!g) missing += (missing.empty() ? "" : ", ") + std::string(k); };
        miss(gN, "n"); miss(gT, "tau_star"); miss(gD1, "d1"); miss(gD2, "d2"); miss(gA1, "a1"); miss(gA2, "a2");

        bool done = false, outOfRange = false;
        if (core)
        {
            TestMaterial::CrossWLF cw{ *d.cwlfN, *d.cwlfTauStar, *d.cwlfD1, *d.cwlfD2,
                                       d.cwlfD3.value_or(0.0), *d.cwlfA1, *d.cwlfA2 };
            const double e0 = cw.eta0(recMeltK);
            if (cw.n > 0.0 && cw.n < 1.0 && cw.tauStar > 0.0 && cw.D1 > 0.0 && std::isfinite(e0) && e0 > 0.0)
            {
                P.viscosity = cw;
                done = true;
            }
            else
                outOfRange = true;
        }

        const bool haveFlow = Pos(d.meltVolumeRate) || Pos(d.meltFlowRate);
        const bool mfrTest = haveFlow && d.mfrTemperature && Pos(d.mfrLoad);
        if (!done && mfrTest)
        {
            TestMaterial::CrossWLF cw = kPP.viscosity;
            std::vector<std::string> generic;
            if (gN) cw.n = *d.cwlfN; else generic.push_back("n = " + Fg(cw.n));
            if (gT) cw.tauStar = *d.cwlfTauStar; else generic.push_back(U_TAU "* = " + Fg(cw.tauStar / 1000.0) + " kPa");
            cw.D3 = d.cwlfD3.value_or(0.0);
            std::string tempBasis;
            const double wlfA1 = 17.44, wlfA2 = 51.6;   // universal WLF (Tg reference)
            if (gD2)
            {
                cw.D2 = *d.cwlfD2;
                cw.A1 = gA1 ? *d.cwlfA1 : kPP.viscosity.A1;
                cw.A2tilde = gA2 ? *d.cwlfA2 : kPP.viscosity.A2tilde;
                tempBasis = "given D2";
                if (!gA1 || !gA2) generic.push_back("A1/A2");
            }
            else if (d.glassTransitionTemp)
            {
                cw.D2 = *d.glassTransitionTemp + kZeroC;
                cw.A1 = gA1 ? *d.cwlfA1 : wlfA1;
                cw.A2tilde = gA2 ? *d.cwlfA2 : wlfA2;
                tempBasis = "D2 = Tg (" + DegC(*d.glassTransitionTemp) + ")" +
                            ((!gA1 || !gA2) ? std::string(", universal WLF A1/A2") : std::string());
            }
            else
            {
                cw.A1 = gA1 ? *d.cwlfA1 : kPP.viscosity.A1;
                cw.A2tilde = gA2 ? *d.cwlfA2 : kPP.viscosity.A2tilde;
                generic.push_back("D2" + std::string((!gA1 || !gA2) ? "/A1/A2" : ""));
                tempBasis = "no Tg given";
            }

            // Volumetric flow of the test: MVR directly, else MFR / melt density.
            const bool useMvr = Pos(d.meltVolumeRate);
            const double flow = useMvr ? *d.meltVolumeRate * 1e-6 / 600.0
                                       : (*d.meltFlowRate * 1e-3 / 600.0) / P.densityMelt;
            double eta0Test = 0.0;
            if (FitCrossWlfD1(flow, *d.mfrLoad, *d.mfrTemperature, cw, eta0Test))
            {
                P.viscosity = cw;
                done = true;
                std::string msg = std::string("fitted to the melt-flow test (") +
                    (useMvr ? Fg(*d.meltVolumeRate) + " cm" U_SUP3 "/10 min" : Fg(*d.meltFlowRate) + " g/10 min") +
                    " at " + DegC(*d.mfrTemperature) + " / " + Fg(*d.mfrLoad) + " kg): " U_ETA0 " " +
                    Fg(cw.eta0(recMeltK)) + " Pa" U_DOT "s at " + DegC(P.recMeltTempC) + " (" + tempBasis + ").";
                if (!generic.empty())
                {
                    msg += " Generic PP shape constants: ";
                    for (size_t i = 0; i < generic.size(); ++i) msg += (i ? ", " : "") + generic[i];
                    msg += " " U_DASH " one test point sets the viscosity level, not how it shear-thins.";
                }
                if (anyGiven && !core) msg += " Given Cross-WLF values were used; d1 was fitted.";
                N.Derived(MatUseFill, "Viscosity (Cross-WLF)", msg);
                // A fallback melt density fed the fit: its warning now matters to every run.
                if (!useMvr && !meltReal && meltNote != SIZE_MAX) R.notes[meltNote].uses |= MatUseFill;
            }
        }
        if (!done)
        {
            std::string why;
            if (outOfRange) why = "the Cross-WLF set is out of range (needs 0 < n < 1, tau_star > 0, d1 > 0)";
            else if (anyGiven) why = "incomplete Cross-WLF set (missing " + missing + ")";
            else why = "no Cross-WLF set";
            why += mfrTest ? ", and the melt-flow fit failed"
                           : ", and no complete melt-flow test to fit (needs MFR or MVR, its test temperature and load)";
            // Star the inputs that would let the fit run (or the bad set).
            std::vector<std::string> f;
            if (outOfRange) f = { "additional.cross_wlf.n", "additional.cross_wlf.tau_star", "additional.cross_wlf.d1" };
            else if (mfrTest) f = { "physical.melt_flow_rate" };
            else
            {
                if (!haveFlow) f.push_back("physical.melt_flow_rate");
                if (!d.mfrTemperature) f.push_back("physical.mfr_temperature");
                if (!Pos(d.mfrLoad)) f.push_back("physical.mfr_load");
            }
            N.Fields(std::move(f)).Fallback(MatUseFill, "Viscosity (Cross-WLF)",
                       why + " " U_DASH " using the generic Polypropylene viscosity.");
        }
    }

    // ---- Melt specific heat / conductivity -----------------------------------------
    if (Pos(d.meltSpecificHeat)) P.specificHeat = *d.meltSpecificHeat;
    else if (Pos(d.specificHeat))
    {
        P.specificHeat = *d.specificHeat;
        N.Derived(MatUseThermal, "Melt specific heat",
                  "the datasheet value (" + Fg(*d.specificHeat, 4) + " J/(kg" U_DOT "K)) is used for the melt; "
                  "datasheets usually quote the solid, and melt c_p is typically higher.");
    }
    else
        N.Fields({"thermal.specific_heat"}).Fallback(MatUseThermal, "Melt specific heat",
                   "not given " U_DASH " using " + Fg(kPP.specificHeat, 4) + " J/(kg" U_DOT "K)" + kGenericPP + ".");

    if (Pos(d.meltThermalConductivity)) P.thermalConductivity = *d.meltThermalConductivity;
    else if (Pos(d.thermalConductivity))
    {
        P.thermalConductivity = *d.thermalConductivity;
        N.Derived(MatUseThermal, "Melt thermal conductivity",
                  "the datasheet value (" + Fg(*d.thermalConductivity) + " W/(m" U_DOT "K)) is used for the melt.");
    }
    else
        N.Fields({"thermal.thermal_conductivity"}).Fallback(MatUseThermal, "Melt thermal conductivity",
                   "not given " U_DASH " using " + Fg(kPP.thermalConductivity) + " W/(m" U_DOT "K)" + kGenericPP + ".");

    // ---- No-flow temperature ---------------------------------------------------------
    bool noFlowReal = true;
    if (d.noFlowTemp) P.noFlowTempC = *d.noFlowTemp;
    else
    {
        std::optional<double> nf;
        std::string how;
        if (st != Structure::Amorphous && d.meltingTemp)
        {
            nf = *d.meltingTemp - 25.0;
            how = "melting temperature (" + DegC(*d.meltingTemp) + ") " U_DASH " 25 " U_DEGC;
        }
        else if (st != Structure::SemiCrystalline && (d.glassTransitionTemp || d.vicatTemp))
        {
            const bool tg = d.glassTransitionTemp.has_value();
            nf = (tg ? *d.glassTransitionTemp : *d.vicatTemp) + 30.0;
            how = std::string(tg ? "glass transition (" : "Vicat temperature, standing in for Tg (") +
                  DegC(tg ? *d.glassTransitionTemp : *d.vicatTemp) + ") + 30 " U_DEGC;
        }
        if (nf && *nf < P.meltTempMinC)
        {
            P.noFlowTempC = *nf;
            N.Derived(MatUseThermal, "No-flow temperature", DegC(*nf) + " = " + how + " (estimate).");
        }
        else
        {
            noFlowReal = false;
            std::vector<std::string> f;
            if (nf) f = { "additional.thermal.no_flow_temp" };
            else if (st == Structure::SemiCrystalline) f = { "thermal.melting_temp" };
            else if (st == Structure::Amorphous) f = { "thermal.glass_transition_temp" };
            else f = { "thermal.melting_temp", "thermal.glass_transition_temp" };
            N.Fields(std::move(f)).Fallback(MatUseThermal, "No-flow temperature",
                       (nf ? "the estimate from the " + how + " is not below the melt range"
                           : std::string("not given and can't be estimated (needs no_flow_temp, the melting "
                                         "temperature, or the glass transition / Vicat temperature)")) +
                       " " U_DASH " using " + DegC(kPP.noFlowTempC) + kGenericPP + ".");
        }
    }

    // ---- Ejection temperature -----------------------------------------------------------
    if (d.ejectionTemp) P.ejectionTempC = *d.ejectionTemp;
    else if (d.hdt045 || d.hdt180)
    {
        const bool h045 = d.hdt045.has_value();
        P.ejectionTempC = h045 ? *d.hdt045 : *d.hdt180;
        N.Derived(MatUsePack, "Ejection temperature",
                  DegC(P.ejectionTempC) + std::string(", the heat deflection temperature at ") +
                  (h045 ? "0.45" : "1.80") + " MPa.");
    }
    else
        N.Fields({"processing.ejection_temp"}).Fallback(MatUsePack, "Ejection temperature",
                   "not given and no heat deflection temperature to estimate it from " U_DASH " using " +
                   DegC(kPP.ejectionTempC) + kGenericPP + ".");

    // ---- Tait pvT (when not given in full) ------------------------------------------------
    if (!taitComplete)
    {
        const std::string partial = taitCount > 0
            ? "incomplete Tait set (missing " + taitMissing + ")"
            : std::string("no Tait set");
        if (meltReal && solidReal)
        {
            const double kM = kPP.densityMelt / P.densityMelt;    // specific-volume ratios
            const double kS = kPP.densitySolid / P.densitySolid;
            P.pvt = kPP.pvt;
            P.pvt.b1m *= kM; P.pvt.b2m *= kM;
            P.pvt.b1s *= kS; P.pvt.b2s *= kS;
            if (noFlowReal) P.pvt.b5 = P.noFlowTempC + kZeroC;
            N.Derived(MatUsePack, "Tait pvT",
                      partial + " " U_DASH " the generic Polypropylene pvT scaled to this material's melt and "
                      "solid densities" + (noFlowReal ? ", transition at the no-flow temperature" : "") +
                      ". Packing mass and shrinkage are approximate.");
        }
        else
            N.Fields({"physical.density"}).Fallback(MatUsePack, "Tait pvT",
                       partial + ", and no melt + solid density to scale the generic one " U_DASH
                       " using the generic Polypropylene pvT.");
    }

    // ---- Shear limit / mechanical -------------------------------------------------------------
    if (Pos(d.maxShearRate)) P.maxShearRate = *d.maxShearRate;
    else
        N.Fields({"processing.max_shear_rate"}).Fallback(MatUseFill, "Maximum shear rate",
                   "not given " U_DASH " using " + Fg(kPP.maxShearRate, 6) + " 1/s" + kGenericPP + " for the gate / cavity shear checks.");

    if (Pos(d.tensileModulus)) P.elasticModulusMPa = *d.tensileModulus;
    else if (Pos(d.flexuralModulus))
    {
        P.elasticModulusMPa = *d.flexuralModulus;
        N.Derived(MatUseWarp, "Elastic modulus", "the flexural modulus (" + Fg(*d.flexuralModulus, 4) +
                                                 " MPa), as no tensile modulus is given.");
    }
    else
        N.Fields({"mechanical.tensile_modulus"}).Fallback(MatUseWarp, "Elastic modulus",
                   "not given " U_DASH " using " + Fg(kPP.elasticModulusMPa, 4) + " MPa" + kGenericPP + ".");

    if (d.poissonRatio && *d.poissonRatio > 0.0 && *d.poissonRatio < 0.5) P.poissonRatio = *d.poissonRatio;
    else
        N.Fields({"mechanical.poisson_ratio"}).Fallback(MatUseWarp, "Poisson's ratio",
                   std::string(d.poissonRatio ? "out of range (0" U_NDASH "0.5)" : "not given") +
                   " " U_DASH " using " + Fx(kPP.poissonRatio, 2) + ", typical for polymers.");

    return R;
}

// ===========================================================================
// Mould material
// ===========================================================================
ResolvedMouldMaterial ResolveMouldMaterial(const MouldMaterialData& d)
{
    ResolvedMouldMaterial R;
    R.name = d.name.empty() ? std::string("Unnamed material") : d.name;
    TestMaterial::MouldMaterial& P = R.props;
    P = kSteel;
    P.name = "";
    Notes N{ R.notes, {} };
    const char* kGenericSteel = " (generic P20 steel)";

    const bool rhoReal = Pos(d.density);
    if (rhoReal) P.density = *d.density * 1000.0;
    else
        N.Fields({"physical.density"}).Fallback(MatUseThermal, "Mould density",
                   "not given " U_DASH " using " + Fx(kSteel.density / 1000.0, 2) + " g/cm" U_SUP3 + kGenericSteel + ".");

    const bool kGiven = Pos(d.thermalConductivity), cpGiven = Pos(d.specificHeat);
    const bool alpha = Pos(d.thermalDiffusivity);
    if (kGiven) P.thermalConductivity = *d.thermalConductivity;
    if (cpGiven) P.specificHeat = *d.specificHeat;

    if (!kGiven)
    {
        if (alpha && rhoReal && cpGiven)
        {
            P.thermalConductivity = *d.thermalDiffusivity * 1e-6 * P.density * P.specificHeat;
            N.Derived(MatUseThermal, "Mould thermal conductivity",
                      Fg(P.thermalConductivity) + " W/(m" U_DOT "K) = diffusivity " U_TIMES " density " U_TIMES " c_p.");
        }
        else
            N.Fields({"thermal.thermal_conductivity"}).Fallback(MatUseThermal, "Mould thermal conductivity",
                       "not given and can't be derived (needs thermal_conductivity, or the diffusivity with "
                       "density and specific heat) " U_DASH " using " + Fg(kSteel.thermalConductivity) +
                       " W/(m" U_DOT "K)" + kGenericSteel + ".");
    }
    if (!cpGiven)
    {
        if (alpha && rhoReal && kGiven)
        {
            P.specificHeat = P.thermalConductivity / (P.density * *d.thermalDiffusivity * 1e-6);
            N.Derived(MatUseThermal, "Mould specific heat",
                      Fg(P.specificHeat) + " J/(kg" U_DOT "K) = conductivity / (density " U_TIMES " diffusivity).");
        }
        else
            N.Fields({"thermal.specific_heat"}).Fallback(MatUseThermal, "Mould specific heat",
                       "not given and can't be derived (needs specific_heat, or the diffusivity with density "
                       "and conductivity) " U_DASH " using " + Fg(kSteel.specificHeat) + " J/(kg" U_DOT "K)" +
                       kGenericSteel + ".");
    }
    return R;
}

std::vector<std::string> MaterialUseDescriptions(unsigned uses)
{
    std::vector<std::string> out;
    if (uses & MatUseFill)    out.push_back("Hele-Shaw 2.5D Flow " U_DASH " every run (the fill)");
    if (uses & MatUseThermal) out.push_back("Hele-Shaw 2.5D Flow " U_DASH " thermal runs (frozen layer, melt front temperature)");
    if (uses & MatUsePack)    out.push_back("Hele-Shaw 2.5D Flow " U_DASH " packing, hold and cooling");
    if (uses & MatUseWarp)    out.push_back("Hele-Shaw 2.5D Flow " U_DASH " warpage");
    return out;
}

std::vector<const MaterialNote*> MaterialNotesFor(const std::vector<MaterialNote>& notes,
                                                  unsigned activeUses)
{
    std::vector<const MaterialNote*> out;
    for (const MaterialNote& n : notes)
        if (n.uses & activeUses) out.push_back(&n);
    return out;
}
