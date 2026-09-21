#include "PreviewPanel.h"
#include "GLCanvas.h"
#include <wx/progdlg.h>
#include <thread>
#include <atomic>
#include "style.h"
#include "DesignChecks.h"
#include "RoundedButton.h"
#include "MouldCastDialog.h"
#include "MeshBoolean.h"   // split the shot at y=0 and fuse a half into each base

// OCC — BREP cast bodies (STEP-exportable) for BREP scenes.
#include <opencascade/BRepPrimAPI_MakeBox.hxx>
#include <opencascade/BRepAlgoAPI_Common.hxx>
#include <opencascade/BRepAlgoAPI_Fuse.hxx>
#include <opencascade/BRepAlgoAPI_Cut.hxx>
#include <opencascade/gp_Pnt.hxx>
#include <opencascade/BRepBuilderAPI_Sewing.hxx>     // overlap mesh -> shell (sep overlay)
#include <opencascade/BRepBuilderAPI_MakePolygon.hxx>
#include <opencascade/BRepBuilderAPI_MakeFace.hxx>

#include <wx/spinctrl.h>
#include <wx/scrolwin.h>
#include <wx/tglbtn.h>
#include <wx/bmpbndl.h>
#include <wx/file.h>
#include <wx/filename.h>
#include <wx/stdpaths.h>

#include <algorithm>
#include <cmath>
#include <functional>
#include <string>
#include <utility>
#include <vector>

// ---------------------------------------------------------------------------
// Chevron icons + SVG loader, kept file-local so the preview's collapsible
// cards match the Prepare-side collapsible sections. (MainFrame has its own
// internal-linkage copy; duplicated here to avoid coupling the two TUs.)
// ---------------------------------------------------------------------------
static const wxString kChevronDownSvg = "res/icons/chevron-down.svg";
static const wxString kChevronRightSvg = "res/icons/chevron-right.svg";

static wxBitmapBundle LoadSvgBundle(const wxString& svgPath,
    const wxSize& size, bool recolorWhite = false)
{
    if (svgPath.IsEmpty()) return wxBitmapBundle();

    wxFileName fn(svgPath);
    if (fn.IsRelative())
    {
        wxFileName exeDir(wxStandardPaths::Get().GetExecutablePath());
        fn.MakeAbsolute(exeDir.GetPath());
    }

    wxFile file(fn.GetFullPath());
    if (!file.IsOpened()) return wxBitmapBundle();

    wxString svg;
    file.ReadAll(&svg);

    if (recolorWhite)
    {
        svg.Replace("currentColor", "white");
        svg.Replace("\"black\"", "\"white\"");
        svg.Replace("\"#000000\"", "\"white\"");
        svg.Replace("\"#000\"", "\"white\"");
    }

    const wxScopedCharBuffer utf8 = svg.utf8_str();
    return wxBitmapBundle::FromSVG(utf8.data(), size);
}

// ---------------------------------------------------------------------------
// Id base for the visibility checkboxes — one sequential id per part so each
// checkbox has a unique, stable id (the part index it controls is captured in
// the handler lambda).
// ---------------------------------------------------------------------------
static const int kHalfToggleIdBase = wxID_HIGHEST + 5000;

// Letter label for a half index: 0 -> "A", 1 -> "B", ... wrapping is not a
// concern (moulds have two halves), but stay defined past 'Z' just in case.
static std::string HalfLetter(int index)
{
    if (index < 0) return std::string();
    if (index < 26) return std::string(1, char('A' + index));
    return std::to_string(index + 1);
}

// Feature-input field metrics, mirroring the Prepare-side mould-feature rows.
static const int kFieldWidth = 90;   // text-entry width (px)
static const int kUnitWidth = 28;    // fixed unit-label column (px)
static const int kFieldGap = 4;      // gap between field and unit label

// A feature-style parameter row: label (left) + text field + unit label
// (right), matching the mould-feature inputs. Returns the field for reads.
static wxTextCtrl* AddFieldRow(wxWindow* parent, wxBoxSizer* into,
    const wxString& label, const wxString& defaultVal, const wxString& unitStr)
{
    auto* row = new wxBoxSizer(wxHORIZONTAL);

    auto* lbl = new wxStaticText(parent, wxID_ANY, label);
    lbl->SetForegroundColour(Style::TextMuted);
    lbl->SetFont(wxFont(8, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
        wxFONTWEIGHT_NORMAL, false, "Segoe UI"));

    auto* ctrl = new wxTextCtrl(parent, wxID_ANY, defaultVal,
        wxDefaultPosition, wxSize(kFieldWidth, 22));
    ctrl->SetBackgroundColour(Style::BtnSmall);
    ctrl->SetForegroundColour(Style::TextPrimary);

    auto* unit = new wxStaticText(parent, wxID_ANY, unitStr);
    unit->SetForegroundColour(Style::TextSubtle);
    unit->SetFont(wxFont(8, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
        wxFONTWEIGHT_NORMAL, false, "Segoe UI"));
    unit->SetMinSize(wxSize(kUnitWidth, -1));

    row->Add(lbl, 0, wxALIGN_CENTER_VERTICAL);
    row->AddStretchSpacer(1);
    row->Add(ctrl, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, kFieldGap);
    row->Add(unit, 0, wxALIGN_CENTER_VERTICAL);
    into->Add(row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 10);
    return ctrl;
}

// Parse a numeric field, falling back to a default on empty/garbage input.
static double ParseField(wxTextCtrl* ctrl, double defaultVal)
{
    if (!ctrl) return defaultVal;
    double v = 0.0;
    if (ctrl->GetValue().ToDouble(&v)) return v;
    return defaultVal;
}

// Accumulate a mesh's position extents into [mn, mx]. Reads whichever position
// buffer the mesh carries (interleaved pos+normal, else bare positions). No-op
// on an empty mesh. `any` tracks whether anything has been folded in yet so the
// first point seeds the box rather than unioning against a zero default.
static void AccumulateMeshBounds(const FileImporter::MeshData& m,
    glm::vec3& mn, glm::vec3& mx, bool& any)
{
    const std::vector<float>* buf = nullptr;
    int stride = 3;
    if (!m.posNorm.empty()) { buf = &m.posNorm; stride = 6; }
    else if (!m.vertices.empty()) { buf = &m.vertices; stride = 3; }
    if (!buf) return;

    for (size_t i = 0; i + 2 < buf->size(); i += stride)
    {
        const glm::vec3 p((*buf)[i], (*buf)[i + 1], (*buf)[i + 2]);
        if (!any) { mn = mx = p; any = true; }
        else { mn = glm::min(mn, p); mx = glm::max(mx, p); }
    }
}

// Build an axis-aligned box mesh spanning [mn, mx], as an interleaved
// position+normal buffer (6 floats/vertex) with flat per-face normals so it
// shades like the other preview parts. 24 vertices (4 per face) + 36 indices.
static FileImporter::MeshData MakeBoxMesh(const glm::vec3& mn, const glm::vec3& mx)
{
    FileImporter::MeshData mesh;
    mesh.aabbMin = mn;
    mesh.aabbMax = mx;

    // Six faces, each: outward normal + four corner positions wound CCW as seen
    // from outside. (Back-face culling is off in the preview, so winding only
    // affects nothing visible — the explicit normals drive the shading.)
    struct Face { glm::vec3 n; glm::vec3 v[4]; };
    const Face faces[6] = {
        { {  1.0f, 0.0f, 0.0f }, { { mx.x, mn.y, mn.z }, { mx.x, mx.y, mn.z }, { mx.x, mx.y, mx.z }, { mx.x, mn.y, mx.z } } },
        { { -1.0f, 0.0f, 0.0f }, { { mn.x, mn.y, mx.z }, { mn.x, mx.y, mx.z }, { mn.x, mx.y, mn.z }, { mn.x, mn.y, mn.z } } },
        { { 0.0f,  1.0f, 0.0f }, { { mn.x, mx.y, mn.z }, { mn.x, mx.y, mx.z }, { mx.x, mx.y, mx.z }, { mx.x, mx.y, mn.z } } },
        { { 0.0f, -1.0f, 0.0f }, { { mn.x, mn.y, mx.z }, { mn.x, mn.y, mn.z }, { mx.x, mn.y, mn.z }, { mx.x, mn.y, mx.z } } },
        { { 0.0f, 0.0f,  1.0f }, { { mn.x, mn.y, mx.z }, { mx.x, mn.y, mx.z }, { mx.x, mx.y, mx.z }, { mn.x, mx.y, mx.z } } },
        { { 0.0f, 0.0f, -1.0f }, { { mx.x, mn.y, mn.z }, { mn.x, mn.y, mn.z }, { mn.x, mx.y, mn.z }, { mx.x, mx.y, mn.z } } },
    };

    mesh.posNorm.reserve(6 * 4 * 6);
    mesh.indices.reserve(6 * 6);
    for (int f = 0; f < 6; ++f)
    {
        const uint32_t base = (uint32_t)f * 4;
        for (int k = 0; k < 4; ++k)
        {
            const glm::vec3& p = faces[f].v[k];
            const glm::vec3& n = faces[f].n;
            mesh.posNorm.insert(mesh.posNorm.end(),
                { p.x, p.y, p.z, n.x, n.y, n.z });
        }
        mesh.indices.insert(mesh.indices.end(),
            { base + 0, base + 1, base + 2, base + 0, base + 2, base + 3 });
    }
    return mesh;
}

// ---------------------------------------------------------------------------
// Boolean helpers — the cast bases fuse a split half of the shot into each
// base body. The booleans run on the MeshBoolean (Manifold) exchange mesh
// ([x,y,z] positions + triangle indices), so these convert between it and the
// display MeshData the preview canvas draws.
// ---------------------------------------------------------------------------

// A manifold box (8 shared corner vertices, 12 triangles) spanning [mn, mx],
// suitable as a boolean operand.
static MeshBoolean::Mesh MakeBoxBool(const glm::vec3& mn, const glm::vec3& mx)
{
    MeshBoolean::Mesh m;
    m.verts = {
        mn.x, mn.y, mn.z,  mx.x, mn.y, mn.z,  mx.x, mx.y, mn.z,  mn.x, mx.y, mn.z,
        mn.x, mn.y, mx.z,  mx.x, mn.y, mx.z,  mx.x, mx.y, mx.z,  mn.x, mx.y, mx.z,
    };
    // 12 triangles, outward-consistent winding (winding is immaterial to the
    // boolean, which re-derives orientation, but keep it sane).
    m.indices = {
        0,2,1, 0,3,2,   // -Z
        4,5,6, 4,6,7,   // +Z
        0,1,5, 0,5,4,   // -Y
        3,7,6, 3,6,2,   // +Y
        0,4,7, 0,7,3,   // -X
        1,2,6, 1,6,5,   // +X
    };
    return m;
}

// Extract a boolean operand from a display mesh: pull [x,y,z] from whichever
// position buffer the mesh carries (interleaved pos+normal, else bare pos) and
// copy the triangle indices verbatim (they index the same vertices either way).
static MeshBoolean::Mesh ToBoolMesh(const FileImporter::MeshData& src)
{
    MeshBoolean::Mesh m;
    const std::vector<float>* buf = nullptr;
    int stride = 3;
    if (!src.posNorm.empty()) { buf = &src.posNorm; stride = 6; }
    else if (!src.vertices.empty()) { buf = &src.vertices; stride = 3; }
    if (!buf) return m;

    m.verts.reserve(buf->size() / stride * 3);
    for (size_t i = 0; i + 2 < buf->size(); i += stride)
    {
        m.verts.push_back((*buf)[i]);
        m.verts.push_back((*buf)[i + 1]);
        m.verts.push_back((*buf)[i + 2]);
    }
    m.indices = src.indices;
    return m;
}

// ---------------------------------------------------------------------------
// OCC helpers — the BREP path builds the cast bodies as real solids (so a BREP
// scene exports STEP). Boxes come from BRepPrimAPI; the base fuses a
// perimeter-clipped, y-split half of the Cast Shot Body solid.
// ---------------------------------------------------------------------------

// Axis-aligned box solid spanning [mn, mx]. Null on a degenerate extent.
static TopoDS_Shape MakeBoxSolid(const glm::vec3& mn, const glm::vec3& mx)
{
    const double dx = (double)mx.x - mn.x;
    const double dy = (double)mx.y - mn.y;
    const double dz = (double)mx.z - mn.z;
    if (dx <= 1e-9 || dy <= 1e-9 || dz <= 1e-9) return TopoDS_Shape();

    BRepPrimAPI_MakeBox mk(gp_Pnt(mn.x, mn.y, mn.z), dx, dy, dz);
    mk.Build();
    return (mk.IsDone() && !mk.Shape().IsNull()) ? mk.Shape() : TopoDS_Shape();
}

// Boolean intersection a ∩ b. Null on failure / empty result.
static TopoDS_Shape CommonSolid(const TopoDS_Shape& a, const TopoDS_Shape& b)
{
    if (a.IsNull() || b.IsNull()) return TopoDS_Shape();
    BRepAlgoAPI_Common op(a, b);
    op.Build();
    return (op.IsDone() && !op.Shape().IsNull()) ? op.Shape() : TopoDS_Shape();
}

// Boolean union a ∪ b. A null operand is treated as identity (returns the
// other), so a base with no shot half to fuse just yields the plain box.
static TopoDS_Shape FuseSolid(const TopoDS_Shape& a, const TopoDS_Shape& b)
{
    if (a.IsNull()) return b;
    if (b.IsNull()) return a;
    BRepAlgoAPI_Fuse op(a, b);
    op.Build();
    return (op.IsDone() && !op.Shape().IsNull()) ? op.Shape() : a;
}

// Boolean difference a − b (used to cut the tongue-and-groove groove). A null
// operand leaves `a` unchanged; a failed cut also falls back to `a`.
static TopoDS_Shape CutSolid(const TopoDS_Shape& a, const TopoDS_Shape& b)
{
    if (a.IsNull() || b.IsNull()) return a;
    BRepAlgoAPI_Cut op(a, b);
    op.Build();
    return (op.IsDone() && !op.Shape().IsNull()) ? op.Shape() : a;
}

// Position extent of a boolean mesh. Returns false on an empty mesh.
static bool BoolMeshBounds(const MeshBoolean::Mesh& m, glm::vec3& mn, glm::vec3& mx)
{
    bool any = false;
    for (size_t i = 0; i + 2 < m.verts.size(); i += 3)
    {
        const glm::vec3 p(m.verts[i], m.verts[i + 1], m.verts[i + 2]);
        if (!any) { mn = mx = p; any = true; }
        else { mn = glm::min(mn, p); mx = glm::max(mx, p); }
    }
    return any;
}

