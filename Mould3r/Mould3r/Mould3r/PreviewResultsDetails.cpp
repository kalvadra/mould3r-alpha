// ===========================================================================
// PreviewResultsDetails.cpp — PreviewPanel's results reports: the numbers
// behind each results card (Draft Angle Checks, Separation Test, Flow
// Analysis), shown by the card's "Details" button in a ResultsDetailsDialog.
//
// Each Build*Report turns the last run's data into tabs of numerical results
// (the card's Export button saves the same tables as CSV). Split out of
// PreviewPanel.cpp to keep the reporting apart from the analyses themselves.
// ===========================================================================
#include "PreviewPanel.h"

#include "ResultsDetails.h"
#include "RoundedButton.h"
#include "style.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <unordered_map>

namespace RD = ResultsDetails;
using RD::Status;

namespace
{
    wxString U(const char* utf8) { return wxString::FromUTF8(utf8); }
    wxString F(double v, int decimals) { return wxString::Format("%.*f", decimals, v); }
    wxString I(long long v) { return wxString::Format("%lld", v); }
    float Z(float v) { return std::fabs(v) < 0.05f ? 0.0f : v; }   // no "-0.0"
    wxString Pt(const glm::vec3& p) { return wxString::Format("%.1f, %.1f, %.1f", Z(p.x), Z(p.y), Z(p.z)); }
    wxString Size3(const glm::vec3& s)
    {
        return wxString::Format("%.1f", s.x) + U(" \xc3\x97 ") + wxString::Format("%.1f", s.y) +
               U(" \xc3\x97 ") + wxString::Format("%.1f", s.z);
    }
    wxString Pct(double part, double whole, int decimals = 1)
    {
        return (whole > 0.0) ? F(100.0 * part / whole, decimals) + "%" : wxString("-");
    }

    const wxString& Deg()  { static const wxString s = U("\xc2\xb0"); return s; }
    const wxString& DegC() { static const wxString s = U("\xc2\xb0""C"); return s; }
    const wxString& Mm2()  { static const wxString s = U(" mm\xc2\xb2"); return s; }
    const wxString& Mm3()  { static const wxString s = U(" mm\xc2\xb3"); return s; }
    const wxString& Cm3()  { static const wxString s = U(" cm\xc2\xb3"); return s; }
    const wxString& Dash() { static const wxString s = U("\xe2\x80\x94"); return s; }
    const wxString& EnDash() { static const wxString s = U("\xe2\x80\x93"); return s; }

    const char* HalfName(int side) { return side == 0 ? "A" : side == 1 ? "B" : "none"; }

    // Union-find over triangle indices.
    struct DisjointSet
    {
        std::vector<int> p;
        explicit DisjointSet(size_t n) : p(n) { std::iota(p.begin(), p.end(), 0); }
        int Find(int x) { while (p[(size_t)x] != x) { p[(size_t)x] = p[(size_t)p[(size_t)x]]; x = p[(size_t)x]; } return x; }
        void Union(int a, int b) { a = Find(a); b = Find(b); if (a != b) p[(size_t)b] = a; }
    };
}

// ===========================================================================
// Card plumbing
// ===========================================================================
void PreviewPanel::SetResultsReport(int card, const ResultsDetails::Report& report)
{
    if (card < 0 || card >= CardCount) return;
    m_reports[card] = report;
    for (RoundedButton* b : { m_detailsBtn[card], m_exportBtn[card] })
        if (b)
        {
            b->Enable(report.valid);
            b->Refresh();
        }
    if (m_detailsDlg[card]) m_detailsDlg[card]->SetReport(report);
}

void PreviewPanel::ClearResultsReports()
{
    static const char* names[CardCount] = { "Draft Angle Checks", "Separation Test", "Flow Analysis" };
    for (int c = 0; c < CardCount; ++c)
    {
        RD::Report r;
        r.test = names[c];
        r.verdict = "Not run";
        SetResultsReport(c, r);
    }
}

void PreviewPanel::ExportResults(int card)
{
    if (card < 0 || card >= CardCount || !m_reports[card].valid) return;
    RD::ExportCsv(m_reports[card], this);
}

void PreviewPanel::ShowResultsDetails(int card)
{
    if (card < 0 || card >= CardCount) return;
    ResultsDetailsDialog*& dlg = m_detailsDlg[card];
    if (!dlg)
    {
        wxWindow* top = wxGetTopLevelParent(this);
        dlg = new ResultsDetailsDialog(top ? top : this, m_reports[card].test);
        dlg->onOpenView = [this, card](int view) { OpenResultsView(card, view); };
        dlg->SetReport(m_reports[card]);
        dlg->CentreOnParent();
        if (card > 0)   // stagger, so several windows don't stack exactly
            dlg->Move(dlg->GetPosition() + dlg->FromDIP(wxPoint(28 * card, 28 * card)));
    }
    dlg->Show();
    dlg->Raise();
}

