#include "CastingPanel.h"
#include "GLCanvas.h"
#include "MainFrame.h"        // GetIndexerRadius / GetIndexerExtraTolerance
#include "style.h"
#include "RoundedButton.h"
#include "MouldCastDialog.h"   // MouldCastValues / MouldCastPart value structs
#include "MeshBoolean.h"       // split the shot at y=0 and fuse a half into each base

#include <opencascade/BRepPrimAPI_MakeBox.hxx>
#include <opencascade/BRepPrimAPI_MakeSphere.hxx>
#include <opencascade/BRepAlgoAPI_Common.hxx>
#include <opencascade/BRepAlgoAPI_Fuse.hxx>
#include <opencascade/BRepAlgoAPI_Cut.hxx>
#include <opencascade/gp_Pnt.hxx>

#include <wx/bmpbndl.h>
#include <wx/file.h>
#include <wx/filename.h>
#include <wx/stdpaths.h>
#include <wx/scrolwin.h>
#include <wx/tglbtn.h>
#include <wx/progdlg.h>   // Generate Mould Casts progress feedback

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

// ---------------------------------------------------------------------------
// Chevron icons + SVG loader, kept file-local so the casting cards' collapsible
// groups match the rest of the app. (Other TUs carry their own internal-linkage
// copy; duplicated here to avoid coupling.)
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

// Id base for the Cast Bodies visibility checkboxes — one sequential id per
// part so each maps cleanly to a preview-part index. Part 0 is the Cast Shot
// Body; generated cast bodies take the indices above it.
static const int kCastToggleIdBase = wxID_HIGHEST + 6000;

// ---------------------------------------------------------------------------
// Accumulate a mesh's vertices into a running AABB. Mirrors the helper used by
// PreviewPanel so the cast perimeter matches the mould-half bounds exactly.
// ---------------------------------------------------------------------------
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

// ---------------------------------------------------------------------------
// Cast geometry helpers — box meshes / solids and the boolean plumbing used
// to fuse the shot halves into the bases and cut the tongue-and-groove
// joints. Ported from PreviewPanel; kept file-local (internal linkage).
// ---------------------------------------------------------------------------
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

// ---------------------------------------------------------------------------
// MakeIndexerSphereSolid — a full sphere at `center`/`radius`, used for the
// indexer join/cut on the top/bottom base (see GenerateCasts).
//
// This used to hand Fuse/Cut a pre-clipped hemisphere (built by intersecting
// the sphere with a half-space box), but that hemisphere's flat cut face
// lands EXACTLY on the base box's own y=0 face — a coincident/tangential
// face is the classic degenerate case for an OCC boolean, and it was
// silently failing (or producing a shape whose boss/pocket effectively
// vanished) rather than throwing anything actionable.
//
// Passing the FULL sphere instead sidesteps that: half of it genuinely
// overlaps the target solid's volume (not just touches a face), so Fuse/Cut
// have well-defined 3D geometry to work with. The other half — past the
// box's y=0 face — either extends the fuse as the same protruding boss
// (join case) or has no effect since there's nothing there to remove (cut
// case), so the visible result is identical to the hemisphere approach; it's
// just numerically robust instead of borderline-degenerate.
// ---------------------------------------------------------------------------
static TopoDS_Shape MakeIndexerSphereSolid(const glm::vec3& center, float radius)
{
    if (radius < 1e-6f) return TopoDS_Shape();

    BRepPrimAPI_MakeSphere sphereMk(gp_Pnt(center.x, center.y, center.z), radius);
    sphereMk.Build();
    return (sphereMk.IsDone() && !sphereMk.Shape().IsNull())
        ? sphereMk.Shape() : TopoDS_Shape();
}

