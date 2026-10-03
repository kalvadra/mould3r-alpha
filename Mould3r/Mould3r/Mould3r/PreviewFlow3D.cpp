// ===========================================================================
// PreviewFlow3D.cpp — PreviewPanel's 3D Flow Analysis card (see the plan in
// the Phase 1 notes): the volume-mesh stage, the Sim Viewer "3D mesh" view
// with its section plane, the mesh export and the card's report.
//
// The mesh itself is made by fTetWild in the worker process
// (TetMeshRunner / MeshWorker); everything here is app-side: choosing the
// surface and the element size, keeping the result, and showing it.
// ===========================================================================
#include "PreviewPanel.h"   // wx first

#include <wx/filedlg.h>
#include <wx/filename.h>
#include <wx/progdlg.h>
#include <wx/slider.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>
#include <numeric>
#include <thread>

#include "FlowResultsBar.h"
#include "GLCanvas.h"
#include "RoundedButton.h"
#include "DesignChecks.h"    // FaceDraftParams: the draw axis / parting plane
#include "TetMeshRunner.h"
#include "style.h"

namespace RD = ResultsDetails;
using RD::Status;

namespace
{
    wxString U(const char* utf8) { return wxString::FromUTF8(utf8); }
    wxString DegC() { return wxString::FromUTF8("\xc2\xb0""C"); }

    const wxColour kGreen(0x26, 0xAB, 0x36);
    const wxColour kAmber(0xE0, 0x9B, 0x20);
    const wxColour kRed(0xD0, 0x46, 0x46);

    // Elements across the thinnest wall, per the card's density choice.
    constexpr int kDensityAcross[4] = { 2, 3, 4, 6 };

    // Volume of a regular tetrahedron of edge a is a^3 / (6 sqrt 2). fTetWild
    // meshes run somewhat denser than an ideal packing (smaller elements
    // along the surface), hence the factor.
    double EstimateTets(double volumeMm3, double edgeMm)
    {
        if (!(edgeMm > 0.0)) return 0.0;
        return 1.4 * volumeMm3 / (edgeMm * edgeMm * edgeMm / (6.0 * std::sqrt(2.0)));
    }

    // "412k" / "1.3M" style counts.
    wxString Count(double n)
    {
        if (n >= 1.0e6) return wxString::Format("%.1fM", n / 1.0e6);
        if (n >= 1.0e4) return wxString::Format("%.0fk", n / 1.0e3);
        return wxString::Format("%.0f", n);
    }

    // Area-weighted percentile of the paired facets' wall thickness.
    double ThicknessPercentile(const Flow::FlowMesh& fm, double fraction)
    {
        std::vector<std::pair<float, double>> v;   // thickness, facet area
        const std::vector<float>& P = fm.posNorm;
        const std::vector<unsigned int>& I = fm.indices;
        double total = 0.0;
        for (size_t t = 0; t < fm.triCount(); ++t)
        {
            if (t >= fm.paired.size() || !fm.paired[t] || !(fm.thicknessMm[t] > 0.0f)) continue;
            const float* a = &P[6 * (size_t)I[3 * t]];
            const float* b = &P[6 * (size_t)I[3 * t + 1]];
            const float* c = &P[6 * (size_t)I[3 * t + 2]];
            const glm::vec3 e1(b[0] - a[0], b[1] - a[1], b[2] - a[2]);
            const glm::vec3 e2(c[0] - a[0], c[1] - a[1], c[2] - a[2]);
            const double area = 0.5 * (double)glm::length(glm::cross(e1, e2));
            if (!(area > 0.0)) continue;
            v.emplace_back(fm.thicknessMm[t], area);
            total += area;
        }
        if (v.empty() || !(total > 0.0)) return 0.0;
        std::sort(v.begin(), v.end(), [](const auto& x, const auto& y) { return x.first < y.first; });
        double acc = 0.0;
        for (const auto& [th, area] : v)
        {
            acc += area;
            if (acc >= fraction * total) return th;
        }
        return v.back().first;
    }
}

