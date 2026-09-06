#include "PreviewPanel.h"
#include "GLCanvas.h"
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

#include <wx/spinctrl.h>
#include <wx/scrolwin.h>
#include <wx/tglbtn.h>
#include <wx/bmpbndl.h>
#include <wx/file.h>
#include <wx/filename.h>
#include <wx/stdpaths.h>

#include <algorithm>
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
    m_halfShapes.clear();
    m_hasShot = false;
    m_shotVolumeMm3 = 0.0;
    m_castShotMesh = FileImporter::MeshData();
    m_castShotShape = TopoDS_Shape();
    m_hasCastShot = false;
    m_hasCastShotShape = false;
    // Set unconditionally (a mesh scene has no BREP shot to attach below).
    m_sceneIsMesh = shot.sceneIsMesh;

    if (shot.mesh)
    {
        m_shotMesh = *shot.mesh;
        if (shot.shape)   m_shotShape = *shot.shape;
        if (shot.faceIds) m_shotFaceIds = *shot.faceIds;
        if (shot.halves)  m_halfShapes = *shot.halves;
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
    m_hasResult = false;
    m_activeDebugCategory = -1;
    m_showRays = false;
    m_showContacts = false;
    m_undercutRays.clear();
    m_hasSepOverlay = false;
    if (m_draftOverlayCheck) m_draftOverlayCheck->SetValue(false);
    if (m_sepOverlayCheck)   m_sepOverlayCheck->SetValue(false);

    // Drop the previous generation's GPU parts now (clears its own context).
    if (m_canvas)
    {
        m_canvas->ClearShotDebugColoring();
        m_canvas->ShowShotDebugRays(false);
        m_canvas->ShowShotDebugContacts(false);
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
    m_halfShapes.clear();
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

    m_hasResult = false;
    m_activeDebugCategory = -1;
    m_showRays = false;
    m_showContacts = false;
    m_undercutRays.clear();
    m_hasSepOverlay = false;
    if (m_draftOverlayCheck) m_draftOverlayCheck->SetValue(false);
    if (m_sepOverlayCheck)   m_sepOverlayCheck->SetValue(false);

    if (m_canvas)
    {
        m_canvas->ClearShotDebugColoring();
        m_canvas->ShowShotDebugRays(false);
        m_canvas->ShowShotDebugContacts(false);
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
    // Draft thresholds (fail/warn), then Start, then a single "Show mould
    // overlay" checkbox that collapses warnings + fails into one highlighted
    // view (fails red, warnings yellow). Undercuts are NOT assessed here — see
    // the Separation Test.
    makeCard("Draft Angle Checks", [this, &addStart](wxWindow* body, wxBoxSizer* bs)
    {
        const wxString deg = wxString::FromUTF8("\xC2\xB0");
        m_failDraftCtrl = AddFieldRow(body, bs, "Fail below:", "1.0", deg);
        m_warnDraftCtrl = AddFieldRow(body, bs, "Warn below:", "3.0", deg);

        addStart(body, bs, "Draft Angle Checks");

        m_draftOverlayCheck = new wxCheckBox(body, wxID_ANY, "Show mould overlay");
        m_draftOverlayCheck->SetForegroundColour(Style::TextPrimary);
        m_draftOverlayCheck->SetBackgroundColour(Style::CardBg);
        m_draftOverlayCheck->SetToolTip(
            "Highlight fail faces (red) and warning faces (yellow) on the shot");
        m_draftOverlayCheck->Bind(wxEVT_CHECKBOX,
            [this](wxCommandEvent&) { UpdateDraftOverlay(); });
        bs->Add(m_draftOverlayCheck, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 10);
    });

    // ---- Separation Test ----------------------------------------------------
    // Lift distance, then Start, then a "Show mould overlay" checkbox that
    // shows the interference region from the last run. Covers trapping /
    // undercuts via collision.
    makeCard("Separation Test", [this, &addStart](wxWindow* body, wxBoxSizer* bs)
    {
        m_liftCtrl = AddFieldRow(body, bs, "Lift:", "1.0", "mm");
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
    // Mesh toolpath: the design checks are BREP-only (they analyse the shot's
    // faces and half solids, which a mesh scene doesn't produce). Refuse every
    // simulation with a clear message rather than the generic "no shot model"
    // one. Mesh-native checks are a separate, later step.
    if (m_sceneIsMesh)
    {
        wxMessageBox(
            "This simulation can't be run on a mesh-type scene.\n\n"
            "The design checks require a BREP (STEP) mould. Mesh-scene "
            "simulations aren't available yet.",
            simName, wxOK | wxICON_INFORMATION, this);
        return;
    }

    if (simName == "Draft Angle Checks")
    {
        RunDemoldabilityCheck();
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
// Compute (and cache) the demoldability result. No UI.
// ---------------------------------------------------------------------------
bool PreviewPanel::ComputeDemoldability()
{
    if (!m_hasShot || m_shotShape.IsNull())
    {
        m_hasResult = false;
        return false;
    }

    DesignChecks::Params params;
    params.checkUndercuts = false;   // draft-angle assessment only — undercuts
                                     // are covered by the demoulding test
    params.failDraftDeg = (float)std::clamp(ParseField(m_failDraftCtrl, 1.0), 0.0, 45.0);
    params.warnDraftDeg = (float)std::clamp(ParseField(m_warnDraftCtrl, 3.0), 0.0, 45.0);
    // Keep thresholds ordered: warn must be >= fail.
    if (params.warnDraftDeg < params.failDraftDeg)
        params.warnDraftDeg = params.failDraftDeg;

    m_lastResult = DesignChecks::CheckDemoldability(m_shotShape, params, &m_undercutRays);
    m_hasResult = true;
    return true;
}

// ---------------------------------------------------------------------------
// Demoldability: analyse the whole shot against the draft thresholds and the
// straight ±draw-axis pull, then report the verdict.
// ---------------------------------------------------------------------------
void PreviewPanel::RunDemoldabilityCheck()
{
    if (!ComputeDemoldability())
    {
        wxMessageBox("There is no shot model to analyse.",
            "Draft Angle Checks", wxOK | wxICON_INFORMATION, this);
        return;
    }

    const DesignChecks::DemoldabilityResult& res = m_lastResult;

    // ---- Verdict text + colour -------------------------------------------
    wxString verdict;
    wxColour verdictColour;
    long icon = wxICON_INFORMATION;
    switch (res.overall)
    {
    case DesignChecks::Severity::Pass:
        verdict = "PASS"; verdictColour = wxColour(0x26, 0xAB, 0x36);
        icon = wxICON_INFORMATION; break;
    case DesignChecks::Severity::Warning:
        verdict = "WARNING"; verdictColour = wxColour(0xE0, 0x9B, 0x20);
        icon = wxICON_WARNING; break;
    case DesignChecks::Severity::Fail:
        verdict = "FAIL"; verdictColour = wxColour(0xD0, 0x46, 0x46);
        icon = wxICON_ERROR; break;
    }

    if (m_draftStatus)
    {
        m_draftStatus->SetLabel(verdict);
        m_draftStatus->SetForegroundColour(verdictColour);
        if (m_infoPanel) m_infoPanel->Layout();
    }

    // If the overlay is showing, refresh it against this run's faces/thresholds.
    UpdateDraftOverlay();

    // ---- Detailed dialog --------------------------------------------------
    const wxString deg = wxString::FromUTF8("\xC2\xB0");
    wxString msg;
    msg << "Draft Angle Checks: " << verdict << "\n\n";
    if (res.issues.empty())
    {
        msg << "No draft or undercut problems found.\n\n";
    }
    else
    {
        for (const DesignChecks::Issue& is : res.issues)
            msg << wxString::FromUTF8(is.description.c_str()) << "\n";
        msg << "\n";
    }
    msg << "Minimum draft: "
        << wxString::Format("%.2f", res.minDraftDeg) << deg << "\n";
    msg << "Undercut faces: " << res.undercutCount << "\n";
    msg << "Below fail: " << res.failDraftCount << "\n";
    msg << "Below warn: " << res.warnDraftCount << "\n";
    msg << "Faces analysed: " << res.totalFaces;

    wxMessageBox(msg, "Draft Angle Checks", wxOK | icon, this);
}

// ---------------------------------------------------------------------------
// Separation/collision demoldability — lift each half off the shot and test
// for interference. Reports the verdict and shows the overlap region in red.
// ---------------------------------------------------------------------------
void PreviewPanel::RunSeparationCheck()
{
    if (!m_hasShot || m_shotShape.IsNull() || m_halfShapes.empty())
    {
        wxMessageBox("There is no shot model and mould halves to analyse.",
            "Separation Test", wxOK | wxICON_INFORMATION, this);
        return;
    }

    DesignChecks::SeparationParams params;
    params.liftMm = (float)std::clamp(ParseField(m_liftCtrl, 1.0), 0.05, 25.0);

    TopoDS_Shape overlap;
    const DesignChecks::SeparationResult res =
        DesignChecks::CheckSeparation(m_shotShape, m_halfShapes, params, &overlap);

    // Upload the interference region (red). Its visibility follows this card's
    // "Show mould overlay" checkbox; the draft overlay (if shown) is left
    // untouched so the two views stay independent.
    m_hasSepOverlay = !overlap.IsNull();
    if (m_canvas)
        m_canvas->SetShotDebugSolid(overlap, glm::vec3(0.90f, 0.15f, 0.15f));
    UpdateSeparationOverlay();

    // ---- Verdict + status -------------------------------------------------
    wxString verdict;
    wxColour verdictColour;
    long iconFlag = wxICON_INFORMATION;
    switch (res.overall)
    {
    case DesignChecks::Severity::Pass:
        verdict = "PASS"; verdictColour = wxColour(0x26, 0xAB, 0x36);
        iconFlag = wxICON_INFORMATION; break;
    case DesignChecks::Severity::Warning:
        verdict = "INCONCLUSIVE"; verdictColour = wxColour(0xE0, 0x9B, 0x20);
        iconFlag = wxICON_WARNING; break;
    case DesignChecks::Severity::Fail:
        verdict = "FAIL"; verdictColour = wxColour(0xD0, 0x46, 0x46);
        iconFlag = wxICON_ERROR; break;
    }

    if (m_demouldStatus)
    {
        m_demouldStatus->SetLabel(verdict);
        m_demouldStatus->SetForegroundColour(verdictColour);
        if (m_infoPanel) m_infoPanel->Layout();
    }

    // ---- Dialog -----------------------------------------------------------
    wxString msg;
    msg << "Separation test: " << verdict << "\n\n";
    msg << "Lift: " << wxString::Format("%.2f", params.liftMm) << " mm\n";
    msg << "Halves tested: " << res.halvesTested << "\n";
    msg << "Halves collided: " << res.halvesCollided << "\n";
    if (res.halvesFailedToEval > 0)
        msg << "Halves not evaluable: " << res.halvesFailedToEval
            << " (boolean failed)\n";
    msg << "Total overlap: "
        << wxString::Format("%.3f", res.totalOverlapVolume)
        << wxString::FromUTF8(" mm\xC2\xB3") << "\n\n";

    for (size_t i = 0; i < res.perHalfStatus.size(); ++i)
    {
        const char* s = res.perHalfStatus[i] == 1 ? "COLLISION"
            : res.perHalfStatus[i] == 2 ? "not evaluable" : "clear";
        msg << "  Half " << (int)(i + 1) << ": " << s
            << wxString::Format("  (overlap %.3f mm", res.perHalfVolume[i])
            << wxString::FromUTF8("\xC2\xB3)") << "\n";
    }

    if (res.halvesCollided > 0)
        msg << "\nEnable \"Show mould overlay\" to see the interference region "
               "(red); hide the Shot toggle to view it clearly.";

    wxMessageBox(msg, "Separation Test", wxOK | iconFlag, this);
}

// ---------------------------------------------------------------------------
// Push a debug overlay: partition display triangles by their source face's
// group and hand the groups to the canvas.
// ---------------------------------------------------------------------------
void PreviewPanel::ApplyFaceGroups(const std::unordered_map<int, int>& groupOfFace,
    const std::vector<glm::vec3>& colors, int defaultGroup,
    const std::vector<bool>& emissive)
{
    if (!m_canvas || colors.empty()) return;

    std::vector<GLCanvas::ShotDebugGroup> groups(colors.size());
    for (size_t g = 0; g < colors.size(); ++g)
    {
        groups[g].color = colors[g];
        groups[g].emissive = (g < emissive.size()) ? emissive[g] : false;
    }

    const std::vector<uint32_t>& I = m_shotMesh.indices;
    const size_t numTris = I.size() / 3;
    for (size_t t = 0; t < numTris; ++t)
    {
        const int fid = (t < m_shotFaceIds.size()) ? m_shotFaceIds[t] : 0;
        auto it = groupOfFace.find(fid);
        int g = (it != groupOfFace.end()) ? it->second : defaultGroup;
        if (g < 0 || g >= (int)colors.size()) g = defaultGroup;
        groups[(size_t)g].indices.push_back(I[t * 3 + 0]);
        groups[(size_t)g].indices.push_back(I[t * 3 + 1]);
        groups[(size_t)g].indices.push_back(I[t * 3 + 2]);
    }

    m_canvas->SetShotDebugGroups(m_shotHalfIndex, groups);
}

// ---------------------------------------------------------------------------
// Draft Angle Checks "Show mould overlay" — collapse warnings + fails into one
// highlighted view (fail red, warn yellow), or clear it. Recomputes against
// the current thresholds so the overlay always matches the fields.
// ---------------------------------------------------------------------------
void PreviewPanel::UpdateDraftOverlay()
{
    if (!m_canvas) return;

    const bool show = m_draftOverlayCheck && m_draftOverlayCheck->GetValue();
    if (!show)
    {
        m_canvas->ClearShotDebugColoring();
        m_activeDebugCategory = -1;
        return;
    }

    if (m_shotHalfIndex < 0 || !ComputeDemoldability())
    {
        // Nothing to analyse yet — leave the box checked, show nothing.
        m_canvas->ClearShotDebugColoring();
        return;
    }

    // Group 0 = fails (red), 1 = warnings (yellow), 2 = everything else
    // (default, neutral shot colour). The fail/warn face lists are disjoint
    // (DesignChecks classifies each face into at most one bucket), so no
    // precedence handling is needed.
    std::unordered_map<int, int> groupOfFace;
    for (int f : m_lastResult.failDraftFaces) if (f > 0) groupOfFace[f] = 0;
    for (int f : m_lastResult.warnDraftFaces) if (f > 0) groupOfFace[f] = 1;

    const std::vector<glm::vec3> colors = {
        glm::vec3(0.92f, 0.16f, 0.16f),   // red    — fail
        glm::vec3(0.95f, 0.80f, 0.10f),   // yellow — warn
        glm::vec3(0.80f, 0.80f, 0.85f),   // rest   — neutral (stays shaded)
    };
    // Highlight the flagged faces (groups 0 + 1) at full intensity; leave the
    // rest shaded so the shot keeps its 3D form.
    const std::vector<bool> emissive = { true, true, false };

    ApplyFaceGroups(groupOfFace, colors, /*defaultGroup*/ 2, emissive);
    m_activeDebugCategory = -1;  // this combined view isn't one of the categories
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
// Debug: recolour the shot for one category (red) vs the rest (green).
// ---------------------------------------------------------------------------
void PreviewPanel::ShowDebugCategory(int category)
{
    if (!m_canvas || m_shotHalfIndex < 0)
    {
        wxMessageBox("There is no shot model to visualise.",
            "Debug", wxOK | wxICON_INFORMATION, this);
        return;
    }

    // Pressing the active category again returns the shot to normal.
    if (m_activeDebugCategory == category)
    {
        m_canvas->ClearShotDebugColoring();
        m_activeDebugCategory = -1;
        return;
    }

    // Recompute against the current thresholds so the overlay always matches
    // the fields (the fail/warn categories depend on them).
    if (!ComputeDemoldability())
    {
        wxMessageBox("There is no shot model to analyse.",
            "Debug", wxOK | wxICON_INFORMATION, this);
        return;
    }

    const std::vector<int>* flaggedFaces = nullptr;
    switch (category)
    {
    case 0: flaggedFaces = &m_lastResult.undercutFaces;  break;
    case 1: flaggedFaces = &m_lastResult.warnDraftFaces; break;
    case 2: flaggedFaces = &m_lastResult.failDraftFaces; break;
    default: return;
    }

    // Group 0 = flagged (red), group 1 = everything else (green, default).
    std::unordered_map<int, int> groupOfFace;
    for (int f : *flaggedFaces)
        if (f > 0) groupOfFace[f] = 0;

    const std::vector<glm::vec3> colors = {
        glm::vec3(0.85f, 0.16f, 0.16f),   // red   — flagged
        glm::vec3(0.18f, 0.70f, 0.22f),   // green — rest
    };
    ApplyFaceGroups(groupOfFace, colors, /*defaultGroup*/ 1);
    m_activeDebugCategory = category;
}

// ---------------------------------------------------------------------------
// Debug: recolour the shot by draft sign (up / down / vertical / mixed) using
// the analytic normals the checks use — to expose any inverted normals.
// ---------------------------------------------------------------------------
void PreviewPanel::ShowDraftSign()
{
    if (!m_canvas || m_shotHalfIndex < 0 || m_shotShape.IsNull())
    {
        wxMessageBox("There is no shot model to visualise.",
            "Debug", wxOK | wxICON_INFORMATION, this);
        return;
    }

    const int kDraftSignCategory = 3;
    if (m_activeDebugCategory == kDraftSignCategory)
    {
        m_canvas->ClearShotDebugColoring();
        m_activeDebugCategory = -1;
        return;
    }

    DesignChecks::Params params;  // draw axis + vertical tolerance defaults
    const DesignChecks::DraftSignResult sign =
        DesignChecks::ClassifyDraftSign(m_shotShape, params);

    // Group 0 up, 1 down, 2 vertical (default), 3 mixed.
    std::unordered_map<int, int> groupOfFace;
    for (int f : sign.upFaces)       groupOfFace[f] = 0;
    for (int f : sign.downFaces)     groupOfFace[f] = 1;
    for (int f : sign.verticalFaces) groupOfFace[f] = 2;
    for (int f : sign.mixedFaces)    groupOfFace[f] = 3;

    const std::vector<glm::vec3> colors = {
        glm::vec3(0.20f, 0.45f, 0.95f),   // up       — blue
        glm::vec3(0.95f, 0.55f, 0.15f),   // down     — orange
        glm::vec3(0.55f, 0.55f, 0.58f),   // vertical — grey
        glm::vec3(0.65f, 0.30f, 0.80f),   // mixed    — purple
    };
    ApplyFaceGroups(groupOfFace, colors, /*defaultGroup*/ 2);
    m_activeDebugCategory = kDraftSignCategory;
}

// ---------------------------------------------------------------------------
// Build and upload the undercut-ray debug geometry from the last result.
// ---------------------------------------------------------------------------
void PreviewPanel::RefreshRayGeometry()
{
    if (!m_canvas) return;

    // Recompute so the rays match the current shot (undercut rays don't depend
    // on the draft thresholds, but this keeps everything consistent).
    if (!ComputeDemoldability()) return;

    std::vector<glm::vec3> rayVerts;     // GL_LINES pairs
    std::vector<glm::vec3> contactVerts; // GL_POINTS
    rayVerts.reserve(m_undercutRays.size() * 2);
    contactVerts.reserve(m_undercutRays.size());

    const float kRayLen = 10.0f;  // first 10 mm of each ray path
    for (const DesignChecks::UndercutRay& r : m_undercutRays)
    {
        rayVerts.push_back(r.origin);
        rayVerts.push_back(r.origin + r.dir * kRayLen);
        contactVerts.push_back(r.hit);
    }

    m_canvas->SetShotDebugRays(rayVerts, contactVerts);
}

// ---------------------------------------------------------------------------
// Toggle the ray-segment overlay (first 10 mm of each failing ray).
// ---------------------------------------------------------------------------
void PreviewPanel::ToggleDebugRays()
{
    if (!m_canvas || m_shotShape.IsNull())
    {
        wxMessageBox("There is no shot model to analyse.",
            "Debug", wxOK | wxICON_INFORMATION, this);
        return;
    }
    m_showRays = !m_showRays;
    if (m_showRays) RefreshRayGeometry();
    m_canvas->ShowShotDebugRays(m_showRays);
}

// ---------------------------------------------------------------------------
// Toggle the contact-point overlay (where failing rays struck the shot).
// ---------------------------------------------------------------------------
void PreviewPanel::ToggleDebugContacts()
{
    if (!m_canvas || m_shotShape.IsNull())
    {
        wxMessageBox("There is no shot model to analyse.",
            "Debug", wxOK | wxICON_INFORMATION, this);
        return;
    }
    m_showContacts = !m_showContacts;
    if (m_showContacts) RefreshRayGeometry();
    m_canvas->ShowShotDebugContacts(m_showContacts);
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