// Clip a boolean mesh to a rectangular XZ perimeter by subtracting a slab on
// each side that overhangs it: everything with x < pxMin, x > pxMax, z < pzMin,
// or z > pzMax is removed. Slabs are only applied where the mesh actually
// overhangs, and any failed/empty difference leaves the running mesh intact.
static MeshBoolean::Mesh ClipMeshToPerimeterXZ(const MeshBoolean::Mesh& in,
    float pxMin, float pxMax, float pzMin, float pzMax)
{
    glm::vec3 mn, mx;
    if (!BoolMeshBounds(in, mn, mx)) return in;

    const float e = 1.0f;
    const float yLo = mn.y - e, yHi = mx.y + e;
    MeshBoolean::Mesh m = in;
    std::string err;

    auto subtract = [&](const glm::vec3& bmin, const glm::vec3& bmax)
    {
        MeshBoolean::Mesh out;
        if (MeshBoolean::Difference(m, MakeBoxBool(bmin, bmax), out, err) && !out.empty())
            m = out;
    };

    if (mn.x < pxMin) subtract(glm::vec3(mn.x - e, yLo, mn.z - e),
                               glm::vec3(pxMin,    yHi, mx.z + e));
    if (mx.x > pxMax) subtract(glm::vec3(pxMax,    yLo, mn.z - e),
                               glm::vec3(mx.x + e,  yHi, mx.z + e));
    if (mn.z < pzMin) subtract(glm::vec3(mn.x - e, yLo, mn.z - e),
                               glm::vec3(mx.x + e,  yHi, pzMin));
    if (mx.z > pzMax) subtract(glm::vec3(mn.x - e, yLo, pzMax),
                               glm::vec3(mx.x + e,  yHi, mx.z + e));
    return m;
}

// Extrude the selected triangles of a split shot mesh into a closed prism: the
// selected facets form the top cap (original winding), a copy translated by
// `translate` forms the bottom cap (reversed), and boundary edges (used by
// exactly one selected facet) get side walls. The connectivity is orientation-
// consistent, so the result is a manifold solid ready for a boolean. `posNorm`
// is 6 floats/vertex; `includeTri` is 1 per selected triangle. This is the
// Separation Test's "travel volume": the owned surface swept through the mould.
static MeshBoolean::Mesh BuildTravelPrism(
    const std::vector<float>& posNorm,
    const std::vector<unsigned int>& idx,
    const std::vector<unsigned char>& includeTri,
    const glm::vec3& translate,
    const glm::vec3& startOffset = glm::vec3(0.0f))
{
    MeshBoolean::Mesh out;
    const size_t stride = 6;
    const size_t vcount = posNorm.size() / stride;
    const size_t ntri   = idx.size() / 3;
    if (vcount == 0 || ntri == 0) return out;

    // The top cap starts at startOffset (a small nudge along the sweep lifts it
    // off the coincident cavity wall); the bottom cap ends at translate.
    std::vector<int> topIdx(vcount, -1), botIdx(vcount, -1);
    auto ensureVert = [&](unsigned int v)
    {
        if (topIdx[v] >= 0) return;
        const float x = posNorm[v*stride+0], y = posNorm[v*stride+1], z = posNorm[v*stride+2];
        topIdx[v] = (int)(out.verts.size() / 3);
        out.verts.push_back(x + startOffset.x);
        out.verts.push_back(y + startOffset.y);
        out.verts.push_back(z + startOffset.z);
        botIdx[v] = (int)(out.verts.size() / 3);
        out.verts.push_back(x + translate.x);
        out.verts.push_back(y + translate.y);
        out.verts.push_back(z + translate.z);
    };

    // Directed edge bookkeeping: count uses within the selected patch, and keep
    // the first directed instance (a->b as it appears in its cap triangle) so a
    // boundary wall can mate the cap's a->b with a b->a.
    struct EdgeInfo { int count = 0; unsigned int a = 0, b = 0; };
    std::unordered_map<uint64_t, EdgeInfo> edges;
    auto key = [](unsigned int a, unsigned int b) -> uint64_t
    {
        const unsigned int lo = a < b ? a : b, hi = a < b ? b : a;
        return ((uint64_t)lo << 32) | (uint64_t)hi;
    };

    for (size_t t = 0; t < ntri; ++t)
    {
        if (t >= includeTri.size() || !includeTri[t]) continue;
        const unsigned int a = idx[t*3+0], b = idx[t*3+1], c = idx[t*3+2];
        if (a >= vcount || b >= vcount || c >= vcount) continue;
        ensureVert(a); ensureVert(b); ensureVert(c);
        // Top cap (original winding) + bottom cap (reversed).
        out.indices.push_back((unsigned)topIdx[a]); out.indices.push_back((unsigned)topIdx[b]); out.indices.push_back((unsigned)topIdx[c]);
        out.indices.push_back((unsigned)botIdx[a]); out.indices.push_back((unsigned)botIdx[c]); out.indices.push_back((unsigned)botIdx[b]);
        const unsigned int tri[3] = { a, b, c };
        for (int e = 0; e < 3; ++e)
        {
            const unsigned int u = tri[e], w = tri[(e+1)%3];
            EdgeInfo& ei = edges[key(u, w)];
            if (ei.count == 0) { ei.a = u; ei.b = w; }
            ei.count++;
        }
    }
    if (out.indices.empty()) return MeshBoolean::Mesh();

    for (const auto& kv : edges)
    {
        if (kv.second.count != 1) continue;              // interior edge: no wall
        const unsigned int a = kv.second.a, b = kv.second.b;
        // Wall mating the cap's a->b: tris (b,a,a') and (b,a',b').
        out.indices.push_back((unsigned)topIdx[b]); out.indices.push_back((unsigned)topIdx[a]); out.indices.push_back((unsigned)botIdx[a]);
        out.indices.push_back((unsigned)topIdx[b]); out.indices.push_back((unsigned)botIdx[a]); out.indices.push_back((unsigned)botIdx[b]);
    }

    // Orient outward (positive signed volume) so the boolean kernel reads this
    // as a solid rather than its (infinite) complement. The connectivity is
    // already consistent; this only picks the global sign.
    double sv = 0.0;
    for (size_t t = 0; t + 2 < out.indices.size(); t += 3)
    {
        const uint32_t ia = out.indices[t], ib = out.indices[t+1], ic = out.indices[t+2];
        const glm::dvec3 a(out.verts[ia*3], out.verts[ia*3+1], out.verts[ia*3+2]);
        const glm::dvec3 b(out.verts[ib*3], out.verts[ib*3+1], out.verts[ib*3+2]);
        const glm::dvec3 c(out.verts[ic*3], out.verts[ic*3+1], out.verts[ic*3+2]);
        sv += glm::dot(a, glm::cross(b, c));
    }
    if (sv < 0.0)
        for (size_t t = 0; t + 2 < out.indices.size(); t += 3)
            std::swap(out.indices[t+1], out.indices[t+2]);

    return out;
}

// Sew a boolean mesh's triangles into a TopoDS shell/compound for display via
// SetShotDebugSolid. Not required to be a valid solid — it's only rendered.
static TopoDS_Shape MakeShapeFromBoolMesh(const MeshBoolean::Mesh& m)
{
    const size_t ntri = m.indices.size() / 3;
    if (ntri == 0 || m.verts.empty()) return TopoDS_Shape();
    BRepBuilderAPI_Sewing sew(1.0e-4);
    for (size_t t = 0; t < ntri; ++t)
    {
        const uint32_t ia = m.indices[t*3+0], ib = m.indices[t*3+1], ic = m.indices[t*3+2];
        if (ia*3+2 >= m.verts.size() || ib*3+2 >= m.verts.size() || ic*3+2 >= m.verts.size()) continue;
        const gp_Pnt pa(m.verts[ia*3], m.verts[ia*3+1], m.verts[ia*3+2]);
        const gp_Pnt pb(m.verts[ib*3], m.verts[ib*3+1], m.verts[ib*3+2]);
        const gp_Pnt pc(m.verts[ic*3], m.verts[ic*3+1], m.verts[ic*3+2]);
        BRepBuilderAPI_MakePolygon poly(pa, pb, pc, Standard_True);
        if (!poly.IsDone()) continue;
        BRepBuilderAPI_MakeFace face(poly.Wire(), /*onlyPlane=*/true);
        if (!face.IsDone()) continue;
        sew.Add(face.Face());
    }
    sew.Perform();
    return sew.SewedShape();
}