// ---------------------------------------------------------------------------
// The surface to mesh.
// ---------------------------------------------------------------------------
bool PreviewPanel::BuildVolumeSurface(bool fullShot, TetMesh::Surface& surface, double& volumeMm3, wxString& why)
{
    surface = TetMesh::Surface{};
    volumeMm3 = 0.0;
    if (fullShot)
    {
        if (!m_hasShot || m_shotMesh.posNorm.empty() || m_shotMesh.indices.empty())
        {
            why = "There is no shot to mesh. Generate the mould first.";
            return false;
        }
        // The display buffer is flat-shaded (split per face): weld it.
        surface = TetMesh::WeldSurface(m_shotMesh.posNorm.data(), m_shotMesh.posNorm.size() / 6, 6,
                                       m_shotMesh.indices.data(), m_shotMesh.indices.size());
        volumeMm3 = std::fabs(TetMesh::SurfaceVolume(surface));
    }
    else
    {
        // Every moulded part's own surface (world space, captured at Generate
        // Mould), in one job: fTetWild fills each closed component. Welded
        // per part so parts that touch stay separate volumes.
        for (const Flow::PartSurface& ps : m_partSurfaces)
        {
            if (ps.xyz.size() < 9 || ps.indices.size() < 3) continue;
            const TetMesh::Surface one = TetMesh::WeldSurface(ps.xyz.data(), ps.xyz.size() / 3, 3,
                                                              ps.indices.data(), ps.indices.size());
            if (one.TriangleCount() < 4) continue;
            const int32_t base = (int32_t)surface.VertexCount();
            surface.verts.insert(surface.verts.end(), one.verts.begin(), one.verts.end());
            for (int32_t v : one.tris) surface.tris.push_back(v + base);
            volumeMm3 += std::fabs(TetMesh::SurfaceVolume(one));
        }
        if (surface.TriangleCount() < 4)
        {
            why = m_partSurfaces.empty()
                ? "There are no part cavities to mesh. Generate the mould first (or tick "
                  "\"Simulate full shot volume\" to mesh the shot)."
                : "The part surfaces are empty.";
            return false;
        }
    }
    if (surface.TriangleCount() < 4)
    {
        why = "The shot surface is empty.";
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Thin-wall thickness (5th area percentile of the dual-domain thickness).
// ---------------------------------------------------------------------------
double PreviewPanel::ThinWallMm(bool fullShot)
{
    double& cached = m_thinWallMm[fullShot ? 1 : 0];
    if (cached >= 0.0) return cached;
    cached = 0.0;

    if (fullShot)
    {
        if (EnsureFlowMesh()) cached = ThicknessPercentile(m_flowMesh, 0.05);
        return cached;
    }

    // Cavity only: pair the parts' own walls (the runners and sprue are
    // thicker and would only dilute the percentile). Facet normals from the
    // winding, which FlowMesh reads as outward.
    std::vector<float> pn;
    std::vector<unsigned int> idx;
    for (const Flow::PartSurface& ps : m_partSurfaces)
    {
        for (size_t t = 0; t + 2 < ps.indices.size(); t += 3)
        {
            const unsigned int i0 = ps.indices[t], i1 = ps.indices[t + 1], i2 = ps.indices[t + 2];
            if ((size_t)std::max({ i0, i1, i2 }) * 3 + 2 >= ps.xyz.size()) continue;
            const glm::vec3 a(ps.xyz[3 * i0], ps.xyz[3 * i0 + 1], ps.xyz[3 * i0 + 2]);
            const glm::vec3 b(ps.xyz[3 * i1], ps.xyz[3 * i1 + 1], ps.xyz[3 * i1 + 2]);
            const glm::vec3 c(ps.xyz[3 * i2], ps.xyz[3 * i2 + 1], ps.xyz[3 * i2 + 2]);
            glm::vec3 n = glm::cross(b - a, c - a);
            const float L = glm::length(n);
            if (!(L > 0.0f)) continue;
            n /= L;
            for (const glm::vec3& p : { a, b, c })
            {
                idx.push_back((unsigned int)(pn.size() / 6));
                pn.insert(pn.end(), { p.x, p.y, p.z, n.x, n.y, n.z });
            }
        }
    }
    if (idx.size() < 3) return cached;
    Flow::FlowMesh fm;
    Flow::FlowMeshStats st;
    if (Flow::BuildFlowMesh(pn, idx, Flow::FlowMeshParams{}, fm, &st))
        cached = ThicknessPercentile(fm, 0.05);
    return cached;
}

// ---------------------------------------------------------------------------
// Keep a finished mesh.
// ---------------------------------------------------------------------------
void PreviewPanel::StoreVolumeMesh(TetMesh::Mesh&& mesh, bool fullShot, double edgeMm, double epsMm,
                                   double wallMm, double sourceVolMm3, size_t surfaceTris,
                                   double meshSeconds, double wallSeconds)
{
    m_volMesh = VolumeMesh{};
    m_fill3d = Flow3D::FillResult{};   // it belonged to the old mesh
    m_hasFill3d = false;
    VolumeMesh& V = m_volMesh;
    V.mesh = std::move(mesh);
    V.stats = TetMesh::ComputeStats(V.mesh);
    V.minDihedral = TetMesh::TetMinDihedral(V.mesh);
    V.neighbours = TetMesh::TetFaceNeighbours(V.mesh);
    V.fullShot = fullShot;
    V.edgeMm = edgeMm;
    V.epsMm = epsMm;
    V.wallMm = wallMm;
    V.sourceVolMm3 = sourceVolMm3;
    V.surfaceTris = surfaceTris;
    V.meshSeconds = meshSeconds;
    V.wallSeconds = wallSeconds;
    for (int k = 0; k < 3; ++k) { V.bbMin[k] = 1e300; V.bbMax[k] = -1e300; }
    for (size_t i = 0; i < V.mesh.VertexCount(); ++i)
        for (int k = 0; k < 3; ++k)
        {
            V.bbMin[k] = std::min(V.bbMin[k], V.mesh.verts[3 * i + (size_t)k]);
            V.bbMax[k] = std::max(V.bbMax[k], V.mesh.verts[3 * i + (size_t)k]);
        }

    // Open the section across the longest extent, halfway: a cut through the
    // whole part that shows the elements through the wall.
    if (m_sectionAxisChoice && m_sectionSlider && m_sectionFlipCheck)
    {
        int longest = 0;
        for (int k = 1; k < 3; ++k)
            if (V.bbMax[k] - V.bbMin[k] > V.bbMax[longest] - V.bbMin[longest]) longest = k;
        m_sectionAxisChoice->SetSelection(longest + 1);
        m_sectionSlider->SetValue(500);
        m_sectionFlipCheck->SetValue(false);
    }
    if (m_flow3dExportMeshBtn)
    {
        m_flow3dExportMeshBtn->Enable(V.ready());
        m_flow3dExportMeshBtn->Refresh();
    }
}

// ---------------------------------------------------------------------------
// Boundary conditions.
// ---------------------------------------------------------------------------
void PreviewPanel::TagVolumeBoundary()
{
    VolumeMesh& V = m_volMesh;
    V.boundary = Flow3D::Boundary{};
    if (!V.ready()) return;

    Flow3D::BoundarySpec spec;
    DesignChecks::FaceDraftParams draft;         // the draw axis + parting plane the checks use
    spec.drawAxis = glm::dvec3(draft.drawAxis);
    spec.partingOffset = draft.partingOffset;
    spec.tolMm = std::max(V.epsMm, 1.0e-3);
    spec.pushMm = 2.0 * V.epsMm + 0.02;   // clear of the envelope, well inside a gate

    const Flow::FeedNetwork& net = m_feedNetwork;
    const bool haveNet = m_hasFeedNetwork && !net.empty();
    auto nodePos = [&net](int n) { return glm::dvec3(net.nodes[(size_t)n].pos); };
    auto dirTo = [&](int from, int to)
    {
        const glm::dvec3 d = nodePos(to) - nodePos(from);
        const double L = glm::length(d);
        return L > 0.0 ? d / L : glm::dvec3(0.0);
    };
    auto sectionRadius = [](const Flow::FeedSection& sec)
    {
        if (sec.shape == Flow::SectionShape::Circle) return 0.5 * (double)sec.diameterMm;
        return sec.areaMm2 > 0.0f ? std::sqrt((double)sec.areaMm2 / 3.14159265358979323846) : 0.0;
    };
    // The melt-carrying edge that leaves node `n` (a gate / the sprue), if any.
    auto feedEdgeAt = [&net](int n, Flow::FeedEdgeKind kind) -> const Flow::FeedEdge*
    {
        for (const Flow::FeedEdge& e : net.edges)
            if (e.kind == kind && (e.a == n || e.b == n)) return &e;
        return nullptr;
    };

    // ---- Inlets -----------------------------------------------------------------
    double minInletR = 1e300;
    if (V.fullShot)
    {
        // The sprue's entry cap, where the nozzle seats.
        if (haveNet && net.inletNode >= 0)
        {
            Flow3D::InletSpec in;
            in.label = "Sprue inlet";
            in.feedNode = net.inletNode;
            in.pos = nodePos(net.inletNode);
            if (const Flow::FeedEdge* e = feedEdgeAt(net.inletNode, Flow::FeedEdgeKind::Sprue))
            {
                const int other = e->a == net.inletNode ? e->b : e->a;
                in.dirOut = -dirTo(net.inletNode, other);   // out of the shot, toward the nozzle
                in.radiusMm = sectionRadius(e->section);
            }
            spec.sprueCap = true;
            spec.inlets.push_back(in);
        }
    }
    else if (haveNet)
    {
        // Every cavity entry: a gate mouth, or a direct-injection sprue end.
        for (const Flow::FeedEdge& ce : net.edges)
        {
            if (ce.kind != Flow::FeedEdgeKind::CavityIn) continue;
            const int n = ce.a;
            if (n < 0 || (size_t)n >= net.nodes.size()) continue;
            Flow3D::InletSpec in;
            in.feedNode = n;
            in.pos = nodePos(n);
            in.label = net.nodes[(size_t)n].label.empty() ? std::string("Inlet") : net.nodes[(size_t)n].label;
            const Flow::FeedEdge* e = feedEdgeAt(n, Flow::FeedEdgeKind::Gate);
            if (!e) e = feedEdgeAt(n, Flow::FeedEdgeKind::Sprue);
            if (e)
            {
                in.dirOut = dirTo(n, e->a == n ? e->b : e->a);   // toward the feed
                in.radiusMm = sectionRadius(e->section);
            }
            if (net.nodes[(size_t)n].kind == Flow::FeedNodeKind::SprueEnd) in.label = "Sprue (direct injection)";
            minInletR = std::min(minInletR, in.radiusMm);
            spec.inlets.push_back(in);
        }
    }
    if (minInletR < 1e300 && minInletR > 0.0)
        spec.pushMm = std::max(std::min(spec.pushMm, 0.5 * minInletR), 1.2 * V.epsMm);

    // ---- Vents ----------------------------------------------------------------------
    if (haveNet)
        for (const Flow::FeedEdge& e : net.edges)
        {
            if (e.kind != Flow::FeedEdgeKind::Vent) continue;
            int mouth = e.a, outlet = e.b;
            if (net.nodes[(size_t)mouth].kind != Flow::FeedNodeKind::VentStart) std::swap(mouth, outlet);
            Flow3D::VentSpec v;
            v.feedNode = mouth;
            v.pos = nodePos(mouth);
            v.dirOut = dirTo(mouth, outlet);
            v.halfWidthMm = 0.5 * (double)(e.section.shape == Flow::SectionShape::Rect ? e.section.widthMm
                                                                                         : e.section.diameterMm);
            v.label = net.nodes[(size_t)mouth].label.empty() ? std::string("Vent") : net.nodes[(size_t)mouth].label;
            spec.vents.push_back(v);
        }

    // ---- Gate footprints (cavity only) ------------------------------------------------
    // A wall face is a gate mouth when a point just outside it lies inside
    // the shot (so in gate / sprue material) but not inside any part.
    Flow3D::SolidTester inShot, inParts;
    if (!V.fullShot && m_hasShot && !m_shotMesh.posNorm.empty())
    {
        std::vector<float> xyz;
        std::vector<glm::vec3> tn;
        const std::vector<float>& P = m_shotMesh.posNorm;
        xyz.reserve(P.size() / 2);
        for (size_t i = 0; i + 5 < P.size(); i += 6) xyz.insert(xyz.end(), { P[i], P[i + 1], P[i + 2] });
        std::vector<unsigned int> idx(m_shotMesh.indices.begin(), m_shotMesh.indices.end());
        for (size_t t = 0; t + 2 < idx.size(); t += 3)
        {
            const float* a = &P[6 * (size_t)idx[t]];
            const float* b = &P[6 * (size_t)idx[t + 1]];
            const float* c = &P[6 * (size_t)idx[t + 2]];
            glm::vec3 n = glm::cross(glm::vec3(b[0] - a[0], b[1] - a[1], b[2] - a[2]),
                                     glm::vec3(c[0] - a[0], c[1] - a[1], c[2] - a[2]));
            // Orient by the display normals (outward), as the flow mesh does.
            const glm::vec3 vn(a[3] + b[3] + c[3], a[4] + b[4] + c[4], a[5] + b[5] + c[5]);
            if (glm::dot(n, vn) < 0.0f) n = -n;
            tn.push_back(n);
        }
        inShot.Build(std::move(xyz), std::move(idx), std::move(tn));

        std::vector<float> pxyz;
        std::vector<unsigned int> pidx;
        std::vector<glm::vec3> pn;
        for (const Flow::PartSurface& ps : m_partSurfaces)
        {
            if (ps.xyz.size() < 9 || ps.indices.size() < 3) continue;
            const unsigned int base = (unsigned int)(pxyz.size() / 3);
            const size_t firstTri = pn.size();
            double vol = 0.0;
            for (size_t t = 0; t + 2 < ps.indices.size(); t += 3)
            {
                const unsigned int i0 = ps.indices[t], i1 = ps.indices[t + 1], i2 = ps.indices[t + 2];
                if ((size_t)std::max({ i0, i1, i2 }) * 3 + 2 >= ps.xyz.size()) continue;
                const glm::vec3 a(ps.xyz[3 * i0], ps.xyz[3 * i0 + 1], ps.xyz[3 * i0 + 2]);
                const glm::vec3 b(ps.xyz[3 * i1], ps.xyz[3 * i1 + 1], ps.xyz[3 * i1 + 2]);
                const glm::vec3 c(ps.xyz[3 * i2], ps.xyz[3 * i2 + 1], ps.xyz[3 * i2 + 2]);
                pidx.insert(pidx.end(), { base + i0, base + i1, base + i2 });
                pn.push_back(glm::cross(b - a, c - a));
                vol += (double)glm::dot(a, glm::cross(b, c));
            }
            if (vol < 0.0)   // wound inward: flip this part's normals
                for (size_t k = firstTri; k < pn.size(); ++k) pn[k] = -pn[k];
            pxyz.insert(pxyz.end(), ps.xyz.begin(), ps.xyz.end());
        }
        inParts.Build(std::move(pxyz), std::move(pidx), std::move(pn));
        if (!inShot.Empty())
            spec.coveredByFeed = [&inShot, &inParts](const glm::dvec3& p)
            {
                return inShot.Inside(p) && (inParts.Empty() || !inParts.Inside(p));
            };
    }

    V.boundary = Flow3D::TagBoundary(V.mesh, V.neighbours, spec);
    if (!haveNet)
        V.boundary.warnings.push_back("No feed system was captured at Generate Mould, so the inlets and vents "
                                      "couldn't be placed. Generate the mould again.");
    else if (V.fullShot && net.inletNode < 0)
        V.boundary.warnings.push_back("No sprue is placed, so the shot has no inlet.");
    else if (!V.fullShot && spec.inlets.empty())
        V.boundary.warnings.push_back("No gate or direct-injection sprue reaches a part, so the cavities have no inlet.");
}

// ---------------------------------------------------------------------------
// Start: the mesh stage.
// ---------------------------------------------------------------------------
void PreviewPanel::Run3DFlow()
{
    wxWindow* top = wxGetTopLevelParent(this);
    const wxString title = "3D Flow Analysis";
    const bool fullShot = m_flow3dFullShotCheck && m_flow3dFullShotCheck->GetValue();

    // ---- Material and process (checked before any long work) -------------------
    ResolvedInjectionMaterial injMat;
    ResolvedMouldMaterial mouldMat;
    std::vector<wxString> fileWarnings;
    if (!ResolveSelectedMaterials(injMat, mouldMat, fileWarnings)) return;
    const TestMaterial::PolymerMaterial& poly = injMat.props;
    auto field = [](wxTextCtrl* c, double def)
    {
        double v = 0.0;
        return (c && c->GetValue().ToDouble(&v) && std::isfinite(v)) ? v : def;
    };
    const double fillTime = std::max(0.01, field(m_flow3dFillTimeCtrl, 1.0));
    const double meltC = std::clamp(field(m_flow3dMeltTempCtrl, poly.recMeltTempC),
                                    (double)poly.meltTempMinC, (double)poly.meltTempMaxC);
    const double maxP = std::clamp(field(m_flow3dMaxPressureCtrl, 150.0), 1.0, 1000.0);
    const double mouldC = std::clamp(field(m_flow3dMouldTempCtrl, poly.recMouldTempC), 10.0, 200.0);
    const bool thermalOn = !m_flow3dThermalCheck || m_flow3dThermalCheck->GetValue();
    const bool packOn = thermalOn && (!m_flow3dPackCheck || m_flow3dPackCheck->GetValue());
    Flow3D::FillSetup::Pack pack;
    pack.enabled = packOn;
    pack.packFraction = std::clamp(field(m_flow3dPackPressureCtrl, 80.0), 5.0, 150.0) / 100.0;
    pack.holdTimeS = std::clamp(field(m_flow3dHoldTimeCtrl, 0.0), 0.0, 120.0);
    pack.pvt = poly.pvt;
    pack.ejectionTempC = poly.ejectionTempC;

    // Thermal: the melt's properties and the wall contact temperature from the
    // melt's and the mould's effusivities (as the 2.5D thermal fill).
    Flow3D::FillSetup::Thermal thermal;
    thermal.enabled = thermalOn;
    if (thermalOn)
    {
        const TestMaterial::MouldMaterial& mould = mouldMat.props;
        const double eMelt = std::sqrt(poly.thermalConductivity * poly.densityMelt * poly.specificHeat);
        const double eMould = mould.effusivity();
        const double wallC = (eMelt + eMould) > 0.0 ? (eMelt * meltC + eMould * mouldC) / (eMelt + eMould) : mouldC;
        thermal.viscosity = poly.viscosity;
        thermal.meltK = meltC + TestMaterial::kZeroC;
        thermal.wallK = wallC + TestMaterial::kZeroC;
        thermal.noFlowK = poly.noFlowTempC + TestMaterial::kZeroC;
        thermal.rhoCp = poly.densityMelt * poly.specificHeat;
        thermal.k = poly.thermalConductivity;
    }

    // ---- The surface and the element size -------------------------------------------
    TetMesh::Surface surf;
    double srcVol = 0.0;
    wxString why;
    {
        wxBusyCursor busy;
        if (!BuildVolumeSurface(fullShot, surf, srcVol, why))
        {
            wxMessageBox(why, title, wxOK | wxICON_INFORMATION, top);
            return;
        }
    }

    double mn[3] = { 1e300, 1e300, 1e300 }, mx[3] = { -1e300, -1e300, -1e300 };
    for (size_t i = 0; i < surf.VertexCount(); ++i)
        for (int k = 0; k < 3; ++k)
        {
            mn[k] = std::min(mn[k], surf.verts[3 * i + (size_t)k]);
            mx[k] = std::max(mx[k], surf.verts[3 * i + (size_t)k]);
        }
    const double diag = std::sqrt((mx[0] - mn[0]) * (mx[0] - mn[0]) + (mx[1] - mn[1]) * (mx[1] - mn[1]) +
                                  (mx[2] - mn[2]) * (mx[2] - mn[2]));
    if (!(diag > 0.0))
    {
        wxMessageBox("The surface to mesh has no extent.", title, wxOK | wxICON_WARNING, top);
        return;
    }

    double wall = 0.0;
    {
        wxBusyCursor busy;
        wall = ThinWallMm(fullShot);
    }
    const int densSel = m_flow3dDensityChoice ? std::clamp(m_flow3dDensityChoice->GetSelection(), 0, 3) : 1;
    const int across = kDensityAcross[densSel];

    double edge = 0.0;
    bool autoEdge = true;
    wxString edgeText = m_flow3dEdgeCtrl ? m_flow3dEdgeCtrl->GetValue().Strip(wxString::both) : wxString();
    if (!edgeText.empty() && edgeText.CmpNoCase("auto") != 0)
    {
        if (!edgeText.ToDouble(&edge) || !(edge > 0.0))
        {
            wxMessageBox("Edge length: enter a length in mm, or \"auto\".", title, wxOK | wxICON_WARNING, top);
            return;
        }
        autoEdge = false;
    }
    wxString sizeNote;
    if (autoEdge)
    {
        if (wall > 0.0)
            edge = wall / (double)across;
        else
        {
            edge = diag / 40.0;
            sizeNote = "No opposing walls could be paired to measure the wall thickness, so the "
                       "element size is a fraction of the part size instead.";
        }
    }
    // Surface envelope: how far the mesh boundary may stray from the surface.
    // fTetWild's default (1/1000 of the size) is too loose for a thin wall.
    double eps = diag / 1000.0;
    if (wall > 0.0) eps = std::min(eps, wall / 20.0);
    eps = std::min(eps, edge / 6.0);

    // ---- Mesh: reuse the last one when nothing about it changed ------------------------
    const bool reuse = m_volMesh.ready() && m_volMesh.fullShot == fullShot &&
                       std::fabs(m_volMesh.edgeMm - edge) <= 1e-9 * edge &&
                       std::fabs(m_volMesh.epsMm - eps) <= 1e-9 * eps;
    if (!reuse)
    {
        const double estTets = EstimateTets(srcVol, edge);
        if (estTets > 1.5e6)
        {
            wxString q;
            q << "This will make roughly " << Count(estTets) << " elements ("
              << wxString::Format("%.3g", edge) << " mm edges";
            if (autoEdge && wall > 0.0) q << wxString::Format(", %d across a %.3g mm wall", across, wall);
            q << ").\n\nA mesh this size can take many minutes and several GB of memory, and the fill "
                 "longer still. A coarser density or a larger edge length is quicker.\n\nMesh anyway?";
            if (wxMessageBox(q, title, wxYES_NO | wxNO_DEFAULT | wxICON_WARNING, top) != wxYES) return;
        }

        TetMesh::Params prm;
        prm.edgeLength = edge;
        prm.epsilon = eps;
        TetMeshOutcome o = RunTetMeshJobModal(top, surf, prm, title);
        switch (o.kind)
        {
        case TetMeshOutcome::Kind::Cancelled:
            return;
        case TetMeshOutcome::Kind::LaunchFailed:
        case TetMeshOutcome::Kind::Crashed:
        case TetMeshOutcome::Kind::Failed:
        {
            if (m_flow3dStatus)
            {
                m_flow3dStatus->SetLabel("MESH FAILED");
                m_flow3dStatus->SetForegroundColour(kRed);
                if (m_infoPanel) m_infoPanel->Layout();
            }
            wxMessageBox(o.detail.empty() ? wxString("Meshing failed.") : o.detail, title, wxOK | wxICON_ERROR, top);
            return;
        }
        case TetMeshOutcome::Kind::Ok:
            break;
        }
        StoreVolumeMesh(std::move(o.result.mesh), fullShot, edge, eps, wall, srcVol, surf.TriangleCount(),
                        o.result.workerSeconds, o.wallSeconds);
        m_volMesh.sizeNote = sizeNote;
    }
    if (!m_volMesh.boundary.Ready())
    {
        wxBusyCursor busy;
        TagVolumeBoundary();
    }
    const VolumeMesh& V = m_volMesh;
    const Flow3D::Boundary& BC = V.boundary;
    const int nInlets = BC.Count(Flow3D::TagInlet);
    bool placedByPosition = false;
    for (const Flow3D::BoundaryRegion& r : BC.regions) placedByPosition |= r.byPosition;

    // Mesh verdict: amber for more than 0.1% slivers, the volume off by 2-5%
    // (sharp edges rounded within the envelope cost ~1% on a thin part) or a
    // gate placed by position; red for inverted elements or the volume off by
    // more than 5% (the surface wasn't closed, or a feature was lost).
    const double volErr = V.sourceVolMm3 > 0.0 ? std::fabs(V.stats.volumeMm3 / V.sourceVolMm3 - 1.0) : 0.0;
    const double sliverShare = V.stats.tets ? (double)V.stats.slivers / (double)V.stats.tets : 0.0;
    int meshLevel = 0;   // 0 fine, 1 check, 2 bad
    if (V.stats.nonPositive > 0 || volErr > 0.05) meshLevel = 2;
    else if (sliverShare > 0.001 || volErr > 0.02 || placedByPosition) meshLevel = 1;

    auto publish = [&](const wxString& verdict, const wxColour& colour)
    {
        if (m_flow3dStatus)
        {
            m_flow3dStatus->SetLabel(verdict);
            m_flow3dStatus->SetForegroundColour(colour);
            if (m_infoPanel) m_infoPanel->Layout();
        }
        wxString oneLine = verdict;
        oneLine.Replace("\n", ", ");
        SetResultsReport(CardFlow3D, BuildFlow3DReport(oneLine, colour));
    };

    // ---- No inlet: stop at the mesh, show the boundary ----------------------------------
    if (nInlets == 0)
    {
        m_fill3d = Flow3D::FillResult{};
        m_hasFill3d = false;
        publish(wxString("NO INLET\n") + Count((double)V.stats.tets) + " ELEMENTS", kRed);
        if (m_meshColourChoice) m_meshColourChoice->SetSelection(2);
        if (m_sectionAxisChoice) m_sectionAxisChoice->SetSelection(0);
        ShowOnlyShot();
        if (m_debugModeChoice) m_debugModeChoice->SetSelection(kViewVolumeMesh);
        UpdateDraftOverlay();
        ShowSimCompleteNotice(title, title, m_flow3dStatus);
        return;
    }

    // ---- Fill -------------------------------------------------------------------------
    m_fill3dMaterial = wxString::FromUTF8(injMat.name.c_str());
    m_fill3dMeltC = meltC;
    m_fill3dTargetS = fillTime;
    m_fill3dLimitMPa = maxP;
    m_fill3dMaxShear = poly.maxShearRate;
    m_fill3dMouldC = mouldC;
    const bool ran = RunFill3D(poly.viscosity, thermal, pack, fillTime, meltC, maxP, title);
    if (m_fill3d.cancelled) return;   // keep the previous results on the card
    if (!ran)
    {
        publish(wxString("FILL FAILED\n") + Count((double)V.stats.tets) + " ELEMENTS", kRed);
        wxMessageBox("The 3D fill didn't run:\n\n" + wxString::FromUTF8(m_fill3d.message.c_str()),
                     title, wxOK | wxICON_ERROR, top);
        return;
    }

    // With the whole shot meshed, the sprue and runners usually cool last:
    // the part's own ejection time is over the nodes inside the parts.
    m_fill3dPartEjectS = -1.0;
    if (m_fill3d.pack.ran)
    {
        m_fill3dPartEjectS = m_fill3d.pack.ejectS;
        if (V.fullShot && !m_partSurfaces.empty())
        {
            std::vector<float> pxyz;
            std::vector<unsigned int> pidx;
            std::vector<glm::vec3> pn;
            for (const Flow::PartSurface& ps : m_partSurfaces)
            {
                if (ps.xyz.size() < 9 || ps.indices.size() < 3) continue;
                const unsigned int base = (unsigned int)(pxyz.size() / 3);
                const size_t firstTri = pn.size();
                double vol = 0.0;
                for (size_t k = 0; k + 2 < ps.indices.size(); k += 3)
                {
                    const unsigned int i0 = ps.indices[k], i1 = ps.indices[k + 1], i2 = ps.indices[k + 2];
                    if ((size_t)std::max({ i0, i1, i2 }) * 3 + 2 >= ps.xyz.size()) continue;
                    const glm::vec3 a(ps.xyz[3 * i0], ps.xyz[3 * i0 + 1], ps.xyz[3 * i0 + 2]);
                    const glm::vec3 b(ps.xyz[3 * i1], ps.xyz[3 * i1 + 1], ps.xyz[3 * i1 + 2]);
                    const glm::vec3 c(ps.xyz[3 * i2], ps.xyz[3 * i2 + 1], ps.xyz[3 * i2 + 2]);
                    pidx.insert(pidx.end(), { base + i0, base + i1, base + i2 });
                    pn.push_back(glm::cross(b - a, c - a));
                    vol += (double)glm::dot(a, glm::cross(b, c));
                }
                if (vol < 0.0) for (size_t k = firstTri; k < pn.size(); ++k) pn[k] = -pn[k];
                pxyz.insert(pxyz.end(), ps.xyz.begin(), ps.xyz.end());
            }
            Flow3D::SolidTester inParts;
            inParts.Build(std::move(pxyz), std::move(pidx), std::move(pn));
            if (!inParts.Empty())
            {
                wxBusyCursor busy;
                double e = -1.0;
                bool any = false;
                const std::vector<float>& ej = m_fill3d.pack.ejectTimeS;
                for (size_t i = 0; i < V.mesh.VertexCount() && i < ej.size(); ++i)
                {
                    const glm::dvec3 p(V.mesh.verts[3 * i], V.mesh.verts[3 * i + 1], V.mesh.verts[3 * i + 2]);
                    if (!inParts.Inside(p)) continue;
                    any = true;
                    if (ej[i] < 0.0f) { e = -1.0; break; }   // part of a part never got there
                    e = std::max(e, (double)ej[i]);
                }
                if (any) m_fill3dPartEjectS = e;
            }
        }
    }
    const Flow3D::FillResult& F = m_fill3d;
    wxString verdict;
    wxColour colour;
    if (F.incomplete)
    {
        verdict = F.stalled ? "SHORT SHOT (FROZE OFF)" : "SHORT SHOT";
        colour = kRed;
    }
    else if (F.pressureLimited)
    {
        verdict = "FILLED (PRESSURE LIMITED)";
        colour = kAmber;
    }
    else if (meshLevel > 0)
    {
        verdict = "FILLED (CHECK MESH)";
        colour = meshLevel > 1 ? kRed : kAmber;
    }
    else
    {
        verdict = "FILLED";
        colour = kGreen;
    }
    verdict << "\n" << wxString::Format("%.2f s, %.1f MPa", F.endTimeS, F.maxInletMPa);
    if (F.pack.ran)
    {
        const Flow3D::FillResult::PackResult& K = F.pack;
        verdict << "\n" << (m_fill3dPartEjectS >= 0.0 ? wxString::Format("EJECT %.1f s", m_fill3dPartEjectS) : wxString("NOT EJECTABLE"))
                << wxString::Format(", %.1f%% SHRINK", K.meanShrinkPct);
        if (!K.ejectReached && colour == kGreen) colour = kAmber;
    }
    else
        verdict << "\n" << Count((double)V.stats.tets) << " ELEMENTS";
    publish(verdict, colour);

    // Show it: only the shot (the mesh draws in its place), 3D fill time,
    // uncut, at the end of fill.
    if (m_sectionAxisChoice) m_sectionAxisChoice->SetSelection(0);
    ShowOnlyShot();
    if (m_debugModeChoice) m_debugModeChoice->SetSelection(kViewFill3DTime);
    UpdateDraftOverlay();
    ShowSimCompleteNotice(title, title, m_flow3dStatus);
}

// ---------------------------------------------------------------------------
// The 3D fill (worker thread, progress + cancel).
// ---------------------------------------------------------------------------
bool PreviewPanel::RunFill3D(const TestMaterial::CrossWLF& viscosity, const Flow3D::FillSetup::Thermal& thermal,
                             const Flow3D::FillSetup::Pack& pack, double fillTimeS, double meltC,
                             double maxPressureMPa, const wxString& title)
{
    m_fill3d = Flow3D::FillResult{};
    m_hasFill3d = false;
    const VolumeMesh& V = m_volMesh;
    if (!V.ready() || !V.boundary.Ready()) { m_fill3d.message = "There is no tagged 3D mesh."; return false; }

    // ---- The feed side ------------------------------------------------------------
    Flow3D::FillSetup S;
    const Flow::FeedNetwork& net = m_feedNetwork;
    const size_t nReg = V.boundary.regions.size();
    S.regionNetNode.assign(nReg, -1);
    if (V.fullShot)
    {
        // The inlet is the sprue's entry: the machine drives it directly.
        S.netNodes = 1;
        S.sourceNode = 0;
        S.netNodeLabels = { "Sprue inlet" };
        for (size_t r = 0; r < nReg; ++r)
            if (V.boundary.regions[r].tag == Flow3D::TagInlet) S.regionNetNode[r] = 0;
    }
    else
    {
        // The 1D feed network (sprue, runners, sub-runners, gates) as beams,
        // each gate mouth tied to its inlet region.
        S.netNodes = (int)net.nodes.size();
        S.sourceNode = net.inletNode;
        for (const Flow::FeedNode& n : net.nodes) S.netNodeLabels.push_back(n.label);
        for (const Flow::FeedEdge& e : net.edges)
        {
            if (!e.carriesMelt()) continue;
            Flow3D::FeedBeam b;
            b.a = e.a;
            b.b = e.b;
            b.lengthMm = e.lengthMm;
            b.section = e.section;
            b.label = e.label;
            S.beams.push_back(b);
        }
        for (size_t r = 0; r < nReg; ++r)
        {
            const Flow3D::BoundaryRegion& R = V.boundary.regions[r];
            if (R.tag == Flow3D::TagInlet && R.feedNode >= 0 && R.feedNode < S.netNodes) S.regionNetNode[r] = R.feedNode;
        }
    }
    const double meltK = meltC + TestMaterial::kZeroC;
    S.viscosity = [viscosity, meltK](double gammaDot) { return viscosity.eta(gammaDot, meltK); };
    S.flowRateMm3s = V.stats.volumeMm3 / fillTimeS;
    S.maxPressureMPa = maxPressureMPa;
    S.thermal = thermal;
    S.pack = pack;
    S.pack.enabled = pack.enabled && thermal.enabled;
    // The 1D gates freeze as cylinders of their own radius (cavity-only meshes):
    // per network node at a cavity entry, its gate's (or direct sprue's) radius.
    if (!V.fullShot)
    {
        S.pack.gateRadiusMm.assign((size_t)std::max(0, S.netNodes), 0.0);
        for (const Flow::FeedEdge& ce : net.edges)
        {
            if (ce.kind != Flow::FeedEdgeKind::CavityIn || ce.a < 0 || ce.a >= S.netNodes) continue;
            for (const Flow::FeedEdge& e : net.edges)
                if ((e.kind == Flow::FeedEdgeKind::Gate || e.kind == Flow::FeedEdgeKind::Sprue) && (e.a == ce.a || e.b == ce.a))
                {
                    const double r = e.section.shape == Flow::SectionShape::Circle
                                   ? 0.5 * e.section.diameterMm
                                   : std::sqrt(std::max(0.0f, e.section.areaMm2) / 3.14159265358979323846);
                    S.pack.gateRadiusMm[(size_t)ce.a] = r;
                    break;
                }
        }
    }

    // ---- Run it off the UI thread ----------------------------------------------------
    std::atomic<bool> cancel{ false }, done{ false };
    std::atomic<int> permille{ 0 };
    std::atomic<double> simTime{ 0.0 };
    S.progress = [&cancel, &permille, &simTime](double frac, double t)
    {
        permille = (int)std::lround(1000.0 * std::clamp(frac, 0.0, 1.0));
        simTime = t;
        return !cancel.load();
    };
    Flow3D::FillResult result;
    std::thread worker([&]
    {
        try
        {
            result = Flow3D::RunIsothermalFill(V.mesh, V.neighbours, V.boundary, S);
        }
        catch (const std::exception& e)
        {
            result = Flow3D::FillResult{};
            result.message = std::string("The solver stopped: ") + e.what();
        }
        catch (...)
        {
            result = Flow3D::FillResult{};
            result.message = "The solver stopped unexpectedly.";
        }
        done = true;
    });
    {
        wxProgressDialog prog(title, "Filling the 3D mesh...", 1000, wxGetTopLevelParent(this),
                              wxPD_APP_MODAL | wxPD_CAN_ABORT | wxPD_ELAPSED_TIME | wxPD_REMAINING_TIME);
        while (!done)
        {
            const bool packing = S.pack.enabled && permille.load() > 500;
            const wxString msg = cancel ? wxString("Stopping...")
                               : packing ? wxString::Format("Packing and cooling... (t = %.2f s)", simTime.load())
                                         : wxString::Format("Filling the 3D mesh... (t = %.3f s)", simTime.load());
            if (!prog.Update(std::min(999, permille.load()), msg) && !cancel) cancel = true;
            wxMilliSleep(100);
        }
    }
    worker.join();

    m_fill3d = std::move(result);
    if (cancel) m_fill3d.cancelled = true;
    if (!m_fill3d.ok || m_fill3d.cancelled) return false;
    m_hasFill3d = true;
    OnFill3DChanged();
    return true;
}

void PreviewPanel::OnFill3DChanged()
{
    ++m_fill3dSerial;
    m_fillFrame = -1;
    float pmax = 0.0f;
    for (const std::vector<float>& snap : m_fill3d.framePressureMPa)
        for (float v : snap) pmax = std::max(pmax, v);
    for (float v : m_fill3d.endPressureMPa) pmax = std::max(pmax, v);
    m_fill3dPressMax = pmax > 0.0f ? pmax : 1.0f;

    // Melt temperature views: one fixed range over the whole fill.
    float lo = 1e9f, hi = -1e9f;
    auto scan = [&](const std::vector<float>& v)
    {
        for (float c : v)
            if (c > Flow3D::FillResult::kNotFilledC + 1.0f) { lo = std::min(lo, c); hi = std::max(hi, c); }
    };
    for (const std::vector<float>& snap : m_fill3d.frameTempC) scan(snap);
    scan(m_fill3d.endTempC);
    scan(m_fill3d.frontTempC);
    if (!(hi > lo)) { lo = 0.0f; hi = 1.0f; }
    m_fill3dTempLo = lo;
    m_fill3dTempHi = hi;
}

// ---------------------------------------------------------------------------
// Report.
// ---------------------------------------------------------------------------
ResultsDetails::Report PreviewPanel::BuildFlow3DReport(const wxString& verdict, const wxColour& colour) const
{
    const VolumeMesh& V = m_volMesh;
    const TetMesh::Stats& st = V.stats;
    const wxString deg = U("\xc2\xb0");
    const wxString mm3 = U(" mm\xc2\xb3");

    RD::Report R;
    R.valid = V.ready();
    R.test = "3D Flow Analysis";
    R.verdict = verdict;
    R.verdictColour = colour;
    R.view = m_hasFill3d ? kViewFill3DTime : kViewVolumeMesh;

    // ---- Fill (first tab when there is one) -------------------------------------------
    if (m_hasFill3d)
    {
        const Flow3D::FillResult& F = m_fill3d;
        const wxString mpa = " MPa";
        RD::Table ft("Fill");
        ft.title = "Fill";
        ft.view = kViewFill3DTime;
        const wxString result = F.incomplete ? wxString("Short shot: part of the mesh was never reached")
                              : F.pressureLimited ? wxString("Filled, at the machine's pressure limit")
                                                  : wxString("Filled");
        ft.Add({ "Result", result }, F.incomplete ? Status::Fail : F.pressureLimited ? Status::Warn : Status::Pass);
        ft.Add({ "Fill time", wxString::Format("%.3f s (target %.3g s)", F.endTimeS, m_fill3dTargetS) });
        ft.Add({ "Injection rate", wxString::Format("%.1f", V.stats.volumeMm3 / std::max(1e-9, m_fill3dTargetS)) + mm3 + "/s" });
        ft.Add({ "Peak machine pressure", wxString::Format("%.2f", F.maxInletMPa) + mpa +
                                          wxString::Format(" (limit %.0f MPa)", m_fill3dLimitMPa) },
               F.pressureLimited ? Status::Warn : Status::Pass);
        ft.Add({ "Filled volume", wxString::Format("%.1f of %.1f", F.filledVolumeMm3, F.cavityVolumeMm3) + mm3 });
        ft.Add({ "Peak shear rate", wxString::Format("%.0f 1/s (material guideline %.0f 1/s)", F.maxShearRate, m_fill3dMaxShear) },
               F.maxShearRate > m_fill3dMaxShear ? Status::Warn : Status::Pass);
        ft.Add({ "Material", m_fill3dMaterial });
        if (F.thermal)
        {
            ft.Add({ "Melt temperature", wxString::Format("%.0f", m_fill3dMeltC) + DegC() + " at the inlets" });
            ft.Add({ "Mould temperature", wxString::Format("%.0f", m_fill3dMouldC) + DegC() +
                                          wxString::Format(" (the melt sees %.1f", F.wallTempC) + DegC() + " at the wall)" });
            ft.Add({ "Coldest melt arriving at the front", wxString::Format("%.1f", F.minFrontTempC) + DegC() });
            ft.Add({ "Hottest melt (shear heating)", wxString::Format("%.1f", F.maxTempC) + DegC() });
            ft.Add({ "Thickest frozen skin at the end of fill", wxString::Format("%.3f mm", F.maxFrozenMm) });
        }
        else
            ft.Add({ "Melt temperature", wxString::Format("%.0f", m_fill3dMeltC) + DegC() + " (held constant: isothermal fill)" });
        ft.note = F.thermal
            ? "3D Stokes flow of a shear-thinning melt (Cross-WLF) with its temperature carried by the flow, "
              "conducted and heated by shear. At the walls a sub-grid model (a thin boundary layer against the "
              "mould) sets the heat lost and grows the frozen skin, which narrows the flow. Latent heat and "
              "pressure-dependent viscosity are not modelled; the 1D feed beams stay at the melt temperature."
            : "3D Stokes flow of a shear-thinning melt (Cross-WLF at the melt temperature): no inertia, "
              "no cooling or frozen layer. Pressures run high when few elements span a wall "
              "(about 10% with 3 across).";
        R.tables.push_back(ft);

        // ---- Pack & cool ----
        if (F.pack.ran)
        {
            const Flow3D::FillResult::PackResult& K = F.pack;
            RD::Table pk("Pack & cool");
            pk.title = "Packing, holding and cooling";
            pk.view = kViewPack3DShrink;
            pk.Add({ "Result", wxString::FromUTF8(K.message.c_str()) }, K.ejectReached ? Status::Pass : Status::Warn);
            pk.Add({ "Pack pressure", wxString::Format("%.1f MPa", K.packPressureMPa) });
            pk.Add({ "Pressure held for", wxString::Format("%.2f s after the fill", K.holdTimeS) });
            pk.Add({ "Gates sealed", K.gatesFrozen ? wxString::Format("%.2f s (from injection start)", K.gateFreezeS)
                                                   : wxString("not within the hold") },
                   K.gatesFrozen ? Status::Pass : Status::Warn);
            if (V.fullShot)
                pk.Add({ "Parts ejectable (their melt below the ejection temperature)",
                         m_fill3dPartEjectS >= 0.0 ? wxString::Format("%.2f s (from injection start)", m_fill3dPartEjectS)
                                                   : wxString("not reached") },
                       m_fill3dPartEjectS >= 0.0 ? Status::Pass : Status::Warn);
            pk.Add({ V.fullShot ? "Whole shot below the ejection temperature (incl. sprue and runners)"
                                : "Ejectable (all melt below the ejection temperature)",
                     K.ejectReached ? wxString::Format("%.2f s (from injection start)", K.ejectS) : wxString("not reached") },
                   K.ejectReached ? Status::Pass : Status::Warn);
            pk.Add({ "Shot mass at ejection", wxString::Format("%.3f g (%.3f g packed in after the fill)", K.massG, K.packedMassG) });
            pk.Add({ "Volumetric shrinkage", wxString::Format("%.2f%% mean, %.2f%% to %.2f%%", K.meanShrinkPct, K.minShrinkPct, K.maxShrinkPct) });
            pk.Add({ "Mass check (gained vs in through the inlets)", wxString::Format("%.3f%%", K.massBalanceErrPct) },
                   K.massBalanceErrPct > 1.0 ? Status::Warn : Status::None);
            pk.Add({ "Steps / solves", wxString::Format("%d / %d", K.steps, K.solves) });
            pk.note = "Shrinkage is each node's volume loss cooled to room temperature, unloaded, from the mass it "
                      "holds at the end (Tait pvT). The nozzle holds the pack pressure until the gates seal (the "
                      "1D gates of a cavity-only mesh freeze as cylinders of their own radius; with a full-shot "
                      "mesh, once melt stops coming in), then drops to zero.";
            R.tables.push_back(pk);

            if (!K.gateFreezePerRegion.empty())
            {
                RD::Table gt("Pack & cool", { "Inlet", "Sealed at" });
                gt.title = "Gate freeze";
                for (size_t r = 0; r < V.boundary.regions.size() && r < K.gateFreezePerRegion.size(); ++r)
                {
                    if (V.boundary.regions[r].tag != Flow3D::TagInlet) continue;
                    const float g = K.gateFreezePerRegion[r];
                    gt.Add({ wxString::FromUTF8(V.boundary.regions[r].label.c_str()),
                             g >= 0.0f ? wxString::Format("%.2f s", g) : wxString("not sealed") },
                           g >= 0.0f ? Status::None : Status::Warn);
                }
                R.tables.push_back(gt);
            }
        }

        // Per inlet: its flow at the end of fill and its share.
        RD::Table it("Fill", { "Inlet", "Flow", "Share", "Pressure at the inlet" });
        it.title = "Inlets";
        double qSum = 0.0;
        for (size_t r = 0; r < V.boundary.regions.size() && r < F.regionFlowMm3s.size(); ++r)
            if (V.boundary.regions[r].tag == Flow3D::TagInlet) qSum += std::max(0.0f, F.regionFlowMm3s[r]);
        for (size_t r = 0; r < V.boundary.regions.size() && r < F.regionFlowMm3s.size(); ++r)
        {
            const Flow3D::BoundaryRegion& reg = V.boundary.regions[r];
            if (reg.tag != Flow3D::TagInlet) continue;
            const int k = V.fullShot ? 0 : reg.feedNode;
            const wxString p = (k >= 0 && (size_t)k < F.netPressureMPa.size())
                             ? wxString::Format("%.2f", F.netPressureMPa[(size_t)k]) + mpa : wxString("-");
            it.Add({ wxString::FromUTF8(reg.label.c_str()), wxString::Format("%.1f", F.regionFlowMm3s[r]) + mm3 + "/s",
                     qSum > 0.0 ? wxString::Format("%.1f%%", 100.0 * F.regionFlowMm3s[r] / qSum) : wxString("-"), p });
        }
        it.note = "At the end of fill.";
        R.tables.push_back(it);

        // The feed network's pressures (cavity-only meshes).
        if (!V.fullShot && m_hasFeedNetwork)
        {
            RD::Table nt("Fill", { "Feed node", "Pressure" });
            nt.title = "Feed system (1D beams)";
            for (size_t k = 0; k < m_feedNetwork.nodes.size() && k < F.netPressureMPa.size(); ++k)
            {
                const Flow::FeedNode& n = m_feedNetwork.nodes[k];
                if (n.kind == Flow::FeedNodeKind::Part || n.kind == Flow::FeedNodeKind::VentStart ||
                    n.kind == Flow::FeedNodeKind::VentOutlet) continue;
                nt.Add({ wxString::FromUTF8(n.label.c_str()), wxString::Format("%.2f", F.netPressureMPa[k]) + mpa });
            }
            nt.note = "At the end of fill. The sprue inlet is the machine nozzle.";
            R.tables.push_back(nt);
        }

        RD::Table sv("Fill");
        sv.title = "Solver";
        sv.Add({ "Time steps", wxString::Format("%d", F.stats.steps) });
        sv.Add({ "Linear solves", wxString::Format("%d (FGMRES + AMG Schur preconditioner)", F.stats.solves) });
        sv.Add({ "Iterations per solve", wxString::Format("%.1f mean, %d max", F.stats.meanIterations, F.stats.maxIterations) });
        sv.Add({ "Largest system", wxString::Format("%zu unknowns", F.stats.maxUnknowns) });
        sv.Add({ "Worst residual", wxString::Format("%.1e", F.stats.worstResidual) },
               F.stats.worstResidual > 1e-4 ? Status::Warn : Status::None);
        sv.Add({ "Backflow at the front (dropped)", wxString::Format("%.2f%%", F.stats.backflowPct) },
               F.stats.backflowPct > 2.0 ? Status::Warn : Status::None);
        sv.Add({ "Time", wxString::Format("%.1f s (%.1f s in the linear solves)", F.stats.totalSeconds, F.stats.solveSeconds) });
        R.tables.push_back(sv);

        if (!F.warnings.empty())
        {
            RD::Table ct("Fill", { "Caution" });
            ct.title = "Cautions";
            for (const std::string& w : F.warnings) ct.Add({ wxString::FromUTF8(w.c_str()) }, Status::Warn);
            R.tables.push_back(ct);
        }
    }

    // Mesh
    RD::Table mesh("Mesh");
    mesh.view = kViewVolumeMesh;
    if (!V.sizeNote.empty()) mesh.note = V.sizeNote;
    mesh.title = "Volume mesh";
    mesh.Add({ "Domain", V.fullShot ? "Full shot (sprue, runners, gates, parts)"
                                    : "Part cavities (feed system as a 1D beam network)" });
    mesh.Add({ "Elements (tetrahedra)", wxString::Format("%zu", st.tets) });
    mesh.Add({ "Nodes", wxString::Format("%zu", st.verts) });
    mesh.Add({ "Boundary faces", wxString::Format("%zu", st.boundaryFaces) });
    mesh.Add({ "Source surface", wxString::Format("%zu triangles", V.surfaceTris) });
    if (V.wallMm > 0.0)
        mesh.Add({ "Thin wall (5th percentile by area)", wxString::Format("%.3g mm", V.wallMm) });
    else
        mesh.Add({ "Thin wall", "not measured" }, Status::Info);
    mesh.Add({ "Target edge length", wxString::Format("%.3g mm", V.edgeMm) });
    mesh.Add({ "Mean edge length", wxString::Format("%.3g mm", st.meanEdgeMm) });
    if (V.wallMm > 0.0 && st.meanEdgeMm > 0.0)
        mesh.Add({ "Elements across the thin wall", wxString::Format("%.1f", V.wallMm / st.meanEdgeMm) });
    mesh.Add({ "Surface envelope", wxString::Format("%.3g mm", V.epsMm) });
    R.tables.push_back(mesh);

    // Volume
    RD::Table vol("Mesh");
    vol.title = "Volume";
    vol.columns = { "Quantity", "Value", "Difference" };
    const double diff = V.sourceVolMm3 > 0.0 ? 100.0 * (st.volumeMm3 / V.sourceVolMm3 - 1.0) : 0.0;
    const Status vs = std::fabs(diff) > 5.0 ? Status::Fail : std::fabs(diff) > 2.0 ? Status::Warn : Status::Pass;
    vol.Add({ "Mesh volume", wxString::Format("%.2f", st.volumeMm3) + mm3, wxString::Format("%+.3f%%", diff) }, vs);
    vol.Add({ "Source surface volume", wxString::Format("%.2f", V.sourceVolMm3) + mm3, "" });
    if (V.fullShot && m_shotVolumeMm3 > 0.0)
        vol.Add({ "Shot at Generate Mould", wxString::Format("%.2f", m_shotVolumeMm3) + mm3,
                  wxString::Format("%+.3f%%", 100.0 * (st.volumeMm3 / m_shotVolumeMm3 - 1.0)) });
    vol.note = "The mesh follows the surface to within the envelope and rounds sharp edges slightly, "
               "so about 1% less on a thin part is normal. More than a few percent means an open "
               "surface or a lost feature.";
    R.tables.push_back(vol);

    // Quality
    RD::Table q("Quality");
    q.view = kViewVolumeMesh;
    q.title = "Element shape";
    const Status sliverS = st.tets && (double)st.slivers / (double)st.tets > 0.001 ? Status::Warn : Status::Pass;
    q.Add({ "Smallest dihedral angle", wxString::Format("%.2f", st.minDihedralDeg) + deg });
    q.Add({ "Largest dihedral angle", wxString::Format("%.2f", st.maxDihedralDeg) + deg });
    q.Add({ U("Slivers (a dihedral under 5\xc2\xb0)"),
            wxString::Format("%zu (%.3f%%)", st.slivers, st.tets ? 100.0 * (double)st.slivers / (double)st.tets : 0.0) },
          sliverS);
    q.Add({ "Inverted elements", wxString::Format("%zu", st.nonPositive) },
          st.nonPositive ? Status::Fail : Status::Pass);
    R.tables.push_back(q);

    // Quality histogram
    RD::Table h("Quality");
    h.title = "Smallest dihedral angle per element";
    h.columns = { "Range", "Elements", "Share" };
    static const double edges[] = { 0, 5, 10, 20, 30, 40, 50, 60, 71 };
    constexpr int nb = (int)(sizeof(edges) / sizeof(edges[0])) - 1;
    size_t counts[nb] = {};
    for (float d : V.minDihedral)
    {
        int b = 0;
        while (b + 1 < nb && d >= edges[b + 1]) ++b;
        ++counts[b];
    }
    for (int b = 0; b < nb; ++b)
    {
        const wxString range = (b + 1 == nb)
            ? wxString::Format("%g", edges[b]) + deg + " and up"
            : wxString::Format("%g", edges[b]) + U("\xe2\x80\x93") + wxString::Format("%g", edges[b + 1]) + deg;
        h.Add({ range, wxString::Format("%zu", counts[b]),
                wxString::Format("%.2f%%", st.tets ? 100.0 * (double)counts[b] / (double)st.tets : 0.0) },
              b == 0 && counts[b] ? Status::Warn : Status::None);
    }
    h.note = U("A regular tetrahedron's dihedral angle is 70.5\xc2\xb0. Elements under about 10\xc2\xb0 "
               "(slivers) slow and degrade the flow solve; a few are normal.");
    R.tables.push_back(h);

    // Boundary conditions
    const Flow3D::Boundary& BC = V.boundary;
    if (BC.Ready())
    {
        const wxString mm2 = U(" mm\xc2\xb2");
        double totalArea = 0.0;
        for (double a : BC.areaMm2) totalArea += a;

        RD::Table bt("Boundary");
        bt.view = kViewVolumeMesh;
        bt.title = "Boundary conditions";
        bt.columns = { "Condition", "Faces", "Area", "Share" };
        static const uint8_t order[] = { Flow3D::TagInlet, Flow3D::TagVent, Flow3D::TagParting, Flow3D::TagWall };
        for (uint8_t tg : order)
        {
            Status st = Status::None;
            if (tg == Flow3D::TagInlet) st = BC.faces[tg] ? Status::Pass : Status::Fail;
            bt.Add({ Flow3D::BoundaryTagName(tg), wxString::Format("%zu", BC.faces[tg]),
                     wxString::Format("%.2f", BC.areaMm2[tg]) + mm2,
                     wxString::Format("%.2f%%", totalArea > 0.0 ? 100.0 * BC.areaMm2[tg] / totalArea : 0.0) }, st);
        }
        bt.note = V.fullShot
            ? "Full shot: the melt enters at the sprue's entry face. Walls are no-slip against the mould; "
              "air escapes at the vents and along the parting line."
            : "Cavities only: the melt enters at each gate mouth, fed by the 1D beam model of the sprue, "
              "runners and gates. Walls are no-slip against the mould; air escapes at the vents and "
              "along the parting line.";
        R.tables.push_back(bt);

        RD::Table rt("Boundary");
        rt.title = "Inlets and vents";
        rt.columns = { "Region", "Condition", "Faces", "Area", "Gate / sprue section", "Centre (mm)", "Found by" };
        for (const Flow3D::BoundaryRegion& r : BC.regions)
        {
            const wxString centre = wxString::Format("%.1f, %.1f, %.1f", r.centre.x, r.centre.y, r.centre.z);
            const wxString how = r.byPosition ? wxString("position (footprint not found)")
                               : r.tag == Flow3D::TagInlet ? wxString(V.fullShot ? "sprue entry face" : "gate footprint")
                                                           : wxString("vent mouth");
            Status st = r.faces == 0 ? Status::Fail : r.byPosition ? Status::Warn : Status::Pass;
            rt.Add({ wxString::FromUTF8(r.label.c_str()), Flow3D::BoundaryTagName(r.tag),
                     wxString::Format("%zu", r.faces), wxString::Format("%.3f", r.areaMm2) + mm2,
                     r.nominalAreaMm2 > 0.0 ? wxString::Format("%.3f", r.nominalAreaMm2) + mm2 : wxString("-"),
                     centre, how }, st);
        }
        rt.note = "A gate mouth's area is the mesh faces the gate covers, so it runs a little under the "
                  "gate's own section at coarse densities.";
        R.tables.push_back(rt);

        if (!BC.warnings.empty())
        {
            RD::Table ct("Boundary", { "Caution" });
            ct.title = "Cautions";
            for (const std::string& w : BC.warnings) ct.Add({ wxString::FromUTF8(w.c_str()) }, Status::Warn);
            R.tables.push_back(ct);
        }
    }

    // Timing
    RD::Table t("Mesh");
    t.title = "Time";
    t.Add({ "Meshing", wxString::Format("%.1f s", V.meshSeconds) });
    t.Add({ "Total (worker start, file transfer)", wxString::Format("%.1f s", V.wallSeconds) });
    R.tables.push_back(t);
    return R;
}

// ---------------------------------------------------------------------------
// Export.
// ---------------------------------------------------------------------------
void PreviewPanel::ExportVolumeMesh()
{
    if (!m_volMesh.ready()) return;
    wxWindow* top = wxGetTopLevelParent(this);
    wxFileDialog dlg(top, "Export 3D Mesh", wxEmptyString, "mesh.vtu",
                     "ParaView unstructured grid (*.vtu)|*.vtu|Gmsh mesh (*.msh)|*.msh",
                     wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
    if (dlg.ShowModal() != wxID_OK) return;

    wxFileName fn(dlg.GetPath());
    const bool msh = dlg.GetFilterIndex() == 1;
    if (fn.GetExt().empty()) fn.SetExt(msh ? "msh" : "vtu");

    std::string err;
    bool ok;
    {
        wxBusyCursor busy;
        const std::filesystem::path path(fn.GetFullPath().ToStdWstring());
        const std::vector<uint8_t>* tags = m_volMesh.boundary.Ready() ? &m_volMesh.boundary.slotTag : nullptr;
        const std::vector<std::string> names = { "wall", "inlet", "vent", "parting_line" };
        // The 3D fill's node results ride along in the .vtu (fill time,
        // end-of-fill pressure and melt speed; -1 = never filled).
        std::vector<std::pair<std::string, const std::vector<float>*>> pointData;
        if (m_hasFill3d && m_fill3d.fillTimeS.size() == m_volMesh.mesh.VertexCount())
        {
            pointData.push_back({ "fill_time_s", &m_fill3d.fillTimeS });
            pointData.push_back({ "pressure_end_MPa", &m_fill3d.endPressureMPa });
            pointData.push_back({ "speed_end_mm_s", &m_fill3d.endSpeedMmS });
            if (m_fill3d.thermal)
            {
                pointData.push_back({ "temperature_end_C", &m_fill3d.endTempC });
                pointData.push_back({ "front_temperature_C", &m_fill3d.frontTempC });
                pointData.push_back({ "frozen_skin_mm", &m_fill3d.endFrozenMm });
            }
            if (m_fill3d.pack.ran)
            {
                pointData.push_back({ "shrinkage_pct", &m_fill3d.pack.shrinkPct });
                pointData.push_back({ "eject_time_s", &m_fill3d.pack.ejectTimeS });
            }
        }
        ok = msh ? TetMesh::WriteGmshMsh(path, m_volMesh.mesh, err, tags, names)
                 : TetMesh::WriteVtu(path, m_volMesh.mesh, &m_volMesh.minDihedral, err, tags, pointData);
    }
    if (!ok)
        wxMessageBox("Couldn't export the mesh:\n\n" + wxString::FromUTF8(err.c_str()) + "\n\n" + fn.GetFullPath(),
                     "Export 3D Mesh", wxOK | wxICON_ERROR, top);
}

// ---------------------------------------------------------------------------
// Sim Viewer "3D mesh".
// ---------------------------------------------------------------------------
void PreviewPanel::UpdateSectionControls()
{
    if (!m_sectionGroup || !m_resultsBar) return;
    const int mode = m_debugModeChoice ? m_debugModeChoice->GetSelection() : 0;
    const bool on = Is3DView(mode);
    if (m_sectionGroup->IsShown() != on)
    {
        m_sectionGroup->Show(on);
        m_resultsBar->ControlsParent()->Layout();
        m_resultsBar->Layout();
    }
    const bool have = m_volMesh.ready();
    const bool cut = m_sectionAxisChoice && m_sectionAxisChoice->GetSelection() > 0;
    m_sectionGroup->Enable(have);
    if (m_sectionSlider) m_sectionSlider->Enable(have && cut);
    if (m_sectionFlipCheck) m_sectionFlipCheck->Enable(have && cut);
    if (m_meshColourChoice) m_meshColourChoice->Enable(have && mode == kViewVolumeMesh);   // the fill views colour by result
}

void PreviewPanel::RequestVolumeMeshRedraw()
{
    if (m_volumeRedrawPending) return;
    m_volumeRedrawPending = true;
    CallAfter([this]
    {
        m_volumeRedrawPending = false;
        if (m_debugModeChoice && Is3DView(m_debugModeChoice->GetSelection()))
            UpdateDraftOverlay();
    });
}

void PreviewPanel::DrawVolumeMeshView(bool wire)
{
    if (!m_canvas) return;
    const int mode = m_debugModeChoice ? m_debugModeChoice->GetSelection() : kViewVolumeMesh;
    const bool fillView = mode >= kViewFill3DTime && mode <= kViewCool3DEject;
    const bool thermalView = mode >= kViewFill3DTemp && mode <= kViewFill3DFrozen;
    const bool packView = mode == kViewPack3DShrink || mode == kViewCool3DEject;   // static, after packing
    if (m_shotHalfIndex < 0 || !m_volMesh.ready() || (fillView && !m_hasFill3d) ||
        (thermalView && !m_fill3d.thermal) || (packView && !m_fill3d.pack.ran))
    {
        m_canvas->ClearShotDebugColoring();
        m_canvas->SetShotDebugWireframe(false);
        ShowResultsBar(false);
        return;
    }

    const VolumeMesh& V = m_volMesh;
    const TetMesh::Mesh& M = V.mesh;
    const size_t nt = M.TetCount();
    const int colourMode = (!fillView && m_meshColourChoice) ? m_meshColourChoice->GetSelection() : 0;
    const bool quality = colourMode == 1;
    const bool bcMode = colourMode == 2 && V.boundary.Ready();

    // Section: keep the tets whose centroid is on the near side of the plane.
    const int axis = m_sectionAxisChoice ? m_sectionAxisChoice->GetSelection() - 1 : -1;
    const bool flip = m_sectionFlipCheck && m_sectionFlipCheck->GetValue();
    double cut = 0.0;
    if (axis >= 0)
    {
        const double t = m_sectionSlider ? (double)m_sectionSlider->GetValue() / 1000.0 : 0.5;
        cut = V.bbMin[axis] + t * (V.bbMax[axis] - V.bbMin[axis]);
    }
    std::vector<unsigned char> keep(nt, 1);
    if (axis >= 0)
        for (size_t i = 0; i < nt; ++i)
        {
            const int32_t* t = &M.tets[4 * i];
            const double c = 0.25 * (M.verts[3 * (size_t)t[0] + (size_t)axis] + M.verts[3 * (size_t)t[1] + (size_t)axis] +
                                     M.verts[3 * (size_t)t[2] + (size_t)axis] + M.verts[3 * (size_t)t[3] + (size_t)axis]);
            keep[i] = (unsigned char)(flip ? (c >= cut) : (c <= cut));
        }

    // ---- Fill views: the node values at the timeline's frame ---------------------------
    const std::vector<float>* nodeVal = nullptr;
    float lo = 0.0f, hi = 1.0f;
    float tFrame = std::numeric_limits<float>::infinity();
    if (fillView)
    {
        const Flow3D::FillResult& FR = m_fill3d;
        const bool packed = FR.pack.ran;
        int f = -1;
        if (!packView)
        {
            FlowResultsBar::TimelineData td;
            td.key = 1000000000L + m_fill3dSerial;   // apart from the 2.5D fills' keys
            for (const Flow3D::FillFrame& fr : FR.frames)
            {
                td.times.push_back(fr.timeS);
                td.filledPct.push_back(fr.filledPct);
                td.inletMPa.push_back(fr.inletMPa);
                td.phases.push_back(fr.phase);
                td.historyT.push_back(fr.timeS);
                td.historyMPa.push_back(fr.inletMPa);
            }
            td.endTime = (float)(packed ? FR.pack.endS : FR.endTimeS);
            td.endTitle = FR.incomplete ? "End (short shot)"
                        : packed ? (FR.pack.ejectReached ? "Ejectable" : "End") : "End of fill";
            m_resultsBar->SetTimeline(td);
            const int pos = m_resultsBar->GetPosition();
            f = (pos >= m_resultsBar->GetEndPosition() || pos < 0 || (size_t)pos >= FR.frames.size()) ? -1 : pos;
            m_fillFrame = f;
            if (f >= 0) tFrame = FR.frames[(size_t)f].timeS;
        }

        // The latest snapshot at or before the frame (the end-of-fill field at the end).
        auto atFrame = [&](const std::vector<std::vector<float>>& snaps, const std::vector<float>& end)
        {
            // After packing the end state is the last pack frame (ejection).
            const std::vector<float>* v = (packed && !snaps.empty()) ? &snaps.back() : &end;
            if (f >= 0)
                for (int k = f; k >= 0; --k)
                {
                    const int sidx = FR.frames[(size_t)k].snapshot;
                    if (sidx >= 0 && (size_t)sidx < snaps.size()) { v = &snaps[(size_t)sidx]; break; }
                }
            return v;
        };
        switch (mode)
        {
        case kViewFill3DTime:
            nodeVal = &FR.fillTimeS;
            hi = std::max(1.0e-6f, (float)FR.endTimeS);
            break;
        case kViewFill3DPressure:
            nodeVal = atFrame(FR.framePressureMPa, FR.endPressureMPa);
            hi = m_fill3dPressMax;
            break;
        case kViewFill3DTemp:
            nodeVal = atFrame(FR.frameTempC, FR.endTempC);
            lo = m_fill3dTempLo;
            hi = m_fill3dTempHi;
            break;
        case kViewFill3DFrontTemp:
            nodeVal = &FR.frontTempC;
            lo = m_fill3dTempLo;
            hi = m_fill3dTempHi;
            break;
        case kViewFill3DFrozen:
        {
            nodeVal = atFrame(FR.frameFrozenMm, FR.endFrozenMm);
            float fmax = (float)FR.maxFrozenMm;
            if (packed)   // the skin keeps growing through packing
                for (const std::vector<float>& snap : FR.frameFrozenMm)
                    for (float v : snap) if (v < Flow3D::FillResult::kFrozenThrough) fmax = std::max(fmax, v);
            hi = std::max(0.05f, fmax);
            break;
        }
        case kViewPack3DShrink:
            nodeVal = &FR.pack.shrinkPct;
            lo = (float)FR.pack.minShrinkPct;
            hi = std::max(lo + 0.01f, (float)FR.pack.maxShrinkPct);
            break;
        default:   // time to eject
        {
            nodeVal = &FR.pack.ejectTimeS;
            float emin = 1e30f, emax = 0.0f;
            for (float v : FR.pack.ejectTimeS) if (v >= 0.0f) { emin = std::min(emin, v); emax = std::max(emax, v); }
            if (!(emax > emin)) { emin = 0.0f; emax = std::max(1.0f, emax); }
            lo = emin;
            hi = emax;
            break;
        }
        }
    }
    auto filledAt = [&](int32_t v)
    {
        const float ft = m_fill3d.fillTimeS[(size_t)v];
        return ft >= 0.0f && ft <= tFrame;
    };

    // ---- Groups ------------------------------------------------------------------------
    // Fill views: value bands + unfilled grey. Boundary: wall, inlet, vent,
    // parting line, section. Quality: bands, worst red. Mesh: surface / section.
    constexpr int nBands = Heatmap::kBands;
    constexpr float kGoodDeg = 70.0f;   // ~ a regular tet (70.5)
    constexpr size_t kBcSection = Flow3D::kBoundaryTagCount;
    const size_t kUnfilled = (size_t)nBands, kThrough = (size_t)nBands + 1;
    std::vector<GLCanvas::ShotDebugGroup> groups((fillView || quality) ? (size_t)nBands + (fillView ? 2 : 0)
                                                 : bcMode ? kBcSection + 1 : 2);
    if (fillView || quality)
    {
        for (int b = 0; b < nBands; ++b)
        {
            float r, g, bl;
            Heatmap::Band(b, r, g, bl);
            groups[(size_t)b].color = glm::vec3(r, g, bl);
            groups[(size_t)b].emissive = true;
        }
        if (fillView)
        {
            groups[kUnfilled].color = glm::vec3(Heatmap::kNoValue[0], Heatmap::kNoValue[1], Heatmap::kNoValue[2]);
            groups[kThrough].color = glm::vec3(0.93f, 0.95f, 1.00f);   // frozen through (frozen-layer view)
            groups[kThrough].emissive = true;
        }
    }
    else if (bcMode)
    {
        // The Layout view's feature colours: gates yellow, vents green, the
        // parting-plane marker blue.
        groups[Flow3D::TagWall].color = glm::vec3(0.58f, 0.63f, 0.70f);      // lit steel-grey
        groups[Flow3D::TagInlet].color = glm::vec3(1.00f, 0.85f, 0.10f);     // gate yellow
        groups[Flow3D::TagInlet].emissive = true;
        groups[Flow3D::TagVent].color = glm::vec3(0.10f, 0.92f, 0.25f);      // vent green
        groups[Flow3D::TagVent].emissive = true;
        groups[Flow3D::TagParting].color = glm::vec3(0.10f, 0.40f, 0.95f);   // parting blue
        groups[Flow3D::TagParting].emissive = true;
        groups[kBcSection].color = glm::vec3(0.85f, 0.80f, 0.70f);           // section (lit)
    }
    else
    {
        groups[0].color = glm::vec3(0.46f, 0.62f, 0.80f);   // mesh surface (lit)
        groups[1].color = glm::vec3(0.93f, 0.72f, 0.38f);   // section faces (lit)
    }
    auto band = [](float v01)
    {
        return (size_t)std::clamp((int)std::floor(std::clamp(v01, 0.0f, 1.0f) * (float)nBands), 0, nBands - 1);
    };

    // ---- Faces: a kept tet's face on the boundary, or against a cut-away
    // neighbour. Flat-shaded soup (3 vertices per face).
    std::vector<float> pn;
    pn.reserve(18 * (V.stats.boundaryFaces + 1024));
    for (size_t i = 0; i < nt; ++i)
    {
        if (!keep[i]) continue;
        const int32_t* t = &M.tets[4 * i];
        for (int f = 0; f < 4; ++f)
        {
            const int32_t nb = V.neighbours[4 * i + (size_t)f];
            const bool boundary = nb < 0;
            if (!boundary && keep[(size_t)nb]) continue;
            const int32_t fv[3] = { t[TetMesh::kTetFace[f][0]], t[TetMesh::kTetFace[f][1]], t[TetMesh::kTetFace[f][2]] };
            const double* a = &M.verts[3 * (size_t)fv[0]];
            const double* b = &M.verts[3 * (size_t)fv[1]];
            const double* c = &M.verts[3 * (size_t)fv[2]];
            const glm::dvec3 A(a[0], a[1], a[2]), B(b[0], b[1], b[2]), C(c[0], c[1], c[2]);
            glm::dvec3 n = glm::cross(B - A, C - A);
            const double L = glm::length(n);
            n = L > 0.0 ? n / L : glm::dvec3(0.0, 1.0, 0.0);

            size_t g;
            if (fillView)
            {
                g = kUnfilled;
                if (filledAt(fv[0]) && filledAt(fv[1]) && filledAt(fv[2]) && nodeVal->size() == M.VertexCount())
                {
                    const float v0 = (*nodeVal)[(size_t)fv[0]], v1 = (*nodeVal)[(size_t)fv[1]], v2 = (*nodeVal)[(size_t)fv[2]];
                    // Temperatures use -1000 for "no value", shrinkage may be
                    // negative (over-packed); the others -1.
                    const float none = (mode == kViewFill3DTemp || mode == kViewFill3DFrontTemp)
                                     ? Flow3D::FillResult::kNotFilledC + 1.0f
                                     : mode == kViewPack3DShrink ? -1.0e9f : 0.0f;
                    const float thr = Flow3D::FillResult::kFrozenThrough;
                    if (mode == kViewFill3DFrozen && (v0 >= thr || v1 >= thr || v2 >= thr))
                        g = kThrough;
                    else if (v0 >= none && v1 >= none && v2 >= none)
                        g = band(((v0 + v1 + v2) / 3.0f - lo) / std::max(1e-12f, hi - lo));
                }
            }
            else if (quality)
                g = band(1.0f - V.minDihedral[i] / kGoodDeg);
            else if (bcMode)
            {
                const uint8_t tag = boundary ? V.boundary.slotTag[4 * i + (size_t)f] : (uint8_t)Flow3D::TagInterior;
                g = tag < Flow3D::kBoundaryTagCount ? (size_t)tag : kBcSection;
            }
            else
                g = boundary ? 0 : 1;
            const uint32_t base = (uint32_t)(pn.size() / 6);
            for (const glm::dvec3* p : { &A, &B, &C })
                pn.insert(pn.end(), { (float)p->x, (float)p->y, (float)p->z, (float)n.x, (float)n.y, (float)n.z });
            groups[g].indices.insert(groups[g].indices.end(), { base, base + 1, base + 2 });
        }
    }
    if (pn.empty())
    {
        // Everything is on the far side of the plane: draw nothing (one
        // degenerate, ungrouped triangle keeps the shot itself hidden).
        pn.assign(18, 0.0f);
        for (auto& g : groups) g.indices.clear();
    }
    m_canvas->SetShotDebugMesh(m_shotHalfIndex, pn, groups);
    m_canvas->SetShotDebugWireframe(wire);
    m_canvas->SetShotDebugEdges(!wire && !fillView);   // the fill views read better without the mesh lines

    // ---- Legend --------------------------------------------------------------------------
    FlowResultsBar::Legend Lg;
    Lg.valid = true;
    auto sw = [&groups](size_t g) { return Heatmap::ToColour(groups[g].color.r, groups[g].color.g, groups[g].color.b); };
    if (fillView)
    {
        switch (mode)
        {
        case kViewFill3DTime:      Lg.title = "3D fill time";         Lg.unit = "s"; break;
        case kViewFill3DPressure:  Lg.title = "3D pressure";          Lg.unit = "MPa"; break;
        case kViewFill3DTemp:      Lg.title = "3D melt temperature";  Lg.unit = U("\xc2\xb0""C"); break;
        case kViewFill3DFrontTemp: Lg.title = "3D front temperature"; Lg.unit = U("\xc2\xb0""C"); break;
        case kViewFill3DFrozen:    Lg.title = "3D frozen skin";       Lg.unit = "mm"; break;
        case kViewPack3DShrink:    Lg.title = "3D volumetric shrinkage"; Lg.unit = "%"; break;
        default:                   Lg.title = "3D time to ejection temperature (from injection start)"; Lg.unit = "s"; break;
        }
        Lg.lo = lo;
        Lg.hi = hi;
        Lg.swatches.push_back({ mode == kViewCool3DEject ? "Not reached" : "Unfilled", sw(kUnfilled) });
        if (mode == kViewFill3DFrozen) Lg.swatches.push_back({ "Frozen through", sw(kThrough) });
    }
    else if (quality)
    {
        // Reversed range: well-shaped (70) at the blue end, slivers at red.
        Lg.title = "Min dihedral angle";
        Lg.unit = U("\xc2\xb0");
        Lg.lo = kGoodDeg;
        Lg.hi = 0.0f;
        Lg.decimals = 0;
    }
    else if (bcMode)
    {
        const Flow3D::Boundary& BC = V.boundary;
        Lg.title = "Boundary conditions";
        Lg.showRamp = false;
        Lg.swatches.push_back({ wxString::Format("Inlet (%d)", BC.Count(Flow3D::TagInlet)), sw(Flow3D::TagInlet) });
        Lg.swatches.push_back({ wxString::Format("Vent (%d)", BC.Count(Flow3D::TagVent)), sw(Flow3D::TagVent) });
        Lg.swatches.push_back({ "Parting line", sw(Flow3D::TagParting) });
        Lg.swatches.push_back({ "Wall", sw(Flow3D::TagWall) });
        if (axis >= 0) Lg.swatches.push_back({ "Section", sw(kBcSection) });
    }
    else
    {
        Lg.title = wxString::Format("3D mesh: %s elements", Count((double)V.stats.tets));
        Lg.showRamp = false;
        Lg.swatches.push_back({ "Mesh surface", sw(0) });
        if (axis >= 0) Lg.swatches.push_back({ "Section", sw(1) });
    }
    m_resultsBar->SetLegend(Lg);
    ShowResultsBar(true, fillView && !packView);
}