// ===========================================================================
// Draft Angle Checks
// ===========================================================================
ResultsDetails::Report PreviewPanel::BuildDraftReport(const DesignChecks::FaceDraftParams& params, int source,
                                                      const wxString& verdict, const wxColour& colour) const
{
    const bool cavity = source == SourceCavity;
    const OwnershipMesh& own = m_ownership[cavity ? SourceCavity : SourceShot];
    const DesignChecks::FaceDraftStats& st = m_lastFaceDraftStats;

    RD::Report R;
    R.valid = true;
    R.test = "Draft Angle Checks";
    R.verdict = verdict;
    R.verdictColour = colour;
    R.view = 1;                                          // Sim Viewer: Draft (ray)

    const float failDeg = params.failDraftDeg, warnDeg = params.warnDraftDeg, eps = params.backdraftEpsDeg;
    // Band per facet: 0 back-draft, 1 fail (not back-draft), 2 warn, 3 pass.
    auto bandOf = [&](float d) { return d < -eps ? 0 : d < failDeg ? 1 : d < warnDeg ? 2 : 3; };

    // Per-triangle data (faceId = split triangle + 1).
    const size_t ntri = own.idx.size() / 3;
    std::vector<int> sampleOfTri(ntri, -1);
    for (size_t i = 0; i < own.samples.size(); ++i)
    {
        const int t = own.samples[i].faceId - 1;
        if (t >= 0 && (size_t)t < ntri) sampleOfTri[(size_t)t] = (int)i;
    }
    auto vpos = [&](unsigned v) { return glm::vec3(own.posNorm[v * 6], own.posNorm[v * 6 + 1], own.posNorm[v * 6 + 2]); };
    auto centroid = [&](size_t t) { return (vpos(own.idx[t * 3]) + vpos(own.idx[t * 3 + 1]) + vpos(own.idx[t * 3 + 2])) / 3.0f; };

    double totalArea = 0.0, bandArea[4] = { 0, 0, 0, 0 };
    int bandCount[4] = { 0, 0, 0, 0 };
    struct HalfAgg { int facets = 0; double area = 0, fail = 0, warn = 0; float minDraft = 90.0f; };
    HalfAgg half[3];   // A, B, none
    for (const DesignChecks::DraftSample& smp : own.samples)
    {
        const int b = bandOf(smp.signedDraftDeg);
        totalArea += smp.area;
        bandArea[b] += smp.area;
        bandCount[b] += 1;
        HalfAgg& h = half[(smp.half == 0 || smp.half == 1) ? smp.half : 2];
        h.facets += 1;
        h.area += smp.area;
        if (b <= 1) h.fail += smp.area;
        else if (b == 2) h.warn += smp.area;
        h.minDraft = std::min(h.minDraft, smp.signedDraftDeg);
    }
    const double failArea = bandArea[0] + bandArea[1];
    const double warnArea = bandArea[2];

    // Connected regions of failing (and of warning) facets, per half, joined
    // through shared (welded-by-position) vertices.
    struct Region { int band = 0, half = -1, facets = 0; double area = 0; float minDraft = 90.0f;
                    glm::dvec3 wsum{ 0.0 }; glm::vec3 lo{ 0.0f }, hi{ 0.0f }; };
    std::vector<Region> regions[2];   // [0] failing, [1] warning
    {
        DisjointSet ds(ntri);
        struct Key { long long x, y, z; int group; bool operator==(const Key& o) const
                     { return x == o.x && y == o.y && z == o.z && group == o.group; } };
        struct KeyHash { size_t operator()(const Key& k) const
        {
            size_t h = std::hash<long long>()(k.x);
            h ^= std::hash<long long>()(k.y) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
            h ^= std::hash<long long>()(k.z) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
            h ^= std::hash<int>()(k.group) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
            return h;
        } };
        std::unordered_map<Key, int, KeyHash> first;
        std::vector<int> groupOf(ntri, -1);
        for (size_t t = 0; t < ntri; ++t)
        {
            if (sampleOfTri[t] < 0) continue;
            const DesignChecks::DraftSample& smp = own.samples[(size_t)sampleOfTri[t]];
            const int b = bandOf(smp.signedDraftDeg);
            if (b == 3) continue;
            const int group = (b <= 1 ? 0 : 1) * 4 + (smp.half + 1);
            groupOf[t] = group;
            for (int k = 0; k < 3; ++k)
            {
                const glm::vec3 p = vpos(own.idx[t * 3 + k]);
                const Key key{ std::llround(p.x * 1.0e4), std::llround(p.y * 1.0e4), std::llround(p.z * 1.0e4), group };
                auto it = first.find(key);
                if (it == first.end()) first.emplace(key, (int)t);
                else ds.Union(it->second, (int)t);
            }
        }
        std::unordered_map<int, size_t> regionOfRoot[2];
        for (size_t t = 0; t < ntri; ++t)
        {
            if (groupOf[t] < 0) continue;
            const DesignChecks::DraftSample& smp = own.samples[(size_t)sampleOfTri[t]];
            const int kind = groupOf[t] / 4;
            const int root = ds.Find((int)t);
            auto it = regionOfRoot[kind].find(root);
            if (it == regionOfRoot[kind].end())
            {
                it = regionOfRoot[kind].emplace(root, regions[kind].size()).first;
                Region r;
                r.band = kind;
                r.half = smp.half;
                r.lo = glm::vec3(std::numeric_limits<float>::max());
                r.hi = glm::vec3(-std::numeric_limits<float>::max());
                regions[kind].push_back(r);
            }
            Region& r = regions[kind][it->second];
            r.facets += 1;
            r.area += smp.area;
            r.minDraft = std::min(r.minDraft, smp.signedDraftDeg);
            const glm::vec3 c = centroid(t);
            r.wsum += glm::dvec3(c) * (double)smp.area;
            for (int k = 0; k < 3; ++k)
            {
                const glm::vec3 p = vpos(own.idx[t * 3 + k]);
                r.lo = glm::min(r.lo, p);
                r.hi = glm::max(r.hi, p);
            }
        }
        for (auto& list : regions)
            std::sort(list.begin(), list.end(), [](const Region& a, const Region& b) { return a.area > b.area; });
    }
    auto regionCentre = [](const Region& r) { return r.area > 0.0 ? glm::vec3(r.wsum / r.area) : (r.lo + r.hi) * 0.5f; };


    const wxString failTxt = F(failDeg, 1) + Deg(), warnTxt = F(warnDeg, 1) + Deg();
    RD::Table sum("Summary");
    sum.Add({ "Mesh analysed", cavity ? wxString("Cavity only (part model meshes)")
                                      : wxString("Whole shot (parts + sprue, runners, gates)") });
    sum.Add({ "Facets analysed", I((long long)own.samples.size()) });
    sum.Add({ "Surface area", F(totalArea, 2) + Mm2() });
    sum.Add({ "Minimum draft", F(st.minDraftDeg, 2) + Deg() },
            st.minDraftDeg < failDeg ? Status::Fail : st.minDraftDeg < warnDeg ? Status::Warn : Status::Pass);
    sum.Add({ "Failing area (< " + failTxt + ")", F(failArea, 2) + Mm2() + "  (" + Pct(failArea, totalArea, 2) + ")" },
            failArea <= 0.0 ? Status::Pass : st.failSuppressed ? Status::Info : Status::Fail);
    sum.Add({ "Warning area (" + F(failDeg, 1) + EnDash() + warnTxt + ")",
              F(warnArea, 2) + Mm2() + "  (" + Pct(warnArea, totalArea, 2) + ")" },
            warnArea <= 0.0 ? Status::Pass : st.warnSuppressed ? Status::Info : Status::Warn);
    sum.Add({ "Back-draft facets", I(bandCount[0]) + "  (" + F(bandArea[0], 2) + Mm2() + ")" },
            bandCount[0] > 0 ? Status::Fail : Status::Pass);
    sum.Add({ "Failing / warning areas", I((long long)regions[0].size()) + " / " + I((long long)regions[1].size()) });
    if (params.significanceValue > 0.0f)
        sum.Add({ "Significance gate", F(st.significanceMm2, 2) + Mm2() +
                                       (st.failSuppressed || st.warnSuppressed ? "  (flagged area below it)" : "") },
                (st.failSuppressed || st.warnSuppressed) ? Status::Info : Status::None);
    if (own.fallback > 0)
        sum.Add({ "Facets that hit no half", I(own.fallback) }, Status::Info);
    R.tables.push_back(sum);


    // Distribution: bands of signed draft.
    {
        RD::Table dist("Distribution", { "Draft", "Facets", U("Area (mm\xc2\xb2)"), "Share of area", "Band" });
        std::vector<float> edges = { -90.0f, -eps, 0.0f, failDeg, warnDeg };
        for (float e : { 5.0f, 10.0f, 30.0f, 90.0f })
            if (e > edges.back()) edges.push_back(e);
        if (edges.back() < 90.0f) edges.push_back(90.0f);
        std::vector<float> clean;
        for (float e : edges) if (clean.empty() || e > clean.back() + 1e-6f) clean.push_back(e);
        const size_t nb = clean.size() - 1;
        std::vector<int> cnt(nb, 0);
        std::vector<double> area(nb, 0.0);
        for (const DesignChecks::DraftSample& smp : own.samples)
        {
            size_t b = 0;
            while (b + 1 < nb && smp.signedDraftDeg >= clean[b + 1]) ++b;
            cnt[b] += 1;
            area[b] += smp.area;
        }
        for (size_t b = 0; b < nb; ++b)
        {
            const float lo = clean[b], hi = clean[b + 1];
            const float mid = 0.5f * (lo + hi);
            const int band = bandOf(mid);
            wxString label;
            if (b == 0 && lo <= -89.9f) label = "below " + U("\xe2\x88\x92") + F(-hi, 2) + Deg() + " (back-draft)";
            else label = (lo < 0 ? U("\xe2\x88\x92") + F(-lo, 2) : F(lo, lo < 1.0f ? 2 : 1)) + Deg() + " to " +
                         (hi < 0 ? U("\xe2\x88\x92") + F(-hi, 2) : F(hi, hi < 1.0f ? 2 : 1)) + Deg();
            const char* bn = band <= 1 ? "fail" : band == 2 ? "warn" : "pass";
            dist.Add({ label, I(cnt[b]), F(area[b], 2), Pct(area[b], totalArea), bn },
                     cnt[b] == 0 ? Status::None : band <= 1 ? Status::Fail : band == 2 ? Status::Warn : Status::Pass);
        }
        R.tables.push_back(dist);
    }

    {
        RD::Table ht("By half", { "Half", "Facets", U("Area (mm\xc2\xb2)"), "Min draft",
                                        U("Failing (mm\xc2\xb2)"), U("Warning (mm\xc2\xb2)") });
        for (int h = 0; h < 3; ++h)
        {
            if (half[h].facets == 0) continue;
            ht.Add({ h == 2 ? wxString("unassigned") : wxString(HalfName(h)), I(half[h].facets), F(half[h].area, 2),
                     F(half[h].minDraft, 2) + Deg(), F(half[h].fail, 2), F(half[h].warn, 2) },
                   half[h].fail > 0.0 ? Status::Fail : half[h].warn > 0.0 ? Status::Warn : Status::Pass);
        }
        R.tables.push_back(ht);
    }

    auto regionTable = [&](int kind, const wxString& title)
    {
        const std::vector<Region>& list = regions[kind];
        if (list.empty()) return;
        RD::Table t(title, { "#", "Half", U("Area (mm\xc2\xb2)"), "Facets", "Min draft", "Centre x, y, z (mm)",
                             U("Size x \xc3\x97 y \xc3\x97 z (mm)") });
        for (size_t i = 0; i < list.size(); ++i)
        {
            const Region& r = list[i];
            t.Add({ I((long long)i + 1), HalfName(r.half), F(r.area, 2), I(r.facets), F(r.minDraft, 2) + Deg(),
                    Pt(regionCentre(r)), Size3(r.hi - r.lo) },
                  kind == 0 ? Status::Fail : Status::Warn);
        }
        R.tables.push_back(t);
    };
    regionTable(0, "Failing areas");
    regionTable(1, "Warning areas");
    return R;
}

