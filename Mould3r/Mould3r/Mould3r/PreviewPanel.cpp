#include "PreviewPanel.h"
#include "GLCanvas.h"
#include "style.h"
#include "DesignChecks.h"
#include "RoundedButton.h"
#include "MouldCastDialog.h"
#include "MeshBoolean.h"   // split the shot at y=0 and fuse a half into each base

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
    m_hasCastShot = false;
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
    m_hasCastShot = false;
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

    // ---- Generate Mould Casts --------------------------------------------
    // Fixed action at the bottom of the column (always visible, below the
    // scrollable simulations). Opens the wall/base cast dialog. Styled like
    // the Prepare-side "Generate Mould" button (green RoundedButton).
    auto* castDivider = new wxPanel(column, wxID_ANY,
        wxDefaultPosition, wxSize(-1, 1));
    castDivider->SetBackgroundColour(Style::Divider);
    colSizer->Add(castDivider, 0, wxEXPAND | wxLEFT | wxRIGHT, 10);

    auto* castBtn = new RoundedButton(column, wxID_ANY, "Generate Mould Casts",
        wxDefaultPosition, wxSize(-1, 34), wxBORDER_NONE);
    castBtn->SetBackgroundColour(Style::BtnGenerate);
    castBtn->SetForegroundColour(*wxWHITE);
    castBtn->SetFont(wxFont(9, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
        wxFONTWEIGHT_SEMIBOLD, false, "Segoe UI"));
    castBtn->SetToolTip("Generate the walls and base that hold the sand / silicone");
    castBtn->Bind(wxEVT_BUTTON,
        [this](wxCommandEvent&) { OnGenerateMouldCasts(); });
    colSizer->Add(castBtn, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP | wxBOTTOM, 12);

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

    int generated = 0;
    wxString notes;

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

        // The cast shot body (shot + vents + scaled inserts + ejectors) drives
        // the fused half; fall back to the plain shot when no cast body exists.
        const FileImporter::MeshData& shotSrc =
            m_hasCastShot ? m_castShotMesh : m_shotMesh;
        const bool haveShotSrc = m_hasCastShot || m_hasShot;

        // Split the (perimeter-clipped) cast shot at y = 0 and fuse a half into
        // each base: the y > 0 section joins the top base, the y < 0 section the
        // bottom base. Any boolean failure degrades gracefully to a plain box.
        bool shotUsed = false;
        FileImporter::MeshData topMeshD, botMeshD;

        MeshBoolean::Mesh shotBool;
        if (haveShotSrc) shotBool = ToBoolMesh(shotSrc);

        if (!shotBool.empty())
        {
            // Requirement: trim anything outside the mould perimeter BEFORE the
            // split/join. Clip to the (un-flanged) mould footprint.
            shotBool = ClipMeshToPerimeterXZ(shotBool,
                mn.x, mx.x, mn.z, mx.z);
        }

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

        // Fallback (no usable shot): plain base boxes.
        if (!shotUsed)
        {
            topMeshD = MakeBoxMesh(topBoxMin, topBoxMax);
            botMeshD = MakeBoxMesh(botBoxMin, botBoxMax);
        }

        const glm::vec3 baseColor(0.40f, 0.55f, 0.68f);  // steel blue
        AddCastBody(topMeshD, "Top Base", baseColor,
            "Show / hide the top base (extrudes -Y; shot y>0 half fused in)");
        AddCastBody(botMeshD, "Bottom Base", baseColor,
            "Show / hide the bottom base (extrudes +Y; shot y<0 half fused in)");

        generated += 2;
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

        const glm::vec3 wallColor(0.62f, 0.55f, 0.42f);   // warm tan
        // Overhang assignment forms a consistent (CCW-from-top) pinwheel:
        //   -Z wall overhangs -X,  +Z wall overhangs +X,
        //   +X wall overhangs -Z,  -X wall overhangs +Z.
        AddCastBody(MakeBoxMesh(glm::vec3(X0 - e, Y0, Z0 - w),
                                glm::vec3(X1,     Y1, Z0)),
            "Wall -Z", wallColor, "Show / hide the -Z wall");
        AddCastBody(MakeBoxMesh(glm::vec3(X0,     Y0, Z1),
                                glm::vec3(X1 + e, Y1, Z1 + w)),
            "Wall +Z", wallColor, "Show / hide the +Z wall");
        AddCastBody(MakeBoxMesh(glm::vec3(X1,     Y0, Z0 - e),
                                glm::vec3(X1 + w, Y1, Z1)),
            "Wall +X", wallColor, "Show / hide the +X wall");
        AddCastBody(MakeBoxMesh(glm::vec3(X0 - w, Y0, Z0),
                                glm::vec3(X0,     Y1, Z1 + e)),
            "Wall -X", wallColor, "Show / hide the -X wall");

        generated += 4;
        notes << wxString::Format("Walls: %s, %g mm thick",
            wxString::FromUTF8(v.walls.type.c_str()), (double)w);
        if (clover && ext > 0.0f)
            notes << wxString::Format(", +%g mm extra overhang", (double)ext);
        notes << " (4 bodies)\n";
    }

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
// Drop just the cast-body checkboxes (bases / walls) from the visibility card,
// leaving the half / shot / insert toggles in place. Paired with a canvas
// TruncatePreviewHalves(m_castAnchorCount) so a cast re-generation starts clean.
// ---------------------------------------------------------------------------
void PreviewPanel::ClearCastChecks()
{
    auto* vSizer = m_visPanel ? m_visPanel->GetSizer() : nullptr;
    for (wxCheckBox* cb : m_castChecks)
    {
        if (!cb) continue;
        if (vSizer) vSizer->Detach(cb);
        cb->Destroy();
    }
    m_castChecks.clear();
}

// ---------------------------------------------------------------------------
// Append one cast body to the preview: upload its mesh to the canvas and add a
// matching show/hide checkbox. Returns the preview-part index it loaded at.
// ---------------------------------------------------------------------------
int PreviewPanel::AddCastBody(const FileImporter::MeshData& mesh,
    const wxString& label, const glm::vec3& color, const wxString& tip)
{
    if (!m_canvas) return -1;

    const int partIndex = m_canvas->GetPreviewHalfCount();
    m_canvas->AddPreviewHalf(mesh, label.ToStdString(), color);

    if (m_visPanel)
    {
        auto* cb = new wxCheckBox(m_visPanel, kHalfToggleIdBase + partIndex, label);
        cb->SetForegroundColour(Style::TextPrimary);
        cb->SetBackgroundColour(Style::CardBg);
        cb->SetValue(true);
        cb->SetToolTip(tip);
        cb->Bind(wxEVT_CHECKBOX,
            [this, partIndex](wxCommandEvent& evt)
            {
                if (m_canvas) m_canvas->SetPreviewHalfVisible(partIndex, evt.IsChecked());
            });
        if (m_visEmptyLabel) m_visEmptyLabel->Show(false);
        m_visPanel->GetSizer()->Add(cb, 0, wxEXPAND | wxALL, 6);
        m_castChecks.push_back(cb);
    }
    return partIndex;
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