// Convert a boolean result back to a display mesh with flat (per-face) normals:
// each triangle becomes three unique vertices carrying the triangle's normal,
// so the fused body shades like the other preview parts. Also fills the AABB.
static FileImporter::MeshData FlatDisplayMesh(const MeshBoolean::Mesh& src)
{
    FileImporter::MeshData out;
    if (src.verts.empty() || src.indices.empty()) return out;

    auto pos = [&](uint32_t v) {
        return glm::vec3(src.verts[v * 3 + 0], src.verts[v * 3 + 1],
                         src.verts[v * 3 + 2]);
    };

    bool any = false;
    out.posNorm.reserve(src.indices.size() * 6);
    out.indices.reserve(src.indices.size());
    uint32_t next = 0;
    for (size_t t = 0; t + 2 < src.indices.size(); t += 3)
    {
        const glm::vec3 a = pos(src.indices[t + 0]);
        const glm::vec3 b = pos(src.indices[t + 1]);
        const glm::vec3 c = pos(src.indices[t + 2]);
        glm::vec3 n = glm::cross(b - a, c - a);
        const float len = glm::length(n);
        n = (len > 1e-12f) ? n / len : glm::vec3(0.0f, 1.0f, 0.0f);

        for (const glm::vec3& p : { a, b, c })
        {
            out.posNorm.insert(out.posNorm.end(),
                { p.x, p.y, p.z, n.x, n.y, n.z });
            out.indices.push_back(next++);
            if (!any) { out.aabbMin = out.aabbMax = p; any = true; }
            else { out.aabbMin = glm::min(out.aabbMin, p); out.aabbMax = glm::max(out.aabbMax, p); }
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// Construction — builds the static perspective UI (left simulations panel with
// the show/hide checkboxes, preview canvas, info panel) with no data loaded.
// The canvas renders its grid immediately; parts arrive later via SetData.
// ---------------------------------------------------------------------------
PreviewPanel::PreviewPanel(wxWindow* parent)
    : wxPanel(parent, wxID_ANY)
{
    SetBackgroundColour(Style::AppBg);

    auto* root = new wxBoxSizer(wxVERTICAL);

    // ---- Main row: simulations | canvas | information ---------------------
    auto* middle = new wxBoxSizer(wxHORIZONTAL);

    wxPanel* simPanel = BuildSimPanel(this);
    middle->Add(simPanel, 0, wxEXPAND | wxRIGHT, 1);

    m_canvas = new GLCanvas(this);
    m_canvas->SetPreviewMode(true);
    middle->Add(m_canvas, 1, wxEXPAND);

    wxPanel* infoPanel = BuildInfoPanel(this);
    middle->Add(infoPanel, 0, wxEXPAND | wxLEFT, 1);

    root->Add(middle, 1, wxEXPAND);

    SetSizer(root);

    UpdateInfoPanel();  // populate the "no shot yet" state
}

// ---------------------------------------------------------------------------
// (Re)seed the preview with a fresh set of post-cut halves and an optional shot.
// ---------------------------------------------------------------------------
void PreviewPanel::SetData(const std::vector<FileImporter::MeshData>& halves,
    const ShotPreviewInput& shot,
    const std::vector<FileImporter::MeshData>& inserts)
{
    // Stash the new data, replacing whatever the previous generation left.
    m_pendingHalves = halves;
    m_pendingInserts = inserts;

    // Cache the combined bounding box of the mould halves now, while we still
    // hold their meshes — LoadHalves drops the CPU copies afterwards. The cast
    // bases use the XZ span as their perimeter.
    m_hasHalvesBounds = false;
    {
        bool any = false;
        for (const FileImporter::MeshData& h : halves)
            AccumulateMeshBounds(h, m_halvesMin, m_halvesMax, any);
        m_hasHalvesBounds = any;
    }

    // Which mould kind produced this generation (gates cast generation).
    m_mouldKind = shot.mouldKind;

    m_shotMesh = FileImporter::MeshData();
    m_shotShape = TopoDS_Shape();
    m_shotFaceIds.clear();
    m_halfMeshPos.clear();
    m_halfMeshIdx.clear();
    m_faceDraftSamples.clear();
    m_faceDraftPosNorm.clear();
    m_faceDraftIdx.clear();
    m_lastFaceDraftStats = DesignChecks::FaceDraftStats{};
    m_faceDraftFallback = 0;
    m_hasShot = false;
    m_shotVolumeMm3 = 0.0;
    m_castShotMesh = FileImporter::MeshData();
    m_castShotShape = TopoDS_Shape();
    m_hasCastShot = false;
    m_hasCastShotShape = false;
    // Set unconditionally (a mesh scene has no BREP shot to attach below).
    m_sceneIsMesh = shot.sceneIsMesh;

    // Retain lightweight surface soups (xyz + indices) of the mould halves for
    // the Draft Angle Checks ownership ray — the half MeshData are dropped after
    // the GL upload, so copy what the ray test needs now, while we hold them.
    for (const FileImporter::MeshData& h : halves)
    {
        std::vector<float> pos;
        const auto& pn = h.posNorm;
        pos.reserve(pn.size() / 6 * 3);
        for (size_t i = 0; i + 5 < pn.size(); i += 6)
        { pos.push_back(pn[i]); pos.push_back(pn[i+1]); pos.push_back(pn[i+2]); }
        m_halfMeshPos.push_back(std::move(pos));
        m_halfMeshIdx.emplace_back(h.indices.begin(), h.indices.end());
    }

    if (shot.mesh)
    {
        m_shotMesh = *shot.mesh;
        if (shot.shape)   m_shotShape = *shot.shape;
        if (shot.faceIds) m_shotFaceIds = *shot.faceIds;
        m_shotVolumeMm3 = shot.volumeMm3;
        m_hasShot = true;
    }

    // Cast Shot Body — retained separately for base generation (may be present
    // even independent of what the shot toggle shows).
    if (shot.castMesh && !shot.castMesh->posNorm.empty()
        && !shot.castMesh->indices.empty())
    {
        m_castShotMesh = *shot.castMesh;
        m_hasCastShot = true;
    }
    if (shot.castShape && !shot.castShape->IsNull())
    {
        m_castShotShape = *shot.castShape;
        m_hasCastShotShape = true;
    }
    // The shot is appended after the mould halves in LoadHalves, so its
    // preview-part index is the half count.
    m_shotHalfIndex = m_hasShot ? (int)halves.size() : -1;

    // Inserts load after the halves and the shot, as a contiguous block. Record
    // where that block starts so the single insert checkbox can drive the whole
    // range in one go.
    m_insertCount = (int)inserts.size();
    m_insertFirstIndex = (m_insertCount > 0)
        ? (int)halves.size() + (m_hasShot ? 1 : 0)
        : -1;

    // Cast bodies (bases / walls) append after the halves, shot and inserts.
    // Record that boundary so a cast re-generation can truncate back to it.
    m_castAnchorCount = (int)halves.size() + (m_hasShot ? 1 : 0) + m_insertCount;

    // Reset any debug overlay state carried over from the previous generation.
    m_hasSepOverlay = false;
    if (m_debugModeChoice)   m_debugModeChoice->SetSelection(0);
    if (m_debugWireCheck)    m_debugWireCheck->SetValue(false);
    if (m_sepOverlayCheck)   m_sepOverlayCheck->SetValue(false);

    // Drop the previous generation's GPU parts now (clears its own context).
    if (m_canvas)
    {
        m_canvas->ClearShotDebugColoring();
        m_canvas->ShowShotDebugSolid(false);
        m_canvas->ClearPreviewHalves();
    }

    // Rebuild the dynamic UI for the new part set.
    ClearVisibilityChecks();
    BuildVisibilityChecks((int)halves.size(), m_hasShot, m_insertCount);
    UpdateInfoPanel();

    // The mesh upload waits until we're actually visible — a canvas on a hidden
    // book page may not have a valid drawable yet.
    m_dataDirty = true;
    FlushIfDirty();
}

// ---------------------------------------------------------------------------
// Reset to the empty (grid-only) state.
// ---------------------------------------------------------------------------
void PreviewPanel::ClearData()
{
    m_pendingHalves.clear();
    m_pendingInserts.clear();
    m_insertCheck = nullptr;
    m_insertFirstIndex = -1;
    m_insertCount = 0;
    m_shotMesh = FileImporter::MeshData();
    m_shotShape = TopoDS_Shape();
    m_shotFaceIds.clear();
    m_halfMeshPos.clear();
    m_halfMeshIdx.clear();
    m_faceDraftSamples.clear();
    m_faceDraftPosNorm.clear();
    m_faceDraftIdx.clear();
    m_lastFaceDraftStats = DesignChecks::FaceDraftStats{};
    m_faceDraftFallback = 0;
    m_hasShot = false;
    m_shotVolumeMm3 = 0.0;
    m_castShotMesh = FileImporter::MeshData();
    m_castShotShape = TopoDS_Shape();
    m_hasCastShot = false;
    m_hasCastShotShape = false;
    m_shotHalfIndex = -1;
    m_sceneIsMesh = false;

    m_mouldKind = FixtureKind::Library;
    m_castAnchorCount = 0;
    m_hasHalvesBounds = false;

    m_hasSepOverlay = false;
    if (m_debugModeChoice)   m_debugModeChoice->SetSelection(0);
    if (m_debugWireCheck)    m_debugWireCheck->SetValue(false);
    if (m_sepOverlayCheck)   m_sepOverlayCheck->SetValue(false);

    if (m_canvas)
    {
        m_canvas->ClearShotDebugColoring();
        m_canvas->ShowShotDebugSolid(false);
        m_canvas->ClearPreviewHalves();
        m_canvas->Refresh(false);
    }

    ClearVisibilityChecks();
    UpdateInfoPanel();
    m_dataDirty = false;
}

// ---------------------------------------------------------------------------
// Push staged meshes into the canvas once we're visible. No-op when nothing is
// pending or the panel is still hidden (MainFrame calls this again on show).
// ---------------------------------------------------------------------------
void PreviewPanel::FlushIfDirty()
{
    if (!m_dataDirty || !m_canvas) return;
    if (!IsShownOnScreen()) return;   // wait until the book page is visible

    // CallAfter so the page is laid out and the canvas realized (its GL context
    // valid) before AddPreviewHalf issues any GL call.
    CallAfter([this]()
        {
            if (!m_dataDirty) return;
            LoadHalves();
            m_dataDirty = false;
        });
}

void PreviewPanel::SetGridSettings(const GridSettings& s)
{
    // GLCanvas::SetGridSettings defers the actual push to its next paint, so
    // this is safe even before the preview canvas has realized its GL context.
    if (m_canvas)
        m_canvas->SetGridSettings(s);
}

// ---------------------------------------------------------------------------
// Left panel — runnable simulations. Styled to match the Prepare-side left
// panel: a fixed-width column (AppBg) with a section title and a scrollable
// stack of collapsible cards (CardBg, chevron headers — the mould-feature
// look). Each simulation is its own collapsible card.
// ---------------------------------------------------------------------------
wxPanel* PreviewPanel::BuildSimPanel(wxWindow* parent)
{
    auto* outer = new wxPanel(parent, wxID_ANY,
        wxDefaultPosition, wxSize(280, -1));
    outer->SetBackgroundColour(Style::AppBg);
    auto* outerSizer = new wxBoxSizer(wxHORIZONTAL);

    auto* column = new wxPanel(outer, wxID_ANY);
    column->SetBackgroundColour(Style::AppBg);
    auto* colSizer = new wxBoxSizer(wxVERTICAL);

    // ---- Preview Output Bodies (visibility), above Simulations ------------
    // Section label + a card holding one checkbox per loaded part, or a
    // "No bodies generated" message before anything is generated.
    auto* visTitle = new wxStaticText(column, wxID_ANY, "PREVIEW OUTPUT BODIES");
    visTitle->SetForegroundColour(Style::TextPrimary);
    visTitle->SetFont(wxFont(7, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
        wxFONTWEIGHT_BOLD, false, "Segoe UI"));
    colSizer->Add(visTitle, 0, wxLEFT | wxTOP, 12);
    colSizer->AddSpacer(6);

    m_visPanel = new wxPanel(column, wxID_ANY);
    m_visPanel->SetBackgroundColour(Style::CardBg);
    auto* visSizer = new wxBoxSizer(wxVERTICAL);

    m_visEmptyLabel = new wxStaticText(m_visPanel, wxID_ANY, "No bodies generated");
    m_visEmptyLabel->SetForegroundColour(Style::TextMuted);
    m_visEmptyLabel->SetFont(wxFont(9, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_ITALIC,
        wxFONTWEIGHT_NORMAL, false, "Segoe UI"));
    visSizer->Add(m_visEmptyLabel, 0, wxALL, 10);

    m_visPanel->SetSizer(visSizer);
    colSizer->Add(m_visPanel, 0, wxEXPAND | wxLEFT | wxRIGHT, 12);

    // Divider between the bodies card and the simulations (matches the
    // Prepare panel's section divider).
    auto* divider = new wxPanel(column, wxID_ANY, wxDefaultPosition, wxSize(-1, 1));
    divider->SetBackgroundColour(Style::Divider);
    colSizer->Add(divider, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 10);

    // Section title (matches the Prepare panel's "MOULD TOOL SETTINGS").
    auto* title = new wxStaticText(column, wxID_ANY, "SIMULATIONS");
    title->SetForegroundColour(Style::TextPrimary);
    title->SetFont(wxFont(7, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
        wxFONTWEIGHT_BOLD, false, "Segoe UI"));
    colSizer->Add(title, 0, wxLEFT | wxTOP, 12);
    colSizer->AddSpacer(8);

    auto* scrollWin = new wxScrolledWindow(column, wxID_ANY,
        wxDefaultPosition, wxDefaultSize, wxVSCROLL | wxBORDER_NONE);
    scrollWin->SetScrollRate(0, 8);
    scrollWin->SetBackgroundColour(Style::AppBg);
    auto* sizer = new wxBoxSizer(wxVERTICAL);
    sizer->AddSpacer(4);

    // Collapsible card factory: a CardBg card with a chevron header that
    // expands/collapses its body. `fill` populates the body (parent + sizer
    // supplied) so each simulation drops its own controls in.
    auto makeCard = [&](const wxString& simName,
        const std::function<void(wxWindow*, wxBoxSizer*)>& fill)
    {
        auto* card = new wxPanel(scrollWin, wxID_ANY);
        card->SetBackgroundColour(Style::CardBg);
        auto* cardSizer = new wxBoxSizer(wxVERTICAL);

        auto* header = new wxToggleButton(card, wxID_ANY, simName,
            wxDefaultPosition, wxSize(-1, 28), wxBU_LEFT | wxBORDER_NONE);
        header->SetValue(true);
        header->SetBackgroundColour(Style::CardBg);
        header->SetForegroundColour(*wxWHITE);
        header->SetFont(wxFont(9, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
            wxFONTWEIGHT_BOLD, false, "Segoe UI"));
        header->SetBitmap(LoadSvgBundle(kChevronDownSvg, wxSize(12, 12), true));
        header->SetBitmapPosition(wxRIGHT);
        cardSizer->Add(header, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 8);

        auto* body = new wxPanel(card, wxID_ANY);
        body->SetBackgroundColour(Style::CardBg);
        auto* bodySizer = new wxBoxSizer(wxVERTICAL);
        fill(body, bodySizer);
        body->SetSizer(bodySizer);
        cardSizer->Add(body, 0, wxEXPAND);

        header->Bind(wxEVT_TOGGLEBUTTON, [header, body, card](wxCommandEvent&)
        {
            const bool expanded = header->GetValue();
            header->SetBitmap(LoadSvgBundle(
                expanded ? kChevronDownSvg : kChevronRightSvg,
                wxSize(12, 12), true));
            header->SetBitmapPosition(wxRIGHT);
            body->Show(expanded);
            card->Layout();
            if (card->GetParent()) card->GetParent()->Layout();
        });

        card->SetSizer(cardSizer);
        sizer->Add(card, 0, wxEXPAND | wxTOP, 8);
    };

    // Shared "Start" button for a card — styled like the mould-feature "Place"
    // button: RoundedButton, BtnPlace fill, white semibold, 32px tall.
    auto addStart = [this](wxWindow* body, wxBoxSizer* bs, const wxString& simName)
    {
        auto* startBtn = new RoundedButton(body, wxID_ANY, "Start",
            wxDefaultPosition, wxSize(-1, 32), wxBORDER_NONE);
        startBtn->SetBackgroundColour(Style::BtnPlace);
        startBtn->SetForegroundColour(*wxWHITE);
        startBtn->SetFont(wxFont(9, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
            wxFONTWEIGHT_SEMIBOLD, false, "Segoe UI"));
        startBtn->Bind(wxEVT_BUTTON,
            [this, simName](wxCommandEvent&) { OnStartSimulation(simName); });
        bs->Add(startBtn, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 10);
        bs->AddSpacer(6);
    };

    // ---- Draft Angle Checks -------------------------------------------------
    // Per-facet draft against the mould half that forms each facet (ownership by
    // a ray along each face normal into the generated halves; the shot is split
    // at the parting plane first). Fields: fail/warn thresholds, the back-draft
    // epsilon (near-vertical isn't back-draft), and a minimum-significant-area
    // filter (dropdown: % of surface area or absolute mm²) that quiets isolated
    // mesh-artifact facets. Visualise via Debug View -> "Draft (ray)". Undercuts
    // are NOT assessed here — see the Separation Test.
    makeCard("Draft Angle Checks", [this, &addStart](wxWindow* body, wxBoxSizer* bs)
    {
        const wxString deg = wxString::FromUTF8("\xC2\xB0");
        m_failDraftCtrl    = AddFieldRow(body, bs, "Fail below:", "1.0", deg);
        m_warnDraftCtrl    = AddFieldRow(body, bs, "Warn below:", "3.0", deg);
        m_backdraftEpsCtrl = AddFieldRow(body, bs,
            wxString::FromUTF8("Back-draft \xce\xb5:"), "0.1", deg);
        m_backdraftEpsCtrl->SetToolTip(
            "Facets within this angle of vertical read as zero-draft rather "
            "than back-draft.");

        // Minimum significant area: dropdown (mode) + value + unit label. A
        // flagged band (fail, or warn) counts toward the verdict only once its
        // total facet area reaches this threshold; 0 disables it. The unit label
        // tracks the dropdown (% of surface area, or absolute mm²).
        auto* sigLbl = new wxStaticText(body, wxID_ANY, "Min. significant area:");
        sigLbl->SetForegroundColour(Style::TextMuted);
        sigLbl->SetFont(wxFont(8, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
            wxFONTWEIGHT_NORMAL, false, "Segoe UI"));
        bs->Add(sigLbl, 0, wxLEFT | wxRIGHT | wxTOP, 10);

        wxArrayString sigModes;
        sigModes.Add("% of surface area");
        sigModes.Add(wxString::FromUTF8("Absolute (mm\xC2\xB2)"));
        m_sigModeChoice = new wxChoice(body, wxID_ANY, wxDefaultPosition,
            wxDefaultSize, sigModes);
        m_sigModeChoice->SetSelection(0);
        m_sigModeChoice->SetToolTip(
            "Interpret the threshold below as a percentage of the shot's total "
            "surface area, or as an absolute area in mm².");
        bs->Add(m_sigModeChoice, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 8);

        auto* sigRow = new wxBoxSizer(wxHORIZONTAL);
        m_sigValueCtrl = new wxTextCtrl(body, wxID_ANY, "0",
            wxDefaultPosition, wxSize(kFieldWidth, 22));
        m_sigValueCtrl->SetBackgroundColour(Style::BtnSmall);
        m_sigValueCtrl->SetForegroundColour(Style::TextPrimary);
        m_sigValueCtrl->SetToolTip(
            "0 disables the filter (any flagged facet counts).");
        m_sigUnitLbl = new wxStaticText(body, wxID_ANY, "%");
        m_sigUnitLbl->SetForegroundColour(Style::TextSubtle);
        m_sigUnitLbl->SetFont(wxFont(8, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
            wxFONTWEIGHT_NORMAL, false, "Segoe UI"));
        m_sigUnitLbl->SetMinSize(wxSize(kUnitWidth, -1));
        sigRow->AddStretchSpacer(1);
        sigRow->Add(m_sigValueCtrl, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, kFieldGap);
        sigRow->Add(m_sigUnitLbl, 0, wxALIGN_CENTER_VERTICAL);
        bs->Add(sigRow, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 6);

        m_sigModeChoice->Bind(wxEVT_CHOICE, [this](wxCommandEvent&)
        {
            if (m_sigUnitLbl)
                m_sigUnitLbl->SetLabel(m_sigModeChoice->GetSelection() == 0
                    ? wxString("%") : wxString::FromUTF8("mm\xC2\xB2"));
        });

        addStart(body, bs, "Draft Angle Checks");
    });

    // ---- Separation Test ----------------------------------------------------
    // Sweeps each half's owned shot surface toward the parting plane through the
    // mould and tests for overlap with that half's steel — a hard lock (undercut)
    // is a FAIL. Reuses the Draft Angle Checks' ownership mesh; runs it first if
    // needed. The noise floor ignores tiny coincident-surface slivers. Then Start
    // and a "Show mould overlay" checkbox for the interference region. Two noise
    // controls: a start epsilon that lifts the swept prism off the coincident
    // cavity wall, and a per-region noise floor so a swarm of tiny slivers is
    // filtered while one continuous overlap still counts.
    makeCard("Separation Test", [this, &addStart](wxWindow* body, wxBoxSizer* bs)
    {
        const wxString mm  = "mm";
        const wxString mm3 = wxString::FromUTF8("mm\xC2\xB3");
        m_sepStartEpsCtrl = AddFieldRow(body, bs,
            wxString::FromUTF8("Start \xce\xb5:"), "0.001", mm);
        m_sepStartEpsCtrl->SetToolTip(
            "Nudge the swept surface this far off the cavity wall before testing, "
            "so coincidence at the start doesn't read as a collision.");
        m_sepMinOverlapCtrl = AddFieldRow(body, bs, "Noise floor:", "0.1", mm3);
        m_sepMinOverlapCtrl->SetToolTip(
            "Per-region floor: a connected overlap region below this volume is "
            "discarded as noise. A continuous region at or above it counts as a "
            "collision, so many tiny separate slivers are filtered out.");
        addStart(body, bs, "Separation Test");

        m_sepOverlayCheck = new wxCheckBox(body, wxID_ANY, "Show mould overlay");
        m_sepOverlayCheck->SetForegroundColour(Style::TextPrimary);
        m_sepOverlayCheck->SetBackgroundColour(Style::CardBg);
        m_sepOverlayCheck->SetToolTip(
            "Show the interference region (red) where the halves collide with the shot");
        m_sepOverlayCheck->Bind(wxEVT_CHECKBOX,
            [this](wxCommandEvent&) { UpdateSeparationOverlay(); });
        bs->Add(m_sepOverlayCheck, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 10);
    });

    // ---- Debug View ---------------------------------------------------------
    // Colour the shot by the ownership analysis, or draw it as a wireframe.
    // "Draft (ray)" renders the parting-split mesh coloured by per-facet signed
    // draft (wireframe reveals the parting ring). "Travel volume A / B" show each
    // mould half's swept travel volume (the Separation Test's sweep) as a solid.
    // Any of these builds the analysis on demand if a check hasn't been run.
    makeCard("Debug View", [this](wxWindow* body, wxBoxSizer* bs)
    {
        auto* lbl = new wxStaticText(body, wxID_ANY, "Colour by:");
        lbl->SetForegroundColour(Style::TextPrimary);
        lbl->SetBackgroundColour(Style::CardBg);
        bs->Add(lbl, 0, wxLEFT | wxRIGHT | wxTOP, 10);

        wxArrayString modes;
        modes.Add("None");
        modes.Add("Draft (ray)");
        modes.Add("Travel volume A (+draw)");
        modes.Add("Travel volume B (-draw)");
        m_debugModeChoice = new wxChoice(body, wxID_ANY, wxDefaultPosition,
            wxDefaultSize, modes);
        m_debugModeChoice->SetSelection(0);
        m_debugModeChoice->Bind(wxEVT_CHOICE,
            [this](wxCommandEvent&) { UpdateDraftOverlay(); });
        bs->Add(m_debugModeChoice, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 10);

        m_debugWireCheck = new wxCheckBox(body, wxID_ANY, "Wireframe");
        m_debugWireCheck->SetForegroundColour(Style::TextPrimary);
        m_debugWireCheck->SetBackgroundColour(Style::CardBg);
        m_debugWireCheck->SetToolTip(
            "With \"Colour by: None\", show the shot as a transparent "
            "wireframe. For the analysis modes it draws the debug mesh as a "
            "wireframe (e.g. reveals the parting ring, or a travel volume's "
            "interior).");
        m_debugWireCheck->Bind(wxEVT_CHECKBOX,
            [this](wxCommandEvent&) { UpdateDraftOverlay(); });
        bs->Add(m_debugWireCheck, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP | wxBOTTOM, 10);
    });

    sizer->AddSpacer(12);
    scrollWin->SetSizer(sizer);
    colSizer->Add(scrollWin, 1, wxEXPAND);

    // NOTE: the old "Generate Mould Casts" action button was removed from the
    // Preview perspective — casting now lives entirely in the dedicated Casting
    // perspective (CastingPanel), reached from the ribbon's Casting button.
    // PreviewPanel::OnGenerateMouldCasts and its cast helpers remain in this
    // file as (currently unreferenced) legacy code; they can be purged in a
    // dedicated cleanup pass.

    column->SetSizer(colSizer);
    outerSizer->Add(column, 1, wxEXPAND);

    // Right border line, matching the Prepare panel's divider.
    auto* borderLine = new wxPanel(outer, wxID_ANY,
        wxDefaultPosition, wxSize(1, -1));
    borderLine->SetBackgroundColour(Style::Divider);
    outerSizer->Add(borderLine, 0, wxEXPAND);

    outer->SetSizer(outerSizer);
    return outer;
}


// ---------------------------------------------------------------------------
// Right panel — read-only results, styled to match the Prepare side: an AppBg
// column with a left divider border. An "Information" section header over a
// shot-volume card, then a "Results" section header (same style) over one
// verdict card per simulation.
// ---------------------------------------------------------------------------
wxPanel* PreviewPanel::BuildInfoPanel(wxWindow* parent)
{
    auto* outer = new wxPanel(parent, wxID_ANY,
        wxDefaultPosition, wxSize(240, -1));
    outer->SetBackgroundColour(Style::AppBg);
    m_infoPanel = outer;
    auto* outerSizer = new wxBoxSizer(wxHORIZONTAL);

    // Left border line (mirror of the sim panel's right border).
    auto* borderLine = new wxPanel(outer, wxID_ANY,
        wxDefaultPosition, wxSize(1, -1));
    borderLine->SetBackgroundColour(Style::Divider);
    outerSizer->Add(borderLine, 0, wxEXPAND);

    auto* column = new wxPanel(outer, wxID_ANY);
    column->SetBackgroundColour(Style::AppBg);
    auto* colSizer = new wxBoxSizer(wxVERTICAL);

    // Section header (standalone, above its card[s]) — matches the left
    // column's "SIMULATIONS" / "PREVIEW OUTPUT BODIES" section titles.
    auto addSectionHeader = [&](const wxString& text)
    {
        auto* h = new wxStaticText(column, wxID_ANY, text);
        h->SetForegroundColour(Style::TextPrimary);
        h->SetFont(wxFont(7, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
            wxFONTWEIGHT_BOLD, false, "Segoe UI"));
        colSizer->Add(h, 0, wxLEFT | wxRIGHT | wxTOP, 12);
    };

    // Card factory: a CardBg card; when titleText is non-empty it gets a bold
    // white in-card title (used by the per-simulation result cards). Returns
    // the card so the caller can drop content beneath.
    auto makeCard = [&](const wxString& titleText) -> std::pair<wxPanel*, wxBoxSizer*>
    {
        auto* card = new wxPanel(column, wxID_ANY);
        card->SetBackgroundColour(Style::CardBg);
        auto* cs = new wxBoxSizer(wxVERTICAL);

        if (!titleText.IsEmpty())
        {
            auto* t = new wxStaticText(card, wxID_ANY, titleText);
            t->SetForegroundColour(*wxWHITE);
            t->SetFont(wxFont(9, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
                wxFONTWEIGHT_BOLD, false, "Segoe UI"));
            cs->Add(t, 0, wxLEFT | wxRIGHT | wxTOP, 10);
        }

        card->SetSizer(cs);
        colSizer->Add(card, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 6);
        return { card, cs };
    };

    // ---- Information: shot volume -----------------------------------------
    addSectionHeader("INFORMATION");
    {
        auto [card, cs] = makeCard(wxEmptyString);

        auto* volLabel = new wxStaticText(card, wxID_ANY, "Shot Volume");
        volLabel->SetForegroundColour(Style::TextSubtle);
        volLabel->SetFont(wxFont(8, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
            wxFONTWEIGHT_NORMAL, false, "Segoe UI"));
        cs->Add(volLabel, 0, wxLEFT | wxRIGHT | wxTOP, 10);

        m_volPrimary = new wxStaticText(card, wxID_ANY, wxEmptyString);
        m_volPrimary->SetForegroundColour(Style::TextPrimary);
        m_volPrimary->SetFont(wxFont(13, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
            wxFONTWEIGHT_BOLD, false, "Segoe UI"));
        cs->Add(m_volPrimary, 0, wxLEFT | wxRIGHT | wxTOP, 10);

        m_volSecondary = new wxStaticText(card, wxID_ANY, wxEmptyString);
        m_volSecondary->SetForegroundColour(Style::TextMuted);
        m_volSecondary->SetFont(wxFont(9, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
            wxFONTWEIGHT_NORMAL, false, "Segoe UI"));
        cs->Add(m_volSecondary, 0, wxLEFT | wxRIGHT | wxBOTTOM, 10);
    }

    // ---- Results: one verdict card per simulation -------------------------
    addSectionHeader("RESULTS");
    auto makeVerdictCard = [&](const wxString& titleText) -> wxStaticText*
    {
        auto [card, cs] = makeCard(titleText);
        auto* value = new wxStaticText(card, wxID_ANY, "Not run");
        value->SetForegroundColour(Style::TextMuted);
        value->SetFont(wxFont(11, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
            wxFONTWEIGHT_BOLD, false, "Segoe UI"));
        cs->Add(value, 0, wxLEFT | wxRIGHT | wxTOP | wxBOTTOM, 10);
        return value;
    };

    m_draftStatus = makeVerdictCard("Draft Angle Checks");
    m_demouldStatus = makeVerdictCard("Separation Test");

    column->SetSizer(colSizer);
    outerSizer->Add(column, 1, wxEXPAND);

    outer->SetSizer(outerSizer);
    return outer;
}

// ---------------------------------------------------------------------------
// Fill the information panel's value labels from the current data.
// ---------------------------------------------------------------------------
void PreviewPanel::UpdateInfoPanel()
{
    // 1 cm³ = 1000 mm³; 1 in³ = 16387.064 mm³.
    const double cm3 = m_shotVolumeMm3 / 1000.0;
    const double in3 = m_shotVolumeMm3 / 16387.064;

    if (m_volPrimary)
    {
        if (m_hasShot)
        {
            // \xC2\xB3 is UTF-8 for the superscript-three; build via FromUTF8
            // so it renders regardless of source-file encoding.
            const wxString cm3u = wxString::FromUTF8("cm\xC2\xB3");
            m_volPrimary->SetLabel(wxString::Format("%.3f ", cm3) + cm3u);
            m_volPrimary->SetForegroundColour(Style::TextPrimary);
        }
        else
        {
            m_volPrimary->SetLabel("No shot model");
            m_volPrimary->SetForegroundColour(Style::TextMuted);
        }
    }

    if (m_volSecondary)
    {
        if (m_hasShot)
        {
            const wxString in3u = wxString::FromUTF8("in\xC2\xB3");
            m_volSecondary->SetLabel(wxString::Format("%.4f ", in3) + in3u);
        }
        else
        {
            m_volSecondary->SetLabel(wxEmptyString);
        }
    }

    if (m_draftStatus)
    {
        m_draftStatus->SetLabel("Not run");
        m_draftStatus->SetForegroundColour(Style::TextMuted);
    }
    if (m_demouldStatus)
    {
        m_demouldStatus->SetLabel("Not run");
        m_demouldStatus->SetForegroundColour(Style::TextMuted);
    }

    if (m_infoPanel) m_infoPanel->Layout();
}

// ---------------------------------------------------------------------------
// A simulation Start button was pressed.
// ---------------------------------------------------------------------------
void PreviewPanel::OnStartSimulation(const wxString& simName)
{
    // Both checks are mesh/facet-based (they analyse the shot mesh and the
    // generated half meshes), so they run on BREP and mesh scenes alike.
    if (simName == "Draft Angle Checks")
    {
        RunFaceDraftCheck();
        return;
    }
    if (simName == "Separation Test")
    {
        RunSeparationCheck();
        return;
    }

    wxMessageBox(simName + " is not implemented yet.",
        "Simulation", wxOK | wxICON_INFORMATION, this);
}

// ---------------------------------------------------------------------------
// "Generate Mould Casts" — prompt for the wall + base characteristics used to
// hold the sand / silicone around the finished mould, then build them. Locked
// to procedural (Parametric / Dynamic) moulds. The base generates two boxes
// straddling the y = 0 parting plane, grown past the perimeter by the Extra
// Flange Distance, each fused with the matching y-split half of the Cast Shot
// Body (clipped to the mould perimeter first). Walls are not built yet.
// ---------------------------------------------------------------------------
void PreviewPanel::OnGenerateMouldCasts()
{
    MouldCastDialog dlg(this);
    if (dlg.ShowModal() != wxID_OK)
        return;

    // Cast generation is locked to procedural moulds — Parametric (fixed box)
    // and Dynamic (adaptive box) — whose perimeter is a clean rectangle. On any
    // other mould kind, pressing "Generate Casts" warns and cancels.
    if (m_mouldKind != FixtureKind::Parametric &&
        m_mouldKind != FixtureKind::Dynamic)
    {
        wxMessageBox(
            "Mould casts can only be generated for parametric or adaptive "
            "moulds.\n\nRegenerate the mould from a Parametric or Adaptive "
            "(Dynamic) fixture, then try again.",
            "Generate Mould Casts", wxOK | wxICON_WARNING, this);
        return;
    }

    const MouldCastValues v = dlg.GetValues();

    if (!v.base.enabled && !v.walls.enabled)
    {
        wxMessageBox("No cast parts were enabled.",
            "Generate Mould Casts", wxOK | wxICON_INFORMATION, this);
        return;
    }

    // Both base and walls need the mould perimeter (halves bounds).
    if (!m_hasHalvesBounds || !m_canvas)
    {
        wxMessageBox("There are no mould halves to build casts from.",
            "Generate Mould Casts", wxOK | wxICON_WARNING, this);
        return;
    }

    // Idempotent re-generation: strip off any previously generated cast bodies
    // (and their toggles) once, up front, then rebuild whatever is enabled.
    m_canvas->TruncatePreviewHalves(m_castAnchorCount);
    ClearCastChecks();

    const glm::vec3 mn = m_halvesMin;
    const glm::vec3 mx = m_halvesMax;

    // A BREP scene builds the cast bodies as OCC solids (STEP-exportable) and
    // shows their tessellation; a mesh scene builds them as meshes (STL only).
    const bool brepScene = !m_sceneIsMesh;

    // Axis-aligned box (min, max corners) used throughout the cast builders.
    using Box = std::pair<glm::vec3, glm::vec3>;

    // ---- Shared wall geometry --------------------------------------------
    // Computed up front because the base needs it too: a base↔wall tongue is
    // added to the base at each wall's y = 0 face midpoint (the matching groove
    // is cut into the wall). Reuses the wall section's tongue fields.
    const bool  wallsOn = v.walls.enabled && v.walls.ThicknessMm() > 0.0;
    const float wThk = wallsOn ? (float)v.walls.ThicknessMm() : 0.0f;
    const bool  wClover = (v.walls.type == "Clover");
    const float wExtra = (wallsOn && wClover) ? (float)v.walls.ExtraDistanceMm() : 0.0f;
    const float wE = (wallsOn && wClover) ? (wThk + wExtra) : 0.0f;   // clover overhang
    const float jtw = (float)v.walls.TongueWidthMm();
    const float jtt = (float)v.walls.TongueThicknessMm();
    const float jtol = (float)v.walls.GrooveToleranceMm();
    const bool  baseWallJoint = wallsOn && jtw > 1e-6f && jtt > 1e-6f;

    // Midpoint of each wall's y = 0 face (-Z, +Z, +X, -X order), plus the XZ
    // half-extents of the base↔wall tongue (tHalf) and groove (gHalf). The joint
    // runs the FULL wall length (parallel to the perimeter, including the clover
    // overhang), and is the tongue width ACROSS the wall (the groove adds the
    // tolerance across that same width).
    glm::vec2 wallMid[4] = {}, tHalf[4] = {}, gHalf[4] = {};
    if (wallsOn)
    {
        const float X0 = mn.x, X1 = mx.x, Z0 = mn.z, Z1 = mx.z;
        wallMid[0] = glm::vec2((X0 - wE + X1) * 0.5f, Z0 - wThk * 0.5f);   // -Z
        wallMid[1] = glm::vec2((X0 + X1 + wE) * 0.5f, Z1 + wThk * 0.5f);   // +Z
        wallMid[2] = glm::vec2(X1 + wThk * 0.5f, (Z0 - wE + Z1) * 0.5f);   // +X
        wallMid[3] = glm::vec2(X0 - wThk * 0.5f, (Z0 + Z1 + wE) * 0.5f);   // -X

        const float LhalfX = (X1 - X0 + wE) * 0.5f;  // -Z / +Z wall length / 2
        const float LhalfZ = (Z1 - Z0 + wE) * 0.5f;  // +X / -X wall length / 2
        const float hW  = jtw * 0.5f;                // half tongue width (across)
        const float hWG = (jtw + jtol) * 0.5f;       // groove half-width (+ tol)
        // -Z / +Z run along X (width across Z); +X / -X run along Z (width across X).
        tHalf[0] = tHalf[1] = glm::vec2(LhalfX, hW);
        tHalf[2] = tHalf[3] = glm::vec2(hW, LhalfZ);
        gHalf[0] = gHalf[1] = glm::vec2(LhalfX, hWG);
        gHalf[2] = gHalf[3] = glm::vec2(hWG, LhalfZ);
    }

    wxString notes;

    // Cast bodies are grouped by which half-cast they belong to: the Top Cast
    // (top base + the y>0 wall halves) and the Bottom Cast (bottom base + the
    // y<0 wall halves). Collect the child bodies here, then build one collapsible
    // group per side at the end.
    std::vector<CastChild> topChildren;
    std::vector<CastChild> bottomChildren;

    // ---- Base — two boxes straddling the y = 0 parting plane --------------
    if (v.base.enabled && v.base.ThicknessMm() <= 0.0)
        notes << "Base skipped: thickness must be greater than zero\n";
    else if (v.base.enabled)
    {
        const double thk = v.base.ThicknessMm();

        // Perimeter = the mould halves' XZ footprint, grown on every side by the
        // Extra Flange Distance. The two bases straddle the y = 0 parting plane:
        // the top base fills [-thk, 0] (extrudes in -Y), the bottom base fills
        // [0, +thk] (extrudes in +Y).
        const float t = (float)thk;
        const float fl = (float)v.base.ExtraDistanceMm();   // per-side flange (mm)

        const glm::vec3 topBoxMin(mn.x - fl, -t, mn.z - fl);
        const glm::vec3 topBoxMax(mx.x + fl, 0.0f, mx.z + fl);
        const glm::vec3 botBoxMin(mn.x - fl, 0.0f, mn.z - fl);
        const glm::vec3 botBoxMax(mx.x + fl, t, mx.z + fl);

        bool shotUsed = false;
        bool haveShapes = false;
        FileImporter::MeshData topMeshD, botMeshD;
        TopoDS_Shape topShape, botShape;

        // ---- BREP path (BREP scenes): build the bases as OCC solids --------
        // The perimeter-clip + y=0 split is one Common each (intersect the cast
        // shot solid with a box spanning the mould XZ footprint and the wanted
        // Y half), then fuse the resulting half into the base box.
        if (brepScene)
        {
            TopoDS_Shape shotShapeSrc = m_hasCastShotShape ? m_castShotShape
                : (!m_shotShape.IsNull() ? m_shotShape : TopoDS_Shape());

            TopoDS_Shape upperHalf, lowerHalf;
            if (!shotShapeSrc.IsNull())
            {
                const FileImporter::MeshData& srcMesh =
                    m_hasCastShot ? m_castShotMesh : m_shotMesh;
                glm::vec3 sMin, sMax; bool any = false;
                AccumulateMeshBounds(srcMesh, sMin, sMax, any);
                if (any)
                {
                    const float pad = 1.0f;
                    const TopoDS_Shape upperBox = MakeBoxSolid(
                        glm::vec3(mn.x, 0.0f, mn.z),
                        glm::vec3(mx.x, sMax.y + pad, mx.z));
                    const TopoDS_Shape lowerBox = MakeBoxSolid(
                        glm::vec3(mn.x, sMin.y - pad, mn.z),
                        glm::vec3(mx.x, 0.0f, mx.z));
                    upperHalf = CommonSolid(shotShapeSrc, upperBox);
                    lowerHalf = CommonSolid(shotShapeSrc, lowerBox);
                }
            }

            topShape = FuseSolid(MakeBoxSolid(topBoxMin, topBoxMax), upperHalf);
            botShape = FuseSolid(MakeBoxSolid(botBoxMin, botBoxMax), lowerHalf);

            if (!topShape.IsNull() && !botShape.IsNull())
            {
                GLCanvas::TessellateShapeToMesh(topShape, topMeshD, nullptr);
                GLCanvas::TessellateShapeToMesh(botShape, botMeshD, nullptr);
                haveShapes = !topMeshD.posNorm.empty() && !botMeshD.posNorm.empty();
                shotUsed = !upperHalf.IsNull() || !lowerHalf.IsNull();
            }
        }

        // ---- Mesh path (mesh scenes, or BREP tessellation failure) ---------
        if (!haveShapes)
        {
            topShape = TopoDS_Shape();
            botShape = TopoDS_Shape();
            shotUsed = false;

            const FileImporter::MeshData& shotSrc =
                m_hasCastShot ? m_castShotMesh : m_shotMesh;
            const bool haveShotSrc = m_hasCastShot || m_hasShot;

            MeshBoolean::Mesh shotBool;
            if (haveShotSrc) shotBool = ToBoolMesh(shotSrc);
            if (!shotBool.empty())
                shotBool = ClipMeshToPerimeterXZ(shotBool, mn.x, mx.x, mn.z, mx.z);

            glm::vec3 sMin, sMax;
            if (!shotBool.empty() && BoolMeshBounds(shotBool, sMin, sMax))
            {
                const float pad = 1.0f;
                const MeshBoolean::Mesh belowSlab = MakeBoxBool(
                    glm::vec3(sMin.x - pad, sMin.y - pad, sMin.z - pad),
                    glm::vec3(sMax.x + pad, 0.0f,         sMax.z + pad));
                const MeshBoolean::Mesh aboveSlab = MakeBoxBool(
                    glm::vec3(sMin.x - pad, 0.0f,         sMin.z - pad),
                    glm::vec3(sMax.x + pad, sMax.y + pad, sMax.z + pad));

                std::string err;
                MeshBoolean::Mesh upperHalf, lowerHalf, topFused, botFused;
                const bool haveUpper =
                    MeshBoolean::Difference(shotBool, belowSlab, upperHalf, err)
                    && !upperHalf.empty();
                const bool haveLower =
                    MeshBoolean::Difference(shotBool, aboveSlab, lowerHalf, err)
                    && !lowerHalf.empty();

                const bool topOk = haveUpper &&
                    MeshBoolean::Union({ MakeBoxBool(topBoxMin, topBoxMax), upperHalf },
                        topFused, err) && !topFused.empty();
                const bool botOk = haveLower &&
                    MeshBoolean::Union({ MakeBoxBool(botBoxMin, botBoxMax), lowerHalf },
                        botFused, err) && !botFused.empty();

                if (topOk && botOk)
                {
                    topMeshD = FlatDisplayMesh(topFused);
                    botMeshD = FlatDisplayMesh(botFused);
                    shotUsed = true;
                }
            }

            if (!shotUsed)
            {
                topMeshD = MakeBoxMesh(topBoxMin, topBoxMax);
                botMeshD = MakeBoxMesh(botBoxMin, botBoxMax);
            }
        }

        // Base↔wall tongues: add a tongue at each wall's y = 0 face midpoint,
        // standing proud toward the wall (the wall gets the matching groove).
        // The tongue overlaps into the base so the fuse is robust.
        if (baseWallJoint)
        {
            const float bov = 1.0f;           // overlap into the base for a clean fuse
            std::vector<Box> topT, botT;
            for (int i = 0; i < 4; ++i)
            {
                const glm::vec2 c = wallMid[i], h = tHalf[i];
                // Top base sits below y=0, tongue stands UP into the top wall.
                topT.push_back({ glm::vec3(c.x - h.x, -bov, c.y - h.y),
                                 glm::vec3(c.x + h.x, jtt,  c.y + h.y) });
                // Bottom base sits above y=0, tongue stands DOWN into the bottom wall.
                botT.push_back({ glm::vec3(c.x - h.x, -jtt, c.y - h.y),
                                 glm::vec3(c.x + h.x, bov,  c.y + h.y) });
            }

            if (haveShapes)
            {
                for (const Box& b : topT)
                    topShape = FuseSolid(topShape, MakeBoxSolid(b.first, b.second));
                for (const Box& b : botT)
                    botShape = FuseSolid(botShape, MakeBoxSolid(b.first, b.second));
                GLCanvas::TessellateShapeToMesh(topShape, topMeshD, nullptr);
                GLCanvas::TessellateShapeToMesh(botShape, botMeshD, nullptr);
            }
            else
            {
                auto fuseMesh = [&](FileImporter::MeshData& md,
                    const std::vector<Box>& tongues)
                {
                    std::vector<MeshBoolean::Mesh> parts;
                    parts.push_back(ToBoolMesh(md));
                    for (const Box& b : tongues)
                        parts.push_back(MakeBoxBool(b.first, b.second));
                    MeshBoolean::Mesh u; std::string err;
                    if (MeshBoolean::Union(parts, u, err) && !u.empty())
                    {
                        FileImporter::MeshData nm = FlatDisplayMesh(u);
                        if (!nm.posNorm.empty()) md = std::move(nm);
                    }
                };
                fuseMesh(topMeshD, topT);
                fuseMesh(botMeshD, botT);
            }
        }

        const glm::vec3 baseColor(0.40f, 0.55f, 0.68f);  // steel blue
        // Both bases are exported (they differ — each has its own shot half).
        m_castExports.push_back({ "top_base", topMeshD, topShape, haveShapes });
        m_castExports.push_back({ "bottom_base", botMeshD, botShape, haveShapes });
        topChildren.push_back({ std::move(topMeshD), "Top Base", baseColor,
            "Top base (extrudes -Y; shot y>0 half fused in)" });
        bottomChildren.push_back({ std::move(botMeshD), "Bottom Base", baseColor,
            "Bottom base (extrudes +Y; shot y<0 half fused in)" });

        notes << wxString::Format("Base: %s, %g mm thick",
            wxString::FromUTF8(v.base.type.c_str()), thk);
        if (fl > 0.0f) notes << wxString::Format(", +%g mm flange/side", fl);
        notes << (shotUsed
            ? (m_hasCastShot ? " (cast shot halves fused)" : " (shot halves fused)")
            : " (shot not fused)");
        notes << "\n";
    }

    // ---- Walls — four boxes around the perimeter at mould height ----------
    if (v.walls.enabled && v.walls.ThicknessMm() <= 0.0)
        notes << "Walls skipped: thickness must be greater than zero\n";
    else if (v.walls.enabled)
    {
        const float w = (float)v.walls.ThicknessMm();     // wall thickness
        const bool  clover = (v.walls.type == "Clover");
        const float ext = clover ? (float)v.walls.ExtraDistanceMm() : 0.0f;

        // Clover: each wall overhangs the perimeter at its right end (viewed from
        // outside the mould looking in) by the wall thickness PLUS the Extra Wall
        // Distance, producing the interlocking pinwheel. `e` is that overhang.
        const float e = clover ? (w + ext) : 0.0f;

        // Perimeter rectangle + mould height. Walls sit OUTSIDE each edge (out by
        // the thickness) and rise the full mould height, matching it.
        const float X0 = mn.x, X1 = mx.x, Z0 = mn.z, Z1 = mx.z;
        const float Y0 = mn.y, Y1 = mx.y;

        // Tongue-and-groove joint between adjacent walls (aligns them). At each
        // pinwheel corner one wall's clover overhang meets the neighbour's
        // Flange 1; the TONGUE is added to the overhang's mould-facing face and
        // the matching GROOVE is cut into the neighbour's Flange 1 leftmost face.
        // Both are centred on the corner's mating region and run the full mould
        // height (split per half). Built only when flanges exist and the tongue
        // has positive size.
        const bool  flanged = (ext > 1e-6f);   // clamp flanges present?
        const float tw   = (float)v.walls.TongueWidthMm();      // along the face
        const float tt   = (float)v.walls.TongueThicknessMm();  // stands proud by
        const float tol  = (float)v.walls.GrooveToleranceMm();  // groove clearance
        const bool  joint = flanged && tw > 1e-6f && tt > 1e-6f;
        const float gw   = tw + tol;    // groove width  (= tongue width + tol)
        const float gd   = tt + tol;    // groove depth  (= tongue thick + tol)
        const float jeps = 0.1f;        // clean-cut / fuse overlap for the joint

        // Centre of each corner's mating region (its Flange-1 footprint centre),
        // shared by that corner's groove (one wall) and tongue (the neighbour).
        const float Zc1 = Z0 - w - ext * 0.5f;  // corner (X1,Z0)
        const float Xc2 = X1 + w + ext * 0.5f;  // corner (X1,Z1)
        const float Zc3 = Z1 + w + ext * 0.5f;  // corner (X0,Z1)
        const float Xc4 = X0 - w - ext * 0.5f;  // corner (X0,Z0)

        const glm::vec3 wallColor(0.62f, 0.55f, 0.42f);   // warm tan

        // Each wall as an XZ rectangle (the overhang assignment forms a
        // consistent CCW-from-top pinwheel: -Z overhangs -X, +Z overhangs +X,
        // +X overhangs -Z, -X overhangs +Z), plus the XZ footprints of its two
        // clamping flanges (only built when ext > 0):
        //   f1 — a vertical tab on the wall's LEFT side (looking in from outside):
        //        w wide along the wall length, extruded out by ext, full height.
        //   f2 — a horizontal bar along each half's y = 0 edge (the edge nearest
        //        its base): the full wall width (with overhang), h = wall
        //        thickness w, extruded out by ext. Its Y range is set per half.
        // Both flanges start at the wall's outer face and grow away from the mould.
        //
        // The flange's INNER face is pushed back into the wall by `ov` so the two
        // solids interpenetrate rather than merely share a coplanar face — an
        // exact coplanar contact makes the OCC fuse drop the later operand (this
        // is why Flange 2, fused third, previously vanished). The overlap sits
        // entirely inside the wall, so it doesn't change the visible result.
        const float ov = w * 0.5f;   // < w, so it never reaches the cavity side
        struct WallRect
        {
            const char* name; const char* suffix;
            float xa, xb, za, zb;             // wall box XZ
            float f1xa, f1xb, f1za, f1zb;     // flange 1 (left tab) XZ
            float f2xa, f2xb, f2za, f2zb;     // flange 2 (bottom bar) XZ
            float tgxa, tgxb, tgza, tgzb;     // tongue box XZ (added to overhang)
            float grxa, grxb, grza, grzb;     // groove box XZ (cut from flange 1)
        };
        const WallRect rects[4] = {
            { "Wall -Z", "wall_zmin", X0 - e, X1,     Z0 - w, Z0,
              X1 - w,     X1,         Z0 - w - ext, Z0 - w + ov,
              X0 - e,     X1,         Z0 - w - ext, Z0 - w + ov,
              // tongue on the -X overhang (plane Z0, +Z), centred at Xc4
              Xc4 - tw * 0.5f, Xc4 + tw * 0.5f, Z0 - jeps, Z0 + tt,
              // groove in the +X flange (plane X1, -X), centred at Zc1
              X1 - gd,     X1 + jeps,  Zc1 - gw * 0.5f, Zc1 + gw * 0.5f },
            { "Wall +Z", "wall_zmax", X0,     X1 + e, Z1,     Z1 + w,
              X0,         X0 + w,     Z1 + w - ov,  Z1 + w + ext,
              X0,         X1 + e,     Z1 + w - ov,  Z1 + w + ext,
              // tongue on the +X overhang (plane Z1, -Z), centred at Xc2
              Xc2 - tw * 0.5f, Xc2 + tw * 0.5f, Z1 - tt, Z1 + jeps,
              // groove in the -X flange (plane X0, +X), centred at Zc3
              X0 - jeps,   X0 + gd,    Zc3 - gw * 0.5f, Zc3 + gw * 0.5f },
            { "Wall +X", "wall_xmax", X1,     X1 + w, Z0 - e, Z1,
              X1 + w - ov, X1 + w + ext, Z1 - w,    Z1,
              X1 + w - ov, X1 + w + ext, Z0 - e,    Z1,
              // tongue on the -Z overhang (plane X1, -X), centred at Zc1
              X1 - tt,     X1 + jeps,  Zc1 - tw * 0.5f, Zc1 + tw * 0.5f,
              // groove in the +Z flange (plane Z1, -Z), centred at Xc2
              Xc2 - gw * 0.5f, Xc2 + gw * 0.5f, Z1 - gd, Z1 + jeps },
            { "Wall -X", "wall_xmin", X0 - w, X0,     Z0,     Z1 + e,
              X0 - w - ext, X0 - w + ov, Z0,          Z0 + w,
              X0 - w - ext, X0 - w + ov, Z0,          Z1 + e,
              // tongue on the +Z overhang (plane X0, +X), centred at Zc3
              X0 - jeps,   X0 + tt,    Zc3 - tw * 0.5f, Zc3 + tw * 0.5f,
              // groove in the -Z flange (plane Z0, +Z), centred at Xc4
              Xc4 - gw * 0.5f, Xc4 + gw * 0.5f, Z0 - jeps, Z0 + gd },
        };

        // Build one cast body: fuse the `add` boxes together, then subtract the
        // `sub` boxes (the tongue-and-groove groove). OCC solids + tessellation
        // in a BREP scene (STEP-exportable), or a Manifold union/difference of
        // box meshes otherwise. Returns true when a shape is available.
        auto makeBody = [&](const std::vector<Box>& add, const std::vector<Box>& sub,
            FileImporter::MeshData& outMesh, TopoDS_Shape& outShape) -> bool
        {
            outMesh = FileImporter::MeshData();
            outShape = TopoDS_Shape();
            if (add.empty()) return false;

            if (brepScene)
            {
                TopoDS_Shape acc;
                for (const Box& b : add)
                {
                    const TopoDS_Shape s = MakeBoxSolid(b.first, b.second);
                    if (!s.IsNull()) acc = FuseSolid(acc, s);
                }
                for (const Box& b : sub)
                {
                    const TopoDS_Shape s = MakeBoxSolid(b.first, b.second);
                    if (!s.IsNull()) acc = CutSolid(acc, s);
                }
                if (!acc.IsNull())
                {
                    GLCanvas::TessellateShapeToMesh(acc, outMesh, nullptr);
                    if (!outMesh.posNorm.empty() && !outMesh.indices.empty())
                    {
                        outShape = acc;
                        return true;
                    }
                }
                outShape = TopoDS_Shape();
            }

            // Mesh path: union the add boxes, then difference the sub boxes.
            std::vector<MeshBoolean::Mesh> parts;
            parts.reserve(add.size());
            for (const Box& b : add)
                parts.push_back(MakeBoxBool(b.first, b.second));

            MeshBoolean::Mesh u;
            std::string err;
            if (parts.size() == 1) u = parts[0];
            else if (!MeshBoolean::Union(parts, u, err) || u.empty()) u = parts[0];

            for (const Box& b : sub)
            {
                MeshBoolean::Mesh res;
                if (MeshBoolean::Difference(u, MakeBoxBool(b.first, b.second), res, err)
                    && !res.empty())
                    u = res;
            }

            outMesh = FlatDisplayMesh(u);
            if (outMesh.posNorm.empty())
                outMesh = MakeBoxMesh(add[0].first, add[0].second);
            return false;
        };

        // Split each wall at y = 0: the y>0 half joins the Top Cast, the y<0
        // half the Bottom Cast (matching the base convention). Each half carries
        // BOTH flanges — flange 1 (left tab, spanning the half's height) and
        // flange 2 (bottom bar) along the half's y = 0 edge, i.e. the edge
        // nearest its base. Placing flange 2 on each half's parting-plane edge
        // keeps the two halves mirror-symmetric, so only one is exported per wall.
        const float yMid = 0.0f;
        for (int i = 0; i < 4; ++i)
        {
            const WallRect& r = rects[i];
            const bool hasUpper = (Y1 > yMid + 1e-4f);
            const bool hasLower = (Y0 < yMid - 1e-4f);

            if (hasUpper)
            {
                const float yLo = std::max(Y0, yMid), yHi = Y1;   // [0, Y1]
                std::vector<Box> add, sub;
                add.push_back({ glm::vec3(r.xa, yLo, r.za),
                                glm::vec3(r.xb, yHi, r.zb) });
                if (flanged)
                {
                    // flange 1 (spans this half's height)
                    add.push_back({ glm::vec3(r.f1xa, yLo, r.f1za),
                                    glm::vec3(r.f1xb, yHi, r.f1zb) });
                    // flange 2 along the y = 0 edge (nearest the top base)
                    add.push_back({ glm::vec3(r.f2xa, yMid, r.f2za),
                                    glm::vec3(r.f2xb, std::min(yMid + w, Y1), r.f2zb) });
                }
                if (joint)   // wall↔wall tongue (add) + groove (cut), full height
                {
                    add.push_back({ glm::vec3(r.tgxa, yLo, r.tgza),
                                    glm::vec3(r.tgxb, yHi, r.tgzb) });
                    sub.push_back({ glm::vec3(r.grxa, yLo, r.grza),
                                    glm::vec3(r.grxb, yHi, r.grzb) });
                }
                if (baseWallJoint)   // groove for the top base's tongue (up into wall)
                {
                    const glm::vec2 c = wallMid[i], hg = gHalf[i];
                    sub.push_back({ glm::vec3(c.x - hg.x, -jeps, c.y - hg.y),
                                    glm::vec3(c.x + hg.x, gd,    c.y + hg.y) });
                }

                FileImporter::MeshData m; TopoDS_Shape s;
                const bool hs = makeBody(add, sub, m, s);
                m_castExports.push_back({ r.suffix, m, s, hs });   // halves symmetric
                topChildren.push_back({ std::move(m),
                    wxString(r.name) + " (top)", wallColor,
                    wxString(r.name) + ", y>0 half" });
            }
            if (hasLower)
            {
                const float yLo = Y0, yHi = std::min(Y1, yMid);   // [Y0, 0]
                std::vector<Box> add, sub;
                add.push_back({ glm::vec3(r.xa, yLo, r.za),
                                glm::vec3(r.xb, yHi, r.zb) });
                if (flanged)
                {
                    // flange 1 (spans this half's height)
                    add.push_back({ glm::vec3(r.f1xa, yLo, r.f1za),
                                    glm::vec3(r.f1xb, yHi, r.f1zb) });
                    // flange 2 along the y = 0 edge (nearest the bottom base)
                    add.push_back({ glm::vec3(r.f2xa, std::max(yMid - w, Y0), r.f2za),
                                    glm::vec3(r.f2xb, yMid, r.f2zb) });
                }
                if (joint)
                {
                    add.push_back({ glm::vec3(r.tgxa, yLo, r.tgza),
                                    glm::vec3(r.tgxb, yHi, r.tgzb) });
                    sub.push_back({ glm::vec3(r.grxa, yLo, r.grza),
                                    glm::vec3(r.grxb, yHi, r.grzb) });
                }
                if (baseWallJoint)   // groove for the bottom base's tongue (down into wall)
                {
                    const glm::vec2 c = wallMid[i], hg = gHalf[i];
                    sub.push_back({ glm::vec3(c.x - hg.x, -gd,   c.y - hg.y),
                                    glm::vec3(c.x + hg.x, jeps,  c.y + hg.y) });
                }

                FileImporter::MeshData m; TopoDS_Shape s;
                const bool hs = makeBody(add, sub, m, s);
                if (!hasUpper)   // symmetric: export the lower only when there's no upper
                    m_castExports.push_back({ r.suffix, m, s, hs });
                bottomChildren.push_back({ std::move(m),
                    wxString(r.name) + " (bottom)", wallColor,
                    wxString(r.name) + ", y<0 half" });
            }
        }

        notes << wxString::Format("Walls: %s, %g mm thick",
            wxString::FromUTF8(v.walls.type.c_str()), (double)w);
        if (clover && ext > 0.0f)
            notes << wxString::Format(", +%g mm overhang & clamp flanges", (double)ext);
        if (joint)
            notes << wxString::Format(", tongue %g\xC3\x97%g mm", (double)tw, (double)tt);
        notes << " (split at y=0)\n";
    }

    // Build one collapsible group per side from the collected children.
    const int generated = (int)(topChildren.size() + bottomChildren.size());
    AddCastGroup("Top Cast", topChildren);
    AddCastGroup("Bottom Cast", bottomChildren);

    // One relayout / repaint after all cast bodies are added.
    if (m_visPanel)
    {
        m_visPanel->Layout();
        if (m_visPanel->GetParent()) m_visPanel->GetParent()->Layout();
    }
    m_canvas->Refresh(false);

    wxString msg;
    msg << "Generated " << generated << " cast bod"
        << (generated == 1 ? "y" : "ies") << ".\n\n" << notes;
    wxMessageBox(msg, "Generate Mould Casts", wxOK | wxICON_INFORMATION, this);
}

// ---------------------------------------------------------------------------
// Ensure the parting-split, ownership-assigned shot mesh exists (shared by the
// Draft Angle Checks and the Separation Test): split the shot at the parting
// plane, then cast the ownership ray into the generated half meshes. Cached in
// m_faceDraft* until the shot changes (SetData clears it). False when there is
// no shot or no generated mould halves.
// ---------------------------------------------------------------------------
bool PreviewPanel::EnsureFaceDraftAnalysis()
{
    if (!m_faceDraftSamples.empty() && m_faceDraftIdx.size() >= 3)
        return true;                                   // already built for this shot

    if (!m_hasShot || m_shotMesh.posNorm.empty() || m_shotMesh.indices.empty())
        return false;
    bool haveHalves = false;
    for (const auto& hi : m_halfMeshIdx) if (hi.size() >= 3) { haveHalves = true; break; }
    if (!haveHalves) return false;

    // Only the draw axis + parting plane drive the split and ownership ray (the
    // thresholds are applied later, per check), so a single cached build serves
    // both checks and survives threshold edits.
    DesignChecks::FaceDraftParams params;
    std::vector<unsigned int> shotIdx(m_shotMesh.indices.begin(), m_shotMesh.indices.end());
    DesignChecks::SplitMeshByPlane(
        m_shotMesh.posNorm, shotIdx, params.drawAxis, params.partingOffset,
        m_faceDraftPosNorm, m_faceDraftIdx);

    int fallback = 0;
    m_faceDraftSamples = DesignChecks::BuildFaceDraftSamples(
        m_faceDraftPosNorm, m_faceDraftIdx, m_halfMeshPos, m_halfMeshIdx,
        /*triFaceId=*/{}, params, &fallback);
    m_faceDraftFallback = fallback;
    return !m_faceDraftSamples.empty();
}

// ---------------------------------------------------------------------------
// One mould half's travel volume: sweep the facets it owns toward the parting
// plane by that side's height. Shared by the Separation Test (overlap) and the
// "Travel volume A/B" debug views. Empty when the side owns nothing or has no
// height. Assumes EnsureFaceDraftAnalysis has run.
// ---------------------------------------------------------------------------
MeshBoolean::Mesh PreviewPanel::BuildSideTravelVolume(int side, float startEps) const
{
    MeshBoolean::Mesh empty;
    if (m_faceDraftSamples.empty() || m_faceDraftIdx.size() < 3) return empty;

    glm::vec3 draw = DesignChecks::FaceDraftParams{}.drawAxis;   // +Y
    { const float L = std::sqrt(glm::dot(draw, draw)); if (L > 1.0e-12f) draw /= L; }
    const float partingOffset = 0.0f;

    // Height = the farthest extent of the half meshes on this side from the
    // parting plane, along the draw axis (the sweep distance).
    const size_t HN = std::min(m_halfMeshPos.size(), m_halfMeshIdx.size());
    float sideHeight = 0.0f;
    for (size_t h = 0; h < HN; ++h)
    {
        const std::vector<float>& hv = m_halfMeshPos[h];
        const size_t n = hv.size() / 3;
        glm::dvec3 c(0.0);
        for (size_t i = 0; i < n; ++i)
            c += glm::dvec3(hv[i*3], hv[i*3+1], hv[i*3+2]);
        if (n > 0) c /= (double)n;
        const double cs = c.x*draw.x + c.y*draw.y + c.z*draw.z - (double)partingOffset;
        if (((cs >= 0.0) ? 0 : 1) != side) continue;
        for (size_t i = 0; i < n; ++i)
        {
            const float d = std::fabs(hv[i*3]*draw.x + hv[i*3+1]*draw.y
                                    + hv[i*3+2]*draw.z - partingOffset);
            if (d > sideHeight) sideHeight = d;
        }
    }
    if (sideHeight <= 0.0f) return empty;

    const size_t splitTris = m_faceDraftIdx.size() / 3;
    std::vector<unsigned char> mask(splitTris, 0);
    bool any = false;
    for (const DesignChecks::DraftSample& smp : m_faceDraftSamples)
    {
        const int t = smp.faceId - 1;
        if (t >= 0 && t < (int)splitTris && smp.half == side) { mask[(size_t)t] = 1; any = true; }
    }
    if (!any) return empty;

    const glm::vec3 dir    = (side == 0 ? -draw : draw);   // toward the parting plane
    const float     eps    = std::max(0.0f, startEps);
    const glm::vec3 travel = dir * sideHeight;
    const glm::vec3 start  = dir * std::min(eps, sideHeight * 0.5f);  // lift off the wall
    return BuildTravelPrism(m_faceDraftPosNorm, m_faceDraftIdx, mask, travel, start);
}

// ---------------------------------------------------------------------------
// Draft Angle Checks — the shot's per-facet draft against the mould half that
// forms each facet, with ownership assigned by casting a ray along the facet's
// outward normal into the generated half meshes. Splits the shot at the parting
// plane first; remesh-free. Reports per-facet pass/warn/fail counts (gated by
// the minimum-significant-area filter) and drives the "Draft (ray)" overlay.
// ---------------------------------------------------------------------------
void PreviewPanel::RunFaceDraftCheck()
{
    if (!EnsureFaceDraftAnalysis())
    {
        wxMessageBox(
            "This check needs a shot model and the generated mould halves to "
            "assign each face to the half that forms it.\n\nGenerate a mould "
            "first, then re-run.",
            "Draft Angle Checks", wxOK | wxICON_INFORMATION, this);
        return;
    }

    DesignChecks::FaceDraftParams params;   // draw axis + parting plane default
    params.failDraftDeg = (float)std::clamp(ParseField(m_failDraftCtrl, 1.0), 0.0, 45.0);
    params.warnDraftDeg = (float)std::clamp(ParseField(m_warnDraftCtrl, 3.0), 0.0, 45.0);
    if (params.warnDraftDeg < params.failDraftDeg)
        params.warnDraftDeg = params.failDraftDeg;
    params.backdraftEpsDeg = (float)std::clamp(ParseField(m_backdraftEpsCtrl, 0.1), 0.0, 45.0);
    // Significance filter: dropdown picks % of surface area vs absolute mm^2.
    params.significanceByPercent = !m_sigModeChoice || m_sigModeChoice->GetSelection() == 0;
    params.significanceValue = (float)std::max(0.0, ParseField(m_sigValueCtrl, 0.0));

    // The parting-split, ownership-assigned mesh was built by
    // EnsureFaceDraftAnalysis (shared with the Separation Test). Classify its
    // samples against the current thresholds + significance filter.
    m_lastFaceDraftStats = DesignChecks::ClassifyFaceDraft(m_faceDraftSamples, params);
    m_lastFaceDraftStats.fallbackCount = m_faceDraftFallback;

    // Verdict card.
    wxString verdict; wxColour col; long icon = wxICON_INFORMATION;
    switch (m_lastFaceDraftStats.overall)
    {
    case DesignChecks::Severity::Pass:
        verdict = "PASS";    col = wxColour(0x26, 0xAB, 0x36); icon = wxICON_INFORMATION; break;
    case DesignChecks::Severity::Warning:
        verdict = "WARNING"; col = wxColour(0xE0, 0x9B, 0x20); icon = wxICON_WARNING;     break;
    case DesignChecks::Severity::Fail:
        verdict = "FAIL";    col = wxColour(0xD0, 0x46, 0x46); icon = wxICON_ERROR;       break;
    }
    if (m_draftStatus)
    {
        m_draftStatus->SetLabel(verdict);
        m_draftStatus->SetForegroundColour(col);
        if (m_infoPanel) m_infoPanel->Layout();
    }

    // Show the result immediately: switch the debug view to the ray overlay.
    if (m_debugModeChoice) m_debugModeChoice->SetSelection(1);
    UpdateDraftOverlay();

    const wxString deg = wxString::FromUTF8("\xC2\xB0");
    const wxString mm2 = wxString::FromUTF8(" mm\xC2\xB2");
    const DesignChecks::FaceDraftStats& st = m_lastFaceDraftStats;
    wxString msg;
    msg << "Draft Angle Checks: " << verdict << "\n";
    msg << "Per-facet draft vs the half that forms each face "
        << "(ray-assigned ownership).\n\n";
    msg << "Facets: " << st.totalFaces << "\n";
    msg << "  Pass: " << st.passCount << "\n";
    msg << "  Warn: " << st.warnCount
        << "   (< " << wxString::Format("%.1f", params.warnDraftDeg) << deg << ",  "
        << wxString::Format("%.3f", st.warnAreaMm2) << mm2 << ")\n";
    msg << "  Fail: " << st.failCount
        << "   (< " << wxString::Format("%.1f", params.failDraftDeg) << deg << ",  "
        << wxString::Format("%.3f", st.failAreaMm2) << mm2 << ")\n";
    msg << "  Back-draft: " << st.backdraftCount
        << "   (< -" << wxString::Format("%.2g", params.backdraftEpsDeg) << deg << ")\n\n";
    msg << "Min draft: " << wxString::Format("%.2f", st.minDraftDeg) << deg << "\n";

    // Significance gate (when enabled): report the threshold and any suppression.
    if (params.significanceValue > 0.0f)
    {
        msg << "\nSignificance gate: ";
        if (params.significanceByPercent)
            msg << wxString::Format("%.3g", params.significanceValue) << "% of surface = "
                << wxString::Format("%.3f", st.significanceMm2) << mm2 << "\n";
        else
            msg << wxString::Format("%.3f", st.significanceMm2) << mm2 << "\n";
        if (st.failSuppressed)
            msg << "  Failing area below the gate \xe2\x80\x94 not counted as a fail.\n";
        if (st.warnSuppressed)
            msg << "  Warning area below the gate \xe2\x80\x94 not counted as a warning.\n";
    }

    if (st.fallbackCount > 0)
        msg << "\n" << st.fallbackCount
            << " facet(s) hit no half; assigned by parting-plane side.";

    wxMessageBox(msg, "Draft Angle Checks", wxOK | icon, this);
}

// ---------------------------------------------------------------------------
// Separation Test (mesh sweep) — for each mould half, take the shot surface it
// owns (from the Draft Angle Checks' ownership pass), sweep it toward the
// parting plane by that half's height to form a "travel volume" (the owned
// surface projected through the mould), and test that volume for overlap with
// the half's steel. Any overlap past the noise floor is a hard lock (undercut)
// => FAIL. Works on BREP and mesh scenes alike; shows the overlap in red.
// ---------------------------------------------------------------------------
void PreviewPanel::RunSeparationCheck()
{
    if (!EnsureFaceDraftAnalysis())
    {
        wxMessageBox(
            "This check needs a shot model and the generated mould halves.\n\n"
            "Generate a mould first, then re-run.",
            "Separation Test", wxOK | wxICON_INFORMATION, this);
        return;
    }

    const float minOverlap =
        (float)std::max(0.0, ParseField(m_sepMinOverlapCtrl, 0.1));
    const float startEps =
        (float)std::max(0.0, ParseField(m_sepStartEpsCtrl, 0.001));

    glm::vec3 draw = DesignChecks::FaceDraftParams{}.drawAxis;   // +Y
    { const float L = std::sqrt(glm::dot(draw, draw)); if (L > 1.0e-12f) draw /= L; }
    const float partingOffset = 0.0f;

    // Classify each retained half mesh by side of the parting plane (0 = +draw,
    // 1 = -draw) so each side's prism is tested against its own steel.
    const size_t HN = std::min(m_halfMeshPos.size(), m_halfMeshIdx.size());
    std::vector<int> halfSide(HN, 0);
    for (size_t h = 0; h < HN; ++h)
    {
        const std::vector<float>& hv = m_halfMeshPos[h];
        const size_t n = hv.size() / 3;
        glm::dvec3 c(0.0);
        for (size_t i = 0; i < n; ++i)
            c += glm::dvec3(hv[i*3], hv[i*3+1], hv[i*3+2]);
        if (n > 0) c /= (double)n;
        const double cs = c.x*draw.x + c.y*draw.y + c.z*draw.z - (double)partingOffset;
        halfSide[h] = (cs >= 0.0) ? 0 : 1;
    }

    // Which sides own any shot surface (for the "tested" count).
    bool sideSurf[2] = { false, false };
    for (const DesignChecks::DraftSample& smp : m_faceDraftSamples)
        if (smp.half == 0 || smp.half == 1) sideSurf[smp.half] = true;

    // Per side: build the travel prism (shared helper), intersect with each half
    // mesh on that side, then split the overlap into connected regions and keep
    // only those at/above the per-region floor. This filters a swarm of tiny
    // slivers while a continuous overlap still counts.
    double perSideVol[2]  = { 0.0, 0.0 };   // summed SIGNIFICANT overlap per side
    int    perSideStat[2] = { 0, 0 };       // 0 clear, 1 collision, 2 not evaluable
    int    sigRegions  = 0;                 // regions kept (>= floor)
    int    tinyRegions = 0;                 // regions discarded (< floor)
    MeshBoolean::Mesh overlapViz;
    std::string err;

    // Accumulate one overlap region into the viz mesh + the running side volume.
    auto keepRegion = [&](const MeshBoolean::Mesh& region, int side, double vol)
    {
        perSideVol[side] += vol;
        const uint32_t base = (uint32_t)(overlapViz.verts.size() / 3);
        overlapViz.verts.insert(overlapViz.verts.end(),
            region.verts.begin(), region.verts.end());
        for (uint32_t id : region.indices) overlapViz.indices.push_back(base + id);
    };

    for (int side = 0; side < 2; ++side)
    {
        if (!sideSurf[side]) continue;
        MeshBoolean::Mesh prism = BuildSideTravelVolume(side, startEps);
        if (prism.empty()) { perSideStat[side] = 2; continue; }

        MeshBoolean::RepairResult rp = MeshBoolean::ValidateAndRepair(prism);
        if (!rp.ok) { perSideStat[side] = 2; continue; }

        bool evalFailed = false;
        for (size_t h = 0; h < HN; ++h)
        {
            if (halfSide[h] != side) continue;
            MeshBoolean::Mesh half;
            half.verts   = m_halfMeshPos[h];
            half.indices = m_halfMeshIdx[h];
            MeshBoolean::RepairResult rh = MeshBoolean::ValidateAndRepair(half);
            if (!rh.ok) { evalFailed = true; continue; }
            MeshBoolean::Mesh ov;
            if (!MeshBoolean::Intersection(rp.mesh, rh.mesh, ov, err) || ov.empty())
                continue;                                   // no overlap: clear

            // Split the overlap into discrete regions; keep those >= the floor.
            std::vector<MeshBoolean::Mesh> comps;
            std::string derr;
            if (!MeshBoolean::Decompose(ov, comps, derr) || comps.empty())
                comps = { ov };                             // fall back to whole
            for (const MeshBoolean::Mesh& comp : comps)
            {
                const double v = MeshBoolean::Volume(comp);
                if (v >= (double)minOverlap) { keepRegion(comp, side, v); ++sigRegions; }
                else if (v > 0.0)            { ++tinyRegions; }
            }
        }
        if (perSideVol[side] > 0.0) perSideStat[side] = 1;
        else if (evalFailed)        perSideStat[side] = 2;
        else                        perSideStat[side] = 0;
    }

    const double totalVol = perSideVol[0] + perSideVol[1];
    const int collided = (perSideStat[0]==1?1:0) + (perSideStat[1]==1?1:0);
    const int notEval  = (perSideStat[0]==2?1:0) + (perSideStat[1]==2?1:0);
    const int tested   = (sideSurf[0]?1:0) + (sideSurf[1]?1:0);

    // Overlay (red): sew the accumulated overlap mesh into a shape. Its
    // visibility follows this card's "Show mould overlay" checkbox.
    TopoDS_Shape overlapShape;
    if (!overlapViz.indices.empty()) overlapShape = MakeShapeFromBoolMesh(overlapViz);
    m_hasSepOverlay = !overlapShape.IsNull();
    if (m_canvas)
        m_canvas->SetShotDebugSolid(overlapShape, glm::vec3(0.90f, 0.15f, 0.15f));
    UpdateSeparationOverlay();

    DesignChecks::Severity overall;
    if      (collided > 0) overall = DesignChecks::Severity::Fail;
    else if (notEval  > 0) overall = DesignChecks::Severity::Warning;
    else                   overall = DesignChecks::Severity::Pass;

    wxString verdict; wxColour verdictColour; long iconFlag = wxICON_INFORMATION;
    switch (overall)
    {
    case DesignChecks::Severity::Pass:
        verdict = "PASS"; verdictColour = wxColour(0x26,0xAB,0x36); iconFlag = wxICON_INFORMATION; break;
    case DesignChecks::Severity::Warning:
        verdict = "INCONCLUSIVE"; verdictColour = wxColour(0xE0,0x9B,0x20); iconFlag = wxICON_WARNING; break;
    case DesignChecks::Severity::Fail:
        verdict = "FAIL"; verdictColour = wxColour(0xD0,0x46,0x46); iconFlag = wxICON_ERROR; break;
    }
    if (m_demouldStatus)
    {
        m_demouldStatus->SetLabel(verdict);
        m_demouldStatus->SetForegroundColour(verdictColour);
        if (m_infoPanel) m_infoPanel->Layout();
    }

    const wxString mm3 = wxString::FromUTF8(" mm\xC2\xB3");
    const wxString mm  = " mm";
    wxString msg;
    msg << "Separation Test: " << verdict << "\n";
    msg << "Each half's owned surface swept toward the parting plane, tested "
        << "for overlap with that half's steel.\n\n";
    msg << "Sides tested: " << tested << "\n";
    msg << "Sides collided: " << collided << "\n";
    if (notEval > 0)
        msg << "Sides not evaluable: " << notEval << " (mesh boolean failed)\n";
    msg << "Overlap regions kept: " << sigRegions;
    if (tinyRegions > 0) msg << "   (" << tinyRegions << " below floor, discarded)";
    msg << "\n";
    msg << "Significant overlap: " << wxString::Format("%.3f", totalVol) << mm3 << "\n";
    msg << "Start \xce\xb5: " << wxString::Format("%.3g", startEps) << mm
        << ",  noise floor: " << wxString::Format("%.3g", minOverlap) << mm3 << "\n\n";

    const char* sideName[2] = { "A (+draw)", "B (-draw)" };
    for (int side = 0; side < 2; ++side)
    {
        if (!sideSurf[side]) continue;
        const char* s = perSideStat[side]==1 ? "COLLISION"
            : perSideStat[side]==2 ? "not evaluable" : "clear";
        msg << "  Half " << sideName[side] << ": " << s
            << wxString::Format("  (overlap %.3f", perSideVol[side]) << mm3 << ")\n";
    }

    if (collided > 0)
        msg << "\nEnable \"Show mould overlay\" to see the interference region "
               "(red); hide the Shot toggle to view it clearly.";

    wxMessageBox(msg, "Separation Test", wxOK | iconFlag, this);
}

// ---------------------------------------------------------------------------
// Debug-view driver for the Draft Angle Checks. Two modes: None (clear, with an
// optional wireframe) and Draft (ray) — colour the parting-split analysis mesh
// by per-facet signed draft (violet back-draft / red fail / yellow warn /
// neutral ok), drawn as its own mesh so the parting ring shows (wireframe
// reveals it). Reads m_faceDraftSamples; no remesh, no re-analysis.
// ---------------------------------------------------------------------------
void PreviewPanel::UpdateDraftOverlay()
{
    if (!m_canvas) return;

    const int  mode = m_debugModeChoice ? m_debugModeChoice->GetSelection() : 0;
    const bool wire = m_debugWireCheck && m_debugWireCheck->GetValue();

    if (mode <= 0)
    {
        // None: clear, honouring the wireframe toggle.
        m_canvas->ClearShotDebugColoring();
        m_canvas->SetShotDebugWireframe(wire && m_shotHalfIndex >= 0);
        return;
    }

    // All other modes need the ownership analysis; build it on demand.
    if (m_shotHalfIndex < 0 || !EnsureFaceDraftAnalysis()
        || m_faceDraftIdx.size() < 3 || m_faceDraftPosNorm.empty())
    {
        m_canvas->ClearShotDebugColoring();
        m_canvas->SetShotDebugWireframe(false);
        return;
    }

    if (mode == 2 || mode == 3)   // Travel volume A (+draw) / B (-draw)
    {
        const int side = (mode == 2) ? 0 : 1;
        const float startEps = (float)std::max(0.0, ParseField(m_sepStartEpsCtrl, 0.001));
        MeshBoolean::Mesh vol = BuildSideTravelVolume(side, startEps);
        if (vol.empty())
        {
            m_canvas->ClearShotDebugColoring();
            m_canvas->SetShotDebugWireframe(false);
            return;
        }
        FileImporter::MeshData disp = FlatDisplayMesh(vol);
        std::vector<GLCanvas::ShotDebugGroup> groups(1);
        groups[0].color = (side == 0) ? glm::vec3(0.30f, 0.55f, 0.95f)    // A: blue
                                      : glm::vec3(0.95f, 0.60f, 0.20f);   // B: orange
        groups[0].emissive = true;
        groups[0].indices.reserve(disp.indices.size());
        for (uint32_t id : disp.indices) groups[0].indices.push_back(id);
        m_canvas->SetShotDebugMesh(m_shotHalfIndex, disp.posNorm, groups);
        m_canvas->SetShotDebugWireframe(wire);
        return;
    }

    const float failDeg = (float)std::clamp(ParseField(m_failDraftCtrl, 1.0), 0.0, 45.0);
    float       warnDeg = (float)std::clamp(ParseField(m_warnDraftCtrl, 3.0), 0.0, 45.0);
    if (warnDeg < failDeg) warnDeg = failDeg;
    const float bdEps = (float)std::clamp(ParseField(m_backdraftEpsCtrl, 0.1), 0.0, 45.0);

    // Worst (lowest) signed draft per split-triangle key (faceId == tri + 1).
    std::unordered_map<int, float> draftOfFace;
    for (const DesignChecks::DraftSample& s : m_faceDraftSamples)
    {
        if (s.faceId <= 0) continue;
        auto it = draftOfFace.find(s.faceId);
        if (it == draftOfFace.end() || s.signedDraftDeg < it->second)
            draftOfFace[s.faceId] = s.signedDraftDeg;
    }

    const glm::vec3 kViolet (0.68f, 0.28f, 0.85f);   // back-draft (< -eps)
    const glm::vec3 kRed    (0.92f, 0.16f, 0.16f);   // fail
    const glm::vec3 kYellow (0.95f, 0.80f, 0.10f);   // warn
    const glm::vec3 kNeutral(0.80f, 0.80f, 0.85f);   // ok
    std::vector<GLCanvas::ShotDebugGroup> groups(4);
    groups[0].color = kViolet;  groups[0].emissive = true;
    groups[1].color = kRed;     groups[1].emissive = true;
    groups[2].color = kYellow;  groups[2].emissive = true;
    groups[3].color = kNeutral; groups[3].emissive = false;

    const std::vector<unsigned int>& I = m_faceDraftIdx;
    for (size_t t = 0; t + 2 < I.size(); t += 3)
    {
        const int key = (int)(t / 3) + 1;
        int g = 3;
        auto it = draftOfFace.find(key);
        if (it != draftOfFace.end())
        {
            const float d = it->second;
            if      (d < -bdEps)  g = 0;
            else if (d < failDeg) g = 1;
            else if (d < warnDeg) g = 2;
            else                  g = 3;
        }
        groups[(size_t)g].indices.push_back(I[t]);
        groups[(size_t)g].indices.push_back(I[t+1]);
        groups[(size_t)g].indices.push_back(I[t+2]);
    }
    m_canvas->SetShotDebugMesh(m_shotHalfIndex, m_faceDraftPosNorm, groups);
    m_canvas->SetShotDebugWireframe(wire);
}

// ---------------------------------------------------------------------------
// Separation Test "Show mould overlay" — show or hide the interference solid
// produced by the last run. A no-op when no run has produced one.
// ---------------------------------------------------------------------------
void PreviewPanel::UpdateSeparationOverlay()
{
    if (!m_canvas) return;
    const bool show = m_sepOverlayCheck && m_sepOverlayCheck->GetValue();
    m_canvas->ShowShotDebugSolid(show && m_hasSepOverlay);
}

// ---------------------------------------------------------------------------
// (Re)build the show/hide visibility checkboxes for the current part set, into
// m_visPanel (left column). One per mould half, then "Shot" if present.
// ---------------------------------------------------------------------------
void PreviewPanel::BuildVisibilityChecks(int halfCount, bool hasShot, int insertCount)
{
    if (!m_visPanel) return;
    auto* vSizer = m_visPanel->GetSizer();

    const bool anyParts = (halfCount > 0) || hasShot || (insertCount > 0);
    if (m_visEmptyLabel) m_visEmptyLabel->Show(!anyParts);

    auto addCheck = [&](int partIndex, const wxString& label, const wxString& tip,
        bool initialVisible)
    {
        auto* cb = new wxCheckBox(m_visPanel, kHalfToggleIdBase + partIndex, label);
        cb->SetForegroundColour(Style::TextPrimary);
        cb->SetBackgroundColour(Style::CardBg);
        cb->SetValue(initialVisible);
        cb->SetToolTip(tip);
        cb->Bind(wxEVT_CHECKBOX,
            [this, partIndex](wxCommandEvent& evt)
            {
                if (m_canvas) m_canvas->SetPreviewHalfVisible(partIndex, evt.IsChecked());
            });
        vSizer->Add(cb, 0, wxEXPAND | wxALL, 6);
        m_halfChecks.push_back(cb);
    };

    // One checkbox per mould half, indices [0 .. halfCount-1]. Half A (index 0)
    // starts hidden so the preview opens looking into the cavity / at the shot.
    for (int i = 0; i < halfCount; ++i)
    {
        const wxString label = "Half " + HalfLetter(i);
        addCheck(i, label, "Show / hide " + label, /*initialVisible*/ i != 0);
    }

    // Shot, loaded as preview part index == halfCount (appended after the
    // halves in LoadHalves, so the indices line up).
    if (hasShot)
        addCheck(halfCount, "Shot",
            "Show / hide the shot model (part + feed system)", /*initialVisible*/ true);

    // Inserts: ONE checkbox for the whole category, not one per body. It drives
    // the contiguous block of preview parts [firstIdx, firstIdx + insertCount).
    // Its command ID is based on firstIdx so it can't collide with the per-part
    // half/shot IDs above. Visible by default.
    m_insertCheck = nullptr;
    if (insertCount > 0)
    {
        const int firstIdx = halfCount + (hasShot ? 1 : 0);
        auto* cb = new wxCheckBox(m_visPanel, kHalfToggleIdBase + firstIdx, "Inserts");
        cb->SetForegroundColour(Style::TextPrimary);
        cb->SetBackgroundColour(Style::CardBg);
        cb->SetValue(true);
        cb->SetToolTip("Show / hide all inserts");
        cb->Bind(wxEVT_CHECKBOX,
            [this, firstIdx, insertCount](wxCommandEvent& evt)
            {
                if (!m_canvas) return;
                const bool on = evt.IsChecked();
                for (int k = 0; k < insertCount; ++k)
                    m_canvas->SetPreviewHalfVisible(firstIdx + k, on);
            });
        vSizer->Add(cb, 0, wxEXPAND | wxALL, 6);
        m_insertCheck = cb;
    }

    m_visPanel->Layout();
    if (m_visPanel->GetParent()) m_visPanel->GetParent()->Layout();
}

// ---------------------------------------------------------------------------
// Drop the current visibility checkboxes and restore the empty-state message.
// ---------------------------------------------------------------------------
void PreviewPanel::ClearVisibilityChecks()
{
    auto* vSizer = m_visPanel ? m_visPanel->GetSizer() : nullptr;
    for (wxCheckBox* cb : m_halfChecks)
    {
        if (!cb) continue;
        if (vSizer) vSizer->Detach(cb);
        cb->Destroy();
    }
    m_halfChecks.clear();
    if (m_insertCheck)
    {
        if (vSizer) vSizer->Detach(m_insertCheck);
        m_insertCheck->Destroy();
        m_insertCheck = nullptr;
    }
    ClearCastChecks();
    if (m_visEmptyLabel) m_visEmptyLabel->Show(true);
    if (m_visPanel) m_visPanel->Layout();
}

// ---------------------------------------------------------------------------
// Drop the cast-body groups (Top Cast / Bottom Cast) from the visibility card,
// leaving the half / shot / insert toggles in place. Destroying each group
// panel frees its child checkboxes too. Paired with a canvas
// TruncatePreviewHalves(m_castAnchorCount) so a cast re-generation starts clean.
// ---------------------------------------------------------------------------
void PreviewPanel::ClearCastChecks()
{
    auto* vSizer = m_visPanel ? m_visPanel->GetSizer() : nullptr;
    for (wxPanel* group : m_castGroupPanels)
    {
        if (!group) continue;
        if (vSizer) vSizer->Detach(group);
        group->Destroy();   // takes its child checkboxes with it
    }
    m_castGroupPanels.clear();
    m_castChecks.clear();   // children already destroyed above
    m_castExports.clear();  // retained export meshes go with the cast bodies
}

// ---------------------------------------------------------------------------
// Upload one cast body to the canvas (no UI) and return its preview-part index.
// ---------------------------------------------------------------------------
int PreviewPanel::AddCastPart(const FileImporter::MeshData& mesh,
    const glm::vec3& color, const wxString& label)
{
    if (!m_canvas) return -1;
    const int partIndex = m_canvas->GetPreviewHalfCount();
    m_canvas->AddPreviewHalf(mesh, label.ToStdString(), color);
    return partIndex;
}

// ---------------------------------------------------------------------------
// Add a collapsible cast group: a parent "master" checkbox (shows/hides the
// whole group) and a chevron that expands to per-child checkboxes. Each child's
// mesh is uploaded to the canvas as its own preview part so it can be toggled.
// ---------------------------------------------------------------------------
void PreviewPanel::AddCastGroup(const wxString& groupLabel,
    std::vector<CastChild>& children)
{
    if (!m_visPanel || children.empty()) return;
    if (m_visEmptyLabel) m_visEmptyLabel->Show(false);

    auto* group = new wxPanel(m_visPanel, wxID_ANY);
    group->SetBackgroundColour(Style::CardBg);
    auto* gSizer = new wxBoxSizer(wxVERTICAL);

    // ---- Header: master checkbox (left) + chevron (right) -----------------
    auto* header = new wxBoxSizer(wxHORIZONTAL);
    auto* parentCheck = new wxCheckBox(group, wxID_ANY, groupLabel);
    parentCheck->SetForegroundColour(Style::TextPrimary);
    parentCheck->SetBackgroundColour(Style::CardBg);
    parentCheck->SetValue(true);
    parentCheck->SetToolTip("Show / hide the whole " + groupLabel);

    auto* chevron = new wxButton(group, wxID_ANY, wxEmptyString,
        wxDefaultPosition, wxSize(22, 22), wxBU_EXACTFIT | wxBORDER_NONE);
    chevron->SetBackgroundColour(Style::CardBg);
    chevron->SetBitmap(LoadSvgBundle(kChevronRightSvg, wxSize(12, 12), true));
    chevron->SetToolTip("Show / hide the individual bodies");

    header->Add(parentCheck, 1, wxALIGN_CENTER_VERTICAL);
    header->Add(chevron, 0, wxALIGN_CENTER_VERTICAL);
    gSizer->Add(header, 0, wxEXPAND | wxALL, 6);

    // ---- Child panel (collapsed by default) -------------------------------
    auto* childPanel = new wxPanel(group, wxID_ANY);
    childPanel->SetBackgroundColour(Style::CardBg);
    auto* cSizer = new wxBoxSizer(wxVERTICAL);

    std::vector<std::pair<wxCheckBox*, int>> childInfo;
    for (CastChild& ch : children)
    {
        const int part = AddCastPart(ch.mesh, ch.color, ch.label);

        auto* cb = new wxCheckBox(childPanel, kHalfToggleIdBase + part, ch.label);
        cb->SetForegroundColour(Style::TextPrimary);
        cb->SetBackgroundColour(Style::CardBg);
        cb->SetValue(true);
        if (!ch.tip.IsEmpty()) cb->SetToolTip(ch.tip);
        cb->Bind(wxEVT_CHECKBOX,
            [this, part](wxCommandEvent& evt)
            {
                if (m_canvas) m_canvas->SetPreviewHalfVisible(part, evt.IsChecked());
            });
        cSizer->Add(cb, 0, wxEXPAND | wxLEFT | wxTOP, 10);   // indented
        m_castChecks.push_back(cb);
        childInfo.push_back({ cb, part });
    }
    cSizer->AddSpacer(4);
    childPanel->SetSizer(cSizer);
    childPanel->Show(false);   // start collapsed
    gSizer->Add(childPanel, 0, wxEXPAND | wxLEFT | wxRIGHT, 6);

    // Master checkbox drives every child (both its checkbox and its 3D part).
    parentCheck->Bind(wxEVT_CHECKBOX,
        [this, childInfo](wxCommandEvent& evt)
        {
            const bool on = evt.IsChecked();
            for (const auto& ci : childInfo)
            {
                if (ci.first) ci.first->SetValue(on);
                if (m_canvas) m_canvas->SetPreviewHalfVisible(ci.second, on);
            }
        });

    // Chevron expands / collapses the child list (UI only — not 3D visibility).
    chevron->Bind(wxEVT_BUTTON,
        [this, chevron, childPanel](wxCommandEvent&)
        {
            const bool expand = !childPanel->IsShown();
            chevron->SetBitmap(LoadSvgBundle(
                expand ? kChevronDownSvg : kChevronRightSvg, wxSize(12, 12), true));
            childPanel->Show(expand);
            m_visPanel->Layout();
            if (m_visPanel->GetParent()) m_visPanel->GetParent()->Layout();
        });

    group->SetSizer(gSizer);
    m_visPanel->GetSizer()->Add(group, 0, wxEXPAND | wxALL, 4);
    m_castGroupPanels.push_back(group);
}

void PreviewPanel::LoadHalves()
{
    if (!m_canvas) return;

    // Mould halves first, in fixture order — their indices match the
    // visibility checkboxes built in BuildVisibilityChecks.
    for (size_t i = 0; i < m_pendingHalves.size(); ++i)
    {
        const std::string label = "Half " + HalfLetter((int)i);
        m_canvas->AddPreviewHalf(m_pendingHalves[i], label);
    }

    // Shot last, so its preview-part index equals m_pendingHalves.size(),
    // matching the shot checkbox. A distinct amber base colour separates it
    // from the grey mould halves. m_shotMesh is kept (not dropped) so the
    // design checks can analyse it on demand.
    if (m_hasShot)
    {
        const glm::vec3 shotColor(0.85f, 0.50f, 0.20f);
        m_canvas->AddPreviewHalf(m_shotMesh, "Shot", shotColor);
    }

    // Inserts last, as a contiguous block after the shot. Each is its own
    // preview part (so the canvas can show/hide them), but they share the one
    // "Inserts" checkbox. Yellow — the same colour they carry in the Prepare
    // perspective — so a body reads as the same insert across both views. The
    // label is per-body (they're distinct parts) but never surfaced as its own
    // toggle, so it only matters for any part-label debug readout.
    for (size_t i = 0; i < m_pendingInserts.size(); ++i)
    {
        const glm::vec3 insertColor(0.82f, 0.62f, 0.28f);
        m_canvas->AddPreviewHalf(m_pendingInserts[i],
            "Insert " + std::to_string(i + 1), insertColor);
    }

    // Half + insert meshes are now on the GPU; drop the CPU copies (the shot is
    // kept for the design checks).
    m_pendingHalves.clear();
    m_pendingHalves.shrink_to_fit();
    m_pendingInserts.clear();
    m_pendingInserts.shrink_to_fit();

    // Apply the initial checkbox states to the freshly loaded parts (parts are
    // added visible, so hide any whose checkbox starts unchecked — e.g. Half A).
    for (size_t i = 0; i < m_halfChecks.size(); ++i)
        if (m_halfChecks[i] && !m_halfChecks[i]->GetValue())
            m_canvas->SetPreviewHalfVisible((int)i, false);

    // The one insert checkbox governs the whole [m_insertFirstIndex, +count)
    // block. It starts checked (visible), so no initial hide is needed; honour
    // it anyway in case a future default flips.
    if (m_insertCheck && !m_insertCheck->GetValue() && m_insertFirstIndex >= 0)
        for (int k = 0; k < m_insertCount; ++k)
            m_canvas->SetPreviewHalfVisible(m_insertFirstIndex + k, false);

    m_canvas->Refresh(false);
}