// ===========================================================================
// Separation Test
// ===========================================================================
ResultsDetails::Report PreviewPanel::BuildSeparationReport(const SeparationRun& run,
                                                           const wxString& verdict, const wxColour& colour) const
{
    const bool cavity = run.source == SourceCavity;
    RD::Report R;
    R.valid = true;
    R.test = "Separation Test";
    R.verdict = verdict;
    R.verdictColour = colour;
    R.view = 0;                                          // the red interference overlay (OpenResultsView)

    std::vector<SeparationRegion> regs = run.regions;
    std::sort(regs.begin(), regs.end(),
              [](const SeparationRegion& a, const SeparationRegion& b) { return a.volumeMm3 > b.volumeMm3; });

    int tested = 0, collided = 0, notEval = 0, tiny = 0;
    for (int s = 0; s < 2; ++s)
    {
        tested += run.sideSurf[s] ? 1 : 0;
        collided += run.sideStat[s] == 1 ? 1 : 0;
        notEval += (run.sideSurf[s] && run.sideStat[s] == 2) ? 1 : 0;
        tiny += run.sideTiny[s];
    }
    const double total = run.sideVol[0] + run.sideVol[1];


    RD::Table sum("Summary");
    sum.Add({ "Mesh analysed", cavity ? wxString("Cavity only (part model meshes)")
                                      : wxString("Whole shot (parts + sprue, runners, gates)") });
    sum.Add({ "Halves tested", I(tested) });
    sum.Add({ "Halves colliding", I(collided) }, collided > 0 ? Status::Fail : Status::Pass);
    if (notEval > 0) sum.Add({ "Halves not evaluable", I(notEval) }, Status::Warn);
    sum.Add({ "Significant overlap", F(total, 3) + Mm3() }, total > 0.0 ? Status::Fail : Status::Pass);
    sum.Add({ "Regions kept / ignored", I((long long)regs.size()) + " / " + I(tiny) });
    R.tables.push_back(sum);

    RD::Table ht("By half", { "Half", "Owned facets", U("Owned area (mm\xc2\xb2)"), "Result",
                                    U("Overlap (mm\xc2\xb3)"), "Regions", "Ignored" });
    for (int s = 0; s < 2; ++s)
    {
        if (!run.sideSurf[s]) continue;
        const int stt = run.sideStat[s];
        ht.Add({ HalfName(s), I(run.ownedTris[s]), F(run.ownedAreaMm2[s], 2),
                 stt == 1 ? "collision" : stt == 2 ? "not evaluable" : "clear", F(run.sideVol[s], 3),
                 I(run.sideRegions[s]), I(run.sideTiny[s]) },
               stt == 1 ? Status::Fail : stt == 2 ? Status::Warn : Status::Pass);
    }
    R.tables.push_back(ht);

    if (!regs.empty())
    {
        RD::Table rt("Interference", { "#", "Half", U("Volume (mm\xc2\xb3)"), "Centre x, y, z (mm)",
                                               U("Size x \xc3\x97 y \xc3\x97 z (mm)") });
        for (size_t i = 0; i < regs.size(); ++i)
            rt.Add({ I((long long)i + 1), HalfName(regs[i].side), F(regs[i].volumeMm3, 3),
                     Pt((regs[i].lo + regs[i].hi) * 0.5f), Size3(regs[i].hi - regs[i].lo) }, Status::Fail);
        R.tables.push_back(rt);
    }
    return R;
}