// MakeIndexerSphereBool — the same indexer sphere as MakeIndexerSphereSolid,
// but as a MeshBoolean::Mesh for the mesh-scene base path. Built by
// tessellating the OCC sphere and pulling positions/indices out via
// ToBoolMesh, so the boss/pocket geometry is identical to the BREP path
// (same source primitive). OCC's sphere tessellation is a clean watertight
// surface; MeshBoolean's own Merge welds any coincident verts on the way in.
// Empty on failure. Defined after ToBoolMesh (above) and MakeIndexerSphereSolid.
static MeshBoolean::Mesh MakeIndexerSphereBool(const glm::vec3& center, float radius)
{
    const TopoDS_Shape sph = MakeIndexerSphereSolid(center, radius);
    if (sph.IsNull()) return MeshBoolean::Mesh{};
    FileImporter::MeshData md;
    GLCanvas::TessellateShapeToMesh(sph, md, nullptr);
    return ToBoolMesh(md);
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
// Construction — builds the static perspective UI (left "Cast Tool Settings"
// toolbar + preview canvas) with no data loaded. The canvas renders its grid
// immediately; the Cast Shot Body arrives via SetData. There is deliberately
// no right-hand information column — the Preview perspective still provides the
// shot volume / body read-outs.
// ---------------------------------------------------------------------------
CastingPanel::CastingPanel(wxWindow* parent)
    : wxPanel(parent, wxID_ANY)
{
    SetBackgroundColour(Style::AppBg);

    auto* root = new wxBoxSizer(wxVERTICAL);

    // ---- Main row: cast tool settings | canvas ----------------------------
    auto* middle = new wxBoxSizer(wxHORIZONTAL);

    wxPanel* toolPanel = BuildToolPanel(this);
    middle->Add(toolPanel, 0, wxEXPAND | wxRIGHT, 1);

    m_canvas = new GLCanvas(this);
    m_canvas->SetPreviewMode(true);
    middle->Add(m_canvas, 1, wxEXPAND);

    root->Add(middle, 1, wxEXPAND);

    SetSizer(root);
}

// ---------------------------------------------------------------------------
// Left toolbar — "CAST TOOL SETTINGS" (wall/base setting cards, added later)
// over a "CAST BODIES" visibility card. Styled to match the Prepare / Preview
// left panels: a fixed-width AppBg column with a right divider border.
// ---------------------------------------------------------------------------
// Toolbar field metrics (narrower than the dialog's — the column is only 280px).
static const int kToolLabelW = 78;
static const int kToolFieldW = 74;
static const int kToolUnitW  = 48;

// A muted fixed-width row label.
static wxStaticText* ToolLabel(wxWindow* parent, const wxString& text)
{
    auto* l = new wxStaticText(parent, wxID_ANY, text,
        wxDefaultPosition, wxSize(kToolLabelW, -1));
    l->SetForegroundColour(Style::TextMuted);
    l->SetFont(wxFont(8, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
        wxFONTWEIGHT_NORMAL, false, "Segoe UI"));
    return l;
}

// A dark text field styled like the mould-feature inputs. Background switched
// from Style::InputBg to Style::BtnSmall to match the dimension fields on the
// Mould Tool Settings cards (CreateEjectorsContent / CreateInsertsContent).
static wxTextCtrl* ToolField(wxWindow* parent, const wxString& val)
{
    auto* t = new wxTextCtrl(parent, wxID_ANY, val,
        wxDefaultPosition, wxSize(kToolFieldW, -1));
    t->SetBackgroundColour(Style::BtnSmall);
    t->SetForegroundColour(Style::TextPrimary);
    return t;
}

// A mm / in unit dropdown, styled to match the mould-feature Type dropdowns
// (was left at system default colours before — a light control on the app's
// dark cards).
static wxChoice* ToolUnit(wxWindow* parent, bool inchesSel)
{
    wxArrayString units; units.Add("mm"); units.Add("in");
    auto* c = new wxChoice(parent, wxID_ANY,
        wxDefaultPosition, wxSize(kToolUnitW, -1), units);
    c->SetBackgroundColour(Style::BtnSmall);
    c->SetForegroundColour(Style::TextMuted);
    c->SetSelection(inchesSel ? 1 : 0);
    return c;
}

// ---------------------------------------------------------------------------
// Left toolbar — "CAST TOOL SETTINGS" (Base / Walls cards) over a "CAST BODIES"
// visibility card, all inside a scroll region so a short window never clips.
// ---------------------------------------------------------------------------
wxPanel* CastingPanel::BuildToolPanel(wxWindow* parent)
{
    auto* outer = new wxPanel(parent, wxID_ANY,
        wxDefaultPosition, wxSize(280, -1));
    outer->SetBackgroundColour(Style::AppBg);
    auto* outerSizer = new wxBoxSizer(wxHORIZONTAL);

    auto* column = new wxPanel(outer, wxID_ANY);
    column->SetBackgroundColour(Style::AppBg);
    auto* colSizer = new wxBoxSizer(wxVERTICAL);

    // Section title (matches the Prepare panel's "MOULD TOOL SETTINGS").
    auto* title = new wxStaticText(column, wxID_ANY, "CAST TOOL SETTINGS");
    title->SetForegroundColour(Style::TextPrimary);
    title->SetFont(wxFont(7, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
        wxFONTWEIGHT_BOLD, false, "Segoe UI"));
    colSizer->Add(title, 0, wxLEFT | wxTOP, 12);
    colSizer->AddSpacer(8);

    // Everything below the title scrolls, so tall setting cards plus a growing
    // Cast Bodies list never clip on a short window.
    auto* scroll = new wxScrolledWindow(column, wxID_ANY,
        wxDefaultPosition, wxDefaultSize, wxVSCROLL | wxBORDER_NONE);
    scroll->SetScrollRate(0, 8);
    scroll->SetBackgroundColour(Style::AppBg);
    auto* sSizer = new wxBoxSizer(wxVERTICAL);

    // ---- Base / Walls setting cards ---------------------------------------
    // Same defaults the retired MouldCastDialog used: 5 mm thickness, 0 extra,
    // 2 mm tongue width / thickness, 0 groove tolerance. Mounted full width
    // (no left/right margin) with an 8px top gap, matching how the Mould
    // Tool Settings cards (CreateEjectorsContent etc.) are mounted in
    // MainFrame::CreateLeftPanel.
    sSizer->AddSpacer(4);
    {
        wxArrayString baseTypes;  baseTypes.Add("Flange");
        auto* baseCard = BuildPartCard(scroll, "Base", baseTypes,
            /*defThickness*/ 5.0, "Extra Flange:", /*withJoint*/ false, m_baseUI);
        sSizer->Add(baseCard, 0, wxEXPAND | wxTOP, 8);

        wxArrayString wallTypes;  wallTypes.Add("Clover");
        auto* wallCard = BuildPartCard(scroll, "Walls", wallTypes,
            /*defThickness*/ 5.0, "Extra Wall:", /*withJoint*/ true, m_wallsUI);
        sSizer->Add(wallCard, 0, wxEXPAND | wxTOP, 8);
    }

    // Divider between the settings and the Cast Bodies card.
    auto* divider = new wxPanel(scroll, wxID_ANY, wxDefaultPosition, wxSize(-1, 1));
    divider->SetBackgroundColour(Style::Divider);
    sSizer->Add(divider, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 12);

    // ---- Cast Bodies (visibility) -----------------------------------------
    auto* visTitle = new wxStaticText(scroll, wxID_ANY, "CAST BODIES");
    visTitle->SetForegroundColour(Style::TextPrimary);
    visTitle->SetFont(wxFont(7, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
        wxFONTWEIGHT_BOLD, false, "Segoe UI"));
    sSizer->Add(visTitle, 0, wxLEFT | wxTOP, 12);
    sSizer->AddSpacer(6);

    m_visPanel = new wxPanel(scroll, wxID_ANY);
    m_visPanel->SetBackgroundColour(Style::CardBg);
    auto* visSizer = new wxBoxSizer(wxVERTICAL);

    m_visEmptyLabel = new wxStaticText(m_visPanel, wxID_ANY, "No bodies generated");
    m_visEmptyLabel->SetForegroundColour(Style::TextMuted);
    m_visEmptyLabel->SetFont(wxFont(9, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_ITALIC,
        wxFONTWEIGHT_NORMAL, false, "Segoe UI"));
    visSizer->Add(m_visEmptyLabel, 0, wxALL, 10);

    m_visPanel->SetSizer(visSizer);
    sSizer->Add(m_visPanel, 0, wxEXPAND | wxLEFT | wxRIGHT, 12);
    sSizer->AddSpacer(12);

    scroll->SetSizer(sSizer);
    colSizer->Add(scroll, 1, wxEXPAND);

    column->SetSizer(colSizer);
    outerSizer->Add(column, 1, wxEXPAND);

    // Right border line, matching the Prepare / Preview panels' divider.
    auto* borderLine = new wxPanel(outer, wxID_ANY,
        wxDefaultPosition, wxSize(1, -1));
    borderLine->SetBackgroundColour(Style::Divider);
    outerSizer->Add(borderLine, 0, wxEXPAND);

    outer->SetSizer(outerSizer);

    // Reflect the initial (disabled) enable state — greys each card's rows.
    SyncPartEnabled(m_baseUI);
    SyncPartEnabled(m_wallsUI);
    return outer;
}

// ---------------------------------------------------------------------------
// Build one Base / Walls setting card, populating `ui`. Restyled to match the
// Mould Tool Settings cards (CreateEjectorsContent / CreateInsertsContent in
// MainFrame.cpp): a plain (non-collapsible) title, the card's primary control
// always visible below it — here "Enable Generation", playing the role the
// "Place X" button plays on those cards — then a separate collapsible
// "Settings" section (collapsed by default) holding the Type dropdown and the
// dimension rows. Previously the title itself WAS the collapse toggle for the
// whole card; that's replaced by the dedicated Settings chevron so this card
// reads like every other feature card in the app. Rows/logic unchanged —
// still mirrors MouldCastDialog::BuildPartSection's fields and defaults.
// ---------------------------------------------------------------------------
wxPanel* CastingPanel::BuildPartCard(wxWindow* parent, const wxString& title,
    const wxArrayString& typeChoices, double defThickness,
    const wxString& extraLabel, bool withJoint, PartUI& ui)
{
    auto* card = new wxPanel(parent, wxID_ANY);
    card->SetBackgroundColour(Style::CardBg);
    auto* cardSizer = new wxBoxSizer(wxVERTICAL);

    // Title — plain heading, matching the Mould Tool Settings cards.
    auto* titleLabel = new wxStaticText(card, wxID_ANY, title);
    titleLabel->SetForegroundColour(*wxWHITE);
    titleLabel->SetFont(wxFont(9, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
        wxFONTWEIGHT_BOLD, false, "Segoe UI"));
    cardSizer->Add(titleLabel, 0, wxLEFT | wxTOP, 12);
    cardSizer->AddSpacer(6);

    // Enable Generation — the card's primary, always-visible control.
    ui.enable = new wxCheckBox(card, wxID_ANY, "Enable Generation");
    ui.enable->SetForegroundColour(Style::TextPrimary);
    ui.enable->SetBackgroundColour(Style::CardBg);
    ui.enable->SetValue(false);
    cardSizer->Add(ui.enable, 0, wxLEFT | wxRIGHT | wxTOP, 12);
    cardSizer->AddSpacer(8);

    // Collapsible Settings — same chevron / debounce pattern as the Mould
    // Tool Settings cards.
    auto* settingsBtn = new wxToggleButton(card, wxID_ANY, "Settings",
        wxDefaultPosition, wxSize(-1, 22), wxBU_LEFT | wxBORDER_NONE);
    settingsBtn->SetValue(false);
    settingsBtn->SetBackgroundColour(Style::CardBg);
    settingsBtn->SetForegroundColour(Style::TextSubtle);
    settingsBtn->SetFont(wxFont(9, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
        wxFONTWEIGHT_NORMAL, false, "Segoe UI"));
    settingsBtn->SetBitmap(LoadSvgBundle(kChevronRightSvg, wxSize(12, 12), true));
    settingsBtn->SetBitmapPosition(wxRIGHT);
    cardSizer->Add(settingsBtn, 0, wxEXPAND | wxLEFT | wxRIGHT, 12);

    auto* body = new wxPanel(card, wxID_ANY);
    body->SetBackgroundColour(Style::CardBg);
    auto* bs = new wxBoxSizer(wxVERTICAL);

    // Type row.
    {
        auto* row = new wxBoxSizer(wxHORIZONTAL);
        row->Add(ToolLabel(body, "Type:"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
        ui.type = new wxChoice(body, wxID_ANY, wxDefaultPosition,
            wxSize(kToolFieldW + kToolUnitW + 6, -1), typeChoices);
        ui.type->SetBackgroundColour(Style::BtnSmall);
        ui.type->SetForegroundColour(Style::TextMuted);
        ui.type->SetSelection(0);
        row->Add(ui.type, 0, wxALIGN_CENTER_VERTICAL);
        bs->Add(row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 10);
    }

    // Value + unit row helper (label / field / mm-in dropdown).
    auto addValueRow = [&](const wxString& label, const wxString& defVal,
        wxTextCtrl*& fieldOut, wxChoice*& unitOut)
    {
        auto* row = new wxBoxSizer(wxHORIZONTAL);
        row->Add(ToolLabel(body, label), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
        fieldOut = ToolField(body, defVal);
        unitOut = ToolUnit(body, false);
        row->Add(fieldOut, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
        row->Add(unitOut, 0, wxALIGN_CENTER_VERTICAL);
        bs->Add(row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 8);
    };

    addValueRow("Thickness:", wxString::Format("%g", defThickness),
        ui.thickness, ui.unit);
    addValueRow(extraLabel, "0", ui.extra, ui.extraUnit);

    if (withJoint)
    {
        addValueRow("Tongue Width:", "2", ui.tongueW, ui.tongueWUnit);
        addValueRow("Tongue Thick:", "2", ui.tongueT, ui.tongueTUnit);
        addValueRow("Groove Tol:",   "0", ui.grooveTol, ui.grooveTolUnit);
    }

    bs->AddSpacer(6);
    body->SetSizer(bs);
    body->Show(false);
    cardSizer->Add(body, 0, wxEXPAND);

    // Settings chevron collapse / expand.
    settingsBtn->Bind(wxEVT_TOGGLEBUTTON, [settingsBtn, body, card](wxCommandEvent&)
    {
        // Same 200ms debounce as the Mould Tool Settings cards — a mid-frame
        // double-toggle otherwise re-collapses the panel before layout finishes.
        static wxLongLong lastToggleMs = 0;
        wxLongLong now = wxGetLocalTimeMillis();
        if ((now - lastToggleMs).GetValue() < 200) { settingsBtn->SetValue(!settingsBtn->GetValue()); return; }
        lastToggleMs = now;
        const bool expanded = settingsBtn->GetValue();
        settingsBtn->SetBitmap(LoadSvgBundle(
            expanded ? kChevronDownSvg : kChevronRightSvg, wxSize(12, 12), true));
        settingsBtn->SetBitmapPosition(wxRIGHT);
        body->Show(expanded);
        card->Layout();
        if (card->GetParent()) card->GetParent()->Layout();
    });

    // Enable greys the rows, live.
    PartUI* uip = &ui;
    ui.enable->Bind(wxEVT_CHECKBOX,
        [this, uip](wxCommandEvent&) { SyncPartEnabled(*uip); });

    card->SetSizer(cardSizer);
    return card;
}

// ---------------------------------------------------------------------------
// Grey a card's rows to match its Enable checkbox. Mirrors
// MouldCastDialog::SyncEnabled.
// ---------------------------------------------------------------------------
void CastingPanel::SyncPartEnabled(const PartUI& ui)
{
    const bool on = ui.enable && ui.enable->GetValue();
    if (ui.type)          ui.type->Enable(on);
    if (ui.thickness)     ui.thickness->Enable(on);
    if (ui.unit)          ui.unit->Enable(on);
    if (ui.extra)         ui.extra->Enable(on);
    if (ui.extraUnit)     ui.extraUnit->Enable(on);
    if (ui.tongueW)       ui.tongueW->Enable(on);
    if (ui.tongueWUnit)   ui.tongueWUnit->Enable(on);
    if (ui.tongueT)       ui.tongueT->Enable(on);
    if (ui.tongueTUnit)   ui.tongueTUnit->Enable(on);
    if (ui.grooveTol)     ui.grooveTol->Enable(on);
    if (ui.grooveTolUnit) ui.grooveTolUnit->Enable(on);
}

// ---------------------------------------------------------------------------
// Read the toolbar controls back into a MouldCastValues. ReadPartUI mirrors
// MouldCastDialog::ReadPart one-for-one.
// ---------------------------------------------------------------------------
MouldCastPart CastingPanel::ReadPartUI(const PartUI& ui) const
{
    MouldCastPart p;
    p.enabled = ui.enable && ui.enable->GetValue();

    if (ui.type && ui.type->GetSelection() != wxNOT_FOUND)
        p.type = ui.type->GetStringSelection().ToStdString();

    double t = 0.0;
    if (ui.thickness && !ui.thickness->GetValue().ToDouble(&t)) t = 0.0;
    p.thickness = t;
    p.inches = ui.unit && ui.unit->GetSelection() == 1;

    double ex = 0.0;
    if (ui.extra && !ui.extra->GetValue().ToDouble(&ex)) ex = 0.0;
    p.extraDistance = (ex > 0.0) ? ex : 0.0;
    p.extraDistanceInches = ui.extraUnit && ui.extraUnit->GetSelection() == 1;

    auto readVal = [](wxTextCtrl* c) -> double {
        double v = 0.0;
        if (c && c->GetValue().ToDouble(&v) && v > 0.0) return v;
        return 0.0;
    };
    p.tongueWidth = readVal(ui.tongueW);
    p.tongueWidthInches = ui.tongueWUnit && ui.tongueWUnit->GetSelection() == 1;
    p.tongueThickness = readVal(ui.tongueT);
    p.tongueThicknessInches = ui.tongueTUnit && ui.tongueTUnit->GetSelection() == 1;
    // Groove tolerance may legitimately be 0, so don't clamp it away.
    double gt = 0.0;
    if (ui.grooveTol && !ui.grooveTol->GetValue().ToDouble(&gt)) gt = 0.0;
    p.grooveTolerance = (gt > 0.0) ? gt : 0.0;
    p.grooveToleranceInches = ui.grooveTolUnit && ui.grooveTolUnit->GetSelection() == 1;
    return p;
}

MouldCastValues CastingPanel::ReadToolbarValues() const
{
    MouldCastValues v;
    v.base = ReadPartUI(m_baseUI);
    v.walls = ReadPartUI(m_wallsUI);
    return v;
}

// ---------------------------------------------------------------------------
// (Re)seed the perspective from a Generate Mould run.
// ---------------------------------------------------------------------------
void CastingPanel::SetData(const CastPreviewInput& in)
{
    m_mouldKind = in.mouldKind;
    m_sceneIsMesh = in.sceneIsMesh;

    // Retain the Cast Shot Body (mesh + BREP). Empty inputs leave it absent.
    m_castShotMesh = FileImporter::MeshData();
    m_castShotShape = TopoDS_Shape();
    m_hasCastShot = false;
    m_hasCastShotShape = false;
    if (in.castMesh && !in.castMesh->posNorm.empty() && !in.castMesh->indices.empty())
    {
        m_castShotMesh = *in.castMesh;
        m_hasCastShot = true;
    }
    if (in.castShape && !in.castShape->IsNull())
    {
        m_castShotShape = *in.castShape;
        m_hasCastShotShape = true;
    }

    // Cache the combined bounding box of the mould halves (the cast perimeter).
    m_hasHalvesBounds = false;
    if (in.halves)
    {
        bool any = false;
        for (const FileImporter::MeshData& h : *in.halves)
            AccumulateMeshBounds(h, m_halvesMin, m_halvesMax, any);
        m_hasHalvesBounds = any;
    }

    // Snapshot the indexer placement points (see the member / struct comments
    // for why these can't be read from m_canvas at cast-generation time).
    m_indexerPoints.clear();
    if (in.indexerPoints) m_indexerPoints = *in.indexerPoints;

    // The Cast Shot Body is preview part 0; generated cast bodies append above
    // it, and a re-generation truncates the canvas back to this count.
    m_castAnchorCount = m_hasCastShot ? 1 : 0;

    // Drop the previous generation's GPU parts now (clears its own context;
    // a no-op when nothing has been uploaded yet, i.e. the page is unvisited).
    if (m_canvas)
        m_canvas->ClearPreviewHalves();

    // Rebuild the Cast Bodies toggle list for the new part set.
    ClearVisibilityChecks();
    BuildVisibilityChecks(m_hasCastShot);

    // The mesh upload waits until we're actually visible — a canvas on a hidden
    // book page may not have a valid drawable yet.
    m_dataDirty = true;
    FlushIfDirty();
}

// ---------------------------------------------------------------------------
// Reset to the empty (grid-only) state.
// ---------------------------------------------------------------------------
void CastingPanel::ClearData()
{
    m_castShotMesh = FileImporter::MeshData();
    m_castShotShape = TopoDS_Shape();
    m_hasCastShot = false;
    m_hasCastShotShape = false;
    m_hasHalvesBounds = false;
    m_mouldKind = FixtureKind::Library;
    m_sceneIsMesh = false;
    m_castAnchorCount = 0;
    m_indexerPoints.clear();

    if (m_canvas)
    {
        m_canvas->ClearPreviewHalves();
        m_canvas->Refresh(false);
    }

    ClearVisibilityChecks();
    m_dataDirty = false;
}

// ---------------------------------------------------------------------------
// Push staged data into the canvas once we're visible.
// ---------------------------------------------------------------------------
void CastingPanel::FlushIfDirty()
{
    if (!m_dataDirty || !m_canvas) return;
    if (!IsShownOnScreen()) return;   // wait until the book page is visible

    // CallAfter so the page is laid out and the canvas realized (its GL context
    // valid) before AddPreviewHalf issues any GL call.
    CallAfter([this]()
        {
            if (!m_dataDirty) return;
            LoadCastBodies();
            m_dataDirty = false;
        });
}

void CastingPanel::SetGridSettings(const GridSettings& s)
{
    // GLCanvas::SetGridSettings defers the actual push to its next paint, so
    // this is safe even before the casting canvas has realized its GL context.
    if (m_canvas)
        m_canvas->SetGridSettings(s);
}

// ---------------------------------------------------------------------------
// Upload the retained Cast Shot Body as preview part 0 and honour its toggle.
// ---------------------------------------------------------------------------
void CastingPanel::LoadCastBodies()
{
    if (!m_canvas) return;

    // Cast Shot Body — part 0. A warm amber base colour, matching the shot in
    // the Preview perspective, so the intermediate body reads consistently.
    if (m_hasCastShot)
    {
        const glm::vec3 shotColor(0.85f, 0.50f, 0.20f);
        m_canvas->AddPreviewHalf(m_castShotMesh, "Cast Shot Body", shotColor);
    }

    // Apply the initial checkbox state (parts are added visible, so only hide
    // one whose checkbox starts unchecked — none do by default).
    if (m_castShotCheck && !m_castShotCheck->GetValue())
        m_canvas->SetPreviewHalfVisible(0, false);

    m_canvas->Refresh(false);
}

// ---------------------------------------------------------------------------
// Build the Cast Bodies visibility checkboxes.
// ---------------------------------------------------------------------------
void CastingPanel::BuildVisibilityChecks(bool hasCastShot)
{
    if (!m_visPanel) return;
    auto* vSizer = m_visPanel->GetSizer();

    if (m_visEmptyLabel) m_visEmptyLabel->Show(!hasCastShot);

    // Cast Shot Body — preview part index 0, visible by default so the
    // perspective opens looking at the intermediate body.
    m_castShotCheck = nullptr;
    if (hasCastShot)
    {
        auto* cb = new wxCheckBox(m_visPanel, kCastToggleIdBase + 0,
            "Cast Shot Body");
        cb->SetForegroundColour(Style::TextPrimary);
        cb->SetBackgroundColour(Style::CardBg);
        cb->SetValue(true);
        cb->SetToolTip("Show / hide the casting shot body");
        cb->Bind(wxEVT_CHECKBOX,
            [this](wxCommandEvent& evt)
            {
                if (m_canvas) m_canvas->SetPreviewHalfVisible(0, evt.IsChecked());
            });
        vSizer->Add(cb, 0, wxEXPAND | wxALL, 6);
        m_castShotCheck = cb;
    }

    m_visPanel->Layout();
    if (m_visPanel->GetParent()) m_visPanel->GetParent()->Layout();
}

// ---------------------------------------------------------------------------
// Drop the Cast Bodies checkboxes and restore the "No bodies generated" state.
// ---------------------------------------------------------------------------
void CastingPanel::ClearVisibilityChecks()
{
    auto* vSizer = m_visPanel ? m_visPanel->GetSizer() : nullptr;

    // Explicitly drop the Cast Shot Body checkbox — the persistent empty-state
    // label stays.
    if (m_castShotCheck)
    {
        if (vSizer) vSizer->Detach(m_castShotCheck);
        m_castShotCheck->Destroy();
        m_castShotCheck = nullptr;
    }

    // Drop any generated wall / base groups too.
    ClearCastChecks();

    if (m_visEmptyLabel) m_visEmptyLabel->Show(true);
    if (m_visPanel) m_visPanel->Layout();
    if (m_visPanel && m_visPanel->GetParent()) m_visPanel->GetParent()->Layout();
}

// ---------------------------------------------------------------------------
// GenerateCasts — prompt for the wall + base characteristics and build them
// around the Cast Shot Body. Ported from PreviewPanel::OnGenerateMouldCasts;
// the wall / base geometry is unchanged. (The MouldCastDialog prompt is
// replaced by the Cast Tool Settings toolbar in a later step.)
// ---------------------------------------------------------------------------
void CastingPanel::GenerateCasts()
{
    // Cast generation is locked to procedural moulds — Parametric (fixed box)
    // and Dynamic (adaptive box) — whose perimeter is a clean rectangle. On any
    // other mould kind, pressing "Generate Mould Casts" warns and cancels.
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

    // Settings now come from the Cast Tool Settings toolbar (the modal
    // MouldCastDialog it replaced is retired).
    const MouldCastValues v = ReadToolbarValues();

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

    // Progress dialog — the boolean ops (shot y=0 split, per-indexer fuse/cut,
    // wall interlock booleans) can take a noticeable moment on a dense mesh, so
    // give feedback rather than freezing. Phase-granular: one tick per major
    // stage. Update() pumps the event loop; AUTO_HIDE closes it at the end.
    int totalSteps = 1;                                 // finalise
    if (v.base.enabled)
    {
        totalSteps += 1;                                // base solids
        if (!m_indexerPoints.empty()) totalSteps += 1;  // indexer fuse/cut
    }
    if (v.walls.enabled) totalSteps += 1;               // walls
    wxProgressDialog progress(
        "Generating Mould Casts", "Initialising...", totalSteps, this,
        wxPD_APP_MODAL | wxPD_AUTO_HIDE | wxPD_SMOOTH | wxPD_ELAPSED_TIME);
    int pstep = 0;

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
        progress.Update(pstep++, "Building base...");
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
                                                            : TopoDS_Shape();

            TopoDS_Shape upperHalf, lowerHalf;
            if (!shotShapeSrc.IsNull())
            {
                const FileImporter::MeshData& srcMesh =
                    m_castShotMesh;
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
                m_castShotMesh;
            const bool haveShotSrc = m_hasCastShot;

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

        // ---- Indexer registration bosses -----------------------------------
        if (!m_indexerPoints.empty())
            progress.Update(pstep++, "Fusing indexers...");
        // Physical direction (Clayton, Aug 2026): the top/bottom base is a
        // POSITIVE MASTER the actual cast (silicone, etc.) is poured around,
        // so a positive feature here becomes a NEGATIVE impression on the
        // cast and vice versa. Each indexer therefore JOINS a boss (exact
        // Radius, no tolerance) into the BOTTOM base and CUTS a slightly
        // larger pocket (Radius + Extra Tolerance) from the TOP base, so the
        // enlarged cast-side boss presses snugly into the exact-size cast-side
        // pocket.
        //
        // FULL spheres (not clipped hemispheres) are used for both ops: the
        // indexer sits at y=0, the bases meet at y=0, so a hemisphere's flat
        // cut face would land exactly on a base's y=0 face — a coincident-face
        // boolean that silently produces nothing. With a full sphere, one half
        // genuinely overlaps the target's volume (well-defined boolean) while
        // the other half either extends the boss (bottom join) or has nothing
        // to remove (top cut), so the visible result is exactly the intended
        // half.
        //
        // Points come from m_indexerPoints (snapshotted in SetData), NOT
        // m_canvas->GetIndexers(): m_canvas is this panel's own preview canvas,
        // a different GLCanvas from the mould-editing one where indexers are
        // placed, so its list is always empty.
        //
        // Both scene types are handled: BREP scenes fuse/cut the OCC base
        // solids then re-tessellate; mesh scenes do the equivalent
        // MeshBoolean union/difference on the tessellated base meshes. The
        // helper sphere is built through OCC either way (MakeIndexerSphereBool
        // tessellates MakeIndexerSphereSolid), so the geometry is identical
        // across both paths. A failed op leaves that base unchanged, matching
        // the tongue-joint fallback above.
        {
            float indexerRadius = 3.0f, indexerExtraTol = 0.1f;
            if (auto* frame = dynamic_cast<MainFrame*>(wxGetTopLevelParent(this)))
            {
                indexerRadius = frame->GetIndexerRadius();
                indexerExtraTol = frame->GetIndexerExtraTolerance();
            }

            if (!m_indexerPoints.empty() && indexerRadius > 1e-4f)
            {
                const float bossRadius = indexerRadius + indexerExtraTol; // fuse: enlarged
                const float pocketRadius = indexerRadius;                 // cut: exact

                if (haveShapes)
                {
                    // BREP path: fuse/cut the OCC base solids, then re-tessellate.
                    for (const glm::vec3& ipt : m_indexerPoints)
                    {
                        botShape = FuseSolid(botShape,
                            MakeIndexerSphereSolid(ipt, bossRadius));
                        topShape = CutSolid(topShape,
                            MakeIndexerSphereSolid(ipt, pocketRadius));
                    }
                    GLCanvas::TessellateShapeToMesh(topShape, topMeshD, nullptr);
                    GLCanvas::TessellateShapeToMesh(botShape, botMeshD, nullptr);
                }
                else
                {
                    // Mesh path: equivalent MeshBoolean union / difference on
                    // the tessellated base meshes.
                    std::string err;

                    // Bottom base: union all (enlarged) bosses in one pass.
                    {
                        std::vector<MeshBoolean::Mesh> parts;
                        parts.push_back(ToBoolMesh(botMeshD));
                        for (const glm::vec3& ipt : m_indexerPoints)
                        {
                            MeshBoolean::Mesh s = MakeIndexerSphereBool(ipt, bossRadius);
                            if (!s.empty()) parts.push_back(std::move(s));
                        }
                        MeshBoolean::Mesh u;
                        if (parts.size() > 1 &&
                            MeshBoolean::Union(parts, u, err) && !u.empty())
                        {
                            FileImporter::MeshData nm = FlatDisplayMesh(u);
                            if (!nm.posNorm.empty()) botMeshD = std::move(nm);
                        }
                    }

                    // Top base: subtract each (exact) pocket in turn (Difference
                    // is single-minuend/single-subtrahend), mirroring the
                    // tongue-groove mesh path's sequential cuts.
                    {
                        MeshBoolean::Mesh acc = ToBoolMesh(topMeshD);
                        bool changed = false;
                        for (const glm::vec3& ipt : m_indexerPoints)
                        {
                            MeshBoolean::Mesh s = MakeIndexerSphereBool(ipt, pocketRadius);
                            if (s.empty()) continue;
                            MeshBoolean::Mesh res;
                            if (MeshBoolean::Difference(acc, s, res, err) && !res.empty())
                            {
                                acc = std::move(res);
                                changed = true;
                            }
                        }
                        if (changed)
                        {
                            FileImporter::MeshData nm = FlatDisplayMesh(acc);
                            if (!nm.posNorm.empty()) topMeshD = std::move(nm);
                        }
                    }
                }
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
            ? " (shot halves fused)"
            : " (shot not fused)");
        notes << "\n";
    }

    // ---- Walls — four boxes around the perimeter at mould height ----------
    if (v.walls.enabled && v.walls.ThicknessMm() <= 0.0)
        notes << "Walls skipped: thickness must be greater than zero\n";
    else if (v.walls.enabled)
    {
        progress.Update(pstep++, "Building walls...");
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

    progress.Update(totalSteps, "Done.");

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

void CastingPanel::ClearCastChecks()
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

int CastingPanel::AddCastPart(const FileImporter::MeshData& mesh,
    const glm::vec3& color, const wxString& label)
{
    if (!m_canvas) return -1;
    const int partIndex = m_canvas->GetPreviewHalfCount();
    m_canvas->AddPreviewHalf(mesh, label.ToStdString(), color);
    return partIndex;
}

void CastingPanel::AddCastGroup(const wxString& groupLabel,
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

        auto* cb = new wxCheckBox(childPanel, kCastToggleIdBase + part, ch.label);
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