// ===========================================================================
// Flow Analysis
// ===========================================================================
ResultsDetails::Report PreviewPanel::BuildFlowReport(const FlowRunInputs& in) const
{
    using EK = Flow::FeedEdgeKind;
    const Flow::FeedNetwork& net = m_feedNetwork;
    const Flow::FeedSolveResult& fs = m_feedSolve;
    const Flow::CoupledFillResult& fr = m_fill;
    const Flow::FillDefects& fd = m_defects;

    RD::Report R;
    R.valid = true;
    R.test = "Flow Analysis";
    R.verdict = m_flowStatus ? m_flowStatus->GetLabel() : wxString();
    R.verdictColour = m_flowStatus ? m_flowStatus->GetForegroundColour() : wxColour(0xB0, 0xB8, 0xC8);

    auto partName = [&](int k) -> wxString
    {
        return (k >= 0 && (size_t)k < fr.parts.size()) ? wxString::FromUTF8(fr.parts[(size_t)k].label.c_str())
                                                       : wxString("?");
    };
    const bool filled = m_hasFill;
    const Flow::PackResult& K = fr.pack;
    // Sim Viewer views: fill time, else the midplane / the feed network.
    bool haveMid = false;
    for (const Flow::MidplaneMesh& m : m_midplanes) haveMid = haveMid || !m.empty();
    R.view = filled ? 7 : haveMid ? 6 : 5;


    // ---- Summary ------------------------------------------------------------------
    RD::Table sum("Summary");
    if (filled)
    {
        const float pct = fr.historyFilledFrac.empty() ? 0.0f : 100.0f * fr.historyFilledFrac.back();
        sum.Add({ "Filled", fr.complete ? wxString("100% of the shot") : F(pct, 1) + "% of the shot" },
                fr.complete ? Status::Pass : Status::Fail);
        if (fr.pressureLimited)   // slowed to stay under the machine limit
            sum.Add({ "Fill time", F(fr.fillTimeS, 3) + " s  (target " + F(in.fillTimeS, 2) + " s)" }, Status::Warn);
        sum.Add({ "Peak injection pressure", F(fr.peakInletPressureMPa, 1) + " MPa  (" +
                                             Pct(fr.peakInletPressureMPa, in.maxInjMPa, 0) + " of the " +
                                             F(in.maxInjMPa, 0) + " MPa limit)" },
                fr.pressureLimited ? Status::Fail : fr.peakInletPressureMPa > 0.8 * in.maxInjMPa ? Status::Warn : Status::Pass);
        sum.Add({ "V/P switchover", F(fr.switchoverInletPressureMPa, 1) + " MPa at " + F(fr.switchoverTimeS, 3) + " s" });
        sum.Add({ "Clamp force", F(fr.clampForceTonne, 2) + " t  over " + F(fr.projectedAreaMm2 / 100.0, 1) +
                                 U(" cm\xc2\xb2 projected") });
        if (fr.thermal)
            sum.Add({ "Melt front temperature", F(fr.minFrontTempC, 0) + EnDash() + F(fr.maxFrontTempC, 0) + DegC() },
                    fr.minFrontTempC < in.noFlowC + 20.0 ? Status::Warn : Status::None);
        if (K.ran)
        {
            sum.Add({ "Part mass", F(K.partsMassG, 2) + " g  (" + F(K.packedMassG, 2) + " g packed in after the fill)" });
            sum.Add({ "Ejectable at", K.ejectReached ? F(K.ejectS, 1) + " s from the start of injection"
                                                     : "not within " + F(K.endS, 0) + " s" },
                    K.ejectReached ? Status::None : Status::Warn);
        }
        sum.Add({ "Weld / meld lines", I(fd.weldCount) + " / " + I(fd.meldCount) });
        sum.Add({ "Air traps", I((long long)fd.traps.size()) }, fd.traps.empty() ? Status::Pass : Status::Warn);
    }
    else if (m_hasFeedSolve)
        sum.Add({ "Feed pressure (steady)", F(fs.inletPressureMPa, 2) + " MPa" });
    R.tables.push_back(sum);

    // What turned the verdict amber, right under the summary.
    if (!in.cautions.empty())
    {
        RD::Table ct("Summary", { "Caution" });
        ct.title = "Cautions";
        for (const wxString& c : in.cautions) ct.Add({ c }, Status::Warn);
        R.tables.push_back(ct);
    }

    // ---- Process --------------------------------------------------------------------
    RD::Table proc("Process");
    proc.Add({ "Injection material", in.material });
    proc.Add({ "Mould material", in.mould });
    proc.Add({ "Melt / mould temperature", F(in.meltC, 0) + DegC() + " / " + F(in.mouldC, 0) + DegC() });
    if (in.thermal) proc.Add({ "Wall contact temperature", F(in.wallC, 1) + DegC() });
    proc.Add({ U("Zero-shear viscosity \xce\xb7""0 at melt"), F(in.eta0, 0) + U(" Pa\xc2\xb7s") });
    proc.Add({ "Fill time (target)", F(in.fillTimeS, 2) + " s" });
    proc.Add({ "Machine pressure limit", F(in.maxInjMPa, 0) + " MPa" });
    proc.Add({ "Model", wxString(in.thermal ? "thermal (frozen layer)" : "isothermal") +
                        (in.pack ? ", pack and cool" : "") });
    if (in.pack)
    {
        proc.Add({ "Pack pressure", F(in.packPct, 0) + "% of the fill's" +
                                    (K.ran ? " = " + F(K.packPressureMPa, 1) + " MPa" : wxString()) });
        proc.Add({ "Hold time", in.holdS <= 0.0 ? (K.ran ? F(K.holdTimeS, 2) + " s (until the gates froze)"
                                                         : wxString("until the gates freeze"))
                                                : F(in.holdS, 2) + " s" });
        proc.Add({ "Flow / cross shrink ratio", F(in.shrinkRatio, 2) });
    }
    proc.Add({ "Midplane triangle area", in.meshAreaMm2 > 0 ? wxString::Format("%.3g", in.meshAreaMm2) + Mm2() : wxString("-") });
    proc.Add({ "No-flow / ejection temp.", F(in.noFlowC, 0) + DegC() + " / " + F(in.ejectC, 0) + DegC() });
    if (filled)
    {
        proc.Add({ "Shot (feed + cavities)", F(fr.feedVolumeMm3 / 1000.0, 3) + " + " + F(fr.cavityVolumeMm3 / 1000.0, 3) + Cm3() });
        proc.Add({ "Injection rate", F(fr.flowRateMm3s / 1000.0, 2) + U(" cm\xc2\xb3/s") });
    }

    // ---- Parts ------------------------------------------------------------------------
    if (filled && !fr.parts.empty())
    {
        RD::Table pt("Fill", { "Part", "Melt arrives (s)", "Full at (s)", "Filled (%)",
                                                    "Max pressure (MPa)", "Coldest front", "Frozen at end of fill" });
        for (const Flow::PartFillResult& p : fr.parts)
        {
            const wxString name = wxString::FromUTF8(p.label.c_str());
            if (!p.fed) { pt.Add({ name, "not reached", "-", "0", "-", "-", "-" }, Status::Fail); continue; }
            const bool cold = fr.thermal && p.minFrontTempC < in.noFlowC + 20.0;
            pt.Add({ name, F(p.fillStartS, 3), p.fillEndS >= 0 ? F(p.fillEndS, 3) : wxString("-"), F(p.filledPct, 1),
                     F(p.maxPressureMPa, 1), fr.thermal ? F(p.minFrontTempC, 0) + DegC() : wxString("-"),
                     fr.thermal && !p.frozenPct.empty() ? F(p.meanFrozenPct, 0) + "% mean, " + F(p.maxFrozenPct, 0) + "% max"
                                                        : wxString("-") },
                   p.filledPct < 99.5f ? Status::Fail : cold ? Status::Warn : Status::Pass);
        }
        pt.view = 7;
        R.tables.push_back(pt);

        if (K.ran)
        {

            RD::Table pa("Packing");
            pa.title = "Pack and hold";
            pa.Add({ "Pack pressure", F(K.packPressureMPa, 1) + " MPa" });
            pa.Add({ "Held for", F(K.holdTimeS, 2) + " s" });
            pa.Add({ "Gates sealed", K.gatesFrozen ? "at " + F(K.gateFreezeS, 2) + " s" : wxString("not before packing ended") },
                   K.gatesFrozen ? Status::None : Status::Info);
            pa.Add({ "Simulated to", F(K.endS, 1) + " s" });
            pa.Add({ "Mass balance (solver check)", F(K.massBalanceErrPct, 2) + "%" });
            pa.view = 13;
            R.tables.push_back(pa);

            RD::Table pk("Packing",
                         { "Part", "Mass (g)", "Vol. shrinkage", "Range", "~Linear", "Ejectable at (s)" });
            for (const Flow::PartFillResult& p : fr.parts)
            {
                if (!p.fed || p.shrinkPct.empty()) continue;
                const bool high = p.maxShrinkPct > 8.0f, uneven = p.maxShrinkPct - p.minShrinkPct > 3.0f;
                pk.Add({ wxString::FromUTF8(p.label.c_str()), F(p.massG, 2), F(p.meanShrinkPct, 1) + "%",
                         F(p.minShrinkPct, 1) + EnDash() + F(p.maxShrinkPct, 1) + "%", F(p.meanShrinkPct / 3.0f, 2) + "%",
                         p.ejectS >= 0 ? F(p.ejectS, 1) : wxString("-") },
                       (high || uneven) ? Status::Warn : Status::Pass);
            }
            pk.title = "Parts";
            pk.view = 13;
            R.tables.push_back(pk);
        }
    }

    // ---- Warpage -----------------------------------------------------------------------
    if (filled && m_warp.ok)
    {
        RD::Table wt("Warpage", { "Part", "Size before (mm)", "Size after (mm)", "Avg linear shrink",
                                  "Max deflection (mm)", "Max warp (mm)", "Warp at x, y, z (mm)", "Out of plane (mm)" });
        for (const Flow::PartWarp& W : m_warp.parts)
        {
            if (!W.ok || W.part < 0 || (size_t)W.part >= fr.parts.size()) continue;
            const float size = std::max({ W.sizeBefore.x, W.sizeBefore.y, W.sizeBefore.z, 1e-3f });
            wt.Add({ partName(W.part), Size3(W.sizeBefore), Size3(W.sizeAfter), F(W.uniformShrinkPct, 2) + "%",
                     F(W.maxDispMm, 3), F(W.maxWarpMm, 3), Pt(W.maxWarpPos), F(W.flatnessMm, 3) },
                   W.maxWarpMm > 0.003f * size ? Status::Warn : Status::Pass);
        }
        wt.note = "Sizes are of the midplane (the wall thickness isn't in it). Warp = shape change with the "
                  "rigid motion and the best uniform shrink removed.";
        wt.view = 16;
        R.tables.push_back(wt);
    }

    // ---- Feed system -------------------------------------------------------------------
    auto sectionText = [](const Flow::FeedSection& s) -> wxString
    {
        if (s.shape == Flow::SectionShape::Circle) return U("\xc3\x98") + wxString::Format("%.2f", s.diameterMm);
        if (s.shape == Flow::SectionShape::Rect) return wxString::Format("%.2f", s.widthMm) + U(" \xc3\x97 ") +
                                                        wxString::Format("%.2f", s.heightMm);
        return "-";
    };
    if (m_hasFeedSolve)
    {
        RD::Table ft("Feed system",
                     { "Edge", "Kind", "Length (mm)", "Section (mm)", U("Flow (cm\xc2\xb3/s)"), "Shear rate (1/s)",
                       U("Viscosity (Pa\xc2\xb7s)"), U("\xce\x94P (MPa)") });
        for (size_t i = 0; i < net.edges.size(); ++i)
        {
            const Flow::FeedEdge& e = net.edges[i];
            if (!e.carriesMelt()) continue;
            const bool reached = i < fs.edgeFlowMm3s.size() && (size_t)e.a < fs.nodePressureMPa.size() &&
                                 fs.nodePressureMPa[(size_t)e.a] >= 0.0f;
            if (!reached)
            {
                ft.Add({ wxString::FromUTF8(e.label.c_str()), Flow::FeedEdgeKindName(e.kind), F(e.lengthMm, 1),
                         sectionText(e.section), "not reached", "-", "-", "-" }, Status::Fail);
                continue;
            }
            ft.Add({ wxString::FromUTF8(e.label.c_str()), Flow::FeedEdgeKindName(e.kind), F(e.lengthMm, 1),
                     sectionText(e.section), F(std::fabs(fs.edgeFlowMm3s[i]) / 1000.0, 3), F(fs.edgeShearRate[i], 0),
                     F(fs.edgeViscosityPaS[i], 1), F(fs.edgeDeltaPMPa[i], 3) },
                   fs.edgeShearRate[i] > in.maxShearRate ? Status::Warn : Status::None);
        }
        ft.title = "Steady solve at melt temperature";
        ft.note = "Inlet pressure " + F(fs.inletPressureMPa, 2) + " MPa with the parts as open ends" +
                  (fs.converged ? wxString() : wxString(" (approximate: not fully converged)")) + ".";
        ft.view = 5;
        R.tables.push_back(ft);

        if (!fs.entryNodes.empty())
        {
            RD::Table st("Feed system", { "Entry", "Part", "Share of flow" });
            st.title = "Melt split (steady, parts empty)";
            const double q = std::max(1e-12, in.flowRateMm3s);
            for (size_t k = 0; k < fs.entryNodes.size(); ++k)
            {
                const int pn = fs.entryPartNode[k];
                st.Add({ wxString::FromUTF8(net.nodes[(size_t)fs.entryNodes[k]].label.c_str()),
                         pn >= 0 ? wxString::FromUTF8(net.nodes[(size_t)pn].label.c_str()) : wxString("?"),
                         F(100.0 * fs.entryFlowMm3s[k] / q, 1) + "%" });
            }
            st.view = 5;
            R.tables.push_back(st);
        }
    }
    if (filled)
    {
        RD::Table gt("Feed system", { "Gate", "Kind", "Max wall shear (1/s)", "Frozen at end of fill",
                                                "Sealed at (s)" });
        for (size_t i = 0; i < net.edges.size() && i < fr.feedEdges.size(); ++i)
        {
            const Flow::FeedEdge& e = net.edges[i];
            const Flow::FeedEdgeFillResult& er = fr.feedEdges[i];
            if (!er.modelled || (e.kind != EK::Gate && e.kind != EK::SubRunner)) continue;
            const bool shear = er.maxWallShearRate > in.maxShearRate;
            const bool frozen = fr.thermal && e.kind == EK::Gate && er.frozenPct > 50.0f;
            gt.Add({ wxString::FromUTF8(e.label.c_str()), Flow::FeedEdgeKindName(e.kind), F(er.maxWallShearRate, 0),
                     (fr.thermal && er.frozenPct >= 0.0f) ? F(er.frozenPct, 0) + "%" : wxString("-"),
                     !K.ran ? wxString("-") : er.freezeS >= 0.0f ? F(er.freezeS, 2) : wxString("still open") },
                   (shear || frozen) ? Status::Warn : Status::Pass);
        }
        gt.title = "Gates during the fill";
        gt.note = "Shear guideline for this material: ~" + F(in.maxShearRate, 0) + "/s.";
        gt.view = 5;
        if (!gt.rows.empty()) R.tables.push_back(gt);
    }

    // ---- Defects ------------------------------------------------------------------------
    if (filled)
    {
        if (!fd.lines.empty())
        {
            RD::Table lt("Defects", { "Part", "Type", "Length (mm)", "Near x, y, z (mm)", "Fronts met (s)",
                                                  "Max angle", "Front temp." });
            for (const Flow::WeldLine& L : fd.lines)
            {
                const bool weak = L.weld && L.hasFrontTemp && L.minFrontTempC < in.meltC - 20.0;
                lt.Add({ partName(L.part), L.weld ? "weld" : "meld", F(L.lengthMm, 1), Pt(L.anchor), F(L.formedS, 3),
                         F(L.maxAngleDeg, 0) + Deg(), L.hasFrontTemp ? F(L.minFrontTempC, 0) + DegC() : wxString("-") },
                       weak ? Status::Warn : Status::Info);
            }
            lt.title = "Weld and meld lines";
            lt.view = 12;
            R.tables.push_back(lt);
        }
        if (!fd.traps.empty())
        {
            RD::Table at("Defects", { "Part", U("Air (mm\xc2\xb3)"), "Cut off at (s)", "Squeezed to x, y, z (mm)" });
            at.title = "Air traps";
            for (const Flow::AirTrap& a : fd.traps)
                at.Add({ partName(a.part), F(a.volumeMm3, 2), F(a.sealedS, 3), Pt(a.pos) }, Status::Warn);
            at.view = 12;
            R.tables.push_back(at);
        }
        if (!fd.lastFill.empty())
        {
            RD::Table lf("Defects", { "Part", "At x, y, z (mm)", "Time (s)", "Venting" });
            lf.title = "Last to fill (where the air leaves)";
            for (const Flow::LastFillPoint& p : fd.lastFill)
                lf.Add({ partName(p.part), Pt(p.pos), F(p.timeS, 3),
                         p.vented ? wxString("vented")
                                  : p.ventDistMm >= 0.0f ? "no vent within reach (nearest " + F(p.ventDistMm, 0) + " mm)"
                                                         : wxString("add a vent here") },
                       p.vented ? Status::Pass : Status::Warn);
            lf.view = 12;
            R.tables.push_back(lf);
        }
    }

    // ---- Mesh + solver ------------------------------------------------------------------
    if (!m_midplanes.empty())
    {
        RD::Table mt("Mesh", { "Part", "Triangles", U("Max tri (mm\xc2\xb2)"), "Regularity", "Gap (mm)",
                                         "Mean gap (mm)", "Volume vs part", "Walls along pull" });
        for (const Flow::MidplaneMesh& m : m_midplanes)
        {
            const Flow::MidplaneStats& s = m.stats;
            if (m.empty())
            {
                mt.Add({ wxString::FromUTF8(m.label.c_str()), "not built: " + wxString::FromUTF8(s.message.c_str()),
                         "", "", "", "", "", "" }, Status::Fail);
                continue;
            }
            const double dv = s.sourceVolumeMm3 > 0.0 ? 100.0 * (s.midplaneVolumeMm3 / s.sourceVolumeMm3 - 1.0) : 0.0;
            mt.Add({ wxString::FromUTF8(m.label.c_str()), I(s.tris), wxString::Format("%.3g", s.maxAreaMm2),
                     F(s.meanQuality, 2), F(s.minThicknessMm, 2) + EnDash() + F(s.maxThicknessMm, 2),
                     F(s.meanThicknessMm, 2), wxString::Format("%+.1f%%", dv), F(s.steepAreaPct, 1) + "%" },
                   std::fabs(dv) > 5.0 || s.multiLayerNodes > 0 ? Status::Warn : Status::None);
        }
        mt.title = "Part midplanes";
        mt.view = 6;
        R.tables.push_back(mt);
    }
    if (filled)
    {
        RD::Table sv("Mesh");
        sv.title = "Solver";
        sv.Add({ "Fill steps / unknowns / solves", I(fr.steps) + " / " + I(fr.dofs) + " / " + I(fr.solves) });
        sv.Add({ "Volume balance (stored vs injected)", F(fr.massErrorPct, 3) + "%" });
        if (K.ran)
            sv.Add({ "Packing steps / solves", I(K.steps) + " / " + I(K.solves) });
        sv.view = 6;
        R.tables.push_back(sv);
    }
    R.tables.push_back(proc);   // inputs last

    // Material values the run didn't have directly: derived from datasheet
    // values (info) or generic fallbacks (warning).
    if (!in.materialNotes.empty())
    {
        RD::Table mt("Materials", { "Material", "Value", "Source" });
        mt.title = "Derived and fallback material values";
        for (const FlowRunInputs::MaterialNoteRow& r : in.materialNotes)
            mt.Add({ r.material, r.quantity, (r.fallback ? wxString("FALLBACK: ") : wxString("Derived: ")) + r.text },
                   r.fallback ? Status::Warn : Status::Info);
        mt.note = "Add the missing values to the material file (or its additional values) to replace these.";
        R.tables.push_back(mt);
    }
    return R;
}
