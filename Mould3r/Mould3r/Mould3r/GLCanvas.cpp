#define _CRT_SECURE_NO_WARNINGS

#include <glad/glad.h>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <unordered_map>
#include <set>

#include "GLCanvas.h"
#include <wx/dcclient.h>
#include <wx/log.h>
#include <wx/msgdlg.h>
#include <wx/progdlg.h>

#include <opencascade/STEPControl_Reader.hxx>
#include <opencascade/STEPControl_Writer.hxx>
#include <opencascade/BRepBuilderAPI_Transform.hxx>
#include <opencascade/gp_Trsf.hxx>
#include <opencascade/gp_Mat.hxx>
#include <opencascade/gp_XYZ.hxx>
#include <opencascade/BRepAlgoAPI_Cut.hxx>
#include <opencascade/BRepAlgoAPI_Fuse.hxx>
#include <opencascade/BRepGProp.hxx>
#include <opencascade/GProp_GProps.hxx>
#include <opencascade/BRepMesh_IncrementalMesh.hxx>
#include <opencascade/TopExp.hxx>
#include <opencascade/TopTools_IndexedMapOfShape.hxx>
#include <opencascade/BRepBuilderAPI_MakeEdge.hxx>
#include <opencascade/BRepBuilderAPI_MakeWire.hxx>
#include <opencascade/BRepBuilderAPI_MakeFace.hxx>
#include <opencascade/BRepPrimAPI_MakePrism.hxx>
#include <opencascade/BRepPrimAPI_MakeCylinder.hxx>
#include <opencascade/BRepPrimAPI_MakeCone.hxx>
#include <opencascade/gp_Ax2.hxx>
#include <opencascade/gp_Dir.hxx>
#include <opencascade/gp_Vec.hxx>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <string>
#include <vector>

#include <glm/gtc/constants.hpp>

#include "camera.h"
#include "FileImporter.h"
#include "GLLoader.h"
#include "GridRenderer.h"
#include "shaders.h"
#include "MeshUtils.h"
#include "MeshOps.h"
#include "MouldFeature.h"

// Radius of the green sphere drawn at each vent placement point (world units)
static constexpr float kVentMarkerRadius = 1.5f;

// How close (world units) the cursor's parting-plane hit must be to a parting
// segment before it snaps — gives a generous click target on the parting line.
static constexpr float kVentSnapRadius = 8.0f;

// Pixel radius for ejector snap pick. Screen-space rather than world-space:
// path candidates (sprue / runner / gate segments) are evaluated by their
// projected screen distance to the cursor, so the snap feel is consistent
// at every zoom level. See RayCastEjectorSnap for the full algorithm.
static constexpr float kEjectorSnapRadiusPx = 12.0f;

// ---------------------------------------------------------------------------
// Shader helpers
// ---------------------------------------------------------------------------
static GLuint Compile(GLenum type, const char* src)
{
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        GLint len = 0;
        glGetShaderiv(s, GL_INFO_LOG_LENGTH, &len);
        std::string log(len, '\0');
        glGetShaderInfoLog(s, len, &len, log.data());
        wxLogError("Shader compile error:\n%s", log);
    }
    return s;
}

static GLuint Link(GLuint vs, GLuint fs)
{
    GLuint p = glCreateProgram();
    glAttachShader(p, vs);
    glAttachShader(p, fs);
    glLinkProgram(p);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        GLint len = 0;
        glGetProgramiv(p, GL_INFO_LOG_LENGTH, &len);
        std::string log(len, '\0');
        glGetProgramInfoLog(p, len, &len, log.data());
        wxLogError("Program link error:\n%s", log);
    }
    glDeleteShader(vs);
    glDeleteShader(fs);
    return p;
}

// glArgs[] (pixel-format / context attributes) now lives in GLLoader.h —
// shared with FixtureCanvas so adding a third viewport doesn't fork the
// pixel-format request.

// ---------------------------------------------------------------------------
// Constructor / Destructor
// ---------------------------------------------------------------------------
GLCanvas::GLCanvas(wxWindow* parent)
    : wxGLCanvas(parent, wxID_ANY, GLLoader::glArgs,
        wxDefaultPosition, wxDefaultSize,
        wxFULL_REPAINT_ON_RESIZE, "GLCanvas")
{
#if WX_CHECK_VERSION(3, 1, 0)
    wxGLContextAttrs ctxAttrs;
#if defined(__APPLE__)
    ctxAttrs.PlatformDefaults().CoreProfile().OGLVersion(3, 2).EndList();
#else
    ctxAttrs.PlatformDefaults().CoreProfile().OGLVersion(3, 3).EndList();
#endif
    m_context = new wxGLContext(this, nullptr, &ctxAttrs);
    if (!m_context->IsOK()) {
        delete m_context;
        m_context = new wxGLContext(this);
        wxLogWarning("Core profile context request failed; using default GL context.");
    }
#else
    m_context = new wxGLContext(this);
#endif

    m_camera.SetOrbitSensitivity(0.15f);
    m_camera.SetPanSensitivity(0.0025f);
    m_camera.SetDollySensitivity(0.08f);

    Bind(wxEVT_PAINT, &GLCanvas::OnPaint, this);
    Bind(wxEVT_SIZE, &GLCanvas::OnResize, this);
    Bind(wxEVT_LEFT_DOWN, &GLCanvas::OnMouse, this);
    Bind(wxEVT_LEFT_UP, &GLCanvas::OnMouse, this);
    Bind(wxEVT_MIDDLE_DOWN, &GLCanvas::OnMouse, this);
    Bind(wxEVT_MIDDLE_UP, &GLCanvas::OnMouse, this);
    Bind(wxEVT_RIGHT_DOWN, &GLCanvas::OnMouse, this);
    Bind(wxEVT_RIGHT_UP, &GLCanvas::OnMouse, this);
    Bind(wxEVT_MOTION, &GLCanvas::OnMouse, this);
    Bind(wxEVT_MOUSEWHEEL, &GLCanvas::OnMouseWheel, this);
    Bind(wxEVT_KEY_DOWN, &GLCanvas::OnKeyDown, this);

    SetFocus();
}

GLCanvas::~GLCanvas()
{
    if (m_context) {
        SetCurrent(*m_context);
        DestroyGL();
    }
    delete m_context;
}

// ---------------------------------------------------------------------------
// SetTransformMode
// ---------------------------------------------------------------------------
void GLCanvas::SetTransformMode(TransformMode mode)
{
    // Clear edit selection when leaving an edit mode
    if (m_transformMode != mode)
    {
        m_editFeatureIndex = -1;
        m_editNeedsUpdate = false;
    }

    // Clear AlignFace hover state and highlight VBO when leaving AlignFace.
    // The VBO itself is kept (just zeroed via vertex count) so we don't
    // re-create GL objects on every mode switch.
    if (m_transformMode == TransformMode::AlignFace && mode != TransformMode::AlignFace)
    {
        m_alignHoverObject = -1;
        m_alignSeedTri = -1;
        m_alignFaceTris.clear();
        m_alignHighlightVertexCount = 0;
    }

    // Clear AlignMidplane state when leaving the mode. Hover state is shared
    // with AlignFace and cleared above; this drops the locked-face data.
    if (m_transformMode == TransformMode::AlignMidplane && mode != TransformMode::AlignMidplane)
    {
        m_alignHoverObject = -1;
        m_alignSeedTri = -1;
        m_alignFaceTris.clear();
        m_alignHighlightVertexCount = 0;

        m_midplaneFaceLocked = false;
        m_midplaneFaceObject = -1;
        m_midplaneFaceTris.clear();
        m_midplaneLockedVertexCount = 0;
    }

    m_transformMode = mode;
    switch (mode)
    {
    case TransformMode::Select:
        SetCursor(wxCursor(wxCURSOR_ARROW));    break;
    case TransformMode::Translate:
        SetCursor(wxCursor(wxCURSOR_SIZING));   break;
    case TransformMode::Rotate:
        SetCursor(wxCursor(wxCURSOR_CROSS));    break;
    case TransformMode::Scale:
        SetCursor(wxCursor(wxCURSOR_SIZENS));   break;
    case TransformMode::PlaceVent:
        SetCursor(wxCursor(wxCURSOR_CROSS));    break;
    case TransformMode::PlaceRunner:
        SetCursor(wxCursor(wxCURSOR_CROSS));    break;
    case TransformMode::PlaceGate:
        SetCursor(wxCursor(wxCURSOR_CROSS));    break;
    case TransformMode::PlaceEjector:
        SetCursor(wxCursor(wxCURSOR_CROSS));    break;
    case TransformMode::RemoveVent:
    case TransformMode::RemoveRunner:
    case TransformMode::RemoveGate:
    case TransformMode::RemoveSprue:
    case TransformMode::RemoveEjector:
        SetCursor(wxCursor(wxCURSOR_HAND));     break;
    case TransformMode::EditVent:
    case TransformMode::EditRunner:
    case TransformMode::EditGate:
    case TransformMode::EditEjector:
        SetCursor(wxCursor(wxCURSOR_HAND));     break;
    case TransformMode::SelectInjectionPoint:
        SetCursor(wxCursor(wxCURSOR_HAND));     break;
    case TransformMode::AlignFace:
        SetCursor(wxCursor(wxCURSOR_HAND));     break;
    case TransformMode::AlignMidplane:
        SetCursor(wxCursor(wxCURSOR_HAND));     break;
    }

    Refresh(false);
}

// ---------------------------------------------------------------------------
// Transform methods — operate on selected object
// ---------------------------------------------------------------------------
void GLCanvas::ApplyRotation(float xDeg, float yDeg, float zDeg)
{
    if (!HasSelection()) return;
    for (int idx : m_selectedIndices)
    {
        if (idx < 0 || idx >= (int)m_objects.size()) continue;
        m_objects[idx].pitchDeg += xDeg;
        m_objects[idx].yawDeg += yDeg;
        m_objects[idx].rollDeg += zDeg;
    }
    // Parented vents/gates ride along with their parent; unparented features
    // are independent of object transforms and are left alone.
    ReanchorFeaturesForObjects(m_selectedIndices);
    Refresh(false);
    NotifySceneMutated();
}

void GLCanvas::ApplyTranslation(float x, float y, float z)
{
    if (!HasSelection()) return;
    const glm::vec3 d(x, y, z);
    for (int idx : m_selectedIndices)
    {
        if (idx < 0 || idx >= (int)m_objects.size()) continue;
        m_objects[idx].pos += d;
    }
    ReanchorFeaturesForObjects(m_selectedIndices);
    Refresh(false);
    NotifySceneMutated();
}

void GLCanvas::ApplyScale(float factor)
{
    if (!HasSelection()) return;
    for (int idx : m_selectedIndices)
    {
        if (idx < 0 || idx >= (int)m_objects.size()) continue;
        m_objects[idx].scale = std::max(0.001f, m_objects[idx].scale * factor);
    }
    ReanchorFeaturesForObjects(m_selectedIndices);
    Refresh(false);
    NotifySceneMutated();
}

void GLCanvas::CenterSelectedObject()
{
    if (!HasSelection()) return;

    // Translate the whole selection so the centroid of its members'
    // positions lands at the world origin. Preserves relative arrangement
    // (single-selection degenerates to "set pos = (0,0,0)" as before).
    glm::vec3 centroid(0.0f);
    int n = 0;
    for (int idx : m_selectedIndices)
    {
        if (idx < 0 || idx >= (int)m_objects.size()) continue;
        centroid += m_objects[idx].pos;
        ++n;
    }
    if (n == 0) return;
    centroid /= static_cast<float>(n);

    for (int idx : m_selectedIndices)
    {
        if (idx < 0 || idx >= (int)m_objects.size()) continue;
        m_objects[idx].pos -= centroid;
    }
    ReanchorFeaturesForObjects(m_selectedIndices);
    Refresh(false);
    NotifySceneMutated();
}

// ---------------------------------------------------------------------------
// Circular pattern around the world origin in the XZ plane.
//
// The original keeps its existing pose; (count - 1) clones are placed at
// equally-spaced angular offsets around (0, *, 0). Each clone shares the
// original's Y, scale, and rotation — only its XZ position changes.
//
// Cloning approach: we re-build the GPU mesh per instance from the cached
// CPU position-only buffer (cpuVerts/cpuIndices), reusing the importer's
// normal-computation and crease-split helpers. This avoids re-running the
// full file import (no progress dialog, no BuildFacetedShape, no file I/O)
// while keeping each clone's GPU buffers independently owned, so the
// existing per-object Destroy() lifetime model still works.
//
// OpenCascade TopoDS_Shape uses value semantics with shared internal data,
// so plain copy-by-assignment of sourceShape is safe and cheap.
// ---------------------------------------------------------------------------
void GLCanvas::ApplyCircularPattern(int count, bool overrideRadius, float radius,
    bool rotateCopies)
{
    if (!HasSelection()) return;
    if (m_selectedIndices.size() != 1)
    {
        // Pattern is inherently a single-seed operation. Refuse to run on
        // a multi-selection rather than guessing which object should be the
        // anchor (or stamping overlapping patterns from each).
        wxMessageBox("Pattern requires exactly one object to be selected.",
            "Circular Pattern", wxOK | wxICON_INFORMATION, this);
        return;
    }
    if (count <= 1)      return;     // a "pattern" of one is just the original

    const int seedIndex = m_selectedIndices.front();

    SetCurrent(*m_context);
    InitGLOnce();

    // Take a copy of the original by value — m_objects may reallocate when we
    // emplace_back the clones below, invalidating any reference we held.
    const SceneObject orig = m_objects[seedIndex];

    // Determine each clone's radius and starting angle in the XZ plane.
    // The original's angle is preserved so it remains part of the pattern.
    const float origRadius = std::sqrt(orig.pos.x * orig.pos.x +
        orig.pos.z * orig.pos.z);
    const float r = overrideRadius ? radius : origRadius;

    // If the original sits exactly on the Y axis (radius == 0), there's no
    // meaningful angular reference, so anchor at angle 0. Otherwise use the
    // original's existing direction so it remains the k=0 instance.
    const float theta0 = (origRadius > 1e-6f)
        ? std::atan2(orig.pos.z, orig.pos.x)
        : 0.0f;
    const float dtheta = 2.0f * 3.14159265358979323846f / static_cast<float>(count);

    for (int k = 1; k < count; ++k)
    {
        const float theta = theta0 + dtheta * static_cast<float>(k);

        m_objects.emplace_back();
        SceneObject& clone = m_objects.back();

        // Copy non-GPU, non-mould state. mouldShape is intentionally left
        // unset — the user re-generates the mould after patterning.
        clone.role = orig.role;
        clone.sourcePath = orig.sourcePath;
        clone.sourceShape = orig.sourceShape;
        clone.hasSourceShape = orig.hasSourceShape;
        clone.cpuVerts = orig.cpuVerts;
        clone.cpuIndices = orig.cpuIndices;
        // triNeighbors / adjacencyBuilt deliberately left at defaults — they
        // depend only on local mesh topology and will be rebuilt lazily on
        // first use, identically to the original.

        // Pose: same Y, scale, pitch, roll. XZ from polar coords. Yaw
        // depends on rotateCopies:
        //   off -> clone keeps the original's local rotation verbatim
        //   on  -> clone's yaw is offset by its angular position so it
        //          "faces outward" like the original (gear-tooth style).
        //
        // Sign note: glm::rotate around +Y by positive yawDeg sends +X to -Z,
        // whereas our position formula (r cos theta, _, r sin theta) sends
        // +X to +Z as theta increases. The two rotations have opposite signs
        // in this convention, so the yaw offset is *negative* k * dtheta to
        // match the angular position rotation.
        clone.pos = glm::vec3(r * std::cos(theta), orig.pos.y,
            r * std::sin(theta));
        clone.yawDeg = orig.yawDeg
            + (rotateCopies
                ? -static_cast<float>(k) * (360.0f / static_cast<float>(count))
                : 0.0f);
        clone.pitchDeg = orig.pitchDeg;
        clone.rollDeg = orig.rollDeg;
        clone.scale = orig.scale;

        // Build the GPU mesh from the cached CPU vertices, mirroring the
        // post-import pipeline in ImportFile().
        FileImporter::MeshData md;
        md.vertices = orig.cpuVerts;
        md.indices = orig.cpuIndices;
        ComputeVertexNormals_Pos3(md.vertices, md.indices, md.posNorm);
        auto split = SplitByCreaseAngle_Pos3(md.vertices, md.indices, 35.0f);
        md.posNorm = std::move(split.posNorm);
        md.indices = std::move(split.indices);
        UploadMeshToGPU(md, clone);
    }

    // Apply the override radius to the original too, if requested. We do this
    // *after* the clone loop so the original's old position is preserved as
    // the angular anchor (theta0) — moving it earlier would re-anchor the
    // whole pattern.
    if (overrideRadius && origRadius > 1e-6f)
    {
        SceneObject& origRef = m_objects[seedIndex];
        origRef.pos.x = r * std::cos(theta0);
        origRef.pos.z = r * std::sin(theta0);
    }

    // Clone the seed's parented vents/gates onto each new clone object so
    // patterning carries placement context (sticky-placement). For each
    // feature parented to the seed, push a fresh feature carrying the same
    // local-space placement but parented to the clone's m_objects index;
    // ReanchorVent / ReanchorGate then materialise its world data + GPU
    // resources from the clone's transform.
    //
    // Clones were just appended to the tail of m_objects in k=1..count-1
    // order, so their indices are firstCloneIdx + 0, +1, ... +(count-2).
    const int firstCloneIdx = (int)m_objects.size() - (count - 1);

    // Snapshot local placements of seed-parented features. Reading m_vents
    // / m_gates while we push to them would loop indefinitely, so we
    // snapshot first, then push.
    struct LocalPlacement { glm::vec3 pos; glm::vec3 normal; };
    std::vector<LocalPlacement> seedVentPlacements;
    std::vector<LocalPlacement> seedGatePlacements;
    for (const auto& vi : m_vents)
        if (vi.parentIndex == seedIndex)
            seedVentPlacements.push_back({ vi.localPos, vi.localNormal });
    for (const auto& gf : m_gates)
        if (gf.parentIndex == seedIndex)
            seedGatePlacements.push_back({ gf.localPos, gf.localNormal });

    std::vector<int> reanchorTargets;
    reanchorTargets.reserve(count);
    reanchorTargets.push_back(seedIndex);   // seed's pos may have shifted

    for (int k = 0; k < count - 1; ++k)
    {
        const int cloneIdx = firstCloneIdx + k;
        reanchorTargets.push_back(cloneIdx);

        for (const auto& lp : seedVentPlacements)
        {
            VentInstance vi;
            vi.parentIndex = cloneIdx;
            vi.localPos = lp.pos;
            vi.localNormal = lp.normal;
            // World point + GPU mesh built by ReanchorVent below.
            m_vents.push_back(std::move(vi));
        }
        for (const auto& lp : seedGatePlacements)
        {
            GateFeature gf;
            gf.parentIndex = cloneIdx;
            gf.localPos = lp.pos;
            gf.localNormal = lp.normal;
            // World point + path + solid built by ReanchorGate +
            // RebuildGatePathVBO/Solids below.
            m_gates.push_back(std::move(gf));
        }
    }

    // Single batch reanchor for the seed (in case override-radius moved it)
    // and every new clone. Also handles RebuildPathVBO / RebuildGatePathVBO
    // / RebuildGateSolids internally if either feature list was touched.
    ReanchorFeaturesForObjects(reanchorTargets);
    Refresh(false);
    NotifySceneMutated();
}

// ---------------------------------------------------------------------------
// ApplyGridPattern — duplicates the selected object into a 2-D grid that is
// CENTRED on the world origin, in the y=0 plane.
//
// The grid spans [-halfX, +halfX] x [-halfZ, +halfZ] with numH x numV cells
// linearly spaced across each axis. (halfX, halfZ) is derived as:
//   default     : (|orig.x|, |orig.z|) - the original's (x,z) is treated as
//                 HALF the full grid extent, so the model already sits at
//                 a corner of the centred grid.
//   override on : (length/2, width/2) - the override fields specify the
//                 FULL grid extents, so we halve them. The original is
//                 moved to the corner that matches its current XZ-quadrant.
//
// The "anchor" cell is the corner indexed by the sign of the original's
// XZ-quadrant: (numH-1, numV-1) for the (+,+) quadrant, (0, numV-1) for
// (-,+), etc. The original ends up at this corner; the remaining
// (numH*numV - 1) cells are filled with clones. With override off, the
// anchor naturally coincides with the original's existing position so no
// movement happens; with override on, the original is repositioned to the
// new corner.
//
// Mirror flags:
//   mirrorH - if true, every clone whose cell X coordinate sits on the
//             opposite side of x=0 from the original has its mesh
//             reflected about the local YZ plane (clone.mirrorX = true).
//   mirrorV - same for Z (reflection about local XY plane).
// Cells that lie exactly on an axis (cell.x == 0 or cell.z == 0, possible
// with odd grid counts) are not flipped on that axis - they're on the
// boundary, not on the other side. With both flags set, a clone in the
// diagonally-opposite quadrant gets reflected on both axes (parity
// preserved overall).
//
// Edge cases:
//   numH == 1 (or numV == 1): that axis collapses to a single position at
//     0, so the grid degenerates to a column (or row) on the perpendicular
//     axis. The anchor index on the collapsed axis is always 0.
//   On-axis original (x == 0 or z == 0): sign defaults to positive so
//     override has a deterministic landing corner.
//   On-origin original with override off: half-extents are zero and all
//     cells overlap at origin - degenerate but not a crash.
//
// Cloning approach mirrors ApplyCircularPattern: each clone re-builds its
// GPU mesh from the cached CPU position-only buffer, sharing the source
// shape and CPU mesh data with the original. Vents and the cached mould
// are invalidated as with other Apply* transforms.
// ---------------------------------------------------------------------------
void GLCanvas::ApplyGridPattern(int numH, int numV, bool mirrorH, bool mirrorV,
    bool overrideLengthWidth, float length, float width)
{
    if (!HasSelection())   return;
    if (m_selectedIndices.size() != 1)
    {
        // Same single-seed restriction as ApplyCircularPattern.
        wxMessageBox("Pattern requires exactly one object to be selected.",
            "Grid Pattern", wxOK | wxICON_INFORMATION, this);
        return;
    }
    if (numH <= 0)         return;
    if (numV <= 0)         return;
    if (numH * numV <= 1)  return;     // 1x1 has no clones to make

    const int seedIndex = m_selectedIndices.front();

    SetCurrent(*m_context);
    InitGLOnce();

    // Take a copy of the original by value - m_objects may reallocate when
    // we emplace_back the clones below, invalidating any reference we held.
    const SceneObject orig = m_objects[seedIndex];

    // Half-extents of the centred grid. See header comment for the rules.
    const float halfX = overrideLengthWidth
        ? length * 0.5f
        : std::abs(orig.pos.x);
    const float halfZ = overrideLengthWidth
        ? width * 0.5f
        : std::abs(orig.pos.z);

    // Sign of the original's XZ-quadrant. Used to choose which corner of
    // the centred grid the original anchors. Zero defaults to positive so
    // an on-axis original gets a deterministic corner when the override
    // moves it off-axis.
    const float signX = (orig.pos.x >= 0.0f) ? 1.0f : -1.0f;
    const float signZ = (orig.pos.z >= 0.0f) ? 1.0f : -1.0f;

    // Anchor cell index. With numH==1 (numV==1) the corresponding axis has
    // only one valid index (0) regardless of sign.
    const int anchorI = (signX > 0.0f && numH > 1) ? (numH - 1) : 0;
    const int anchorJ = (signZ > 0.0f && numV > 1) ? (numV - 1) : 0;

    // Cell-centre coordinates for indices (i, j). Linear interpolation from
    // -halfX..+halfX (or just 0 when numH == 1); same for Z.
    auto cellPos = [&](int i, int j) -> glm::vec3
        {
            const float cx = (numH == 1) ? 0.0f
                : -halfX + static_cast<float>(i) * (2.0f * halfX) /
                static_cast<float>(numH - 1);
            const float cz = (numV == 1) ? 0.0f
                : -halfZ + static_cast<float>(j) * (2.0f * halfZ) /
                static_cast<float>(numV - 1);
            return glm::vec3(cx, orig.pos.y, cz);
        };

    for (int i = 0; i < numH; ++i)
    {
        for (int j = 0; j < numV; ++j)
        {
            if (i == anchorI && j == anchorJ) continue;     // original sits here

            const glm::vec3 cell = cellPos(i, j);

            // "Other side of x=0" iff cell.x and signX have opposite signs
            // (product < 0). On-axis cells (cell.x == 0) have product 0
            // and are treated as boundary - no flip. signX/signZ are always
            // +/-1 so the product test is unambiguous. Same logic for Z.
            const bool flipX = mirrorH && (cell.x * signX < 0.0f);
            const bool flipZ = mirrorV && (cell.z * signZ < 0.0f);

            m_objects.emplace_back();
            SceneObject& clone = m_objects.back();

            // Copy non-GPU, non-mould state. mouldShape is intentionally
            // left unset - the user re-generates the mould after patterning.
            clone.role = orig.role;
            clone.sourcePath = orig.sourcePath;
            clone.sourceShape = orig.sourceShape;
            clone.hasSourceShape = orig.hasSourceShape;
            clone.cpuVerts = orig.cpuVerts;
            clone.cpuIndices = orig.cpuIndices;
            // triNeighbors / adjacencyBuilt deliberately left at defaults -
            // they depend only on local mesh topology and will be rebuilt
            // lazily on first use, identically to the original.

            // Pose: same Y, scale and full rotation. Position from cell
            // grid; mirror flags from the cross-axis test above.
            clone.pos = cell;
            clone.yawDeg = orig.yawDeg;
            clone.pitchDeg = orig.pitchDeg;
            clone.rollDeg = orig.rollDeg;
            clone.scale = orig.scale;
            clone.mirrorX = flipX;
            clone.mirrorZ = flipZ;

            // Build the GPU mesh from the cached CPU vertices, mirroring
            // the post-import pipeline in ImportFile() (same pipeline used
            // by ApplyCircularPattern).
            FileImporter::MeshData md;
            md.vertices = orig.cpuVerts;
            md.indices = orig.cpuIndices;
            ComputeVertexNormals_Pos3(md.vertices, md.indices, md.posNorm);
            auto split = SplitByCreaseAngle_Pos3(md.vertices, md.indices, 35.0f);
            md.posNorm = std::move(split.posNorm);
            md.indices = std::move(split.indices);
            UploadMeshToGPU(md, clone);
        }
    }

    // Move the original to its anchor cell. With override off this is
    // typically a no-op (orig was already at this corner, since the grid
    // was sized to put it there); with override on, the original moves to
    // the matching corner of the new grid extents.
    {
        SceneObject& origRef = m_objects[seedIndex];
        const glm::vec3 anchorPos = cellPos(anchorI, anchorJ);
        origRef.pos.x = anchorPos.x;
        origRef.pos.z = anchorPos.z;
    }

    // Clone the seed's parented vents/gates onto each new clone object so
    // patterning carries placement context (sticky-placement). Mirrored
    // clones (mirrorX / mirrorZ) get correctly-mirrored feature placements
    // for free because BuildModelMatrix encodes the negative scale and
    // ReanchorVent / ReanchorGate go through it. See ApplyCircularPattern
    // for the symmetric explanation.
    const int numClones = numH * numV - 1;
    const int firstCloneIdx = (int)m_objects.size() - numClones;

    struct LocalPlacement { glm::vec3 pos; glm::vec3 normal; };
    std::vector<LocalPlacement> seedVentPlacements;
    std::vector<LocalPlacement> seedGatePlacements;
    for (const auto& vi : m_vents)
        if (vi.parentIndex == seedIndex)
            seedVentPlacements.push_back({ vi.localPos, vi.localNormal });
    for (const auto& gf : m_gates)
        if (gf.parentIndex == seedIndex)
            seedGatePlacements.push_back({ gf.localPos, gf.localNormal });

    std::vector<int> reanchorTargets;
    reanchorTargets.reserve(numClones + 1);
    reanchorTargets.push_back(seedIndex);   // seed's pos may have shifted

    for (int k = 0; k < numClones; ++k)
    {
        const int cloneIdx = firstCloneIdx + k;
        reanchorTargets.push_back(cloneIdx);

        for (const auto& lp : seedVentPlacements)
        {
            VentInstance vi;
            vi.parentIndex = cloneIdx;
            vi.localPos = lp.pos;
            vi.localNormal = lp.normal;
            m_vents.push_back(std::move(vi));
        }
        for (const auto& lp : seedGatePlacements)
        {
            GateFeature gf;
            gf.parentIndex = cloneIdx;
            gf.localPos = lp.pos;
            gf.localNormal = lp.normal;
            m_gates.push_back(std::move(gf));
        }
    }

    ReanchorFeaturesForObjects(reanchorTargets);
    Refresh(false);
    NotifySceneMutated();
}

void GLCanvas::ClearVentPoints()
{
    for (auto& v : m_vents) v.Destroy();
    m_vents.clear();
    RebuildPathVBO();
    RebuildCrossSectionVBO();
    Refresh(false);
    NotifySceneMutated();
}

// ---------------------------------------------------------------------------
// Sprue placement
// ---------------------------------------------------------------------------

// RebuildSpruePathVBO — uploads the two path endpoints as a single GL_LINES pair.
void GLCanvas::RebuildSpruePathVBO()
{
    if (!m_sprue.pathVAO) return;

    glBindVertexArray(m_sprue.pathVAO);
    glBindBuffer(GL_ARRAY_BUFFER, m_sprue.pathVBO);

    if (m_sprue.hasPoint)
    {
        const float verts[6] = {
            m_sprue.pathStart.x, m_sprue.pathStart.y, m_sprue.pathStart.z,
            m_sprue.pathEnd.x,   m_sprue.pathEnd.y,   m_sprue.pathEnd.z
        };
        glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_DYNAMIC_DRAW);
        m_sprue.pathVertexCount = 2;
    }
    else
    {
        glBufferData(GL_ARRAY_BUFFER, 0, nullptr, GL_DYNAMIC_DRAW);
        m_sprue.pathVertexCount = 0;
    }

    glBindVertexArray(0);
}

// RebuildSprueXsecVBO — builds a circle of kSprueCircleSegments line pairs
// centred at m_sprue.pathStart, lying in the plane perpendicular to the sprue
// path direction, with radius m_sprue.radius.
void GLCanvas::RebuildSprueXsecVBO()
{
    if (!m_sprue.xsecVAO) return;

    glBindVertexArray(m_sprue.xsecVAO);
    glBindBuffer(GL_ARRAY_BUFFER, m_sprue.xsecVBO);

    if (!m_sprue.hasPoint)
    {
        glBufferData(GL_ARRAY_BUFFER, 0, nullptr, GL_DYNAMIC_DRAW);
        m_sprue.xsecVertexCount = 0;
        glBindVertexArray(0);
        return;
    }

    // Path direction — normalise; fall back to world-Y if degenerate
    glm::vec3 axisZ = m_sprue.pathEnd - m_sprue.pathStart;
    const float axisLen = glm::length(axisZ);
    axisZ = (axisLen > 1e-6f) ? axisZ / axisLen : glm::vec3(0.0f, -1.0f, 0.0f);

    // Build two axes perpendicular to axisZ using Gram-Schmidt
    glm::vec3 axisX = glm::abs(axisZ.x) < 0.9f
        ? glm::vec3(1.0f, 0.0f, 0.0f)
        : glm::vec3(0.0f, 1.0f, 0.0f);
    axisX = glm::normalize(axisX - glm::dot(axisX, axisZ) * axisZ);
    const glm::vec3 axisY = glm::cross(axisZ, axisX);

    // Generate N evenly-spaced points around the circle, packed as GL_LINES pairs
    constexpr int N = 32;
    std::vector<float> verts;
    verts.reserve(N * 6);   // 2 verts per segment × 3 floats

    auto push = [&](const glm::vec3& v)
        {
            verts.push_back(v.x); verts.push_back(v.y); verts.push_back(v.z);
        };

    for (int i = 0; i < N; ++i)
    {
        const float t0 = (float(i) / float(N)) * glm::two_pi<float>();
        const float t1 = (float(i + 1) / float(N)) * glm::two_pi<float>();
        push(m_sprue.pathStart + m_sprue.radius * (std::cos(t0) * axisX + std::sin(t0) * axisY));
        push(m_sprue.pathStart + m_sprue.radius * (std::cos(t1) * axisX + std::sin(t1) * axisY));
    }

    m_sprue.xsecVertexCount = (GLsizei)verts.size() / 3;
    glBufferData(GL_ARRAY_BUFFER,
        (GLsizeiptr)(verts.size() * sizeof(float)),
        verts.data(), GL_DYNAMIC_DRAW);
    glBindVertexArray(0);
}

// ---------------------------------------------------------------------------
// RebuildRunnerPathVBO — uploads line segments from the sprue parting point
// to each runner point as GL_LINES pairs.
// ---------------------------------------------------------------------------
void GLCanvas::RebuildRunnerPathVBO()
{
    if (!m_runnerPathVAO) return;

    glBindVertexArray(m_runnerPathVAO);
    glBindBuffer(GL_ARRAY_BUFFER, m_runnerPathVBO);

    if (m_sprue.hasPartingPoint && !m_runners.empty())
    {
        std::vector<float> verts;
        verts.reserve(m_runners.size() * 6);  // 2 endpoints × 3 floats per line

        for (const RunnerFeature& rf : m_runners)
        {
            verts.push_back(m_sprue.partingPos.x);
            verts.push_back(m_sprue.partingPos.y);
            verts.push_back(m_sprue.partingPos.z);
            verts.push_back(rf.point.x);
            verts.push_back(rf.point.y);
            verts.push_back(rf.point.z);
        }

        m_runnerPathVertexCount = (GLsizei)(verts.size() / 3);
        glBufferData(GL_ARRAY_BUFFER,
            (GLsizeiptr)(verts.size() * sizeof(float)),
            verts.data(), GL_DYNAMIC_DRAW);
    }
    else
    {
        m_runnerPathVertexCount = 0;
    }

    glBindVertexArray(0);
}

// ---------------------------------------------------------------------------
// RebuildRunnerSolids — destroys existing runner solid meshes and rebuilds
// one swept cylinder per runner, running from the sprue parting point
// to the runner point along the y=0 parting plane.  Uses the runner diameter
// and cold plug distance from the left-panel UI.
// ---------------------------------------------------------------------------
void GLCanvas::RebuildRunnerSolids()
{
    for (auto& rf : m_runners) rf.Destroy();

    if (!m_sprue.hasPartingPoint || m_runners.empty()) return;

    // Read the current runner dimensions from the UI
    float runnerRadius = 2.5f;
    float coldPlugDist = 5.0f;
    if (auto* frame = dynamic_cast<MainFrame*>(wxGetTopLevelParent(this)))
    {
        runnerRadius = frame->GetRunnerDiameter() * 0.5f;
        coldPlugDist = frame->GetRunnerColdPlugDist();
    }

    for (RunnerFeature& rf : m_runners)
    {
        rf.solid = BuildCylinderMesh(m_sprue.partingPos, rf.point, runnerRadius,
            /*draftAngleDeg=*/0.0f);

        if (coldPlugDist > 1e-6f)
        {
            const glm::vec3 runnerDir = glm::normalize(rf.point - m_sprue.partingPos);
            const glm::vec3 plugEnd = rf.point + runnerDir * coldPlugDist;
            rf.coldPlugSolid = BuildCylinderMesh(rf.point, plugEnd, runnerRadius,
                /*draftAngleDeg=*/0.0f);
        }
    }
}

// ---------------------------------------------------------------------------
// RebuildGateSolids — builds a tapered frustum (gate) and a straight cylinder
// (sub-runner) for every placed gate that has a valid path.
//
// The gate frustum starts at the gate point with gateRadius, expands at
// draftAngle along the path direction until it reaches subRunnerRadius —
// that transition point is (subRunnerRadius - gateRadius) / tan(draftAngle)
// from the gate origin.  The sub-runner cylinder continues at subRunnerRadius
// from the transition point to pathEnd.
//
// If the taper length >= total path length, the gate fills the whole path
// and no sub-runner section is produced.
// ---------------------------------------------------------------------------
void GLCanvas::RebuildGateSolids()
{
    for (auto& gf : m_gates) { gf.solid.Destroy(); gf.subRunnerSolid.Destroy(); }
    if (m_gates.empty()) return;

    float gateRadius = 1.5f;
    float draftAngle = 1.0f;
    float subRunnerRadius = 2.5f;
    float overrun = 0.0f;
    if (auto* frame = dynamic_cast<MainFrame*>(wxGetTopLevelParent(this)))
    {
        gateRadius = frame->GetGateDiameter() * 0.5f;
        draftAngle = frame->GetGateDraftAngle();
        subRunnerRadius = frame->GetSubRunnerDiameter() * 0.5f;
        overrun = frame->GetGateOverrun();
    }

    const float draftRad = glm::radians(glm::clamp(draftAngle, 0.0f, 45.0f));
    const float tanDraft = std::tan(draftRad);

    for (GateFeature& gf : m_gates)
    {
        if (!gf.hasPath) continue;

        const glm::vec3 origin = gf.point.worldPos;
        const glm::vec3 pathVec = gf.pathEnd - origin;
        const float     totalLen = glm::length(pathVec);
        if (totalLen < 1e-6f) continue;
        const glm::vec3 pathDir = pathVec / totalLen;

        // Distance along path at which the draft expands gate to sub-runner radius
        float taperLen = std::numeric_limits<float>::max();
        if (tanDraft > 1e-6f && subRunnerRadius > gateRadius)
            taperLen = (subRunnerRadius - gateRadius) / tanDraft;

        // Overrun shifts the cylinder's start backward along -pathDir into
        // the model. The radius at the start is reduced so that the radius
        // at the original origin (parting surface) still equals gateRadius —
        // i.e. the user-typed Diameter remains the diameter of the inlet
        // hole at the parting surface, regardless of overrun. For typical
        // drafts the start-radius shrinkage is sub-millimetre and the
        // visible effect is just "the cylinder pokes back into the model".
        // Clamped to a small floor so aggressive overrun + draft combos
        // don't degenerate into a zero-radius cap (BuildCylinderMesh
        // rejects radii below 1e-6).
        const glm::vec3 startPt = origin - pathDir * overrun;
        const float startRadius = std::max(0.01f, gateRadius - tanDraft * overrun);

        if (taperLen >= totalLen)
        {
            // Gate fills the entire path — no sub-runner section
            gf.solid = BuildCylinderMesh(startPt, gf.pathEnd, startRadius, draftAngle);
        }
        else
        {
            const glm::vec3 transitionPt = origin + pathDir * taperLen;
            gf.solid = BuildCylinderMesh(startPt, transitionPt,
                startRadius, draftAngle);
            gf.subRunnerSolid = BuildCylinderMesh(transitionPt, gf.pathEnd,
                subRunnerRadius, 0.0f);
        }
    }
}

// ---------------------------------------------------------------------------
// RebuildEjectorSolids — builds a straight cylinder for every placed ejector,
// extruded from the ejector's snap point in the -Y direction (toward the B
// mould half). Diameter and length come from the MainFrame UI at call time.
//
// No taper / draft is applied: the UI exposes only diameter and length, so
// BuildCylinderMesh is invoked with draftAngleDeg = 0. If a tapered ejector
// type is added later (e.g. shoulder pin), branch on the type dropdown the
// same way RebuildGateSolids handles its frustum / sub-runner split.
//
// Ejectors snapped onto an object face start above y=0 — the geometry
// passes through the parting plane as a single straight cylinder. That's
// fine for the preview; the eventual mould-cut step will be responsible
// for clipping to the B half.
// ---------------------------------------------------------------------------
void GLCanvas::RebuildEjectorSolids()
{
    for (auto& ef : m_ejectors) ef.solid.Destroy();
    if (m_ejectors.empty()) return;

    float ejectorRadius = 1.5f;
    float ejectorLength = 25.0f;
    if (auto* frame = dynamic_cast<MainFrame*>(wxGetTopLevelParent(this)))
    {
        ejectorRadius = frame->GetEjectorDiameter() * 0.5f;
        ejectorLength = frame->GetEjectorLength();
    }

    // Defensive: degenerate dimensions would produce an invalid SolidMesh,
    // which is harmless but pointless to send to the GPU.
    if (ejectorRadius < 1e-4f || ejectorLength < 1e-4f) return;

    for (EjectorFeature& ef : m_ejectors)
    {
        const glm::vec3 start = ef.point;
        const glm::vec3 end = start + glm::vec3(0.0f, -ejectorLength, 0.0f);
        ef.solid = BuildCylinderMesh(start, end, ejectorRadius, 0.0f);
    }
}
// feed-network segment (sprue parting point, or any point along a runner path)
// in XZ, stores the result on the GateFeature, then uploads one GL_LINES pair
// per gate.  Runner paths are treated as full segments (sprue parting pt →
// runner pt), not just their endpoints.
// ---------------------------------------------------------------------------
void GLCanvas::RebuildGatePathVBO()
{
    if (!m_gatePathVAO) return;

    std::vector<float> verts;
    verts.reserve(m_gates.size() * 6);

    for (GateFeature& gf : m_gates)
    {
        gf.hasPath = false;

        const glm::vec2 gateXZ(gf.point.worldPos.x, gf.point.worldPos.z);
        float     bestDist = std::numeric_limits<float>::max();
        glm::vec3 bestPt(0.0f);

        // Candidate 1: sprue parting-plane intersection point (a degenerate
        // segment of length zero — also serves as the shared segment origin).
        if (m_sprue.hasPartingPoint)
        {
            const glm::vec2 d(m_sprue.partingPos.x - gateXZ.x,
                m_sprue.partingPos.z - gateXZ.y);
            const float dist = glm::length(d);
            if (dist < bestDist) { bestDist = dist; bestPt = m_sprue.partingPos; }

            // Candidate 2: closest point along each runner segment
            // (segment runs from sprue parting pt → runner pt, all on y=0)
            for (const RunnerFeature& rf : m_runners)
            {
                const glm::vec2 A(m_sprue.partingPos.x, m_sprue.partingPos.z);
                const glm::vec2 B(rf.point.x, rf.point.z);
                const glm::vec2 AB = B - A;
                const float     len2 = glm::dot(AB, AB);

                glm::vec2 closest;
                if (len2 < 1e-10f)
                {
                    // Degenerate segment — just the sprue parting point
                    closest = A;
                }
                else
                {
                    const float t = glm::clamp(glm::dot(gateXZ - A, AB) / len2,
                        0.0f, 1.0f);
                    closest = A + t * AB;
                }

                const float dist2 = glm::length(closest - gateXZ);
                if (dist2 < bestDist)
                {
                    bestDist = dist2;
                    bestPt = glm::vec3(closest.x, 0.0f, closest.y);
                }
            }
        }

        if (bestDist < std::numeric_limits<float>::max())
        {
            gf.pathEnd = bestPt;
            gf.hasPath = true;

            verts.push_back(gf.point.worldPos.x);
            verts.push_back(gf.point.worldPos.y);
            verts.push_back(gf.point.worldPos.z);
            verts.push_back(bestPt.x);
            verts.push_back(bestPt.y);
            verts.push_back(bestPt.z);
        }
    }

    m_gatePathVertexCount = (GLsizei)(verts.size() / 3);

    glBindVertexArray(m_gatePathVAO);
    glBindBuffer(GL_ARRAY_BUFFER, m_gatePathVBO);
    glBufferData(GL_ARRAY_BUFFER,
        (GLsizeiptr)(verts.size() * sizeof(float)),
        verts.empty() ? nullptr : verts.data(),
        GL_DYNAMIC_DRAW);
    glBindVertexArray(0);
}

void GLCanvas::SetActiveInjectionPoint(const InjectionPoint& ip)
{
    m_activeInjectionPoint = ip;
    m_hasActiveInjectionPoint = true;
    // Clear any previously placed sphere so stale geometry isn't shown
    // after a fixture change.
    m_sprue.isDirectInjection = false;
    m_sprue.hasPoint = false;
    m_sprue.hasPartingPoint = false;
    m_sprue.solid.Destroy();
    m_sprue.coldSlugSolid.Destroy();
    RebuildSpruePathVBO();
    RebuildSprueXsecVBO();
    RebuildRunnerPathVBO();
    RebuildRunnerSolids();
    RebuildGatePathVBO();
    RebuildGateSolids();
    Refresh(false);
}

void GLCanvas::SetInjectionPoints(const std::vector<InjectionPoint>& pts)
{
    m_injectionPoints = pts;
}

void GLCanvas::PlaceSprue()
{
    if (!m_hasActiveInjectionPoint) return;

    // Transform the injection point from fixture-local space to world space.
    glm::vec4 localPos(m_activeInjectionPoint.x,
        m_activeInjectionPoint.y,
        m_activeInjectionPoint.z,
        1.0f);

    glm::mat4 fixtureMatrix(1.0f);
    if (!m_fixtures.empty())
        fixtureMatrix = m_fixtures[0].BuildModelMatrix();

    m_sprue.worldPos = glm::vec3(fixtureMatrix * localPos);
    m_sprue.hasPoint = true;

    // Read sprue dimensions from the left-panel UI
    if (auto* frame = dynamic_cast<MainFrame*>(wxGetTopLevelParent(this)))
    {
        m_sprue.radius = frame->GetSprueDiameter() * 0.5f;
        m_sprue.draftAngleDeg = frame->GetSprueDraftAngle();
        m_sprue.coldSlugDepth = frame->GetSprueColdSlugDepth();
    }

    if (m_activeInjectionPoint.type == InjectionType::Radial)
    {
        // ---- Radial sprue: lies on the parting plane (y=0). Path direction
        // depends on whether a model is in the way:
        //   - Direct injection: cast a ray inward (from the injection point
        //     toward the cavity interior) and end the sprue at the hit
        //     point on the model. Mirrors the axial direct-injection check.
        //   - Otherwise: legacy outward-toward-perimeter behavior — the
        //     path terminates sprueLength past the fixture perimeter, where
        //     the moulding-machine nozzle would attach.
        // The solid mesh and cold slug are built by the common code below
        // (shared with the axial branch) — this block is responsible only
        // for setting pathStart, pathEnd, partingPos, hasPartingPoint and
        // isDirectInjection.

        // Project injection point onto the parting plane (y=0)
        const glm::vec3 ipAtParting(m_sprue.worldPos.x, 0.0f, m_sprue.worldPos.z);
        const glm::vec2 ipXZ(ipAtParting.x, ipAtParting.z);

        // Find nearest point on the fixture perimeter. Used to derive the
        // outward radial direction (whose negation is the inward
        // direct-injection ray) and, on direct-injection miss, to place
        // the non-direct path's terminus.
        float     bestDist = std::numeric_limits<float>::max();
        glm::vec2 bestPt(0.0f);

        if (m_fixturePerimeter.size() >= 2)
        {
            const int n = (int)m_fixturePerimeter.size();
            for (int i = 0; i < n; ++i)
            {
                const glm::vec2& A = m_fixturePerimeter[i];
                const glm::vec2& B = m_fixturePerimeter[(i + 1) % n];
                const glm::vec2  AB = B - A;
                const float      len2 = glm::dot(AB, AB);
                float            t = 0.0f;
                if (len2 > 1e-10f)
                    t = glm::clamp(glm::dot(ipXZ - A, AB) / len2, 0.0f, 1.0f);
                const glm::vec2 closest = A + t * AB;
                const float     dist = glm::length(closest - ipXZ);
                if (dist < bestDist)
                {
                    bestDist = dist;
                    bestPt = closest;
                }
            }
        }

        // Direction from injection point outward toward the perimeter.
        // Falls back to (0, 1) when the injection point coincides with the
        // perimeter (bestDist ~ 0); the inward ray will then probe along
        // -Z, which is no worse than any other arbitrary axis.
        glm::vec2 outDir(0.0f, 1.0f);
        if (bestDist > 1e-6f)
            outDir = glm::normalize(bestPt - ipXZ);

        m_sprue.pathStart = ipAtParting;

        // Try direct injection first: cast a ray from the injection point
        // INWARD along the parting plane and see if it hits a model. The
        // inward direction is -outDir, pointing from the injection point
        // away from the perimeter and toward the cavity interior where the
        // part lives.
        constexpr float kMaxDist = 1000.0f;
        const glm::vec3 inDir(outDir.x, 0.0f, outDir.y);

        glm::vec3 hitPos;
        if (RayCastWorldRay(ipAtParting, inDir, kMaxDist, hitPos))
        {
            // Direct injection — sprue runs from injection point inward to
            // the model contact point. Same semantics as axial direct
            // injection; the only difference is the ray direction.
            m_sprue.pathEnd = hitPos;
            m_sprue.partingPos = ipAtParting;
            m_sprue.hasPartingPoint = true;
            m_sprue.isDirectInjection = true;
        }
        else
        {
            // No model in the inward direction — fall back to the legacy
            // outward-perimeter behavior. Path goes from the injection
            // point outward, ending sprueLength past the fixture perimeter.
            float sprueLength = 20.0f;
            if (auto* frame = dynamic_cast<MainFrame*>(wxGetTopLevelParent(this)))
                sprueLength = frame->GetSprueLength();

            const glm::vec2 outerXZ = bestPt + outDir * sprueLength;

            m_sprue.pathEnd = glm::vec3(outerXZ.x, 0.0f, outerXZ.y);
            m_sprue.partingPos = m_sprue.pathEnd;
            m_sprue.hasPartingPoint = true;
            m_sprue.isDirectInjection = false;
        }
    }
    else
    {
        // ---- Axial sprue: vertical path downward from injection point ------
        m_sprue.pathStart = m_sprue.worldPos;

        // Fire a ray straight down toward y=0, max 1000mm (1m).
        constexpr float kMaxDist = 1000.0f;
        const glm::vec3 rayDir(0.0f, -1.0f, 0.0f);

        glm::vec3 hitPos;
        if (RayCastWorldRay(m_sprue.worldPos, rayDir, kMaxDist, hitPos))
        {
            // Ray hit an object — this is a direct-injection mould
            m_sprue.pathEnd = hitPos;
            m_sprue.isDirectInjection = true;
        }
        else
        {
            // No object hit — path terminates at the y=0 parting plane
            m_sprue.pathEnd = glm::vec3(m_sprue.worldPos.x, 0.0f, m_sprue.worldPos.z);
            m_sprue.isDirectInjection = false;
        }

        // Compute the point where the sprue path crosses the y=0 parting plane
        {
            const glm::vec3& S = m_sprue.pathStart;
            const glm::vec3& E = m_sprue.pathEnd;
            float dy = E.y - S.y;
            if (std::abs(dy) > 1e-6f)
            {
                float t = -S.y / dy;               // parametric t where y == 0
                if (t >= 0.0f && t <= 1.0f)
                {
                    m_sprue.partingPos = S + t * (E - S);
                    m_sprue.hasPartingPoint = true;
                }
                else
                {
                    m_sprue.hasPartingPoint = false;
                }
            }
            else if (std::abs(S.y) < 1e-6f)
            {
                // Both endpoints are already on the parting plane; use the end
                m_sprue.partingPos = E;
                m_sprue.hasPartingPoint = true;
            }
            else
            {
                m_sprue.hasPartingPoint = false;
            }
        }
    }

    // ---- Common build steps (axial and radial) -----------------------------
    // Both branches set pathStart / pathEnd / isDirectInjection / coldSlugDepth
    // identically as far as the geometry below is concerned, so the sprue
    // cylinder and cold-slug well are built once here.

    // Build the swept sprue cylinder preview mesh.
    m_sprue.solid.Destroy();
    m_sprue.solid = BuildCylinderMesh(m_sprue.pathStart, m_sprue.pathEnd,
        m_sprue.radius, m_sprue.draftAngleDeg);

    // Build cold slug well — a straight cylinder extending beyond pathEnd in
    // the path direction, using the drafted end radius. Skipped for direct
    // injection regardless of injection type.
    //
    // We also zero out m_sprue.coldSlugDepth in the direct-injection case so
    // the stored data matches what's actually drawn. Otherwise the value read
    // from the UI at the top of PlaceSprue (before the ray cast determined
    // direct-injection) lingers on the sprue, gets persisted to project files,
    // and could surface through any future code path that misses the
    // isDirectInjection gate. The downstream BREP cut and RestoreSprue still
    // check isDirectInjection too — defense in depth — but with the depth
    // zeroed, even an accidentally-ungated read returns the right answer.
    m_sprue.coldSlugSolid.Destroy();
    if (m_sprue.isDirectInjection)
    {
        m_sprue.coldSlugDepth = 0.0f;
    }
    else if (m_sprue.coldSlugDepth > 1e-6f)
    {
        const glm::vec3 sprueDir = glm::normalize(m_sprue.pathEnd - m_sprue.pathStart);
        const float sprueLen = glm::length(m_sprue.pathEnd - m_sprue.pathStart);
        const float draftRad = glm::radians(glm::clamp(m_sprue.draftAngleDeg, 0.0f, 45.0f));
        const float endRadius = m_sprue.radius + sprueLen * std::tan(draftRad);

        const glm::vec3 slugStart = m_sprue.pathEnd;
        const glm::vec3 slugEnd = m_sprue.pathEnd + sprueDir * m_sprue.coldSlugDepth;
        m_sprue.coldSlugSolid = BuildCylinderMesh(slugStart, slugEnd, endRadius, 0.0f);
    }

    RebuildSpruePathVBO();
    RebuildSprueXsecVBO();
    RebuildRunnerPathVBO();
    RebuildRunnerSolids();
    RebuildGatePathVBO();
    RebuildGateSolids();
    Refresh(false);
    NotifySceneMutated();
}

void GLCanvas::ClearSprue()
{
    m_sprue.hasPoint = false;
    m_sprue.isDirectInjection = false;
    m_sprue.hasPartingPoint = false;
    m_sprue.solid.Destroy();
    m_sprue.coldSlugSolid.Destroy();
    RebuildSpruePathVBO();
    RebuildSprueXsecVBO();
    RebuildRunnerPathVBO();
    RebuildRunnerSolids();
    RebuildGatePathVBO();
    RebuildGateSolids();
    Refresh(false);
    NotifySceneMutated();
}

// ---------------------------------------------------------------------------
// Generate Mould Operation — Cuts objects, vents, runners, and sprues from blank mold halves
// ---------------------------------------------------------------------------
bool GLCanvas::GenerateMould()
{
    if (m_fixtures.empty())
    {
        wxMessageBox("No fixtures loaded.",
            "Generate Mould", wxOK | wxICON_WARNING, this);
        return false;
    }
    if (m_objects.empty())
    {
        wxMessageBox("No imported objects to subtract.",
            "Generate Mould", wxOK | wxICON_WARNING, this);
        return false;
    }

    // Steps per fixture: 1 read + 1 transform + (1 per object subtract) + (1 per vent subtract) + (1 per runner subtract) + (1 per gate subtract) + (1 per ejector subtract) + 1 sprue + 1 tessellate + 1 upload
    const int stepsPerFixture = 4 + (int)m_objects.size() + (int)m_vents.size() + (int)m_runners.size() + (int)m_gates.size() + (int)m_ejectors.size();
    const int totalSteps = stepsPerFixture * (int)m_fixtures.size();

    wxProgressDialog progress(
        "Generating Mould",
        "Initialising...",
        totalSteps,
        nullptr,
        wxPD_APP_MODAL | wxPD_AUTO_HIDE | wxPD_SMOOTH | wxPD_ELAPSED_TIME
    );

    int step = 0;

    // Reset the per-run capture of post-cut half meshes. These are handed to
    // the preview window after a successful run; the main canvas no longer
    // displays them itself (it keeps showing the original fixtures).
    m_lastMouldMeshes.clear();
    m_lastShotMesh = FileImporter::MeshData{};
    m_hasLastShotMesh = false;
    m_lastShotVolumeMm3 = 0.0;
    m_lastShotShape = TopoDS_Shape();
    m_lastShotFaceIds.clear();
    m_lastHalfShapes.clear();

    SetCurrent(*m_context);
    InitGLOnce();

    for (int fi = 0; fi < (int)m_fixtures.size(); ++fi)
    {
        SceneObject& fix = m_fixtures[fi];
        if (fix.sourcePath.empty()) continue;

        const std::string fixLabel = "Fixture " + std::to_string(fi + 1);

        // Step: read fixture
        progress.Update(step++, fixLabel + ": loading source geometry...");

        TopoDS_Shape fixtureShape;
        if (fix.hasSourceShape)
        {
            // Use the shape cached at import time (STEP native or faceted mesh).
            fixtureShape = fix.sourceShape;
        }
        else
        {
            // Legacy fallback: re-read from disk (only works for STEP).
            STEPControl_Reader fixReader;
            if (fixReader.ReadFile(fix.sourcePath.c_str()) != IFSelect_RetDone)
            {
                wxMessageBox("Failed to re-read fixture: " + fix.sourcePath,
                    "Generate Mould", wxOK | wxICON_ERROR, this);
                step += stepsPerFixture - 1;  // skip remaining steps for this fixture
                continue;
            }
            fixReader.TransferRoots();
            fixtureShape = fixReader.OneShape();
        }
        if (fixtureShape.IsNull()) continue;

        // Step: apply fixture transform
        progress.Update(step++, fixLabel + ": applying transform...");

        gp_Trsf fixTrsf;
        glm::mat4 fm = fix.BuildModelMatrix();
        fixTrsf.SetValues(
            fm[0][0], fm[1][0], fm[2][0], fm[3][0],
            fm[0][1], fm[1][1], fm[2][1], fm[3][1],
            fm[0][2], fm[1][2], fm[2][2], fm[3][2]
        );
        BRepBuilderAPI_Transform fixXform(fixtureShape, fixTrsf, true);
        TopoDS_Shape result = fixXform.Shape();

        // Steps: subtract each object
        for (int oi = 0; oi < (int)m_objects.size(); ++oi)
        {
            const SceneObject& obj = m_objects[oi];
            progress.Update(step++,
                fixLabel + ": subtracting object " +
                std::to_string(oi + 1) + " of " +
                std::to_string((int)m_objects.size()) + "...");

            if (obj.sourcePath.empty() && !obj.hasSourceShape) continue;

            TopoDS_Shape objShape;
            if (obj.hasSourceShape)
            {
                objShape = obj.sourceShape;
            }
            else
            {
                // Legacy fallback: re-read from disk (STEP only).
                STEPControl_Reader objReader;
                if (objReader.ReadFile(obj.sourcePath.c_str()) != IFSelect_RetDone)
                    continue;
                objReader.TransferRoots();
                objShape = objReader.OneShape();
            }
            if (objShape.IsNull()) continue;

            gp_Trsf objTrsf;
            glm::mat4 om = obj.BuildModelMatrix();
            objTrsf.SetValues(
                om[0][0], om[1][0], om[2][0], om[3][0],
                om[0][1], om[1][1], om[2][1], om[3][1],
                om[0][2], om[1][2], om[2][2], om[3][2]
            );
            BRepBuilderAPI_Transform objXform(objShape, objTrsf, true);
            TopoDS_Shape transformedObj = objXform.Shape();

            BRepAlgoAPI_Cut cut(result, transformedObj);
            cut.Build();
            if (!cut.IsDone() || cut.Shape().IsNull())
            {
                wxMessageBox("Boolean subtract failed for object " +
                    std::to_string(oi + 1),
                    "Generate Mould", wxOK | wxICON_WARNING, this);
                continue;
            }
            result = cut.Shape();
        }

        // Steps: subtract each vent
        for (int vi = 0; vi < (int)m_vents.size(); ++vi)
        {
            const VentCrossSection& xs = m_vents[vi].crossSection;
            const VentPath& vp = m_vents[vi].path;

            progress.Update(step++,
                fixLabel + ": cutting vent " +
                std::to_string(vi + 1) + " of " +
                std::to_string((int)m_vents.size()) + "...");

            if (!xs.valid || !vp.valid) continue;

            // Build a planar wire from the 4 cross-section corners
            auto toOCC = [](const glm::vec3& v) { return gp_Pnt(v.x, v.y, v.z); };

            const gp_Pnt p0 = toOCC(xs.corners[0]);
            const gp_Pnt p1 = toOCC(xs.corners[1]);
            const gp_Pnt p2 = toOCC(xs.corners[2]);
            const gp_Pnt p3 = toOCC(xs.corners[3]);

            BRepBuilderAPI_MakeEdge e0(p0, p1);
            BRepBuilderAPI_MakeEdge e1(p1, p2);
            BRepBuilderAPI_MakeEdge e2(p2, p3);
            BRepBuilderAPI_MakeEdge e3(p3, p0);

            if (!e0.IsDone() || !e1.IsDone() || !e2.IsDone() || !e3.IsDone()) continue;

            BRepBuilderAPI_MakeWire wire;
            wire.Add(e0.Edge());
            wire.Add(e1.Edge());
            wire.Add(e2.Edge());
            wire.Add(e3.Edge());
            if (!wire.IsDone()) continue;

            BRepBuilderAPI_MakeFace face(wire.Wire(), /*onlyPlane=*/true);
            if (!face.IsDone()) continue;

            // Extrude face along the vent path vector, extended by the stored overruns.
            // The cross-section sits at path.start; shift it back by overrunStart,
            // then extend the sweep by overrunStart + overrunEnd so both ends clear
            // any surface co-planarity artifacts.
            const glm::vec3 rawSweep = vp.end - vp.start;
            const float     rawLen = glm::length(rawSweep);
            if (rawLen < 1e-6f) continue;
            const glm::vec3 sweepDir = rawSweep / rawLen;

            // Translate the face back by overrunStart before extruding
            const glm::vec3 originOffset = -sweepDir * vp.overrunStart;
            const gp_Trsf   offsetTrsf = [&]() {
                gp_Trsf t;
                t.SetTranslation(gp_Vec(originOffset.x, originOffset.y, originOffset.z));
                return t;
                }();
            const TopoDS_Shape offsetFace =
                BRepBuilderAPI_Transform(face.Face(), offsetTrsf, /*copy=*/true).Shape();

            const float     totalLen = rawLen + vp.overrunStart + vp.overrunEnd;
            const glm::vec3 totalSweep = sweepDir * totalLen;
            const gp_Vec    sweepVec(totalSweep.x, totalSweep.y, totalSweep.z);

            BRepPrimAPI_MakePrism prism(offsetFace, sweepVec);
            if (!prism.IsDone() || prism.Shape().IsNull()) continue;

            BRepAlgoAPI_Cut ventCut(result, prism.Shape());
            ventCut.Build();
            if (!ventCut.IsDone() || ventCut.Shape().IsNull())
            {
                wxMessageBox("Vent cut failed for vent " + std::to_string(vi + 1),
                    "Generate Mould", wxOK | wxICON_WARNING, this);
                continue;
            }
            result = ventCut.Shape();
        }

        // Steps: subtract each runner (cylinder from sprue parting point to runner point)
        // plus cold plug well extending past the endpoint
        if (m_sprue.hasPartingPoint)
        {
            // Read runner dimensions from UI (same source as the preview solids)
            float runnerRadius = 2.5f;
            float coldPlugDist = 5.0f;
            if (auto* frame = dynamic_cast<MainFrame*>(wxGetTopLevelParent(this)))
            {
                runnerRadius = frame->GetRunnerDiameter() * 0.5f;
                coldPlugDist = frame->GetRunnerColdPlugDist();
            }

            for (int ri = 0; ri < (int)m_runners.size(); ++ri)
            {
                progress.Update(step++,
                    fixLabel + ": cutting runner " +
                    std::to_string(ri + 1) + " of " +
                    std::to_string((int)m_runners.size()) + "...");

                const glm::vec3& rp = m_runners[ri].point;
                const glm::vec3 runnerAxis = rp - m_sprue.partingPos;
                const float runnerLen = glm::length(runnerAxis);
                if (runnerLen < 1e-6f || runnerRadius < 1e-6f) continue;

                const glm::vec3 runnerDir = runnerAxis / runnerLen;

                // Runner cylinder: pulled back by kCutEps so it protrudes through
                // both parting faces and avoids co-planar boundary failures.
                static constexpr float kCutEps = 0.1f;
                const glm::vec3 runnerOriginExt = m_sprue.partingPos - runnerDir * kCutEps;
                gp_Ax2 runnerAx(
                    gp_Pnt(runnerOriginExt.x, runnerOriginExt.y, runnerOriginExt.z),
                    gp_Dir(runnerDir.x, runnerDir.y, runnerDir.z)
                );

                BRepPrimAPI_MakeCylinder runnerCyl(runnerAx, runnerRadius, runnerLen + 2.0f * kCutEps);
                runnerCyl.Build();
                if (!runnerCyl.IsDone() || runnerCyl.Shape().IsNull()) continue;

                BRepAlgoAPI_Cut runnerCut(result, runnerCyl.Shape());
                runnerCut.Build();
                if (!runnerCut.IsDone() || runnerCut.Shape().IsNull())
                {
                    wxMessageBox("Runner cut failed for runner " + std::to_string(ri + 1),
                        "Generate Mould", wxOK | wxICON_WARNING, this);
                    continue;
                }
                result = runnerCut.Shape();

                // Cold plug well — extends past the runner endpoint along the
                // same direction.  Not part of the runner path so future
                // sub-runners won't attempt to use this reserved space.
                if (coldPlugDist > 1e-6f)
                {
                    gp_Ax2 plugAx(
                        gp_Pnt(rp.x, rp.y, rp.z),
                        gp_Dir(runnerDir.x, runnerDir.y, runnerDir.z)
                    );

                    BRepPrimAPI_MakeCylinder plugCyl(plugAx, runnerRadius, coldPlugDist);
                    plugCyl.Build();
                    if (plugCyl.IsDone() && !plugCyl.Shape().IsNull())
                    {
                        BRepAlgoAPI_Cut plugCut(result, plugCyl.Shape());
                        plugCut.Build();
                        if (plugCut.IsDone() && !plugCut.Shape().IsNull())
                            result = plugCut.Shape();
                        else
                            wxMessageBox("Cold plug cut failed for runner " + std::to_string(ri + 1),
                                "Generate Mould", wxOK | wxICON_WARNING, this);
                    }
                }
            }
        }
        else
        {
            // No sprue parting point — skip runner steps but still advance progress
            step += (int)m_runners.size();
        }

        // Steps: subtract each gate (tapered frustum) and its sub-runner cylinder
        {
            float gateRadius = 1.5f;
            float draftAngle = 1.0f;
            float subRunnerRadius = 2.5f;
            float overrun = 0.0f;
            if (auto* frame = dynamic_cast<MainFrame*>(wxGetTopLevelParent(this)))
            {
                gateRadius = frame->GetGateDiameter() * 0.5f;
                draftAngle = frame->GetGateDraftAngle();
                subRunnerRadius = frame->GetSubRunnerDiameter() * 0.5f;
                overrun = frame->GetGateOverrun();
            }

            const float draftRad = glm::radians(glm::clamp(draftAngle, 0.0f, 45.0f));
            const float tanDraft = std::tan(draftRad);

            for (int gi = 0; gi < (int)m_gates.size(); ++gi)
            {
                progress.Update(step++,
                    fixLabel + ": cutting gate " +
                    std::to_string(gi + 1) + " of " +
                    std::to_string((int)m_gates.size()) + "...");

                const GateFeature& gf = m_gates[gi];
                if (!gf.hasPath) continue;

                const glm::vec3 origin = gf.point.worldPos;
                const glm::vec3 pathVec = gf.pathEnd - origin;
                const float     totalLen = glm::length(pathVec);
                if (totalLen < 1e-6f || gateRadius < 1e-6f) continue;
                const glm::vec3 pathDir = pathVec / totalLen;

                const gp_Pnt occOrigin(origin.x, origin.y, origin.z);
                const gp_Dir occDir(pathDir.x, pathDir.y, pathDir.z);

                // Back-extension along -pathDir into the model, combining two
                // contributions: a small constant kCutEps so the primitive
                // protrudes through the parting surface and avoids co-planar
                // boundary failures, plus the user-controlled `overrun` field
                // (mm) which extends the cut deeper into the model body to
                // clear irregular perimeter geometry. The two add: overrun=0
                // recovers the legacy behaviour, while a non-zero overrun
                // produces the same shape as the preview built in
                // RebuildGateSolids.
                static constexpr float kCutEps = 0.1f;
                const float backExt = kCutEps + overrun;
                const glm::vec3 originExt = origin - pathDir * backExt;
                const float     totalLenExt = totalLen + backExt;
                const gp_Pnt    occOriginExt(originExt.x, originExt.y, originExt.z);
                const gp_Ax2    gateAxExt(occOriginExt, occDir);

                // Radius at originExt, derived so the radius at the original
                // parting-surface point (origin) is exactly gateRadius — i.e.
                // the user-typed Diameter is the diameter of the inlet hole
                // at the parting surface, regardless of overrun. Floor at
                // 0.01 mm so aggressive overrun + draft combos can't drive
                // the back radius to zero (BRepPrimAPI rejects degenerate
                // primitives). Matches the preview math in RebuildGateSolids.
                const float startRadius = std::max(0.01f,
                    gateRadius - tanDraft * backExt);

                // Distance along path where draft expands to sub-runner radius
                float taperLen = std::numeric_limits<float>::max();
                if (tanDraft > 1e-6f && subRunnerRadius > gateRadius)
                    taperLen = (subRunnerRadius - gateRadius) / tanDraft;

                // Adjust taperLen relative to the extended origin
                const float taperLenExt = taperLen + backExt;

                if (taperLen >= totalLen)
                {
                    // Gate fills the whole path — single cone/cylinder cut.
                    // End radius at pathEnd is the geometric extension from
                    // startRadius over totalLenExt, which simplifies to
                    // gateRadius + totalLen*tanDraft (the back-extension
                    // contributions cancel by construction of startRadius).
                    const float endR = startRadius + totalLenExt * tanDraft;
                    TopoDS_Shape gateShape;
                    if (std::abs(endR - startRadius) > 1e-6f)
                    {
                        BRepPrimAPI_MakeCone cone(gateAxExt, startRadius, endR, totalLenExt);
                        cone.Build();
                        if (cone.IsDone() && !cone.Shape().IsNull())
                            gateShape = cone.Shape();
                    }
                    else
                    {
                        BRepPrimAPI_MakeCylinder cyl(gateAxExt, startRadius, totalLenExt);
                        cyl.Build();
                        if (cyl.IsDone() && !cyl.Shape().IsNull())
                            gateShape = cyl.Shape();
                    }

                    if (!gateShape.IsNull())
                    {
                        BRepAlgoAPI_Cut gateCut(result, gateShape);
                        gateCut.Build();
                        if (gateCut.IsDone() && !gateCut.Shape().IsNull())
                            result = gateCut.Shape();
                        else
                            wxMessageBox("Gate cut failed for gate " + std::to_string(gi + 1),
                                "Generate Mould", wxOK | wxICON_WARNING, this);
                    }
                }
                else
                {
                    // --- Gate frustum (extended origin → transition point) ---
                    // End radius at the transition is exactly subRunnerRadius
                    // by definition of taperLen — the back-extension shifts
                    // the start without changing where the taper hits
                    // subRunnerRadius in world space.
                    {
                        BRepPrimAPI_MakeCone cone(gateAxExt, startRadius, subRunnerRadius, taperLenExt);
                        cone.Build();
                        if (cone.IsDone() && !cone.Shape().IsNull())
                        {
                            BRepAlgoAPI_Cut gateCut(result, cone.Shape());
                            gateCut.Build();
                            if (gateCut.IsDone() && !gateCut.Shape().IsNull())
                                result = gateCut.Shape();
                            else
                                wxMessageBox("Gate frustum cut failed for gate " + std::to_string(gi + 1),
                                    "Generate Mould", wxOK | wxICON_WARNING, this);
                        }
                    }

                    // --- Sub-runner cylinder (transition point → pathEnd) ---
                    {
                        const glm::vec3 transitionPt = originExt + pathDir * taperLenExt;
                        const float     subLen = totalLenExt - taperLenExt;
                        const gp_Ax2    subAx(
                            gp_Pnt(transitionPt.x, transitionPt.y, transitionPt.z),
                            occDir);

                        BRepPrimAPI_MakeCylinder subCyl(subAx, subRunnerRadius, subLen);
                        subCyl.Build();
                        if (subCyl.IsDone() && !subCyl.Shape().IsNull())
                        {
                            BRepAlgoAPI_Cut subCut(result, subCyl.Shape());
                            subCut.Build();
                            if (subCut.IsDone() && !subCut.Shape().IsNull())
                                result = subCut.Shape();
                            else
                                wxMessageBox("Sub-runner cut failed for gate " + std::to_string(gi + 1),
                                    "Generate Mould", wxOK | wxICON_WARNING, this);
                        }
                    }
                }
            }
        }

        // Step: subtract ejector pins
        //
        // Each ejector is a straight cylinder extruded in -Y from the snap
        // point. The cylinder is overrun by kCutEps on BOTH ends — past the
        // snap point and past the nominal endpoint — so that it punches
        // cleanly through whatever parting / exit faces it crosses, rather
        // than leaving sealed pockets. OCC's cut handles this gracefully:
        // a fixture that doesn't intersect the cylinder is unchanged, so we
        // can apply the same cut to every fixture without per-half logic
        // (an ejector that snapped to y=0 only crosses the B half; one
        // snapped to a face high in the A half cuts through both halves on
        // its way down — both are correct for ejector geometry).
        float ejectorRadius = 1.5f;
        float ejectorLen = 25.0f;
        if (auto* frame = dynamic_cast<MainFrame*>(wxGetTopLevelParent(this)))
        {
            ejectorRadius = frame->GetEjectorDiameter() * 0.5f;
            ejectorLen = frame->GetEjectorLength();
        }
        if (!m_ejectors.empty() && ejectorRadius > 1e-6f && ejectorLen > 1e-6f)
        {
            for (int ei = 0; ei < (int)m_ejectors.size(); ++ei)
            {
                progress.Update(step++,
                    fixLabel + ": cutting ejector " +
                    std::to_string(ei + 1) + " of " +
                    std::to_string((int)m_ejectors.size()) + "...");

                static constexpr float kCutEps = 0.1f;
                const glm::vec3 epPt = m_ejectors[ei].point;
                // Origin lifted by kCutEps in +Y so the cylinder protrudes
                // above the snap point; total length includes that overrun
                // plus an equal amount past the bottom.
                const gp_Ax2 ejAx(
                    gp_Pnt(epPt.x, epPt.y + kCutEps, epPt.z),
                    gp_Dir(0.0, -1.0, 0.0)
                );
                BRepPrimAPI_MakeCylinder ejCyl(ejAx, ejectorRadius,
                    ejectorLen + 2.0f * kCutEps);
                ejCyl.Build();
                if (!ejCyl.IsDone() || ejCyl.Shape().IsNull()) continue;

                BRepAlgoAPI_Cut ejCut(result, ejCyl.Shape());
                ejCut.Build();
                if (!ejCut.IsDone() || ejCut.Shape().IsNull())
                {
                    wxMessageBox("Ejector cut failed for ejector " + std::to_string(ei + 1),
                        "Generate Mould", wxOK | wxICON_WARNING, this);
                    continue;
                }
                result = ejCut.Shape();
            }
        }
        else
        {
            // No ejectors / degenerate dimensions — advance progress
            // counter so the bar stays in step with the loop accounting.
            step += (int)m_ejectors.size();
        }

        // Step: subtract sprue (frustum + cold slug)
        progress.Update(step++, fixLabel + ": cutting sprue...");

        if (m_sprue.hasPoint)
        {
            const glm::vec3 sprueAxis = m_sprue.pathEnd - m_sprue.pathStart;
            const float     sprueLen = glm::length(sprueAxis);

            if (sprueLen > 1e-6f)
            {
                const glm::vec3 dir = sprueAxis / sprueLen;

                // OCC axis at sprue start, pointing toward sprue end
                // Pull the sprue start back by kCutEps so it protrudes through
                // the top face of the fixture and avoids co-planar failures.
                static constexpr float kCutEps = 0.1f;
                const glm::vec3 sprueStartExt = m_sprue.pathStart - dir * kCutEps;
                const float     sprueExtLen = sprueLen + kCutEps;  // extra only at entry

                gp_Ax2 sprueAx(
                    gp_Pnt(sprueStartExt.x, sprueStartExt.y, sprueStartExt.z),
                    gp_Dir(dir.x, dir.y, dir.z)
                );

                const float draftRad = glm::radians(glm::clamp(m_sprue.draftAngleDeg, 0.0f, 45.0f));
                const float startR = m_sprue.radius;
                const float endR = m_sprue.radius + sprueExtLen * std::tan(draftRad);

                // Build the drafted sprue solid (frustum or cylinder)
                TopoDS_Shape sprueCutShape;
                bool sprueShapeOK = false;

                if (std::abs(endR - startR) > 1e-6f)
                {
                    // Tapered — use a cone
                    BRepPrimAPI_MakeCone cone(sprueAx, startR, endR, sprueExtLen);
                    cone.Build();
                    if (cone.IsDone() && !cone.Shape().IsNull())
                    {
                        sprueCutShape = cone.Shape();
                        sprueShapeOK = true;
                    }
                }
                else
                {
                    // No taper — straight cylinder
                    BRepPrimAPI_MakeCylinder cyl(sprueAx, startR, sprueExtLen);
                    cyl.Build();
                    if (cyl.IsDone() && !cyl.Shape().IsNull())
                    {
                        sprueCutShape = cyl.Shape();
                        sprueShapeOK = true;
                    }
                }

                if (sprueShapeOK)
                {
                    BRepAlgoAPI_Cut sprueCut(result, sprueCutShape);
                    sprueCut.Build();
                    if (sprueCut.IsDone() && !sprueCut.Shape().IsNull())
                        result = sprueCut.Shape();
                    else
                        wxMessageBox("Sprue boolean cut failed.",
                            "Generate Mould", wxOK | wxICON_WARNING, this);
                }

                // Cold slug well — straight cylinder extending past the sprue end
                if (!m_sprue.isDirectInjection && m_sprue.coldSlugDepth > 1e-6f)
                {
                    gp_Ax2 slugAx(
                        gp_Pnt(m_sprue.pathEnd.x, m_sprue.pathEnd.y, m_sprue.pathEnd.z),
                        gp_Dir(dir.x, dir.y, dir.z)
                    );

                    BRepPrimAPI_MakeCylinder slugCyl(slugAx, endR, m_sprue.coldSlugDepth);
                    slugCyl.Build();
                    if (slugCyl.IsDone() && !slugCyl.Shape().IsNull())
                    {
                        BRepAlgoAPI_Cut slugCut(result, slugCyl.Shape());
                        slugCut.Build();
                        if (slugCut.IsDone() && !slugCut.Shape().IsNull())
                            result = slugCut.Shape();
                        else
                            wxMessageBox("Cold slug boolean cut failed.",
                                "Generate Mould", wxOK | wxICON_WARNING, this);
                    }
                }
            }
        }

        // Step: tessellate + upload
        progress.Update(step++, fixLabel + ": tessellating result...");

        fix.mouldShape = result;
        fix.hasMould = true;

        BRepMesh_IncrementalMesh mesher(result, 0.05, false, 0.5, true);

        FileImporter::MeshData meshData;
        meshData.aabbMin = glm::vec3(std::numeric_limits<float>::infinity());
        meshData.aabbMax = glm::vec3(-std::numeric_limits<float>::infinity());

        for (TopExp_Explorer ex(result, TopAbs_FACE); ex.More(); ex.Next())
        {
            const TopoDS_Face face = TopoDS::Face(ex.Current());
            TopLoc_Location loc;
            Handle(Poly_Triangulation) tri = BRep_Tool::Triangulation(face, loc);
            if (tri.IsNull()) continue;

            const gp_Trsf tr = loc.Transformation();
            const uint32_t baseIndex = (uint32_t)(meshData.vertices.size() / 3);

            for (int i = 1; i <= tri->NbNodes(); ++i)
            {
                gp_Pnt p = tri->Node(i);
                p.Transform(tr);
                meshData.vertices.push_back((float)p.X());
                meshData.vertices.push_back((float)p.Y());
                meshData.vertices.push_back((float)p.Z());
            }
            for (int t = 1; t <= tri->NbTriangles(); ++t)
            {
                int n1, n2, n3;
                tri->Triangle(t).Get(n1, n2, n3);
                meshData.indices.push_back(baseIndex + (uint32_t)(n1 - 1));
                meshData.indices.push_back(baseIndex + (uint32_t)(n2 - 1));
                meshData.indices.push_back(baseIndex + (uint32_t)(n3 - 1));
            }
        }

        if (meshData.vertices.empty() || meshData.indices.empty())
            continue;

        ComputeVertexNormals_Pos3(meshData.vertices, meshData.indices, meshData.posNorm);
        auto split = SplitByCreaseAngle_Pos3(meshData.vertices, meshData.indices, 35.0f);
        meshData.posNorm = std::move(split.posNorm);
        meshData.indices = std::move(split.indices);

        // Hand the tessellated half off to the preview window instead of
        // replacing this fixture's display mesh. fix.mouldShape / hasMould
        // were already set above (before the empty-mesh guard) for export.
        // Per the preview workflow the main canvas keeps showing the original
        // (pre-cut) fixtures at their authored poses — it must not change the
        // mould halves after generation. The mesh captured here is already in
        // world space (the fixture transform was baked into `result` above),
        // so the preview renders it at an identity pose.
        m_lastMouldMeshes.push_back(meshData);

        // Retain the half solid (world space, post-cut) for the separation
        // demoldability check, kept in lockstep with m_lastMouldMeshes.
        m_lastHalfShapes.push_back(result);
    }

    // ---- Shot model -------------------------------------------------------
    // Build the body of injected material (objects + feed system, excluding
    // vents and ejectors) and tessellate it for the preview / future
    // simulation. Done once after the per-fixture loop since the shot is a
    // single world-space body independent of which half it sits between. A
    // failed or empty build just leaves m_hasLastShotMesh false — generation
    // still succeeds; the preview simply won't show a "Shot" part.
    {
        TopoDS_Shape shotShape;
        if (BuildShotModel(shotShape))
        {
            // Volume straight off the fused BREP (not the mesh) — accurate and
            // overlap-safe. Geometry is in mm, so this is cubic mm.
            GProp_GProps volProps;
            BRepGProp::VolumeProperties(shotShape, volProps);
            m_lastShotVolumeMm3 = std::abs(volProps.Mass());

            // Retain the BREP for face-level design checks, and tessellate with
            // a per-triangle face map so face results map back to display tris.
            m_lastShotShape = shotShape;
            TessellateShapeToMesh(shotShape, m_lastShotMesh, &m_lastShotFaceIds);
            m_hasLastShotMesh =
                !m_lastShotMesh.posNorm.empty() && !m_lastShotMesh.indices.empty();
        }
    }

    progress.Update(totalSteps, "Done.");
    Refresh(false);
    wxMessageBox("Mould generated successfully.",
        "Generate Mould", wxOK | wxICON_INFORMATION, this);
    return true;
}

// ===========================================================================
// Preview perspective
//
// These run on the dedicated preview canvas (m_previewMode == true). The
// preview reuses this class's GL setup, shaders, grid and orbit camera so the
// navigation matches the main canvas exactly; it just renders a stripped-down
// scene (grid + the loaded mould halves) and ignores all editing input.
// ===========================================================================

void GLCanvas::AddPreviewHalf(const FileImporter::MeshData& mesh,
    const std::string& label,
    const glm::vec3& baseColor)
{
    // Materialise GPU buffers in this canvas's context. The caller (PreviewFrame)
    // makes the context current before invoking us, but make sure regardless —
    // UploadMeshToGPU issues GL calls that must hit the right context.
    SetCurrent(*m_context);
    InitGLOnce();

    PreviewHalf half;
    half.label = label;
    half.visible = true;
    half.baseColor = baseColor;
    // Identity pose: the mesh vertices are already in world space.
    half.obj.pos = glm::vec3(0.0f);
    half.obj.yawDeg = half.obj.pitchDeg = half.obj.rollDeg = 0.0f;
    half.obj.scale = 1.0f;
    half.obj.role = ObjectRole::Fixture;

    UploadMeshToGPU(mesh, half.obj);

    m_previewHalves.push_back(std::move(half));
    Refresh(false);
}

std::string GLCanvas::GetPreviewHalfLabel(int index) const
{
    if (index < 0 || index >= (int)m_previewHalves.size()) return std::string();
    return m_previewHalves[index].label;
}

void GLCanvas::SetPreviewHalfVisible(int index, bool visible)
{
    if (index < 0 || index >= (int)m_previewHalves.size()) return;
    m_previewHalves[index].visible = visible;
    Refresh(false);
}

bool GLCanvas::IsPreviewHalfVisible(int index) const
{
    if (index < 0 || index >= (int)m_previewHalves.size()) return false;
    return m_previewHalves[index].visible;
}

void GLCanvas::ClearPreviewHalves()
{
    if (!m_previewHalves.empty() && m_context)
        SetCurrent(*m_context);
    for (auto& h : m_previewHalves)
        h.obj.mesh.Destroy();
    m_previewHalves.clear();
    Refresh(false);
}

void GLCanvas::SetShotDebugGroups(int halfIndex,
    const std::vector<ShotDebugGroup>& groups)
{
    if (halfIndex < 0 || halfIndex >= (int)m_previewHalves.size()) return;
    const GPUMesh& src = m_previewHalves[halfIndex].obj.mesh;
    if (src.vbo == 0) return;

    SetCurrent(*m_context);
    InitGLOnce();

    // Tear down any previous debug buffers so repeated calls don't leak.
    for (auto& g : m_shotDebug.groups)
        if (g.ebo) { glDeleteBuffers(1, &g.ebo); g.ebo = 0; }
    m_shotDebug.groups.clear();
    if (m_shotDebug.vao) { glDeleteVertexArrays(1, &m_shotDebug.vao); m_shotDebug.vao = 0; }

    // Debug VAO references the part's existing vertex buffer (pos + normal,
    // 6-float stride) so lighting matches the normal pass; only the element
    // buffer changes between groups.
    glGenVertexArrays(1, &m_shotDebug.vao);
    glBindVertexArray(m_shotDebug.vao);
    glBindBuffer(GL_ARRAY_BUFFER, src.vbo);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 6 * (GLsizei)sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 6 * (GLsizei)sizeof(float),
        (void*)(3 * sizeof(float)));
    glEnableVertexAttribArray(1);

    for (const ShotDebugGroup& grp : groups)
    {
        if (grp.indices.empty()) continue;
        DebugGroupGPU gpu;
        gpu.color = grp.color;
        gpu.count = (GLsizei)grp.indices.size();
        glGenBuffers(1, &gpu.ebo);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, gpu.ebo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER,
            (GLsizeiptr)(grp.indices.size() * sizeof(uint32_t)),
            grp.indices.data(), GL_STATIC_DRAW);
        m_shotDebug.groups.push_back(gpu);
    }

    glBindVertexArray(0);

    m_shotDebug.halfIndex = halfIndex;
    m_shotDebug.active = true;
    Refresh(false);
}

void GLCanvas::ClearShotDebugColoring()
{
    m_shotDebug.active = false;
    m_shotDebug.halfIndex = -1;
    Refresh(false);
}

void GLCanvas::SetShotDebugRays(const std::vector<glm::vec3>& rayLineVerts,
    const std::vector<glm::vec3>& contactVerts)
{
    SetCurrent(*m_context);
    InitGLOnce();

    auto upload = [&](GLuint& vao, GLuint& vbo, GLsizei& count,
        const std::vector<glm::vec3>& verts)
    {
        if (vao == 0) glGenVertexArrays(1, &vao);
        if (vbo == 0) glGenBuffers(1, &vbo);
        glBindVertexArray(vao);
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glBufferData(GL_ARRAY_BUFFER,
            (GLsizeiptr)(verts.size() * sizeof(glm::vec3)),
            verts.empty() ? nullptr : verts.data(), GL_DYNAMIC_DRAW);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * (GLsizei)sizeof(float), (void*)0);
        glEnableVertexAttribArray(0);
        glBindVertexArray(0);
        count = (GLsizei)verts.size();
    };

    upload(m_debugRayVao, m_debugRayVbo, m_debugRayVertCount, rayLineVerts);
    upload(m_debugContactVao, m_debugContactVbo, m_debugContactVertCount, contactVerts);
    Refresh(false);
}

void GLCanvas::ShowShotDebugRays(bool on)
{
    m_showDebugRays = on;
    Refresh(false);
}

void GLCanvas::ShowShotDebugContacts(bool on)
{
    m_showDebugContacts = on;
    Refresh(false);
}

void GLCanvas::ClearShotDebugRays()
{
    m_showDebugRays = false;
    m_showDebugContacts = false;
    m_debugRayVertCount = 0;
    m_debugContactVertCount = 0;
    Refresh(false);
}

void GLCanvas::SetShotDebugSolid(const TopoDS_Shape& shape, const glm::vec3& color)
{
    SetCurrent(*m_context);
    InitGLOnce();

    m_debugSolidObj.mesh.Destroy();
    m_debugSolidColor = color;

    if (shape.IsNull()) { m_showDebugSolid = false; Refresh(false); return; }

    FileImporter::MeshData mesh;
    TessellateShapeToMesh(shape, mesh);
    if (mesh.posNorm.empty() || mesh.indices.empty())
    {
        m_showDebugSolid = false;
        Refresh(false);
        return;
    }
    UploadMeshToGPU(mesh, m_debugSolidObj);
    Refresh(false);
}

void GLCanvas::ShowShotDebugSolid(bool on)
{
    m_showDebugSolid = on;
    Refresh(false);
}

void GLCanvas::ClearShotDebugSolid()
{
    m_showDebugSolid = false;
    m_debugSolidObj.mesh.Destroy();
    Refresh(false);
}

void GLCanvas::RenderPreview(const glm::mat4& view, const glm::mat4& proj,
    const glm::vec3& camPos)
{
    // Lit shader uniforms — mirror the main scene pass so the halves are shaded
    // identically to how objects look in the editor.
    glUseProgram(m_program);

    const glm::vec3 lightDir = glm::normalize(glm::vec3(0.4f, 0.8f, 0.2f));
    const glm::vec3 lightColor = glm::vec3(1.0f);

    glUniform3fv(glGetUniformLocation(m_program, "uCameraPos"), 1, &camPos[0]);
    glUniform3fv(glGetUniformLocation(m_program, "uLightDir"), 1, &lightDir[0]);
    glUniform3fv(glGetUniformLocation(m_program, "uLightColor"), 1, &lightColor[0]);
    glUniform1f(glGetUniformLocation(m_program, "uAmbient"), 0.25f);
    glUniform1f(glGetUniformLocation(m_program, "uDiffuse"), 0.85f);
    glUniform1f(glGetUniformLocation(m_program, "uSpecular"), 0.20f);
    glUniform1f(glGetUniformLocation(m_program, "uShininess"), 64.0f);
    glUniform1f(glGetUniformLocation(m_program, "uAlpha"), 1.0f);
    glUniformMatrix4fv(glGetUniformLocation(m_program, "uView"), 1, GL_FALSE, &view[0][0]);
    glUniformMatrix4fv(glGetUniformLocation(m_program, "uProj"), 1, GL_FALSE, &proj[0][0]);

    const GLint locBase = glGetUniformLocation(m_program, "uBaseColor");
    const GLint locModel = glGetUniformLocation(m_program, "uModel");

    for (int hi = 0; hi < (int)m_previewHalves.size(); ++hi)
    {
        const PreviewHalf& half = m_previewHalves[hi];
        if (!half.visible) continue;
        if (half.obj.mesh.vao == 0 || half.obj.mesh.indexCount == 0) continue;

        const glm::mat4 model = half.obj.BuildModelMatrix();
        glUniformMatrix4fv(locModel, 1, GL_FALSE, &model[0][0]);

        // Debug colouring: draw this part as flat-coloured groups over a debug
        // VAO instead of its normal single colour.
        if (m_shotDebug.active && hi == m_shotDebug.halfIndex && m_shotDebug.vao)
        {
            glBindVertexArray(m_shotDebug.vao);
            for (const DebugGroupGPU& g : m_shotDebug.groups)
            {
                if (g.count <= 0) continue;
                glUniform3fv(locBase, 1, &g.color[0]);
                glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g.ebo);
                glDrawElements(GL_TRIANGLES, g.count, GL_UNSIGNED_INT, 0);
            }
            glBindVertexArray(0);
            continue;
        }

        // Per-part base colour — mould halves render grey, the shot reads
        // distinctly (set by its AddPreviewHalf caller).
        glUniform3fv(locBase, 1, &half.baseColor[0]);

        glBindVertexArray(half.obj.mesh.vao);
        glDrawElements(GL_TRIANGLES, half.obj.mesh.indexCount, GL_UNSIGNED_INT, 0);
        glBindVertexArray(0);
    }

    // Free-standing lit debug solid (separation-test interference region).
    if (m_showDebugSolid && m_debugSolidObj.mesh.vao &&
        m_debugSolidObj.mesh.indexCount > 0)
    {
        const glm::mat4 model(1.0f);  // already world space
        glUniformMatrix4fv(locModel, 1, GL_FALSE, &model[0][0]);
        glUniform3fv(locBase, 1, &m_debugSolidColor[0]);
        glBindVertexArray(m_debugSolidObj.mesh.vao);
        glDrawElements(GL_TRIANGLES, m_debugSolidObj.mesh.indexCount, GL_UNSIGNED_INT, 0);
        glBindVertexArray(0);
    }

    glUseProgram(0);

    // Accessibility-ray debug overlay (flat shader): ray segments + contacts.
    if (m_flatProgram &&
        ((m_showDebugRays && m_debugRayVertCount > 0) ||
         (m_showDebugContacts && m_debugContactVertCount > 0)))
    {
        glUseProgram(m_flatProgram);
        const glm::mat4 VP = proj * view;
        glUniformMatrix4fv(m_flat_uVP, 1, GL_FALSE, &VP[0][0]);

        if (m_showDebugRays && m_debugRayVao && m_debugRayVertCount > 0)
        {
            const glm::vec4 rayColor(1.0f, 0.85f, 0.10f, 1.0f);  // yellow
            glUniform4fv(m_flat_uColor, 1, &rayColor[0]);
            glLineWidth(2.0f);
            glBindVertexArray(m_debugRayVao);
            glDrawArrays(GL_LINES, 0, m_debugRayVertCount);
            glBindVertexArray(0);
            glLineWidth(1.0f);
        }
        if (m_showDebugContacts && m_debugContactVao && m_debugContactVertCount > 0)
        {
            const glm::vec4 contactColor(0.95f, 0.12f, 0.12f, 1.0f);  // red
            glUniform4fv(m_flat_uColor, 1, &contactColor[0]);
            glPointSize(9.0f);
            glBindVertexArray(m_debugContactVao);
            glDrawArrays(GL_POINTS, 0, m_debugContactVertCount);
            glBindVertexArray(0);
            glPointSize(1.0f);
        }
        glUseProgram(0);
    }

    // Grid last, same as the main canvas.
    m_grid.Draw(view, proj);
}

// ---------------------------------------------------------------------------
// BuildShotModel — fuse imported objects + feed-system features into the shot.
//
// The shot is the body of material that fills the cavity and feed network: it
// is the union of everything the cut loop subtracts from the mould EXCEPT the
// vents and ejectors. Each primitive is built in world space with the same
// geometry the cut loop uses (kCutEps overlaps included, which only help the
// fuse), then fused pairwise.
//
// NOTE (known duplication): the per-feature geometry below intentionally
// mirrors the construction in GenerateMould's cut loop. They are kept separate
// for now so this addition can't regress mould generation; a future pass could
// factor the shared primitive builders out and have both call them.
// ---------------------------------------------------------------------------
bool GLCanvas::BuildShotModel(TopoDS_Shape& out)
{
    std::vector<TopoDS_Shape> shapes;

    // ---- Imported objects (transformed to world space) --------------------
    for (const SceneObject& obj : m_objects)
    {
        if (obj.sourcePath.empty() && !obj.hasSourceShape) continue;

        TopoDS_Shape objShape;
        if (obj.hasSourceShape)
        {
            objShape = obj.sourceShape;
        }
        else
        {
            STEPControl_Reader objReader;
            if (objReader.ReadFile(obj.sourcePath.c_str()) != IFSelect_RetDone)
                continue;
            objReader.TransferRoots();
            objShape = objReader.OneShape();
        }
        if (objShape.IsNull()) continue;

        gp_Trsf objTrsf;
        glm::mat4 om = obj.BuildModelMatrix();
        objTrsf.SetValues(
            om[0][0], om[1][0], om[2][0], om[3][0],
            om[0][1], om[1][1], om[2][1], om[3][1],
            om[0][2], om[1][2], om[2][2], om[3][2]
        );
        BRepBuilderAPI_Transform objXform(objShape, objTrsf, true);
        if (!objXform.Shape().IsNull())
            shapes.push_back(objXform.Shape());
    }

    static constexpr float kCutEps = 0.1f;

    // ---- Sprue frustum + cold slug ----------------------------------------
    if (m_sprue.hasPoint)
    {
        const glm::vec3 sprueAxis = m_sprue.pathEnd - m_sprue.pathStart;
        const float     sprueLen = glm::length(sprueAxis);
        if (sprueLen > 1e-6f)
        {
            const glm::vec3 dir = sprueAxis / sprueLen;
            const glm::vec3 sprueStartExt = m_sprue.pathStart - dir * kCutEps;
            const float     sprueExtLen = sprueLen + kCutEps;

            gp_Ax2 sprueAx(
                gp_Pnt(sprueStartExt.x, sprueStartExt.y, sprueStartExt.z),
                gp_Dir(dir.x, dir.y, dir.z));

            const float draftRad = glm::radians(glm::clamp(m_sprue.draftAngleDeg, 0.0f, 45.0f));
            const float startR = m_sprue.radius;
            const float endR = m_sprue.radius + sprueExtLen * std::tan(draftRad);

            if (std::abs(endR - startR) > 1e-6f)
            {
                BRepPrimAPI_MakeCone cone(sprueAx, startR, endR, sprueExtLen);
                cone.Build();
                if (cone.IsDone() && !cone.Shape().IsNull())
                    shapes.push_back(cone.Shape());
            }
            else
            {
                BRepPrimAPI_MakeCylinder cyl(sprueAx, startR, sprueExtLen);
                cyl.Build();
                if (cyl.IsDone() && !cyl.Shape().IsNull())
                    shapes.push_back(cyl.Shape());
            }

            if (!m_sprue.isDirectInjection && m_sprue.coldSlugDepth > 1e-6f)
            {
                gp_Ax2 slugAx(
                    gp_Pnt(m_sprue.pathEnd.x, m_sprue.pathEnd.y, m_sprue.pathEnd.z),
                    gp_Dir(dir.x, dir.y, dir.z));
                BRepPrimAPI_MakeCylinder slugCyl(slugAx, endR, m_sprue.coldSlugDepth);
                slugCyl.Build();
                if (slugCyl.IsDone() && !slugCyl.Shape().IsNull())
                    shapes.push_back(slugCyl.Shape());
            }
        }
    }

    // Feed dimensions from the side panel — same source as the cut loop.
    float runnerRadius = 2.5f, coldPlugDist = 5.0f;
    float gateRadius = 1.5f, draftAngle = 1.0f, subRunnerRadius = 2.5f, overrun = 0.0f;
    if (auto* frame = dynamic_cast<MainFrame*>(wxGetTopLevelParent(this)))
    {
        runnerRadius = frame->GetRunnerDiameter() * 0.5f;
        coldPlugDist = frame->GetRunnerColdPlugDist();
        gateRadius = frame->GetGateDiameter() * 0.5f;
        draftAngle = frame->GetGateDraftAngle();
        subRunnerRadius = frame->GetSubRunnerDiameter() * 0.5f;
        overrun = frame->GetGateOverrun();
    }

    // ---- Runner cylinders + cold plugs ------------------------------------
    if (m_sprue.hasPartingPoint && runnerRadius > 1e-6f)
    {
        for (const RunnerFeature& rf : m_runners)
        {
            const glm::vec3 runnerAxis = rf.point - m_sprue.partingPos;
            const float runnerLen = glm::length(runnerAxis);
            if (runnerLen < 1e-6f) continue;
            const glm::vec3 runnerDir = runnerAxis / runnerLen;

            const glm::vec3 runnerOriginExt = m_sprue.partingPos - runnerDir * kCutEps;
            gp_Ax2 runnerAx(
                gp_Pnt(runnerOriginExt.x, runnerOriginExt.y, runnerOriginExt.z),
                gp_Dir(runnerDir.x, runnerDir.y, runnerDir.z));
            BRepPrimAPI_MakeCylinder runnerCyl(runnerAx, runnerRadius, runnerLen + 2.0f * kCutEps);
            runnerCyl.Build();
            if (runnerCyl.IsDone() && !runnerCyl.Shape().IsNull())
                shapes.push_back(runnerCyl.Shape());

            if (coldPlugDist > 1e-6f)
            {
                gp_Ax2 plugAx(
                    gp_Pnt(rf.point.x, rf.point.y, rf.point.z),
                    gp_Dir(runnerDir.x, runnerDir.y, runnerDir.z));
                BRepPrimAPI_MakeCylinder plugCyl(plugAx, runnerRadius, coldPlugDist);
                plugCyl.Build();
                if (plugCyl.IsDone() && !plugCyl.Shape().IsNull())
                    shapes.push_back(plugCyl.Shape());
            }
        }
    }

    // ---- Gate frustums + sub-runners --------------------------------------
    {
        const float draftRad = glm::radians(glm::clamp(draftAngle, 0.0f, 45.0f));
        const float tanDraft = std::tan(draftRad);

        for (const GateFeature& gf : m_gates)
        {
            if (!gf.hasPath) continue;

            const glm::vec3 origin = gf.point.worldPos;
            const glm::vec3 pathVec = gf.pathEnd - origin;
            const float     totalLen = glm::length(pathVec);
            if (totalLen < 1e-6f || gateRadius < 1e-6f) continue;
            const glm::vec3 pathDir = pathVec / totalLen;
            const gp_Dir occDir(pathDir.x, pathDir.y, pathDir.z);

            const float backExt = kCutEps + overrun;
            const glm::vec3 originExt = origin - pathDir * backExt;
            const float     totalLenExt = totalLen + backExt;
            const gp_Ax2    gateAxExt(gp_Pnt(originExt.x, originExt.y, originExt.z), occDir);

            const float startRadius = std::max(0.01f, gateRadius - tanDraft * backExt);

            float taperLen = std::numeric_limits<float>::max();
            if (tanDraft > 1e-6f && subRunnerRadius > gateRadius)
                taperLen = (subRunnerRadius - gateRadius) / tanDraft;
            const float taperLenExt = taperLen + backExt;

            if (taperLen >= totalLen)
            {
                const float endR = startRadius + totalLenExt * tanDraft;
                if (std::abs(endR - startRadius) > 1e-6f)
                {
                    BRepPrimAPI_MakeCone cone(gateAxExt, startRadius, endR, totalLenExt);
                    cone.Build();
                    if (cone.IsDone() && !cone.Shape().IsNull())
                        shapes.push_back(cone.Shape());
                }
                else
                {
                    BRepPrimAPI_MakeCylinder cyl(gateAxExt, startRadius, totalLenExt);
                    cyl.Build();
                    if (cyl.IsDone() && !cyl.Shape().IsNull())
                        shapes.push_back(cyl.Shape());
                }
            }
            else
            {
                BRepPrimAPI_MakeCone cone(gateAxExt, startRadius, subRunnerRadius, taperLenExt);
                cone.Build();
                if (cone.IsDone() && !cone.Shape().IsNull())
                    shapes.push_back(cone.Shape());

                const glm::vec3 transitionPt = originExt + pathDir * taperLenExt;
                const float     subLen = totalLenExt - taperLenExt;
                if (subLen > 1e-6f)
                {
                    gp_Ax2 subAx(gp_Pnt(transitionPt.x, transitionPt.y, transitionPt.z), occDir);
                    BRepPrimAPI_MakeCylinder subCyl(subAx, subRunnerRadius, subLen);
                    subCyl.Build();
                    if (subCyl.IsDone() && !subCyl.Shape().IsNull())
                        shapes.push_back(subCyl.Shape());
                }
            }
        }
    }

    if (shapes.empty()) return false;

    // ---- Fuse everything --------------------------------------------------
    // Pairwise fuse. Disjoint pieces fuse into a valid compound, so this stays
    // robust even when (say) a gate doesn't quite touch its feed point. A
    // failed individual fuse drops that one piece rather than aborting the
    // whole shot.
    TopoDS_Shape acc = shapes[0];
    for (size_t i = 1; i < shapes.size(); ++i)
    {
        BRepAlgoAPI_Fuse fuse(acc, shapes[i]);
        fuse.Build();
        if (fuse.IsDone() && !fuse.Shape().IsNull())
            acc = fuse.Shape();
        // else: keep acc, skip this piece.
    }

    if (acc.IsNull()) return false;
    out = acc;
    return true;
}

// ---------------------------------------------------------------------------
// TessellateShapeToMesh — OCC shape → render-ready MeshData (posNorm+indices),
// optionally recording each triangle's source-face index.
// ---------------------------------------------------------------------------
void GLCanvas::TessellateShapeToMesh(const TopoDS_Shape& shape,
    FileImporter::MeshData& mesh, std::vector<int>* faceIds)
{
    mesh = FileImporter::MeshData{};
    if (faceIds) faceIds->clear();
    if (shape.IsNull()) return;

    BRepMesh_IncrementalMesh mesher(shape, 0.05, false, 0.5, true);

    // Stable 1-based face indexing, matching what DesignChecks rebuilds via
    // TopExp::MapShapes — so a face-level result maps back to these triangles.
    TopTools_IndexedMapOfShape faceMap;
    if (faceIds) TopExp::MapShapes(shape, TopAbs_FACE, faceMap);

    for (TopExp_Explorer ex(shape, TopAbs_FACE); ex.More(); ex.Next())
    {
        const TopoDS_Face face = TopoDS::Face(ex.Current());
        TopLoc_Location loc;
        Handle(Poly_Triangulation) tri = BRep_Tool::Triangulation(face, loc);
        if (tri.IsNull()) continue;

        const int faceIndex = faceIds ? faceMap.FindIndex(face) : 0;

        const gp_Trsf tr = loc.Transformation();
        const uint32_t baseIndex = (uint32_t)(mesh.vertices.size() / 3);

        // OCC triangulates each face for its FORWARD sense. When the face is
        // REVERSED within the solid, its true outward normal is the opposite
        // of the triangulation winding, so swap two indices to flip the
        // winding back to outward. This keeps geometric (cross-product)
        // normals reliably outward — which the demoldability check depends on.
        const bool reversed = (face.Orientation() == TopAbs_REVERSED);

        for (int i = 1; i <= tri->NbNodes(); ++i)
        {
            gp_Pnt p = tri->Node(i);
            p.Transform(tr);
            mesh.vertices.push_back((float)p.X());
            mesh.vertices.push_back((float)p.Y());
            mesh.vertices.push_back((float)p.Z());
        }
        for (int t = 1; t <= tri->NbTriangles(); ++t)
        {
            int n1, n2, n3;
            tri->Triangle(t).Get(n1, n2, n3);
            if (reversed) std::swap(n2, n3);
            mesh.indices.push_back(baseIndex + (uint32_t)(n1 - 1));
            mesh.indices.push_back(baseIndex + (uint32_t)(n2 - 1));
            mesh.indices.push_back(baseIndex + (uint32_t)(n3 - 1));
            if (faceIds) faceIds->push_back(faceIndex);
        }
    }

    if (mesh.vertices.empty() || mesh.indices.empty()) return;

    ComputeVertexNormals_Pos3(mesh.vertices, mesh.indices, mesh.posNorm);
    auto split = SplitByCreaseAngle_Pos3(mesh.vertices, mesh.indices, 35.0f);
    mesh.posNorm = std::move(split.posNorm);
    mesh.indices = std::move(split.indices);
    // faceIds stays aligned: the crease split preserves triangle order/count.
}

// ---------------------------------------------------------------------------
// BuildSphereGPU — generates a UV sphere and uploads it to the GPU.
// Vertex layout: [pos(3), normal(3)] — compatible with vsLit/fsLit.
// ---------------------------------------------------------------------------
void GLCanvas::BuildSphereGPU(float radius, int stacks, int slices)
{
    std::vector<float>    verts;
    std::vector<uint32_t> idx;

    for (int i = 0; i <= stacks; ++i)
    {
        const float phi = glm::pi<float>() * float(i) / float(stacks);
        const float sinPhi = sinf(phi);
        const float cosPhi = cosf(phi);

        for (int j = 0; j <= slices; ++j)
        {
            const float theta = 2.0f * glm::pi<float>() * float(j) / float(slices);
            const float x = sinPhi * cosf(theta);
            const float y = cosPhi;
            const float z = sinPhi * sinf(theta);

            verts.push_back(x * radius);  verts.push_back(y * radius);  verts.push_back(z * radius);
            verts.push_back(x);           verts.push_back(y);           verts.push_back(z);
        }
    }

    for (int i = 0; i < stacks; ++i)
    {
        for (int j = 0; j < slices; ++j)
        {
            const uint32_t a = uint32_t(i * (slices + 1) + j);
            const uint32_t b = uint32_t(a + slices + 1);
            idx.push_back(a);     idx.push_back(b);     idx.push_back(a + 1);
            idx.push_back(b);     idx.push_back(b + 1); idx.push_back(a + 1);
        }
    }

    glGenVertexArrays(1, &m_sphereVAO);
    glGenBuffers(1, &m_sphereVBO);
    glGenBuffers(1, &m_sphereEBO);

    glBindVertexArray(m_sphereVAO);
    glBindBuffer(GL_ARRAY_BUFFER, m_sphereVBO);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(verts.size() * sizeof(float)), verts.data(), GL_STATIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_sphereEBO);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)(idx.size() * sizeof(uint32_t)), idx.data(), GL_STATIC_DRAW);

    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)(3 * sizeof(float)));
    glEnableVertexAttribArray(1);
    glBindVertexArray(0);

    m_sphereIndexCount = (GLsizei)idx.size();
}

// ---------------------------------------------------------------------------
// RayCastObjects — Möller–Trumbore ray-mesh intersection.
// Tests all triangles of all imported objects (CPU side) and returns the
// closest hit in world space.  Returns false if no surface was hit.
// ---------------------------------------------------------------------------
static bool RayTriangle(const glm::vec3& orig, const glm::vec3& dir,
    const glm::vec3& v0, const glm::vec3& v1, const glm::vec3& v2,
    float& outT)
{
    constexpr float EPS = 1e-7f;
    const glm::vec3 e1 = v1 - v0;
    const glm::vec3 e2 = v2 - v0;
    const glm::vec3 h = glm::cross(dir, e2);
    const float     a = glm::dot(e1, h);
    // Double-sided test: |a| < EPS rejects only the degenerate case where
    // the ray is parallel to the triangle's plane (no intersection or
    // grazing). Either sign of `a` is accepted, so a ray hitting the
    // triangle from the back (winding-wise) is treated the same as one
    // hitting from the front.
    //
    // Earlier revisions used `a < EPS` — front-face-only Möller-Trumbore.
    // That gate caused two pain points: (1) AlignFace / AlignMidplane
    // couldn't pick cavity-interior walls and any imported triangles with
    // inconsistent winding, which is the user-visible "I have to put the
    // camera inside the part to select the face" symptom; (2) when the
    // camera sat inside a closed object, generic object picking
    // (RayCastObjects) skipped the near (back-facing) wall and returned
    // the far front-facing wall on the opposite side — counterintuitive
    // for placement tools.
    //
    // The original comment justified the gate as "prevents hits on the
    // far side of a cavity when the near face is occluded by the fixture
    // (which has no CPU geometry to block the ray)". On closer reading,
    // that scenario is already handled correctly by the closest-T logic
    // in every caller: a near front face always beats a far back face on
    // T, so the gate wasn't actually doing the work the comment claimed.
    // Downstream consumers don't depend on which side was hit — they
    // either flip the returned normal to face the camera (RayCastObjects)
    // or use the triangle's own normal independent of pick side
    // (RayCastFacePick → GrowCoplanarFace).
    if (std::abs(a) < EPS) return false;
    const float     f = 1.0f / a;
    const glm::vec3 s = orig - v0;
    const float     u = f * glm::dot(s, h);
    if (u < 0.0f || u > 1.0f) return false;
    const glm::vec3 q = glm::cross(s, e1);
    const float     v = f * glm::dot(dir, q);
    if (v < 0.0f || u + v > 1.0f) return false;
    outT = f * glm::dot(e2, q);
    return outT > EPS;
}

bool GLCanvas::RayCastObjects(int mouseX, int mouseY,
    glm::vec3& outPos, glm::vec3& outNormal)
{
    const wxSize sz = GetClientSize();
    const int    w = std::max(1, sz.x);
    const int    h = std::max(1, sz.y);

    // Build world-space ray from NDC mouse position
    const float ndcX = (2.0f * float(mouseX)) / float(w) - 1.0f;
    const float ndcY = 1.0f - (2.0f * float(mouseY)) / float(h);

    m_camera.SetAspect(float(w) / float(h));
    const glm::mat4 view = m_camera.View();
    const glm::mat4 proj = m_camera.Projection();
    const glm::mat4 invVP = glm::inverse(proj * view);

    glm::vec4 nearH = invVP * glm::vec4(ndcX, ndcY, -1.0f, 1.0f);
    glm::vec4 farH = invVP * glm::vec4(ndcX, ndcY, 1.0f, 1.0f);
    nearH /= nearH.w;
    farH /= farH.w;

    const glm::vec3 rayOrig = glm::vec3(nearH);
    const glm::vec3 rayDir = glm::normalize(glm::vec3(farH) - glm::vec3(nearH));

    float     bestWorldT = std::numeric_limits<float>::max();
    bool      hit = false;

    for (const auto& obj : m_objects)
    {
        if (obj.cpuVerts.empty() || obj.cpuIndices.empty()) continue;

        const glm::mat4 model = obj.BuildModelMatrix();
        const glm::mat4 invModel = glm::inverse(model);

        // Transform ray into object (local) space for intersection test
        const glm::vec3 localOrig = glm::vec3(invModel * glm::vec4(rayOrig, 1.0f));
        const glm::vec3 localDir = glm::normalize(glm::vec3(invModel * glm::vec4(rayDir, 0.0f)));

        for (size_t i = 0; i + 2 < obj.cpuIndices.size(); i += 3)
        {
            const uint32_t i0 = obj.cpuIndices[i];
            const uint32_t i1 = obj.cpuIndices[i + 1];
            const uint32_t i2 = obj.cpuIndices[i + 2];

            const glm::vec3 v0(obj.cpuVerts[i0 * 3], obj.cpuVerts[i0 * 3 + 1], obj.cpuVerts[i0 * 3 + 2]);
            const glm::vec3 v1(obj.cpuVerts[i1 * 3], obj.cpuVerts[i1 * 3 + 1], obj.cpuVerts[i1 * 3 + 2]);
            const glm::vec3 v2(obj.cpuVerts[i2 * 3], obj.cpuVerts[i2 * 3 + 1], obj.cpuVerts[i2 * 3 + 2]);

            float localT = 0.0f;
            if (!RayTriangle(localOrig, localDir, v0, v1, v2, localT))
                continue;

            // Convert local hit back to world space to get a consistent depth
            const glm::vec3 localHit = localOrig + localDir * localT;
            const glm::vec3 worldHit = glm::vec3(model * glm::vec4(localHit, 1.0f));
            const float     worldT = glm::length(worldHit - rayOrig);

            if (worldT < bestWorldT)
            {
                bestWorldT = worldT;
                outPos = worldHit;

                // Face normal → transform to world space
                const glm::vec3 faceN = glm::normalize(glm::cross(v1 - v0, v2 - v0));
                const glm::mat3 normMat = glm::transpose(glm::inverse(glm::mat3(model)));
                outNormal = glm::normalize(normMat * faceN);

                // Flip normal to face the camera
                if (glm::dot(outNormal, rayDir) > 0.0f)
                    outNormal = -outNormal;

                hit = true;
            }
        }
    }

    return hit;
}

// ---------------------------------------------------------------------------
// RayCastWorldRay — fires a pre-built world-space ray against all imported
// objects (Möller–Trumbore, double-sided — see RayTriangle for the rationale).
// Returns the closest hit. Unlike RayCastObjects this takes an explicit
// origin + direction rather than unprojecting mouse coordinates, so it can
// be called without a mouse event.
// ---------------------------------------------------------------------------
bool GLCanvas::RayCastWorldRay(const glm::vec3& origin, const glm::vec3& dir,
    float maxDist, glm::vec3& outPos)
{
    float bestT = maxDist;
    bool  hit = false;

    for (const auto& obj : m_objects)
    {
        if (obj.cpuVerts.empty() || obj.cpuIndices.empty()) continue;

        const glm::mat4 model = obj.BuildModelMatrix();
        const glm::mat4 invModel = glm::inverse(model);

        const glm::vec3 localOrig = glm::vec3(invModel * glm::vec4(origin, 1.0f));
        const glm::vec3 localDir = glm::normalize(glm::vec3(invModel * glm::vec4(dir, 0.0f)));

        for (size_t i = 0; i + 2 < obj.cpuIndices.size(); i += 3)
        {
            const uint32_t i0 = obj.cpuIndices[i];
            const uint32_t i1 = obj.cpuIndices[i + 1];
            const uint32_t i2 = obj.cpuIndices[i + 2];

            const glm::vec3 v0(obj.cpuVerts[i0 * 3], obj.cpuVerts[i0 * 3 + 1], obj.cpuVerts[i0 * 3 + 2]);
            const glm::vec3 v1(obj.cpuVerts[i1 * 3], obj.cpuVerts[i1 * 3 + 1], obj.cpuVerts[i1 * 3 + 2]);
            const glm::vec3 v2(obj.cpuVerts[i2 * 3], obj.cpuVerts[i2 * 3 + 1], obj.cpuVerts[i2 * 3 + 2]);

            float localT = 0.0f;
            if (!RayTriangle(localOrig, localDir, v0, v1, v2, localT)) continue;

            const glm::vec3 worldHit = glm::vec3(model * glm::vec4(localOrig + localDir * localT, 1.0f));
            const float     worldT = glm::length(worldHit - origin);

            if (worldT < bestT)
            {
                bestT = worldT;
                outPos = worldHit;
                hit = true;
            }
        }
    }

    return hit;
}

// ===========================================================================
// Align Face — face-region picking and alignment math.
// ===========================================================================
//
// RayCastFacePick — like RayCastObjects but returns which object and which
// triangle was hit, without computing the surface normal (the BFS does that
// once it knows the triangle). Manual unprojection rather than reusing
// RayCastObjects because we need the triangle index, not just the position.
// ---------------------------------------------------------------------------
bool GLCanvas::RayCastFacePick(int mouseX, int mouseY, int& outObj, int& outTri)
{
    outObj = -1;
    outTri = -1;

    const wxSize sz = GetClientSize();
    const int    w = std::max(1, sz.x);
    const int    h = std::max(1, sz.y);

    const float ndcX = (2.0f * float(mouseX)) / float(w) - 1.0f;
    const float ndcY = 1.0f - (2.0f * float(mouseY)) / float(h);

    m_camera.SetAspect(float(w) / float(h));
    const glm::mat4 view = m_camera.View();
    const glm::mat4 proj = m_camera.Projection();
    const glm::mat4 invVP = glm::inverse(proj * view);

    glm::vec4 nearH = invVP * glm::vec4(ndcX, ndcY, -1.0f, 1.0f);
    glm::vec4 farH = invVP * glm::vec4(ndcX, ndcY, 1.0f, 1.0f);
    nearH /= nearH.w;
    farH /= farH.w;

    const glm::vec3 rayOrig = glm::vec3(nearH);
    const glm::vec3 rayDir = glm::normalize(glm::vec3(farH) - glm::vec3(nearH));

    float bestWorldT = std::numeric_limits<float>::max();

    for (int oi = 0; oi < (int)m_objects.size(); ++oi)
    {
        const SceneObject& obj = m_objects[oi];
        if (obj.cpuVerts.empty() || obj.cpuIndices.empty()) continue;

        const glm::mat4 model = obj.BuildModelMatrix();
        const glm::mat4 invModel = glm::inverse(model);

        const glm::vec3 localOrig = glm::vec3(invModel * glm::vec4(rayOrig, 1.0f));
        const glm::vec3 localDir = glm::normalize(glm::vec3(invModel * glm::vec4(rayDir, 0.0f)));

        const size_t triCount = obj.cpuIndices.size() / 3;
        for (size_t t = 0; t < triCount; ++t)
        {
            const uint32_t i0 = obj.cpuIndices[3 * t + 0];
            const uint32_t i1 = obj.cpuIndices[3 * t + 1];
            const uint32_t i2 = obj.cpuIndices[3 * t + 2];

            const glm::vec3 v0(obj.cpuVerts[i0 * 3], obj.cpuVerts[i0 * 3 + 1], obj.cpuVerts[i0 * 3 + 2]);
            const glm::vec3 v1(obj.cpuVerts[i1 * 3], obj.cpuVerts[i1 * 3 + 1], obj.cpuVerts[i1 * 3 + 2]);
            const glm::vec3 v2(obj.cpuVerts[i2 * 3], obj.cpuVerts[i2 * 3 + 1], obj.cpuVerts[i2 * 3 + 2]);

            float localT = 0.0f;
            if (!RayTriangle(localOrig, localDir, v0, v1, v2, localT)) continue;

            const glm::vec3 localHit = localOrig + localDir * localT;
            const glm::vec3 worldHit = glm::vec3(model * glm::vec4(localHit, 1.0f));
            const float     worldT = glm::length(worldHit - rayOrig);

            if (worldT < bestWorldT)
            {
                bestWorldT = worldT;
                outObj = oi;
                outTri = (int)t;
            }
        }
    }

    return outObj >= 0;
}

// ---------------------------------------------------------------------------
// EnsureTriAdjacency — build edge → neighbour-triangle map for one object.
// Manifold edges (exactly 2 triangles) link both ways. Non-manifold edges
// (3+ triangles meeting) are robust but degraded: extras are left unlinked
// rather than overwriting an existing pairing, so BFS may miss some tris on
// truly non-manifold geometry. Acceptable for a UI feature; revisit if it
// hurts.
// ---------------------------------------------------------------------------
void GLCanvas::EnsureTriAdjacency(SceneObject& obj)
{
    if (obj.adjacencyBuilt) return;

    const size_t triCount = obj.cpuIndices.size() / 3;
    obj.triNeighbors.assign(triCount, std::array<int, 3>{ -1, -1, -1 });

    // Sentinel "already paired" value stored in the temporary edge map so a
    // 3rd triangle on the same edge gets ignored rather than corrupting the
    // existing pairing.
    constexpr int kPaired = -2;

    // Pack (a,b) into a 64-bit key with min-first canonical ordering.
    auto pack = [](uint32_t a, uint32_t b) -> uint64_t {
        if (a > b) std::swap(a, b);
        return (uint64_t(a) << 32) | uint64_t(b);
        };

    std::unordered_map<uint64_t, std::pair<int, int>> edgeMap;
    edgeMap.reserve(triCount * 3);

    for (size_t t = 0; t < triCount; ++t)
    {
        const uint32_t verts[3] = {
            obj.cpuIndices[3 * t + 0],
            obj.cpuIndices[3 * t + 1],
            obj.cpuIndices[3 * t + 2]
        };
        for (int s = 0; s < 3; ++s)
        {
            const uint64_t key = pack(verts[s], verts[(s + 1) % 3]);
            auto it = edgeMap.find(key);
            if (it == edgeMap.end())
            {
                edgeMap.emplace(key, std::make_pair((int)t, s));
            }
            else if (it->second.first != kPaired)
            {
                const int otherT = it->second.first;
                const int otherS = it->second.second;
                obj.triNeighbors[t][s] = otherT;
                obj.triNeighbors[otherT][otherS] = (int)t;
                it->second.first = kPaired;  // prevent 3rd-tri corruption
            }
            // else: edge already had its pair — leave the 3rd+ tri unlinked.
        }
    }

    obj.adjacencyBuilt = true;
}

// ---------------------------------------------------------------------------
// GrowCoplanarFace — BFS from a seed triangle across edge-shared neighbours
// whose normal is within ~1° of the seed's normal. Uses dot > cos(1°), so
// only same-side coplanar tris are merged (anti-parallel back-faces are
// excluded — important for thin shells where front and back faces are
// geometrically coplanar but logically distinct).
// ---------------------------------------------------------------------------
void GLCanvas::GrowCoplanarFace(const SceneObject& obj, int seedTri,
    std::vector<uint32_t>& outTris, glm::vec3& outNormalLocal)
{
    outTris.clear();
    outNormalLocal = glm::vec3(0.0f);
    if (seedTri < 0 || seedTri >= (int)obj.triNeighbors.size()) return;

    auto triNormal = [&](int t) -> glm::vec3 {
        const uint32_t i0 = obj.cpuIndices[3 * t + 0];
        const uint32_t i1 = obj.cpuIndices[3 * t + 1];
        const uint32_t i2 = obj.cpuIndices[3 * t + 2];
        const glm::vec3 v0(obj.cpuVerts[i0 * 3], obj.cpuVerts[i0 * 3 + 1], obj.cpuVerts[i0 * 3 + 2]);
        const glm::vec3 v1(obj.cpuVerts[i1 * 3], obj.cpuVerts[i1 * 3 + 1], obj.cpuVerts[i1 * 3 + 2]);
        const glm::vec3 v2(obj.cpuVerts[i2 * 3], obj.cpuVerts[i2 * 3 + 1], obj.cpuVerts[i2 * 3 + 2]);
        const glm::vec3 n = glm::cross(v1 - v0, v2 - v0);
        const float     len = glm::length(n);
        return (len > 1e-12f) ? (n / len) : glm::vec3(0.0f, 1.0f, 0.0f);
        };

    const glm::vec3 seedNormal = triNormal(seedTri);
    outNormalLocal = seedNormal;
    constexpr float kCosTol = 0.99985f;  // cos(~1°)

    const size_t triCount = obj.triNeighbors.size();
    std::vector<uint8_t> visited(triCount, 0);
    std::vector<int>     stack;
    stack.reserve(64);

    visited[seedTri] = 1;
    stack.push_back(seedTri);
    outTris.push_back((uint32_t)seedTri);

    while (!stack.empty())
    {
        const int t = stack.back();
        stack.pop_back();
        for (int s = 0; s < 3; ++s)
        {
            const int nb = obj.triNeighbors[t][s];
            if (nb < 0 || visited[nb]) continue;

            const glm::vec3 nNb = triNormal(nb);
            // dot > kCosTol enforces both "near-parallel" and "same-side".
            if (glm::dot(nNb, seedNormal) > kCosTol)
            {
                visited[nb] = 1;
                stack.push_back(nb);
                outTris.push_back((uint32_t)nb);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// RebuildAlignHighlightVBO — upload the highlighted face's triangles in
// world space (model matrix baked in here on the CPU). Lazily creates the
// VAO/VBO on first use; reuses them on subsequent calls.
// ---------------------------------------------------------------------------
void GLCanvas::RebuildAlignHighlightVBO(const SceneObject& obj,
    const std::vector<uint32_t>& tris)
{
    if (tris.empty())
    {
        m_alignHighlightVertexCount = 0;
        return;
    }

    const glm::mat4 model = obj.BuildModelMatrix();

    std::vector<float> verts;
    verts.reserve(tris.size() * 9);

    for (uint32_t t : tris)
    {
        for (int k = 0; k < 3; ++k)
        {
            const uint32_t vi = obj.cpuIndices[3 * t + k];
            const glm::vec3 vL(obj.cpuVerts[vi * 3],
                obj.cpuVerts[vi * 3 + 1],
                obj.cpuVerts[vi * 3 + 2]);
            const glm::vec3 vW = glm::vec3(model * glm::vec4(vL, 1.0f));
            verts.push_back(vW.x);
            verts.push_back(vW.y);
            verts.push_back(vW.z);
        }
    }

    if (m_alignHighlightVAO == 0)
    {
        glGenVertexArrays(1, &m_alignHighlightVAO);
        glGenBuffers(1, &m_alignHighlightVBO);
        glBindVertexArray(m_alignHighlightVAO);
        glBindBuffer(GL_ARRAY_BUFFER, m_alignHighlightVBO);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void*)0);
        glBindVertexArray(0);
    }

    glBindBuffer(GL_ARRAY_BUFFER, m_alignHighlightVBO);
    glBufferData(GL_ARRAY_BUFFER, verts.size() * sizeof(float),
        verts.data(), GL_DYNAMIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    m_alignHighlightVertexCount = (GLsizei)(verts.size() / 3);
}

// ---------------------------------------------------------------------------
// DecomposeYXZ — extract YXZ Euler angles from a rotation matrix R that was
// constructed as Ry(yaw) * Rx(pitch) * Rz(roll). Returns degrees.
//
// Derivation: with the YXZ convention and column-major glm storage,
//     R[2][1] = -sin(pitch)
//     R[2][0] =  sin(yaw)*cos(pitch)
//     R[2][2] =  cos(yaw)*cos(pitch)
//     R[0][1] =  cos(pitch)*sin(roll)
//     R[1][1] =  cos(pitch)*cos(roll)
// When |cos(pitch)| ≈ 0 (gimbal lock at pitch = ±90°), yaw and roll are
// indistinguishable along one axis. Convention here: lock roll = 0 and put
// the rotation into yaw using R[0][0] = cos(yaw) and R[0][2] = -sin(yaw).
// ---------------------------------------------------------------------------
void GLCanvas::DecomposeYXZ(const glm::mat3& R,
    float& yawDeg, float& pitchDeg, float& rollDeg)
{
    constexpr float kGimbalEps = 1.0e-5f;

    const float sinPitch = -R[2][1];
    const float pitch = std::asin(glm::clamp(sinPitch, -1.0f, 1.0f));
    const float cosPitch = std::cos(pitch);

    float yaw, roll;
    if (std::abs(cosPitch) > kGimbalEps)
    {
        yaw = std::atan2(R[2][0], R[2][2]);
        roll = std::atan2(R[0][1], R[1][1]);
    }
    else
    {
        // Gimbal-locked: pin roll to 0 and recover yaw from the X column.
        roll = 0.0f;
        yaw = std::atan2(-R[0][2], R[0][0]);
    }

    yawDeg = glm::degrees(yaw);
    pitchDeg = glm::degrees(pitch);
    rollDeg = glm::degrees(roll);
}

// ---------------------------------------------------------------------------
// ApplyPlaneAlignmentToObject — core math shared by AlignFace and AlignMidplane.
//
// Snaps the local-space plane (planeNormalLocal, anchorLocal) onto world Y=0.
// The anchor is held fixed laterally (X/Z stays put in world), only Y and the
// rotation change.
//
// Strategy:
//   1. Map the local plane normal to world space using R_old (object rotation
//      without scale). Uniform scale doesn't affect normal direction.
//   2. Pick the target axis as whichever of ±Y is closer to n_w — smallest
//      rotation, so the user's existing orientation is respected.
//   3. Build R_delta as the smallest rotation from n_w to the target axis.
//   4. Compose: R_new = R_delta * R_old, decompose back to YXZ Euler.
//   5. Solve for the new translation so the anchor stays in place laterally
//      and lands exactly on Y=0.
// ---------------------------------------------------------------------------
void GLCanvas::ApplyPlaneAlignmentToObject(int objIdx,
    const glm::vec3& planeNormalLocal,
    const glm::vec3& anchorLocal)
{
    if (objIdx < 0 || objIdx >= (int)m_objects.size()) return;

    SceneObject& obj = m_objects[objIdx];

    // ---- 1. Rotation pieces -------------------------------------------------
    // R_old is the existing object rotation (YXZ). Strip uniform scale from
    // the model matrix's 3x3 block to get pure rotation.
    const glm::mat4 modelOld = obj.BuildModelMatrix();
    glm::mat3 R_old(modelOld);
    if (obj.scale > 1e-12f)
        R_old /= obj.scale;

    const glm::vec3 nLocalUnit = glm::normalize(planeNormalLocal);
    const glm::vec3 nWorld = glm::normalize(R_old * nLocalUnit);

    // ---- 2. Smallest-rotation target on ±Y ---------------------------------
    const glm::vec3 targetY =
        (nWorld.y >= 0.0f) ? glm::vec3(0, 1, 0) : glm::vec3(0, -1, 0);

    // ---- 3. R_delta from nWorld to targetY ---------------------------------
    glm::mat3 R_delta(1.0f);  // identity
    const float d = glm::clamp(glm::dot(nWorld, targetY), -1.0f, 1.0f);
    if (d < 0.99999f)
    {
        if (d < -0.99999f)
        {
            // Anti-parallel: 180° rotation about any axis perpendicular to nWorld.
            // Pick the world axis least parallel to nWorld for numerical stability.
            const glm::vec3 candidate =
                (std::abs(nWorld.x) < 0.9f) ? glm::vec3(1, 0, 0) : glm::vec3(0, 0, 1);
            const glm::vec3 axis = glm::normalize(glm::cross(nWorld, candidate));
            R_delta = glm::mat3(glm::rotate(glm::mat4(1.0f),
                glm::radians(180.0f), axis));
        }
        else
        {
            const glm::vec3 axis = glm::normalize(glm::cross(nWorld, targetY));
            const float     angle = std::acos(d);
            R_delta = glm::mat3(glm::rotate(glm::mat4(1.0f), angle, axis));
        }
    }

    // ---- 4. Compose and decompose ------------------------------------------
    const glm::mat3 R_new = R_delta * R_old;
    DecomposeYXZ(R_new, obj.yawDeg, obj.pitchDeg, obj.rollDeg);

    // ---- 5. Solve translation ----------------------------------------------
    // Anchor world position before the rotation:
    //     a_w = pos_old + R_old * (s * a_local)
    // After: a_w' = pos_new + R_new * (s * a_local) = pos_new + R_delta * (a_w - pos_old)
    // We want a_w'.y = 0 and a_w'.xz = a_w.xz (anchor stays put laterally).
    const glm::vec3 v_old = R_old * (obj.scale * anchorLocal);
    const glm::vec3 a_w = obj.pos + v_old;
    const glm::vec3 v_new = R_delta * v_old;

    obj.pos.x = a_w.x - v_new.x;
    obj.pos.y = -v_new.y;
    obj.pos.z = a_w.z - v_new.z;

    // Both ApplyAlignFaceToObject and ApplyAlignMidplaneToObject delegate
    // their actual transform write to this method, so notifying once here
    // covers both entry points.
    NotifySceneMutated();
}

// ---------------------------------------------------------------------------
// ApplyAlignFaceToObject — thin wrapper. Computes face centroid in local
// space and delegates to the shared aligner using the face's own normal.
// ---------------------------------------------------------------------------
void GLCanvas::ApplyAlignFaceToObject(int objIdx, const glm::vec3& nLocal,
    const std::vector<uint32_t>& faceTris)
{
    if (objIdx < 0 || objIdx >= (int)m_objects.size()) return;
    if (faceTris.empty()) return;

    const SceneObject& obj = m_objects[objIdx];

    glm::vec3 centroidLocal(0.0f);
    int       triCounted = 0;
    for (uint32_t t : faceTris)
    {
        const uint32_t i0 = obj.cpuIndices[3 * t + 0];
        const uint32_t i1 = obj.cpuIndices[3 * t + 1];
        const uint32_t i2 = obj.cpuIndices[3 * t + 2];
        const glm::vec3 v0(obj.cpuVerts[i0 * 3], obj.cpuVerts[i0 * 3 + 1], obj.cpuVerts[i0 * 3 + 2]);
        const glm::vec3 v1(obj.cpuVerts[i1 * 3], obj.cpuVerts[i1 * 3 + 1], obj.cpuVerts[i1 * 3 + 2]);
        const glm::vec3 v2(obj.cpuVerts[i2 * 3], obj.cpuVerts[i2 * 3 + 1], obj.cpuVerts[i2 * 3 + 2]);
        centroidLocal += (v0 + v1 + v2) * (1.0f / 3.0f);
        ++triCounted;
    }
    centroidLocal /= float(triCounted);

    ApplyPlaneAlignmentToObject(objIdx, nLocal, centroidLocal);
}

// ---------------------------------------------------------------------------
// ApplyAlignMidplaneToObject — combine the locked first face with the
// freshly-picked second face to derive a midplane, then align that midplane
// to Y=0.
//
// Bisector selection rule: there are two valid bisector planes. We pick the
// one that *separates the two centroids* — that's bisector B, with normal
// (n1 - n2), when c1 and c2 lie on opposite sides of it. Otherwise we use
// bisector A with normal (n1 + n2). This handles both common cases:
//   • Parallel slab (top + bottom face, n1·n2 ≈ -1): bisector B, parallel to
//     both faces, halfway between.
//   • V-shape / wedge (two angled walls): bisector B, the axis-of-symmetry
//     plane.
//   • Two parallel same-normal faces (rare; n1·n2 ≈ +1): bisector A.
//
// Anchor: midpoint of the two centroids, then projected onto the midplane
// for numerical cleanliness (the midpoint is already on the midplane in
// exact arithmetic, but the projection costs one dot product and avoids
// accumulating any drift).
// ---------------------------------------------------------------------------
void GLCanvas::ApplyAlignMidplaneToObject(int objIdx,
    const glm::vec3& n2Local,
    const std::vector<uint32_t>& faceTris2)
{
    if (objIdx < 0 || objIdx >= (int)m_objects.size()) return;
    if (faceTris2.empty()) return;
    if (!m_midplaneFaceLocked || m_midplaneFaceObject != objIdx) return;

    const SceneObject& obj = m_objects[objIdx];

    // ---- Centroid of face 2 (local) ----------------------------------------
    glm::vec3 c2(0.0f);
    int       triCounted = 0;
    for (uint32_t t : faceTris2)
    {
        const uint32_t i0 = obj.cpuIndices[3 * t + 0];
        const uint32_t i1 = obj.cpuIndices[3 * t + 1];
        const uint32_t i2 = obj.cpuIndices[3 * t + 2];
        const glm::vec3 v0(obj.cpuVerts[i0 * 3], obj.cpuVerts[i0 * 3 + 1], obj.cpuVerts[i0 * 3 + 2]);
        const glm::vec3 v1(obj.cpuVerts[i1 * 3], obj.cpuVerts[i1 * 3 + 1], obj.cpuVerts[i1 * 3 + 2]);
        const glm::vec3 v2(obj.cpuVerts[i2 * 3], obj.cpuVerts[i2 * 3 + 1], obj.cpuVerts[i2 * 3 + 2]);
        c2 += (v0 + v1 + v2) * (1.0f / 3.0f);
        ++triCounted;
    }
    c2 /= float(triCounted);

    const glm::vec3 n1 = glm::normalize(m_midplaneFaceNormalLocal);
    const glm::vec3 n2 = glm::normalize(n2Local);
    const glm::vec3 c1 = m_midplaneFaceCentroidLocal;

    // ---- Bisector selection ------------------------------------------------
    // Two bisector planes exist between the two face planes:
    //   A: normal nA = n1 - n2, offset dA = n1·c1 - n2·c2
    //   B: normal nB = n1 + n2, offset dB = n1·c1 + n2·c2
    //
    // We want the bisector that *separates the two centroids* — that's the
    // one threading between the faces rather than running parallel to both
    // far away from them.
    //
    // Signed distances of c1, c2 from bisector A simplify to:
    //   sd(c1) = n2 · (c2 - c1)
    //   sd(c2) = n1 · (c2 - c1)
    // A separates c1 and c2 iff these have opposite signs (product < 0).
    //
    // Worked examples:
    //   • Parallel slab (n1=+Y, n2=-Y, c1 above, c2 below):
    //       sd(c1) = -2h, sd(c2) = +2h, opposite signs → A wins, plane y=0. ✓
    //   • V-wedge (two angled walls, c1 left, c2 right):
    //       sd(c1) and sd(c2) opposite signs → A wins, plane x=0. ✓
    //   • Two parallel same-normal stairs (n1 ≈ n2):
    //       sd(c1)·sd(c2) > 0 → fall through to B, which is the midline
    //       parallel to both faces. ✓
    //
    // The chosen bisector's *unnormalized* normal and offset are kept in
    // (nRaw, dRaw) so anchor projection below can use the matching sign
    // pair — picking the right normal but the wrong offset would give the
    // wrong plane.
    glm::vec3 nRaw;
    float     dRaw;
    {
        const glm::vec3 nA = n1 - n2;
        const glm::vec3 nB_vec = n1 + n2;
        const float     dA = glm::dot(n1, c1) - glm::dot(n2, c2);
        const float     dB = glm::dot(n1, c1) + glm::dot(n2, c2);

        const float aLen2 = glm::dot(nA, nA);
        const float bLen2 = glm::dot(nB_vec, nB_vec);

        const glm::vec3 dC = c2 - c1;
        const float sd1 = glm::dot(n2, dC);
        const float sd2 = glm::dot(n1, dC);

        if (aLen2 < 1e-10f)
        {
            // n1 ≈ n2 (parallel same-normal): A is degenerate, must use B.
            nRaw = nB_vec;  dRaw = dB;
        }
        else if (bLen2 < 1e-10f)
        {
            // n1 ≈ -n2 (anti-parallel): B is degenerate, must use A.
            nRaw = nA;      dRaw = dA;
        }
        else if (sd1 * sd2 < 0.0f)
        {
            // A separates the centroids — preferred case for slab and wedge.
            nRaw = nA;      dRaw = dA;
        }
        else
        {
            // A doesn't separate — fall back to B.
            nRaw = nB_vec;  dRaw = dB;
        }
    }

    const float     rawLen = glm::length(nRaw);
    const glm::vec3 nMid = nRaw / rawLen;
    const float     dMid = dRaw / rawLen;

    // ---- Anchor: midpoint of centroids, projected onto the midplane --------
    // (mid is generally NOT on either bisector — projection is a real step,
    // not a no-op.)
    const glm::vec3 mid = 0.5f * (c1 + c2);
    const glm::vec3 anchorLocal = mid - (glm::dot(nMid, mid) - dMid) * nMid;

    ApplyPlaneAlignmentToObject(objIdx, nMid, anchorLocal);
}

// ---------------------------------------------------------------------------
// RebuildMidplaneLockedVBO — twin of RebuildAlignHighlightVBO for the
// persistent locked-face overlay shown after the user picks face 1 in
// midplane mode.
// ---------------------------------------------------------------------------
void GLCanvas::RebuildMidplaneLockedVBO(const SceneObject& obj,
    const std::vector<uint32_t>& tris)
{
    if (tris.empty())
    {
        m_midplaneLockedVertexCount = 0;
        return;
    }

    const glm::mat4 model = obj.BuildModelMatrix();

    std::vector<float> verts;
    verts.reserve(tris.size() * 9);

    for (uint32_t t : tris)
    {
        for (int k = 0; k < 3; ++k)
        {
            const uint32_t vi = obj.cpuIndices[3 * t + k];
            const glm::vec3 vL(obj.cpuVerts[vi * 3],
                obj.cpuVerts[vi * 3 + 1],
                obj.cpuVerts[vi * 3 + 2]);
            const glm::vec3 vW = glm::vec3(model * glm::vec4(vL, 1.0f));
            verts.push_back(vW.x);
            verts.push_back(vW.y);
            verts.push_back(vW.z);
        }
    }

    if (m_midplaneLockedVAO == 0)
    {
        glGenVertexArrays(1, &m_midplaneLockedVAO);
        glGenBuffers(1, &m_midplaneLockedVBO);
        glBindVertexArray(m_midplaneLockedVAO);
        glBindBuffer(GL_ARRAY_BUFFER, m_midplaneLockedVBO);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void*)0);
        glBindVertexArray(0);
    }

    glBindBuffer(GL_ARRAY_BUFFER, m_midplaneLockedVBO);
    glBufferData(GL_ARRAY_BUFFER, verts.size() * sizeof(float),
        verts.data(), GL_DYNAMIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    m_midplaneLockedVertexCount = (GLsizei)(verts.size() / 3);
}

// ---------------------------------------------------------------------------
// RayCastParting — snaps to the nearest point on the mesh's parting line.
//
// Steps:
//  1. Cast mouse ray to world y=0 plane → planeHit (x, 0, z)
//  2. Walk every triangle of every imported object in world space.
//     Triangles that straddle y=0 contribute an intersection segment.
//  3. Find the closest point on any segment to planeHit.
//  4. If within kVentSnapRadius, set outPos (y forced to 0) and outNormal
//     (face normal projected onto the XZ plane so it lies on the parting
//     plane surface) and return true.
// ---------------------------------------------------------------------------

// Helper: closest point on segment [a,b] to point p (all in 2D XZ plane)
static glm::vec3 ClosestPointOnSegment(const glm::vec3& a, const glm::vec3& b, const glm::vec3& p)
{
    const glm::vec3 ab = b - a;
    const float     len2 = glm::dot(ab, ab);
    if (len2 < 1e-10f) return a;
    const float t = glm::clamp(glm::dot(p - a, ab) / len2, 0.0f, 1.0f);
    return a + t * ab;
}

bool GLCanvas::RayCastParting(int mouseX, int mouseY,
    glm::vec3& outPos, glm::vec3& outNormal,
    int* outObjectIndex)
{
    const wxSize sz = GetClientSize();
    const int    w = std::max(1, sz.x);
    const int    h = std::max(1, sz.y);

    // Build world-space ray
    const float ndcX = (2.0f * float(mouseX)) / float(w) - 1.0f;
    const float ndcY = 1.0f - (2.0f * float(mouseY)) / float(h);

    m_camera.SetAspect(float(w) / float(h));
    const glm::mat4 invVP = glm::inverse(m_camera.Projection() * m_camera.View());

    glm::vec4 nearH = invVP * glm::vec4(ndcX, ndcY, -1.0f, 1.0f);
    glm::vec4 farH = invVP * glm::vec4(ndcX, ndcY, 1.0f, 1.0f);
    nearH /= nearH.w;
    farH /= farH.w;

    const glm::vec3 rayOrig = glm::vec3(nearH);
    const glm::vec3 rayDir = glm::normalize(glm::vec3(farH) - glm::vec3(nearH));

    // Intersect ray with world y=0
    if (fabsf(rayDir.y) < 1e-6f) return false;   // ray parallel to parting plane
    const float t = -rayOrig.y / rayDir.y;
    if (t < 0.0f) return false;                   // plane is behind camera
    const glm::vec3 planeHit = rayOrig + rayDir * t;   // y == 0

    float     bestDist = kVentSnapRadius;
    glm::vec3 bestPos(0.0f);
    glm::vec3 bestNormal(0.0f, 1.0f, 0.0f);
    int       bestObj = -1;
    bool      found = false;

    for (int objIdx = 0; objIdx < (int)m_objects.size(); ++objIdx)
    {
        const auto& obj = m_objects[objIdx];
        if (obj.cpuVerts.empty() || obj.cpuIndices.empty()) continue;

        const glm::mat4 model = obj.BuildModelMatrix();
        const glm::mat3 normMat = glm::transpose(glm::inverse(glm::mat3(model)));

        for (size_t i = 0; i + 2 < obj.cpuIndices.size(); i += 3)
        {
            const uint32_t i0 = obj.cpuIndices[i];
            const uint32_t i1 = obj.cpuIndices[i + 1];
            const uint32_t i2 = obj.cpuIndices[i + 2];

            // Transform triangle to world space
            const glm::vec3 lv0(obj.cpuVerts[i0 * 3], obj.cpuVerts[i0 * 3 + 1], obj.cpuVerts[i0 * 3 + 2]);
            const glm::vec3 lv1(obj.cpuVerts[i1 * 3], obj.cpuVerts[i1 * 3 + 1], obj.cpuVerts[i1 * 3 + 2]);
            const glm::vec3 lv2(obj.cpuVerts[i2 * 3], obj.cpuVerts[i2 * 3 + 1], obj.cpuVerts[i2 * 3 + 2]);

            const glm::vec3 wv0 = glm::vec3(model * glm::vec4(lv0, 1.0f));
            const glm::vec3 wv1 = glm::vec3(model * glm::vec4(lv1, 1.0f));
            const glm::vec3 wv2 = glm::vec3(model * glm::vec4(lv2, 1.0f));

            // Does this triangle touch the parting band [-0.1, +0.1]?
            const float minY = std::min({ wv0.y, wv1.y, wv2.y });
            const float maxY = std::max({ wv0.y, wv1.y, wv2.y });
            static constexpr float kBand = 0.1f;
            if (minY >= kBand || maxY <= -kBand) continue;

            // Find up to two points on or near y=0 for this triangle.
            // Vertices inside the band are projected directly; edges crossing
            // fully (below<->above) are interpolated to y=0.
            const glm::vec3 verts[3] = { wv0, wv1, wv2 };
            glm::vec3 seg[3];   // up to 3 candidates
            int       segCount = 0;

            for (int v = 0; v < 3; ++v)
                if (fabsf(verts[v].y) <= kBand)
                    seg[segCount++] = glm::vec3(verts[v].x, 0.0f, verts[v].z);

            for (int e = 0; e < 3 && segCount < 3; ++e)
            {
                const glm::vec3& a = verts[e];
                const glm::vec3& b = verts[(e + 1) % 3];
                if (!((a.y < -kBand && b.y > kBand) || (a.y > kBand && b.y < -kBand))) continue;
                const float alpha = -a.y / (b.y - a.y);
                glm::vec3 cross = a + alpha * (b - a);
                cross.y = 0.0f;
                seg[segCount++] = cross;
            }
            if (segCount < 2) continue;

            // Closest point on this parting segment to the plane hit
            const glm::vec3 closest = ClosestPointOnSegment(seg[0], seg[1], planeHit);
            const float     dist = glm::length(glm::vec3(closest.x - planeHit.x,
                0.0f,
                closest.z - planeHit.z));

            if (dist < bestDist)
            {
                bestDist = dist;
                bestPos = closest;
                bestPos.y = 0.0f;   // clamp exactly onto parting plane

                // Face normal projected onto XZ so it lies on the parting surface
                const glm::vec3 localFaceN = glm::normalize(
                    glm::cross(lv1 - lv0, lv2 - lv0));
                glm::vec3 worldFaceN = glm::normalize(normMat * localFaceN);
                worldFaceN.y = 0.0f;
                const float nlen = glm::length(worldFaceN);
                bestNormal = (nlen > 1e-4f) ? worldFaceN / nlen : glm::vec3(0.0f, 0.0f, 1.0f);
                bestObj = objIdx;
                found = true;
            }
        }
    }

    if (found)
    {
        outPos = bestPos;
        outNormal = bestNormal;
    }
    if (outObjectIndex) *outObjectIndex = found ? bestObj : -1;
    return found;
}

// ---------------------------------------------------------------------------
// RayCastToPartingPlane — simple ray–plane intersection with world y=0.
// Returns the hit point on the plane; no mesh snapping.
// ---------------------------------------------------------------------------
bool GLCanvas::RayCastToPartingPlane(int mouseX, int mouseY, glm::vec3& outPos)
{
    const wxSize sz = GetClientSize();
    const int    w = std::max(1, sz.x);
    const int    h = std::max(1, sz.y);

    const float ndcX = (2.0f * float(mouseX)) / float(w) - 1.0f;
    const float ndcY = 1.0f - (2.0f * float(mouseY)) / float(h);

    m_camera.SetAspect(float(w) / float(h));
    const glm::mat4 invVP = glm::inverse(m_camera.Projection() * m_camera.View());

    glm::vec4 nearH = invVP * glm::vec4(ndcX, ndcY, -1.0f, 1.0f);
    glm::vec4 farH = invVP * glm::vec4(ndcX, ndcY, 1.0f, 1.0f);
    nearH /= nearH.w;
    farH /= farH.w;

    const glm::vec3 rayOrig = glm::vec3(nearH);
    const glm::vec3 rayDir = glm::normalize(glm::vec3(farH) - glm::vec3(nearH));

    if (std::abs(rayDir.y) < 1e-6f) return false;   // ray parallel to plane
    const float t = -rayOrig.y / rayDir.y;
    if (t < 0.0f) return false;                       // plane behind camera

    outPos = rayOrig + rayDir * t;                    // y == 0
    return true;
}

// ---------------------------------------------------------------------------
// RayCastEjectorSnap — pick helper for ejector placement.
//
// Considers four candidate sources, picks the one closest to the cursor in
// SCREEN SPACE (pixels), and returns its world-space position. Screen-space
// is the right metric for "snap feel" — feature spacing in 3D varies wildly
// with camera distance, but a fixed pixel radius behaves consistently at
// every zoom level (it's what the user actually sees).
//
// Candidate sources (only those currently valid in the scene):
//   1. Sprue parting point      — m_sprue.partingPos (degenerate "segment")
//   2. Each runner segment      — m_sprue.partingPos -> rf.point, on y=0
//   3. Each gate segment        — gf.point.worldPos -> gf.pathEnd
//   4. Object surfaces          — full mesh ray-cast via RayCastObjects
//
// Resolution rule:
//   - Path snap (1/2/3) wins if any candidate is within kEjectorSnapRadiusPx
//     of the cursor in pixels. Otherwise fall through to the face ray-cast.
//   - This priority ordering is deliberate: hovering over a face that a gate
//     path crosses should still let the user snap to the path, not always
//     win on the face. Outside the snap radius, faces take over.
//
// The "closest 3D point on segment AB to the picking ray" is the foot of the
// common perpendicular between the two lines, clamped to AB's [0,1] range.
// For the sprue parting point (a single point) and for object surfaces (a
// definite hit) the screen-space test is straightforward.
// ---------------------------------------------------------------------------
bool GLCanvas::RayCastEjectorSnap(int mouseX, int mouseY, glm::vec3& outPos)
{
    const wxSize sz = GetClientSize();
    const int    w = std::max(1, sz.x);
    const int    h = std::max(1, sz.y);

    m_camera.SetAspect(float(w) / float(h));
    const glm::mat4 view = m_camera.View();
    const glm::mat4 proj = m_camera.Projection();
    const glm::mat4 viewProj = proj * view;

    // Build the same world-space ray we'd use elsewhere (mirrors
    // RayCastObjects / RayCastToPartingPlane setup).
    const float ndcX = (2.0f * float(mouseX)) / float(w) - 1.0f;
    const float ndcY = 1.0f - (2.0f * float(mouseY)) / float(h);
    const glm::mat4 invVP = glm::inverse(viewProj);
    glm::vec4 nearH = invVP * glm::vec4(ndcX, ndcY, -1.0f, 1.0f);
    glm::vec4 farH = invVP * glm::vec4(ndcX, ndcY, 1.0f, 1.0f);
    nearH /= nearH.w;
    farH /= farH.w;
    const glm::vec3 rayOrig = glm::vec3(nearH);
    const glm::vec3 rayDir = glm::normalize(glm::vec3(farH) - glm::vec3(nearH));

    // Cursor in pixel coords (captured by lambdas below). Brace init is
    // mandatory here: 'glm::vec2 cursorPx(float(mouseX), float(mouseY))' is
    // interpreted by MSVC as a function declaration (the "most vexing
    // parse"), which then causes confusing cascade errors at every use site.
    const glm::vec2 cursorPx{ float(mouseX), float(mouseY) };

    // Helper: project a world-space point to screen pixels. Returns false if
    // the point is behind the camera (clip-space w <= 0). Explicit return
    // type kept off the trailing position to avoid a known MSVC parser
    // quirk where trailing-return lambdas in tight succession can confuse
    // the parser of a following statement; both helpers are written as
    // void/bool returners with the type in front via the declared return.
    auto worldToScreen = [&](const glm::vec3& world, glm::vec2& outPx)
        {
            const glm::vec4 clip = viewProj * glm::vec4(world, 1.0f);
            if (clip.w <= 1e-6f) return false;
            const glm::vec3 ndc(clip.x / clip.w, clip.y / clip.w, clip.z / clip.w);
            outPx.x = (ndc.x * 0.5f + 0.5f) * float(w);
            outPx.y = (1.0f - (ndc.y * 0.5f + 0.5f)) * float(h);
            return true;
        };

    // Helper: closest point on segment AB to the picking ray. Computes the
    // common perpendicular's foot on AB and clamps to [0,1]. Degenerate
    // segments collapse to A.
    auto closestOnSegment = [&](const glm::vec3& A, const glm::vec3& B)
        {
            const glm::vec3 AB = B - A;
            const float lenAB2 = glm::dot(AB, AB);
            if (lenAB2 < 1e-10f) return A;

            const glm::vec3 w0 = A - rayOrig;
            const float aSq = lenAB2;
            const float bDot = glm::dot(AB, rayDir);
            const float cSq = glm::dot(rayDir, rayDir);   // == 1 (rayDir is normalised)
            const float dDot = glm::dot(AB, w0);
            const float eDot = glm::dot(rayDir, w0);
            const float denom = aSq * cSq - bDot * bDot;  // 0 iff AB || ray

            float tSeg = 0.0f;
            if (denom >= 1e-10f)
                tSeg = (bDot * eDot - cSq * dDot) / denom;
            tSeg = glm::clamp(tSeg, 0.0f, 1.0f);
            return A + AB * tSeg;
        };

    float     bestPx = kEjectorSnapRadiusPx;
    glm::vec3 bestWorld(0.0f);
    bool      hit = false;

    // Inlined snap test rather than a third lambda — keeps the math local
    // to each candidate and avoids any chance of parser confusion from
    // back-to-back captured lambdas.
    auto consider = [&](const glm::vec3& candidate)
        {
            glm::vec2 px;
            if (!worldToScreen(candidate, px)) return;
            const glm::vec2 delta = px - cursorPx;
            const float distPx = glm::length(delta);
            if (distPx < bestPx)
            {
                bestPx = distPx;
                bestWorld = candidate;
                hit = true;
            }
        };

    // 1. Sprue parting point (single-point candidate).
    if (m_sprue.hasPartingPoint)
        consider(m_sprue.partingPos);

    // 2. Runner segments (sprue parting -> runner pt, on y=0).
    if (m_sprue.hasPartingPoint)
        for (const RunnerFeature& rf : m_runners)
            consider(closestOnSegment(m_sprue.partingPos, rf.point));

    // 3. Gate segments (gate worldPos -> gate.pathEnd). pathEnd is only
    //    valid when hasPath is true (computed by RebuildGatePathVBO).
    for (const GateFeature& gf : m_gates)
    {
        if (!gf.hasPath) continue;
        consider(closestOnSegment(gf.point.worldPos, gf.pathEnd));
    }

    if (hit)
    {
        outPos = bestWorld;
        return true;
    }

    // 4. Fall through to object surfaces. RayCastObjects does the full
    //    Möller-Trumbore mesh test and returns the world-space hit; we
    //    discard the normal (ejectors don't store one).
    glm::vec3 faceHit, faceNormal;
    if (RayCastObjects(mouseX, mouseY, faceHit, faceNormal))
    {
        outPos = faceHit;
        return true;
    }

    return false;
}

// ---------------------------------------------------------------------------
// ClearRunnerPoints
// ---------------------------------------------------------------------------
void GLCanvas::ClearRunnerPoints()
{
    for (auto& rf : m_runners) rf.Destroy();
    m_runners.clear();
    RebuildRunnerPathVBO();
    RebuildGatePathVBO();
    RebuildGateSolids();
    Refresh(false);
    NotifySceneMutated();
}

void GLCanvas::ClearGatePoints()
{
    for (auto& gf : m_gates) gf.Destroy();
    m_gates.clear();
    m_gateGhostActive = false;
    RebuildGatePathVBO();
    RebuildGateSolids();
    Refresh(false);
    NotifySceneMutated();
}

void GLCanvas::ClearEjectors()
{
    // Mirrors ClearGatePoints — destroys GPU resources, clears the vector,
    // resets the ghost preview, and refreshes. No path VBO equivalent for
    // ejectors (they're standalone cylinders, not connected to a feed
    // network), so RebuildEjectorSolids() is the only rebuild needed; with
    // an empty vector it falls through its early-out cleanly.
    for (auto& ef : m_ejectors) ef.Destroy();
    m_ejectors.clear();
    m_ejectorGhostActive = false;
    Refresh(false);
    NotifySceneMutated();
}

// ---------------------------------------------------------------------------
// BuildMouseRay — unprojects mouse coordinates into a world-space ray.
// ---------------------------------------------------------------------------
void GLCanvas::BuildMouseRay(int mouseX, int mouseY,
    glm::vec3& outOrigin, glm::vec3& outDir)
{
    const wxSize sz = GetClientSize();
    const int    w = std::max(1, sz.x);
    const int    h = std::max(1, sz.y);

    const float ndcX = (2.0f * float(mouseX)) / float(w) - 1.0f;
    const float ndcY = 1.0f - (2.0f * float(mouseY)) / float(h);

    m_camera.SetAspect(float(w) / float(h));
    const glm::mat4 view = m_camera.View();
    const glm::mat4 proj = m_camera.Projection();
    const glm::mat4 invVP = glm::inverse(proj * view);

    glm::vec4 nearH = invVP * glm::vec4(ndcX, ndcY, -1.0f, 1.0f);
    glm::vec4 farH = invVP * glm::vec4(ndcX, ndcY, 1.0f, 1.0f);
    nearH /= nearH.w;
    farH /= farH.w;

    outOrigin = glm::vec3(nearH);
    outDir = glm::normalize(glm::vec3(farH) - glm::vec3(nearH));
}

// ---------------------------------------------------------------------------
// Helper: point-to-ray distance (used for feature marker hit-testing).
// Returns the perpendicular distance from 'point' to the ray (origin, dir).
// ---------------------------------------------------------------------------
static float PointRayDistance(const glm::vec3& point,
    const glm::vec3& origin, const glm::vec3& dir)
{
    const glm::vec3 v = point - origin;
    const float t = glm::dot(v, dir);
    if (t < 0.0f) return glm::length(v);          // behind the camera
    const glm::vec3 closest = origin + dir * t;
    return glm::length(point - closest);
}

// ---------------------------------------------------------------------------
// RemoveVentAtMouse — removes the vent whose marker is closest to the ray.
// ---------------------------------------------------------------------------
void GLCanvas::RemoveVentAtMouse(int mouseX, int mouseY)
{
    if (m_vents.empty()) return;

    glm::vec3 rayOrig, rayDir;
    BuildMouseRay(mouseX, mouseY, rayOrig, rayDir);

    const float hitRadius = kVentMarkerRadius * 2.0f;   // generous pick radius
    float bestDist = hitRadius;
    int   bestIdx = -1;

    for (int i = 0; i < (int)m_vents.size(); ++i)
    {
        const float d = PointRayDistance(m_vents[i].point.worldPos, rayOrig, rayDir);
        if (d < bestDist)
        {
            bestDist = d;
            bestIdx = i;
        }
    }

    if (bestIdx < 0) return;

    m_vents[bestIdx].Destroy();
    m_vents.erase(m_vents.begin() + bestIdx);

    RebuildPathVBO();
    RebuildCrossSectionVBO();
    Refresh(false);
    NotifySceneMutated();
}

// ---------------------------------------------------------------------------
// RemoveRunnerAtMouse — removes the runner whose marker is closest to the ray.
// ---------------------------------------------------------------------------
void GLCanvas::RemoveRunnerAtMouse(int mouseX, int mouseY)
{
    if (m_runners.empty()) return;

    glm::vec3 rayOrig, rayDir;
    BuildMouseRay(mouseX, mouseY, rayOrig, rayDir);

    const float hitRadius = kVentMarkerRadius * 2.0f;
    float bestDist = hitRadius;
    int   bestIdx = -1;

    for (int i = 0; i < (int)m_runners.size(); ++i)
    {
        const float d = PointRayDistance(m_runners[i].point, rayOrig, rayDir);
        if (d < bestDist)
        {
            bestDist = d;
            bestIdx = i;
        }
    }

    if (bestIdx < 0) return;

    m_runners[bestIdx].Destroy();
    m_runners.erase(m_runners.begin() + bestIdx);

    RebuildRunnerPathVBO();
    RebuildRunnerSolids();
    RebuildGatePathVBO();
    RebuildGateSolids();
    Refresh(false);
    NotifySceneMutated();
}

// ---------------------------------------------------------------------------
// RemoveGateAtMouse — removes the gate whose marker is closest to the ray.
// ---------------------------------------------------------------------------
void GLCanvas::RemoveGateAtMouse(int mouseX, int mouseY)
{
    if (m_gates.empty()) return;

    glm::vec3 rayOrig, rayDir;
    BuildMouseRay(mouseX, mouseY, rayOrig, rayDir);

    const float hitRadius = kVentMarkerRadius * 2.0f;
    float bestDist = hitRadius;
    int   bestIdx = -1;

    for (int i = 0; i < (int)m_gates.size(); ++i)
    {
        const float d = PointRayDistance(m_gates[i].point.worldPos, rayOrig, rayDir);
        if (d < bestDist)
        {
            bestDist = d;
            bestIdx = i;
        }
    }

    if (bestIdx < 0) return;

    m_gates[bestIdx].Destroy();
    m_gates.erase(m_gates.begin() + bestIdx);

    RebuildGatePathVBO();
    RebuildGateSolids();
    Refresh(false);
    NotifySceneMutated();
}

// ---------------------------------------------------------------------------
// RemoveEjectorAtMouse — removes the ejector whose marker is closest to the
// ray. Mirrors RemoveGateAtMouse but operates on m_ejectors and only needs
// to rebuild the ejector solids (no path VBO).
// ---------------------------------------------------------------------------
void GLCanvas::RemoveEjectorAtMouse(int mouseX, int mouseY)
{
    if (m_ejectors.empty()) return;

    glm::vec3 rayOrig, rayDir;
    BuildMouseRay(mouseX, mouseY, rayOrig, rayDir);

    const float hitRadius = kVentMarkerRadius * 2.0f;
    float bestDist = hitRadius;
    int   bestIdx = -1;

    for (int i = 0; i < (int)m_ejectors.size(); ++i)
    {
        const float d = PointRayDistance(m_ejectors[i].point, rayOrig, rayDir);
        if (d < bestDist)
        {
            bestDist = d;
            bestIdx = i;
        }
    }

    if (bestIdx < 0) return;

    m_ejectors[bestIdx].Destroy();
    m_ejectors.erase(m_ejectors.begin() + bestIdx);

    RebuildEjectorSolids();
    Refresh(false);
    NotifySceneMutated();
}

// ---------------------------------------------------------------------------
// RemoveSprueAtMouse — removes the sprue if the click hits its marker.
// ---------------------------------------------------------------------------
void GLCanvas::RemoveSprueAtMouse(int mouseX, int mouseY)
{
    if (!m_sprue.hasPoint) return;

    glm::vec3 rayOrig, rayDir;
    BuildMouseRay(mouseX, mouseY, rayOrig, rayDir);

    const float hitRadius = kVentMarkerRadius * 3.0f;   // sprue markers are 1.5× bigger
    const float d = PointRayDistance(m_sprue.worldPos, rayOrig, rayDir);

    if (d > hitRadius) return;

    ClearSprue();     // reuses existing full-cleanup logic
}

// ---------------------------------------------------------------------------
// BuildFixturePerimeter — collects all y=0 XZ crossing points from every
// fixture triangle, then computes their 2D convex hull (Graham scan).
// Result is cached in m_fixturePerimeter and is only rebuilt when fixtures
// change, not per-frame.
// ---------------------------------------------------------------------------

// Convex hull helpers (2D in glm::vec2 = XZ plane)
static float Cross2D(const glm::vec2& O, const glm::vec2& A, const glm::vec2& B)
{
    return (A.x - O.x) * (B.y - O.y) - (A.y - O.y) * (B.x - O.x);
}

// Returns true when pt is inside (or on the boundary of) a convex hull stored
// in CCW order — the winding produced by the ConvexHull function below.
static bool IsInsideConvexHull(const std::vector<glm::vec2>& hull, const glm::vec2& pt)
{
    const int n = (int)hull.size();
    if (n < 3) return false;
    for (int i = 0; i < n; ++i)
    {
        if (Cross2D(hull[i], hull[(i + 1) % n], pt) < 0.0f)
            return false;
    }
    return true;
}

static std::vector<glm::vec2> ConvexHull(std::vector<glm::vec2> pts)
{
    const int n = (int)pts.size();
    if (n < 3) return pts;

    // Sort by x, then y
    std::sort(pts.begin(), pts.end(), [](const glm::vec2& a, const glm::vec2& b) {
        return a.x < b.x || (a.x == b.x && a.y < b.y);
        });

    std::vector<glm::vec2> hull;
    hull.reserve(2 * n);

    // Lower hull
    for (int i = 0; i < n; ++i)
    {
        while (hull.size() >= 2 &&
            Cross2D(hull[hull.size() - 2], hull[hull.size() - 1], pts[i]) <= 0.0f)
            hull.pop_back();
        hull.push_back(pts[i]);
    }

    // Upper hull
    const int lower_size = (int)hull.size() + 1;
    for (int i = n - 2; i >= 0; --i)
    {
        while ((int)hull.size() >= lower_size &&
            Cross2D(hull[hull.size() - 2], hull[hull.size() - 1], pts[i]) <= 0.0f)
            hull.pop_back();
        hull.push_back(pts[i]);
    }

    hull.pop_back();   // last point == first point
    return hull;
}

void GLCanvas::BuildFixturePerimeter()
{
    m_fixturePerimeter.clear();

    // Half-thickness of the virtual parting band (world units).
    // Triangles whose Y extent overlaps [-kBand, +kBand] contribute points.
    static constexpr float kBand = 0.1f;

    // Vertex classification relative to the parting band.
    enum class Side { Below, Band, Above };
    auto classify = [](float y) -> Side {
        if (y < -kBand) return Side::Below;
        if (y > kBand) return Side::Above;
        return Side::Band;
        };

    std::vector<glm::vec2> pts;

    for (const auto& fix : m_fixtures)
    {
        if (fix.cpuVerts.empty() || fix.cpuIndices.empty()) continue;

        const glm::mat4 model = fix.BuildModelMatrix();

        for (size_t i = 0; i + 2 < fix.cpuIndices.size(); i += 3)
        {
            const uint32_t i0 = fix.cpuIndices[i];
            const uint32_t i1 = fix.cpuIndices[i + 1];
            const uint32_t i2 = fix.cpuIndices[i + 2];

            const glm::vec3 lv0(fix.cpuVerts[i0 * 3], fix.cpuVerts[i0 * 3 + 1], fix.cpuVerts[i0 * 3 + 2]);
            const glm::vec3 lv1(fix.cpuVerts[i1 * 3], fix.cpuVerts[i1 * 3 + 1], fix.cpuVerts[i1 * 3 + 2]);
            const glm::vec3 lv2(fix.cpuVerts[i2 * 3], fix.cpuVerts[i2 * 3 + 1], fix.cpuVerts[i2 * 3 + 2]);

            const glm::vec3 wv[3] = {
                glm::vec3(model * glm::vec4(lv0, 1.0f)),
                glm::vec3(model * glm::vec4(lv1, 1.0f)),
                glm::vec3(model * glm::vec4(lv2, 1.0f))
            };

            const Side s[3] = { classify(wv[0].y), classify(wv[1].y), classify(wv[2].y) };

            // Skip triangles entirely above or entirely below the band
            if (s[0] == Side::Above && s[1] == Side::Above && s[2] == Side::Above) continue;
            if (s[0] == Side::Below && s[1] == Side::Below && s[2] == Side::Below) continue;

            // Any vertex inside the band is projected directly onto y=0
            for (int v = 0; v < 3; ++v)
                if (s[v] == Side::Band)
                    pts.emplace_back(wv[v].x, wv[v].z);

            // Edges that cross fully (Below<->Above, skipping Band vertices since
            // those are already included) get an exact y=0 intersection point
            for (int e = 0; e < 3; ++e)
            {
                const int      next = (e + 1) % 3;
                const Side     sa = s[e];
                const Side     sb = s[next];
                const glm::vec3& a = wv[e];
                const glm::vec3& b = wv[next];

                // Only interpolate across a full Below<->Above crossing
                if ((sa == Side::Below && sb == Side::Above) ||
                    (sa == Side::Above && sb == Side::Below))
                {
                    const float alpha = -a.y / (b.y - a.y);
                    const glm::vec3 crossing = a + alpha * (b - a);
                    pts.emplace_back(crossing.x, crossing.z);
                }
            }
        }
    }

    if (pts.size() >= 3)
        m_fixturePerimeter = ConvexHull(pts);
}

// ---------------------------------------------------------------------------
// ComputeVentPath — finds the closest point on the fixture perimeter hull
// to the vent origin and draws a straight line to it on the parting plane.
// ---------------------------------------------------------------------------
VentPath GLCanvas::ComputeVentPath(const VentPoint& vp)
{
    VentPath result;
    result.start = vp.worldPos;
    result.valid = false;

    if (m_fixturePerimeter.size() < 2)
        return result;

    const glm::vec2 origin(vp.worldPos.x, vp.worldPos.z);

    float     bestDist = std::numeric_limits<float>::max();
    glm::vec2 bestPt(0.0f);

    const int n = (int)m_fixturePerimeter.size();
    for (int i = 0; i < n; ++i)
    {
        const glm::vec2& A = m_fixturePerimeter[i];
        const glm::vec2& B = m_fixturePerimeter[(i + 1) % n];

        // Closest point on edge AB to origin
        const glm::vec2 AB = B - A;
        const float     len2 = glm::dot(AB, AB);
        float           t = 0.0f;
        if (len2 > 1e-10f)
            t = glm::clamp(glm::dot(origin - A, AB) / len2, 0.0f, 1.0f);

        const glm::vec2 closest = A + t * AB;
        const float     dist = glm::length(closest - origin);

        if (dist < bestDist)
        {
            bestDist = dist;
            bestPt = closest;
        }
    }

    result.end = glm::vec3(bestPt.x, 0.0f, bestPt.y);
    result.valid = true;
    return result;
}

// ---------------------------------------------------------------------------
// RebuildPathVBO — uploads all vent path line vertices to the GPU.
// Each path = 2 vertices of 3 floats = 6 floats.
// ---------------------------------------------------------------------------
void GLCanvas::RebuildPathVBO()
{
    if (!m_pathVAO) return;

    std::vector<float> verts;
    verts.reserve(m_vents.size() * 6);
    for (const VentInstance& v : m_vents)
    {
        if (!v.path.valid) continue;
        verts.push_back(v.path.start.x); verts.push_back(v.path.start.y); verts.push_back(v.path.start.z);
        verts.push_back(v.path.end.x);   verts.push_back(v.path.end.y);   verts.push_back(v.path.end.z);
    }

    m_pathVertexCount = (GLsizei)verts.size() / 3;

    glBindVertexArray(m_pathVAO);
    glBindBuffer(GL_ARRAY_BUFFER, m_pathVBO);
    glBufferData(GL_ARRAY_BUFFER,
        (GLsizeiptr)(verts.size() * sizeof(float)),
        verts.empty() ? nullptr : verts.data(),
        GL_DYNAMIC_DRAW);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);
    glBindVertexArray(0);
}

// ---------------------------------------------------------------------------
// BuildVentCrossSection — constructs a rectangular profile at the vent origin,
// centred on the parting plane, perpendicular to the path direction.
//
// Orientation:
//   pathDir  = normalised XZ direction from start to end (lies on y=0 plane)
//   sideAxis = pathDir rotated 90° in XZ  (the "width" axis)
//   upAxis   = world Y                    (the "depth" axis, into each half)
//
// The rectangle has:
//   half-width  = width / 2  along sideAxis
//   half-depth  = depth / 2  along ±Y
// ---------------------------------------------------------------------------
VentCrossSection GLCanvas::BuildVentCrossSection(const VentPath& path,
    float width, float depth)
{
    VentCrossSection xs;
    xs.valid = false;

    if (!path.valid) return xs;

    const glm::vec3 start = path.start;
    const glm::vec3 diff = path.end - path.start;
    const float     len = glm::length(glm::vec2(diff.x, diff.z));
    if (len < 1e-6f) return xs;

    // Path direction in XZ (y=0)
    const glm::vec3 pathDir(diff.x / len, 0.0f, diff.z / len);

    // Perpendicular in XZ: rotate pathDir 90° around Y
    const glm::vec3 sideAxis(-pathDir.z, 0.0f, pathDir.x);
    const glm::vec3 upAxis(0.0f, 1.0f, 0.0f);

    const float hw = width * 0.5f;
    const float hd = depth * 0.5f;

    // BL, BR, TR, TL  (B=below parting plane, T=above, L=left, R=right)
    xs.corners[0] = start - sideAxis * hw - upAxis * hd;
    xs.corners[1] = start + sideAxis * hw - upAxis * hd;
    xs.corners[2] = start + sideAxis * hw + upAxis * hd;
    xs.corners[3] = start - sideAxis * hw + upAxis * hd;
    xs.valid = true;
    return xs;
}

// ---------------------------------------------------------------------------
// RebuildCrossSectionVBO — packs all cross-section rectangles as line loops
// into a single VBO (4 verts × 3 floats per cross-section, drawn as
// GL_LINES with explicit pairs so one Draw call covers all of them).
// ---------------------------------------------------------------------------
void GLCanvas::RebuildCrossSectionVBO()
{
    if (!m_xsecVAO) return;

    // Each rectangle = 4 edges = 8 verts (pairs: 0-1, 1-2, 2-3, 3-0)
    std::vector<float> verts;
    verts.reserve(m_vents.size() * 24);

    for (const VentInstance& v : m_vents)
    {
        const VentCrossSection& xs = v.crossSection;
        if (!xs.valid) continue;
        auto push = [&](const glm::vec3& v) {
            verts.push_back(v.x); verts.push_back(v.y); verts.push_back(v.z);
            };
        push(xs.corners[0]); push(xs.corners[1]);
        push(xs.corners[1]); push(xs.corners[2]);
        push(xs.corners[2]); push(xs.corners[3]);
        push(xs.corners[3]); push(xs.corners[0]);
    }

    m_xsecVertexCount = (GLsizei)verts.size() / 3;

    glBindVertexArray(m_xsecVAO);
    glBindBuffer(GL_ARRAY_BUFFER, m_xsecVBO);
    glBufferData(GL_ARRAY_BUFFER,
        (GLsizeiptr)(verts.size() * sizeof(float)),
        verts.empty() ? nullptr : verts.data(),
        GL_DYNAMIC_DRAW);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);
    glBindVertexArray(0);
}


// ---------------------------------------------------------------------------
// GL init
// ---------------------------------------------------------------------------
// GetAnyGLFuncAddress() now lives in GLLoader.cpp — shared with
// FixtureCanvas. Same Win32 wglGetProcAddress + opengl32.dll fallback.

void GLCanvas::InitGLOnce()
{
    if (m_inited) return;
    SetCurrent(*m_context);

    if (!gladLoadGLLoader((GLADloadproc)GLLoader::GetAnyGLFuncAddress)) {
        wxLogError("Failed to load OpenGL functions");
        return;
    }

    glEnable(GL_DEPTH_TEST);
    glEnable(GL_MULTISAMPLE);

    // Lit program
    GLuint vs = Compile(GL_VERTEX_SHADER, m_shaders.vsLit);
    GLuint fs = Compile(GL_FRAGMENT_SHADER, m_shaders.fsLit);
    m_program = Link(vs, fs);

    // Picking program
    {
        GLuint pvs = Compile(GL_VERTEX_SHADER, m_shaders.vsPick);
        GLuint pfs = Compile(GL_FRAGMENT_SHADER, m_shaders.fsPick);
        m_pickProgram = Link(pvs, pfs);
        m_pick_uMVP = glGetUniformLocation(m_pickProgram, "uMVP");
        m_pick_uObjectId = glGetUniformLocation(m_pickProgram, "uObjectId");
    }

    // Outline program
    {
        GLuint ovs = Compile(GL_VERTEX_SHADER, m_shaders.vsFullscreen);
        GLuint ofs = Compile(GL_FRAGMENT_SHADER, m_shaders.fsOutline);
        m_outlineProgram = Link(ovs, ofs);
        m_outline_uIdTex = glGetUniformLocation(m_outlineProgram, "uIdTex");
        m_outline_uTargetId = glGetUniformLocation(m_outlineProgram, "uTargetId");
        m_outline_uTexSize = glGetUniformLocation(m_outlineProgram, "uTexSize");
        m_outline_uAlpha = glGetUniformLocation(m_outlineProgram, "uAlpha");
        m_outline_uThickness = glGetUniformLocation(m_outlineProgram, "uThickness");
        glGenVertexArrays(1, &m_fullscreenVAO);
    }

    // Fallback pyramid
    float verts[] = {
         0.0f,  0.8f,  0.0f,   1.f, 1.f, 1.f,
        -0.5f, -0.5f,  0.5f,   1.f, 0.f, 0.f,
         0.5f, -0.5f,  0.5f,   0.f, 1.f, 0.f,
         0.5f, -0.5f, -0.5f,   0.f, 0.f, 1.f,
        -0.5f, -0.5f, -0.5f,   1.f, 1.f, 0.f
    };
    unsigned int idx[] = { 0,1,2, 0,2,3, 0,3,4, 0,4,1, 1,4,3, 1,3,2 };

    glGenVertexArrays(1, &m_vao);
    glGenBuffers(1, &m_vbo);
    glGenBuffers(1, &m_ebo);
    glBindVertexArray(m_vao);
    glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(idx), idx, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)(3 * sizeof(float)));
    glEnableVertexAttribArray(1);
    glBindVertexArray(0);

    // Build vent-point marker sphere
    BuildSphereGPU(1.0f, 12, 16);

    // Flat shader for vent path lines
    {
        GLuint fvs = Compile(GL_VERTEX_SHADER, m_shaders.vsFlat);
        GLuint ffs = Compile(GL_FRAGMENT_SHADER, m_shaders.fsFlat);
        m_flatProgram = Link(fvs, ffs);
        m_flat_uVP = glGetUniformLocation(m_flatProgram, "uVP");
        m_flat_uColor = glGetUniformLocation(m_flatProgram, "uColor");
    }

    // Vent path line VBO (dynamic, rebuilt whenever paths change)
    glGenVertexArrays(1, &m_pathVAO);
    glGenBuffers(1, &m_pathVBO);
    glBindVertexArray(m_pathVAO);
    glBindBuffer(GL_ARRAY_BUFFER, m_pathVBO);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);
    glBindVertexArray(0);

    // Vent cross-section VBO (dynamic)
    glGenVertexArrays(1, &m_xsecVAO);
    glGenBuffers(1, &m_xsecVBO);
    glBindVertexArray(m_xsecVAO);
    glBindBuffer(GL_ARRAY_BUFFER, m_xsecVBO);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);
    glBindVertexArray(0);

    // Sprue path line VBO (dynamic, holds a single start→end segment)
    glGenVertexArrays(1, &m_sprue.pathVAO);
    glGenBuffers(1, &m_sprue.pathVBO);
    glBindVertexArray(m_sprue.pathVAO);
    glBindBuffer(GL_ARRAY_BUFFER, m_sprue.pathVBO);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);
    glBindVertexArray(0);

    // Sprue cross-section circle VBO (dynamic, N-segment line-loop)
    glGenVertexArrays(1, &m_sprue.xsecVAO);
    glGenBuffers(1, &m_sprue.xsecVBO);
    glBindVertexArray(m_sprue.xsecVAO);
    glBindBuffer(GL_ARRAY_BUFFER, m_sprue.xsecVBO);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);
    glBindVertexArray(0);

    // Runner path line VBO (dynamic, sprue parting point → runner points)
    glGenVertexArrays(1, &m_runnerPathVAO);
    glGenBuffers(1, &m_runnerPathVBO);
    glBindVertexArray(m_runnerPathVAO);
    glBindBuffer(GL_ARRAY_BUFFER, m_runnerPathVBO);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);
    glBindVertexArray(0);

    // Gate path line VBO (dynamic, gate point → nearest feed point)
    glGenVertexArrays(1, &m_gatePathVAO);
    glGenBuffers(1, &m_gatePathVBO);
    glBindVertexArray(m_gatePathVAO);
    glBindBuffer(GL_ARRAY_BUFFER, m_gatePathVBO);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);
    glBindVertexArray(0);

    m_inited = true;
}

void GLCanvas::DestroyGL()
{
    for (auto& obj : m_fixtures) obj.mesh.Destroy();
    m_fixtures.clear();
    for (auto& obj : m_objects)  obj.mesh.Destroy();
    m_objects.clear();
    for (auto& v : m_vents) v.Destroy();
    m_vents.clear();
    for (auto& rf : m_runners) rf.Destroy();
    m_runners.clear();
    for (auto& gf : m_gates) gf.Destroy();
    m_gates.clear();
    m_sprue.DestroyGL();

    if (m_outlineProgram) { glDeleteProgram(m_outlineProgram);        m_outlineProgram = 0; }
    if (m_flatProgram) { glDeleteProgram(m_flatProgram);           m_flatProgram = 0; }
    if (m_fullscreenVAO) { glDeleteVertexArrays(1, &m_fullscreenVAO); m_fullscreenVAO = 0; }
    if (m_pathVBO) { glDeleteBuffers(1, &m_pathVBO);              m_pathVBO = 0; }
    if (m_pathVAO) { glDeleteVertexArrays(1, &m_pathVAO);         m_pathVAO = 0; }
    if (m_xsecVBO) { glDeleteBuffers(1, &m_xsecVBO);              m_xsecVBO = 0; }
    if (m_xsecVAO) { glDeleteVertexArrays(1, &m_xsecVAO);         m_xsecVAO = 0; }
    if (m_runnerPathVBO) { glDeleteBuffers(1, &m_runnerPathVBO);         m_runnerPathVBO = 0; }
    if (m_runnerPathVAO) { glDeleteVertexArrays(1, &m_runnerPathVAO);    m_runnerPathVAO = 0; }
    if (m_gatePathVBO) { glDeleteBuffers(1, &m_gatePathVBO);           m_gatePathVBO = 0; }
    if (m_gatePathVAO) { glDeleteVertexArrays(1, &m_gatePathVAO);      m_gatePathVAO = 0; }
    if (m_alignHighlightVBO) { glDeleteBuffers(1, &m_alignHighlightVBO);     m_alignHighlightVBO = 0; }
    if (m_alignHighlightVAO) { glDeleteVertexArrays(1, &m_alignHighlightVAO); m_alignHighlightVAO = 0; }
    m_alignHighlightVertexCount = 0;
    if (m_midplaneLockedVBO) { glDeleteBuffers(1, &m_midplaneLockedVBO);     m_midplaneLockedVBO = 0; }
    if (m_midplaneLockedVAO) { glDeleteVertexArrays(1, &m_midplaneLockedVAO); m_midplaneLockedVAO = 0; }
    m_midplaneLockedVertexCount = 0;
    if (m_sphereEBO) { glDeleteBuffers(1, &m_sphereEBO);          m_sphereEBO = 0; }
    if (m_sphereVBO) { glDeleteBuffers(1, &m_sphereVBO);          m_sphereVBO = 0; }
    if (m_sphereVAO) { glDeleteVertexArrays(1, &m_sphereVAO);     m_sphereVAO = 0; }
    // Preview debug overlays (shot colouring groups + ray/contact geometry).
    for (auto& g : m_shotDebug.groups)
        if (g.ebo) { glDeleteBuffers(1, &g.ebo); g.ebo = 0; }
    m_shotDebug.groups.clear();
    if (m_shotDebug.vao) { glDeleteVertexArrays(1, &m_shotDebug.vao); m_shotDebug.vao = 0; }
    if (m_debugRayVbo) { glDeleteBuffers(1, &m_debugRayVbo);          m_debugRayVbo = 0; }
    if (m_debugRayVao) { glDeleteVertexArrays(1, &m_debugRayVao);     m_debugRayVao = 0; }
    if (m_debugContactVbo) { glDeleteBuffers(1, &m_debugContactVbo);      m_debugContactVbo = 0; }
    if (m_debugContactVao) { glDeleteVertexArrays(1, &m_debugContactVao); m_debugContactVao = 0; }
    m_debugSolidObj.mesh.Destroy();
    if (m_vbo) { glDeleteBuffers(1, &m_vbo);               m_vbo = 0; }
    if (m_vao) { glDeleteVertexArrays(1, &m_vao);          m_vao = 0; }
    if (m_ebo) { glDeleteBuffers(1, &m_ebo);               m_ebo = 0; }
    if (m_program) { glDeleteProgram(m_program);               m_program = 0; }
    if (m_pickProgram) { glDeleteProgram(m_pickProgram);           m_pickProgram = 0; }
    DestroyPickFBO();
}

// ---------------------------------------------------------------------------
// GPU upload — writes into a SceneObject
// ---------------------------------------------------------------------------
void GLCanvas::UploadMeshToGPU(const FileImporter::MeshData& mesh, SceneObject& obj)
{
    const std::vector<float>& vtx = !mesh.posNorm.empty() ? mesh.posNorm : mesh.vertices;
    const int strideFloats = !mesh.posNorm.empty() ? 6 : 3;

    if (vtx.empty() || mesh.indices.empty()) return;
    if ((vtx.size() % strideFloats) != 0)    return;

    GPUMesh newMesh{};
    glGenVertexArrays(1, &newMesh.vao);
    glGenBuffers(1, &newMesh.vbo);
    glGenBuffers(1, &newMesh.ebo);

    glBindVertexArray(newMesh.vao);
    glBindBuffer(GL_ARRAY_BUFFER, newMesh.vbo);
    glBufferData(GL_ARRAY_BUFFER,
        (GLsizeiptr)(vtx.size() * sizeof(float)),
        vtx.data(), GL_STATIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, newMesh.ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER,
        (GLsizeiptr)(mesh.indices.size() * sizeof(uint32_t)),
        mesh.indices.data(), GL_STATIC_DRAW);

    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE,
        strideFloats * (GLsizei)sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);

    if (strideFloats == 6) {
        glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE,
            6 * (GLsizei)sizeof(float),
            (void*)(3 * sizeof(float)));
        glEnableVertexAttribArray(1);
    }
    else {
        glDisableVertexAttribArray(1);
        glVertexAttrib3f(1, 0.0f, 1.0f, 0.0f);
    }

    glBindVertexArray(0);
    newMesh.indexCount = (GLsizei)mesh.indices.size();

    obj.mesh.Destroy();
    obj.mesh = newMesh;
}

void GLCanvas::ExportFixtures(const std::string& pathA, const std::string& pathB)
{
    if (m_fixtures.empty())
    {
        wxMessageBox("No fixtures loaded to export.",
            "Export Failed", wxOK | wxICON_WARNING, this);
        return;
    }

    const std::vector<std::string> outPaths = { pathA, pathB };

    for (int i = 0; i < (int)m_fixtures.size() && i < 2; ++i)
    {
        const SceneObject& fix = m_fixtures[i];
        if (outPaths[i].empty()) continue;

        TopoDS_Shape shapeToExport;

        if (fix.hasMould)
        {
            // Use the post-cut shape directly — transform already baked in
            shapeToExport = fix.mouldShape;
        }
        else
        {
            // No mould yet: transform the source shape by the fixture's
            // current pose and export that. Prefer the cached import shape;
            // fall back to re-reading a STEP source for old projects.
            TopoDS_Shape sourceShape;
            if (fix.hasSourceShape)
            {
                sourceShape = fix.sourceShape;
            }
            else
            {
                if (fix.sourcePath.empty()) continue;
                STEPControl_Reader reader;
                if (reader.ReadFile(fix.sourcePath.c_str()) != IFSelect_RetDone)
                {
                    wxMessageBox("Failed to re-read: " + fix.sourcePath,
                        "Export Failed", wxOK | wxICON_ERROR, this);
                    continue;
                }
                reader.TransferRoots();
                sourceShape = reader.OneShape();
            }
            if (sourceShape.IsNull()) continue;

            gp_Trsf trsf;
            glm::mat4 m = fix.BuildModelMatrix();
            trsf.SetValues(
                m[0][0], m[1][0], m[2][0], m[3][0],
                m[0][1], m[1][1], m[2][1], m[3][1],
                m[0][2], m[1][2], m[2][2], m[3][2]
            );
            BRepBuilderAPI_Transform xform(sourceShape, trsf, true);
            shapeToExport = xform.Shape();
        }

        STEPControl_Writer writer;
        writer.Transfer(shapeToExport, STEPControl_AsIs);
        if (writer.Write(outPaths[i].c_str()) != IFSelect_RetDone)
        {
            wxMessageBox("Failed to write: " + outPaths[i],
                "Export Failed", wxOK | wxICON_ERROR, this);
        }
    }

    wxMessageBox("Fixtures exported successfully.",
        "Export Complete", wxOK | wxICON_INFORMATION, this);
}

// ---------------------------------------------------------------------------
// Import — appends a new SceneObject (STEP / STL / OBJ)
// ---------------------------------------------------------------------------
void GLCanvas::ImportFile(const std::string& path)
{
    SetCurrent(*m_context);
    InitGLOnce();

    // Give mesh-format imports a more accurate progress label, since
    // BuildFacetedShape() runs synchronously inside ImportAuto and can be
    // the dominant cost for large meshes.
    auto lower = [](std::string s) {
        for (char& c : s) c = (char)std::tolower((unsigned char)c);
        return s;
        };
    const std::string ext = (path.find_last_of('.') == std::string::npos)
        ? std::string()
        : lower(path.substr(path.find_last_of('.') + 1));
    const bool isMeshFormat = (ext == "stl" || ext == "obj");
    const wxString firstMsg = isMeshFormat
        ? "Parsing mesh and building solid (may take a while)..."
        : "Reading STEP file...";

    wxProgressDialog progress(
        "Importing File",
        firstMsg,
        5,
        nullptr,
        wxPD_APP_MODAL | wxPD_AUTO_HIDE | wxPD_SMOOTH | wxPD_ELAPSED_TIME
    );

    int step = 0;

    progress.Update(step++, firstMsg);
    FileImporter importer;
    auto res = importer.ImportAuto(path, 0.05, 0.5);

    if (!res.ok()) {
        wxMessageBox(res.error, "Import failed", wxOK | wxICON_ERROR, this);
        return;
    }

    progress.Update(step++, "Computing vertex normals...");

    // Snapshot position-only geometry for CPU ray casting BEFORE the crease
    // split duplicates vertices and replaces the index buffer.
    std::vector<float>    cpuVerts = res.meshes[0].vertices;
    std::vector<uint32_t> cpuIndices = res.meshes[0].indices;

    ComputeVertexNormals_Pos3(res.meshes[0].vertices,
        res.meshes[0].indices,
        res.meshes[0].posNorm);

    progress.Update(step++, "Splitting by crease angle...");
    auto split = SplitByCreaseAngle_Pos3(res.meshes[0].vertices,
        res.meshes[0].indices, 35.0f);
    res.meshes[0].posNorm = std::move(split.posNorm);
    res.meshes[0].indices = std::move(split.indices);

    progress.Update(step++, "Uploading to GPU...");
    m_objects.emplace_back();
    m_objects.back().sourcePath = path;
    m_objects.back().cpuVerts = std::move(cpuVerts);
    m_objects.back().cpuIndices = std::move(cpuIndices);
    if (res.hasShape) {
        m_objects.back().sourceShape = res.shape;
        m_objects.back().hasSourceShape = true;
    }
    UploadMeshToGPU(res.meshes[0], m_objects.back());

    progress.Update(step++, "Done.");

    // For mesh imports that couldn't be sewn into a closed solid, warn the
    // user: booleans (mould cut) against an open shell are unreliable.
    if (isMeshFormat && res.hasShape && !res.shapeIsClosedSolid) {
        wxMessageBox(
            "The imported mesh could not be sewn into a closed solid. "
            "It will display correctly, but the mould cut may produce "
            "incorrect results against this object. Consider repairing "
            "the mesh (e.g. with MeshLab or Blender) so it is watertight.",
            "Non-manifold mesh", wxOK | wxICON_WARNING, this);
    }

    Refresh(false);
}

void GLCanvas::ImportFileAsFixture(const std::string& path,
    const HalfTransform& xform)
{
    SetCurrent(*m_context);
    InitGLOnce();

    auto lower = [](std::string s) {
        for (char& c : s) c = (char)std::tolower((unsigned char)c);
        return s;
        };
    const std::string ext = (path.find_last_of('.') == std::string::npos)
        ? std::string()
        : lower(path.substr(path.find_last_of('.') + 1));
    const bool isMeshFormat = (ext == "stl" || ext == "obj");
    const wxString firstMsg = isMeshFormat
        ? "Parsing mesh and building solid (may take a while)..."
        : "Reading STEP file...";

    wxProgressDialog progress(
        "Importing Fixture",
        firstMsg,
        5,
        nullptr,
        wxPD_APP_MODAL | wxPD_AUTO_HIDE | wxPD_SMOOTH | wxPD_ELAPSED_TIME
    );

    int step = 0;

    progress.Update(step++, firstMsg);
    FileImporter importer;
    auto res = importer.ImportAuto(path, 0.05, 0.5);

    if (!res.ok()) {
        wxMessageBox(res.error, "Import failed", wxOK | wxICON_ERROR, this);
        return;
    }

    progress.Update(step++, "Computing vertex normals...");

    // Snapshot geometry before crease split (same pattern as ImportFile)
    std::vector<float>    cpuVerts = res.meshes[0].vertices;
    std::vector<uint32_t> cpuIndices = res.meshes[0].indices;

    ComputeVertexNormals_Pos3(res.meshes[0].vertices,
        res.meshes[0].indices,
        res.meshes[0].posNorm);

    progress.Update(step++, "Splitting by crease angle...");
    auto split = SplitByCreaseAngle_Pos3(res.meshes[0].vertices,
        res.meshes[0].indices, 35.0f);
    res.meshes[0].posNorm = std::move(split.posNorm);
    res.meshes[0].indices = std::move(split.indices);

    progress.Update(step++, "Uploading to GPU...");
    m_fixtures.emplace_back();
    m_fixtures.back().role = ObjectRole::Fixture;
    m_fixtures.back().sourcePath = path;
    m_fixtures.back().cpuVerts = std::move(cpuVerts);
    m_fixtures.back().cpuIndices = std::move(cpuIndices);
    if (res.hasShape) {
        m_fixtures.back().sourceShape = res.shape;
        m_fixtures.back().hasSourceShape = true;
    }
    UploadMeshToGPU(res.meshes[0], m_fixtures.back());

    // Apply the per-half pose authored in the FixtureEditor. Axis mapping
    // mirrors the save side in FixtureEditor::OnGenerateFixture:
    //   rotation_x  →  pitchDeg   (rotation around world X)
    //   rotation_y  →  yawDeg     (rotation around world Y)
    //   rotation_z  →  rollDeg    (rotation around world Z)
    // HalfTransform stores doubles for round-trip fidelity in the file;
    // SceneObject is float — narrowing here is fine for runtime display.
    // Done before BuildFixturePerimeter() below because the perimeter
    // builder calls fix.BuildModelMatrix() on each fixture, so an
    // un-applied transform would produce a parting band against the
    // mesh's local origin instead of its placed pose.
    SceneObject& fixObj = m_fixtures.back();
    fixObj.pos = glm::vec3(
        static_cast<float>(xform.posX),
        static_cast<float>(xform.posY),
        static_cast<float>(xform.posZ));
    fixObj.pitchDeg = static_cast<float>(xform.rotX);
    fixObj.yawDeg = static_cast<float>(xform.rotY);
    fixObj.rollDeg = static_cast<float>(xform.rotZ);
    fixObj.scale = static_cast<float>(xform.scale);

    progress.Update(step++, "Done.");

    if (isMeshFormat && res.hasShape && !res.shapeIsClosedSolid) {
        wxMessageBox(
            "The imported fixture mesh could not be sewn into a closed "
            "solid. The mould cut uses this as the blank — results against "
            "an open shell will be unreliable. Consider repairing the mesh "
            "so it is watertight before using it as a fixture.",
            "Non-manifold mesh", wxOK | wxICON_WARNING, this);
    }

    BuildFixturePerimeter();
    Refresh(false);
}

// ---------------------------------------------------------------------------
// Paint
// ---------------------------------------------------------------------------
void GLCanvas::OnPaint(wxPaintEvent&)
{
    wxPaintDC dc(this);
    SetCurrent(*m_context);
    InitGLOnce();
    m_grid.Init();

    const wxSize sz = GetClientSize();
    const int w = std::max(1, sz.x);
    const int h = std::max(1, sz.y);

    glViewport(0, 0, w, h);
    EnsurePickFBO(w, h);

    glEnable(GL_DEPTH_TEST);
    glClearColor(0.12f, 0.14f, 0.18f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    m_camera.SetAspect(float(w) / float(h));
    const glm::mat4 view = m_camera.View();
    const glm::mat4 proj = m_camera.Projection();
    const glm::vec3 camPos = m_camera.Position();

    if (!m_program) { SwapBuffers(); return; }

    // Preview canvas: grid + loaded mould halves only. Skip the entire
    // editing scene (objects, fixture transparency, features, ghosts, picking
    // and selection outline) — none of it applies here.
    if (m_previewMode)
    {
        RenderPreview(view, proj, camPos);
        SwapBuffers();
        return;
    }

    glUseProgram(m_program);

    // Shared lighting uniforms
    const glm::vec3 lightDir = glm::normalize(glm::vec3(0.4f, 0.8f, 0.2f));
    const glm::vec3 lightColor = glm::vec3(1.0f);
    const glm::vec3 baseColor = glm::vec3(0.80f, 0.80f, 0.85f);

    glUniform3fv(glGetUniformLocation(m_program, "uCameraPos"), 1, &camPos[0]);
    glUniform3fv(glGetUniformLocation(m_program, "uLightDir"), 1, &lightDir[0]);
    glUniform3fv(glGetUniformLocation(m_program, "uLightColor"), 1, &lightColor[0]);
    glUniform3fv(glGetUniformLocation(m_program, "uBaseColor"), 1, &baseColor[0]);
    glUniform1f(glGetUniformLocation(m_program, "uAmbient"), 0.25f);
    glUniform1f(glGetUniformLocation(m_program, "uDiffuse"), 0.85f);
    glUniform1f(glGetUniformLocation(m_program, "uSpecular"), 0.20f);
    glUniform1f(glGetUniformLocation(m_program, "uShininess"), 64.0f);
    glUniformMatrix4fv(glGetUniformLocation(m_program, "uView"), 1, GL_FALSE, &view[0][0]);
    glUniformMatrix4fv(glGetUniformLocation(m_program, "uProj"), 1, GL_FALSE, &proj[0][0]);

    // Draw fixtures
    const bool cameraAboveGrid = m_camera.Position().y > 0.0f;


    // Draw imported objects — restore alpha to 1.0 for all regular objects
    glUniform1f(glGetUniformLocation(m_program, "uAlpha"), 1.0f);

    // Draw each object
    for (int i = 0; i < (int)m_objects.size(); ++i)
    {
        const SceneObject& obj = m_objects[i];
        if (obj.mesh.vao == 0 || obj.mesh.indexCount == 0) continue;

        const glm::mat4 model = obj.BuildModelMatrix();
        glUniformMatrix4fv(glGetUniformLocation(m_program, "uModel"), 1, GL_FALSE, &model[0][0]);

        glBindVertexArray(obj.mesh.vao);
        glDrawElements(GL_TRIANGLES, obj.mesh.indexCount, GL_UNSIGNED_INT, 0);
        glBindVertexArray(0);
    }

    // ---- AlignFace hover highlight -----------------------------------------
    // Deferred ray-cast pass: runs once per rendered frame regardless of how
    // many mouse-motion events have queued up. Detects whether the hover has
    // moved to a new face/object and rebuilds the highlight VBO only on change.
    if (m_transformMode == TransformMode::AlignFace ||
        m_transformMode == TransformMode::AlignMidplane)
    {
        int hoverObj = -1, hoverTri = -1;
        const bool hovered = RayCastFacePick(m_alignMousePos.x,
            m_alignMousePos.y, hoverObj, hoverTri);

        if (!hovered)
        {
            if (m_alignHoverObject != -1 || m_alignSeedTri != -1)
            {
                m_alignHoverObject = -1;
                m_alignSeedTri = -1;
                m_alignFaceTris.clear();
                m_alignHighlightVertexCount = 0;
            }
        }
        else if (hoverObj != m_alignHoverObject || hoverTri != m_alignSeedTri)
        {
            // New face hovered: grow region and rebuild VBO.
            EnsureTriAdjacency(m_objects[hoverObj]);
            GrowCoplanarFace(m_objects[hoverObj], hoverTri,
                m_alignFaceTris, m_alignFaceNormalLocal);
            RebuildAlignHighlightVBO(m_objects[hoverObj], m_alignFaceTris);
            m_alignHoverObject = hoverObj;
            m_alignSeedTri = hoverTri;
        }

        // Two render passes share the same flat-shader setup. The locked
        // face (AlignMidplane only) draws first in selection-outline yellow,
        // then the dark-grey hover highlight on top — so when the cursor is
        // hovering over the locked face the hover colour wins, giving the
        // user clear feedback that re-clicking the same face is possible.
        // Both passes use glPolygonOffset to win the depth fight against
        // the underlying object surface.
        const bool drawLocked =
            (m_transformMode == TransformMode::AlignMidplane) &&
            m_midplaneFaceLocked &&
            m_midplaneLockedVAO != 0 &&
            m_midplaneLockedVertexCount > 0;
        const bool drawHover =
            (m_alignHighlightVAO != 0) &&
            (m_alignHighlightVertexCount > 0);

        if (m_flatProgram && (drawLocked || drawHover))
        {
            glEnable(GL_DEPTH_TEST);
            glDepthFunc(GL_LEQUAL);
            glEnable(GL_POLYGON_OFFSET_FILL);
            glPolygonOffset(-1.0f, -1.0f);

            glUseProgram(m_flatProgram);
            const glm::mat4 VP = proj * view;
            glUniformMatrix4fv(m_flat_uVP, 1, GL_FALSE, &VP[0][0]);

            if (drawLocked)
            {
                // Selection-outline yellow (matches fsOutline shader).
                const glm::vec4 lockedColor(1.0f, 0.75f, 0.2f, 1.0f);
                glUniform4fv(m_flat_uColor, 1, &lockedColor[0]);
                glBindVertexArray(m_midplaneLockedVAO);
                glDrawArrays(GL_TRIANGLES, 0, m_midplaneLockedVertexCount);
                glBindVertexArray(0);
            }

            if (drawHover)
            {
                const glm::vec4 hoverColor(0.18f, 0.18f, 0.18f, 1.0f);
                glUniform4fv(m_flat_uColor, 1, &hoverColor[0]);
                glBindVertexArray(m_alignHighlightVAO);
                glDrawArrays(GL_TRIANGLES, 0, m_alignHighlightVertexCount);
                glBindVertexArray(0);
            }

            glDisable(GL_POLYGON_OFFSET_FILL);
            glDepthFunc(GL_LESS);

            // Restore the lit shader for any subsequent passes that expect it.
            glUseProgram(m_program);
        }
    }

    // Determine which fixture is transparent this frame
// Above grid: A (index 0) is transparent. Below grid: B (index 1) is transparent.
    const int transparentIndex = cameraAboveGrid ? 0 : 1;
    const int opaqueIndex = cameraAboveGrid ? 1 : 0;

    // Draw opaque fixture first
    if (opaqueIndex < (int)m_fixtures.size())
    {
        const SceneObject& obj = m_fixtures[opaqueIndex];
        if (obj.mesh.vao && obj.mesh.indexCount > 0)
        {
            const glm::mat4 model = obj.BuildModelMatrix();
            glUniformMatrix4fv(glGetUniformLocation(m_program, "uModel"), 1, GL_FALSE, &model[0][0]);
            glUniform1f(glGetUniformLocation(m_program, "uAlpha"), 1.0f);
            glBindVertexArray(obj.mesh.vao);
            glDrawElements(GL_TRIANGLES, obj.mesh.indexCount, GL_UNSIGNED_INT, 0);
            glBindVertexArray(0);
        }
    }

    // Draw transparent fixture second so it blends over everything behind it
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDepthMask(GL_FALSE);   // don't write depth — keeps vent markers visible through the tint

    // Nudge the transparent half's fragments back in depth-buffer space so
    // its parting face (coplanar with the opaque half's parting face at y=0)
    // loses the depth test cleanly instead of z-fighting against it. Without
    // this, fragments on both faces share the same post-projection depth and
    // floating-point rounding flips individual pixels frame-to-frame, which
    // shows up as flicker along the parting line.
    glEnable(GL_POLYGON_OFFSET_FILL);
    glPolygonOffset(1.0f, 1.0f);

    if (transparentIndex < (int)m_fixtures.size())
    {
        const SceneObject& obj = m_fixtures[transparentIndex];
        if (obj.mesh.vao && obj.mesh.indexCount > 0)
        {
            const glm::mat4 model = obj.BuildModelMatrix();
            glUniformMatrix4fv(glGetUniformLocation(m_program, "uModel"), 1, GL_FALSE, &model[0][0]);
            glUniform1f(glGetUniformLocation(m_program, "uAlpha"), 0.1f);
            glBindVertexArray(obj.mesh.vao);
            glDrawElements(GL_TRIANGLES, obj.mesh.indexCount, GL_UNSIGNED_INT, 0);
            glBindVertexArray(0);
        }
    }

    glPolygonOffset(0.0f, 0.0f);
    glDisable(GL_POLYGON_OFFSET_FILL);

    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);

    // Restore alpha for regular objects
    glUniform1f(glGetUniformLocation(m_program, "uAlpha"), 1.0f);


    glUseProgram(0);

    m_grid.Draw(view, proj);

    // Outline for selected object
    if (!m_selectedIndices.empty())
    {
        RenderPickPass_NoRead(w, h);

        if (m_outlineProgram && m_pickColorTex)
        {
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            glViewport(0, 0, w, h);
            glDisable(GL_DEPTH_TEST);
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

            glUseProgram(m_outlineProgram);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, m_pickColorTex);

            // Per-pass-invariant uniforms set once.
            glUniform1i(m_outline_uIdTex, 0);
            glUniform2i(m_outline_uTexSize, m_pickW, m_pickH);
            glUniform1f(m_outline_uAlpha, 0.9f);
            glUniform1i(m_outline_uThickness, 2);

            glBindVertexArray(m_fullscreenVAO);

            // The outline shader takes a single uTargetId and detects edges
            // between that ID and everything else in the pick texture, so we
            // do one fullscreen pass per selected object. The pick texture
            // is shared across passes — only uTargetId varies.
            for (int idx : m_selectedIndices)
            {
                if (idx < 0 || idx >= (int)m_objects.size()) continue;
                const uint32_t targetId = (uint32_t)(idx + 1);
                glUniform1ui(m_outline_uTargetId, targetId);
                glDrawArrays(GL_TRIANGLES, 0, 3);
            }

            glBindVertexArray(0);
            glBindTexture(GL_TEXTURE_2D, 0);
            glUseProgram(0);
            glDisable(GL_BLEND);
            glEnable(GL_DEPTH_TEST);
        }
    }

    // ---- Deferred edit-drag processing ----------------------------------------
    // Ray cast + geometry rebuild deferred from OnMouse so only one update
    // runs per rendered frame, regardless of how many motion events queued up.
    if (m_editNeedsUpdate)
    {
        m_editNeedsUpdate = false;

        if (m_transformMode == TransformMode::EditVent &&
            m_editFeatureIndex >= 0 && m_editFeatureIndex < (int)m_vents.size())
        {
            glm::vec3 hitPos, hitNormal;
            int       hitObj = -1;
            if (RayCastParting(m_editMousePos.x, m_editMousePos.y, hitPos, hitNormal, &hitObj))
            {
                VentInstance& vi = m_vents[m_editFeatureIndex];
                vi.Destroy();
                vi.point = VentPoint{ hitPos, hitNormal };

                // Edit-drag re-captures parent association: if the user drags
                // a vent onto a different object's surface, the new object
                // becomes its parent. Snapping to no object resets to
                // unparented (vent stops following any object).
                if (hitObj >= 0 && hitObj < (int)m_objects.size())
                {
                    const glm::mat4 m = m_objects[hitObj].BuildModelMatrix();
                    const glm::mat4 invM = glm::inverse(m);
                    vi.parentIndex = hitObj;
                    vi.localPos = glm::vec3(invM * glm::vec4(hitPos, 1.0f));
                    glm::vec3 ln = glm::transpose(glm::mat3(m)) * hitNormal;
                    const float lnLen = glm::length(ln);
                    vi.localNormal = (lnLen > 1e-6f) ? ln / lnLen : glm::vec3(0, 0, 1);
                }
                else
                {
                    vi.parentIndex = -1;
                }

                float ventLength = 5.0f, ventWidth = 2.0f,
                    ventOverrunStart = 0.5f, ventOverrunEnd = 0.5f;
                if (auto* frame = dynamic_cast<MainFrame*>(wxGetTopLevelParent(this)))
                    frame->GetVentDimensions(ventLength, ventWidth,
                        ventOverrunStart, ventOverrunEnd);

                vi.path = ComputeVentPath(vi.point);
                vi.path.overrunStart = ventOverrunStart;
                vi.path.overrunEnd = ventOverrunEnd;
                vi.crossSection = BuildVentCrossSection(vi.path, ventWidth, ventLength);
                vi.solid = BuildBoxSweepMesh(vi.path, ventWidth, ventLength,
                    ventOverrunStart, ventOverrunEnd);

                RebuildPathVBO();
                RebuildCrossSectionVBO();
            }
        }
        else if (m_transformMode == TransformMode::EditRunner &&
            m_editFeatureIndex >= 0 && m_editFeatureIndex < (int)m_runners.size())
        {
            glm::vec3 hitPos;
            if (RayCastToPartingPlane(m_editMousePos.x, m_editMousePos.y, hitPos))
            {
                const glm::vec2 hitXZ(hitPos.x, hitPos.z);
                if (IsInsideConvexHull(m_fixturePerimeter, hitXZ))
                {
                    RunnerFeature& rf = m_runners[m_editFeatureIndex];
                    rf.Destroy();
                    rf.point = hitPos;

                    RebuildRunnerPathVBO();
                    RebuildRunnerSolids();
                    RebuildGatePathVBO();
                    RebuildGateSolids();
                }
            }
        }
        else if (m_transformMode == TransformMode::EditGate &&
            m_editFeatureIndex >= 0 && m_editFeatureIndex < (int)m_gates.size())
        {
            glm::vec3 hitPos, hitNormal;
            int       hitObj = -1;
            if (RayCastParting(m_editMousePos.x, m_editMousePos.y, hitPos, hitNormal, &hitObj))
            {
                GateFeature& gf = m_gates[m_editFeatureIndex];
                gf.Destroy();
                gf.point = VentPoint{ hitPos, hitNormal };

                // Edit-drag re-captures parent association — see the EditVent
                // branch above for the rationale.
                if (hitObj >= 0 && hitObj < (int)m_objects.size())
                {
                    const glm::mat4 m = m_objects[hitObj].BuildModelMatrix();
                    const glm::mat4 invM = glm::inverse(m);
                    gf.parentIndex = hitObj;
                    gf.localPos = glm::vec3(invM * glm::vec4(hitPos, 1.0f));
                    glm::vec3 ln = glm::transpose(glm::mat3(m)) * hitNormal;
                    const float lnLen = glm::length(ln);
                    gf.localNormal = (lnLen > 1e-6f) ? ln / lnLen : glm::vec3(0, 0, 1);
                }
                else
                {
                    gf.parentIndex = -1;
                }

                RebuildGatePathVBO();
                RebuildGateSolids();
            }
        }
        else if (m_transformMode == TransformMode::EditEjector &&
            m_editFeatureIndex >= 0 && m_editFeatureIndex < (int)m_ejectors.size())
        {
            // Re-snap the dragged ejector with the same picker used at
            // initial placement (RayCastEjectorSnap), so dragging onto a
            // runner / gate / sprue parting / object face all behave
            // exactly like placing a new ejector. RebuildEjectorSolids
            // reconstructs the cylinder from the new point + current UI
            // dimensions, mirroring the EditGate convention.
            glm::vec3 hitPos;
            if (RayCastEjectorSnap(m_editMousePos.x, m_editMousePos.y, hitPos))
            {
                m_ejectors[m_editFeatureIndex].point = hitPos;
                RebuildEjectorSolids();
            }
        }
    }

    // ---- Vent point markers (green spheres) --------------------------------
    // Resolve ghost position here — one ray cast per rendered frame regardless
    // of how many motion events queued up since the last paint.
    if (m_transformMode == TransformMode::PlaceVent)
    {
        glm::vec3 hitPos, hitNormal;
        m_ventGhostActive = RayCastParting(m_ghostMousePos.x, m_ghostMousePos.y, hitPos, hitNormal);
        m_ventGhost.worldPos = hitPos;
        m_ventGhost.worldNormal = hitNormal;
    }
    if (m_program && m_sphereVAO && m_sphereIndexCount > 0 &&
        (!m_vents.empty() || m_ventGhostActive))
    {
        glEnable(GL_DEPTH_TEST);
        glUseProgram(m_program);

        glUniform3fv(glGetUniformLocation(m_program, "uCameraPos"), 1, &camPos[0]);
        glUniform3fv(glGetUniformLocation(m_program, "uLightDir"), 1, &lightDir[0]);
        glUniform3fv(glGetUniformLocation(m_program, "uLightColor"), 1, &lightColor[0]);
        glUniform1f(glGetUniformLocation(m_program, "uAmbient"), 0.35f);
        glUniform1f(glGetUniformLocation(m_program, "uDiffuse"), 0.75f);
        glUniform1f(glGetUniformLocation(m_program, "uSpecular"), 0.60f);
        glUniform1f(glGetUniformLocation(m_program, "uShininess"), 48.0f);
        glUniformMatrix4fv(glGetUniformLocation(m_program, "uView"), 1, GL_FALSE, &view[0][0]);
        glUniformMatrix4fv(glGetUniformLocation(m_program, "uProj"), 1, GL_FALSE, &proj[0][0]);

        glBindVertexArray(m_sphereVAO);

        // Ghost preview — translucent, slightly larger, pulsing not needed but distinct
        if (m_ventGhostActive)
        {
            const glm::vec3 ghostColor(0.10f, 0.92f, 0.25f);
            glUniform3fv(glGetUniformLocation(m_program, "uBaseColor"), 1, &ghostColor[0]);
            glUniform1f(glGetUniformLocation(m_program, "uAlpha"), 0.45f);
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            glDepthMask(GL_FALSE);

            glm::mat4 model = glm::translate(glm::mat4(1.0f), m_ventGhost.worldPos);
            model = glm::scale(model, glm::vec3(kVentMarkerRadius * 1.15f));
            glUniformMatrix4fv(glGetUniformLocation(m_program, "uModel"), 1, GL_FALSE, &model[0][0]);
            glDrawElements(GL_TRIANGLES, m_sphereIndexCount, GL_UNSIGNED_INT, 0);

            glDepthMask(GL_TRUE);
            glDisable(GL_BLEND);
        }

        // Confirmed vent points — highlight halo behind, then normal markers
        if (!m_vents.empty())
        {
            // Pass 1: draw highlight halo for the selected marker (behind)
            if (m_transformMode == TransformMode::EditVent &&
                m_editFeatureIndex >= 0 && m_editFeatureIndex < (int)m_vents.size())
            {
                const glm::vec3 haloColor(1.0f, 0.55f, 0.0f);   // orange
                glUniform3fv(glGetUniformLocation(m_program, "uBaseColor"), 1, &haloColor[0]);
                glUniform1f(glGetUniformLocation(m_program, "uAlpha"), 0.55f);
                glEnable(GL_BLEND);
                glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
                glDepthMask(GL_FALSE);

                glm::mat4 model = glm::translate(glm::mat4(1.0f),
                    m_vents[m_editFeatureIndex].point.worldPos);
                model = glm::scale(model, glm::vec3(kVentMarkerRadius * 1.8f));
                glUniformMatrix4fv(glGetUniformLocation(m_program, "uModel"), 1, GL_FALSE, &model[0][0]);
                glDrawElements(GL_TRIANGLES, m_sphereIndexCount, GL_UNSIGNED_INT, 0);

                glDepthMask(GL_TRUE);
                glDisable(GL_BLEND);
            }

            // Pass 2: normal markers on top
            const glm::vec3 ventColor(0.10f, 0.92f, 0.25f);
            glUniform3fv(glGetUniformLocation(m_program, "uBaseColor"), 1, &ventColor[0]);
            glUniform1f(glGetUniformLocation(m_program, "uAlpha"), 1.0f);

            for (int i = 0; i < (int)m_vents.size(); ++i)
            {
                glm::mat4 model = glm::translate(glm::mat4(1.0f), m_vents[i].point.worldPos);
                model = glm::scale(model, glm::vec3(kVentMarkerRadius));
                glUniformMatrix4fv(glGetUniformLocation(m_program, "uModel"), 1, GL_FALSE, &model[0][0]);
                glDrawElements(GL_TRIANGLES, m_sphereIndexCount, GL_UNSIGNED_INT, 0);
            }
        }

        glBindVertexArray(0);
        glUseProgram(0);
    }

    // ---- Runner point markers (blue spheres) --------------------------------
    if (m_transformMode == TransformMode::PlaceRunner)
    {
        glm::vec3 hitPos;
        const bool hit = RayCastToPartingPlane(
            m_runnerGhostMousePos.x, m_runnerGhostMousePos.y, hitPos);
        const glm::vec2 hitXZ(hitPos.x, hitPos.z);
        m_runnerGhostActive = hit && IsInsideConvexHull(m_fixturePerimeter, hitXZ);
        m_runnerGhostPos = hitPos;
    }
    if (m_program && m_sphereVAO && m_sphereIndexCount > 0 &&
        (!m_runners.empty() || m_runnerGhostActive))
    {
        glEnable(GL_DEPTH_TEST);
        glUseProgram(m_program);

        glUniform3fv(glGetUniformLocation(m_program, "uCameraPos"), 1, &camPos[0]);
        glUniform3fv(glGetUniformLocation(m_program, "uLightDir"), 1, &lightDir[0]);
        glUniform3fv(glGetUniformLocation(m_program, "uLightColor"), 1, &lightColor[0]);
        glUniform1f(glGetUniformLocation(m_program, "uAmbient"), 0.35f);
        glUniform1f(glGetUniformLocation(m_program, "uDiffuse"), 0.75f);
        glUniform1f(glGetUniformLocation(m_program, "uSpecular"), 0.60f);
        glUniform1f(glGetUniformLocation(m_program, "uShininess"), 48.0f);
        glUniformMatrix4fv(glGetUniformLocation(m_program, "uView"), 1, GL_FALSE, &view[0][0]);
        glUniformMatrix4fv(glGetUniformLocation(m_program, "uProj"), 1, GL_FALSE, &proj[0][0]);

        glBindVertexArray(m_sphereVAO);

        // Ghost preview — translucent
        if (m_runnerGhostActive)
        {
            const glm::vec3 ghostColor(0.10f, 0.40f, 0.95f);   // blue
            glUniform3fv(glGetUniformLocation(m_program, "uBaseColor"), 1, &ghostColor[0]);
            glUniform1f(glGetUniformLocation(m_program, "uAlpha"), 0.45f);
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            glDepthMask(GL_FALSE);

            glm::mat4 model = glm::translate(glm::mat4(1.0f), m_runnerGhostPos);
            model = glm::scale(model, glm::vec3(kVentMarkerRadius * 1.15f));
            glUniformMatrix4fv(glGetUniformLocation(m_program, "uModel"), 1, GL_FALSE, &model[0][0]);
            glDrawElements(GL_TRIANGLES, m_sphereIndexCount, GL_UNSIGNED_INT, 0);

            glDepthMask(GL_TRUE);
            glDisable(GL_BLEND);
        }

        // Confirmed runner points — highlight halo behind, then normal markers
        if (!m_runners.empty())
        {
            // Pass 1: draw highlight halo for the selected marker (behind)
            if (m_transformMode == TransformMode::EditRunner &&
                m_editFeatureIndex >= 0 && m_editFeatureIndex < (int)m_runners.size())
            {
                const glm::vec3 haloColor(1.0f, 0.55f, 0.0f);
                glUniform3fv(glGetUniformLocation(m_program, "uBaseColor"), 1, &haloColor[0]);
                glUniform1f(glGetUniformLocation(m_program, "uAlpha"), 0.55f);
                glEnable(GL_BLEND);
                glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
                glDepthMask(GL_FALSE);

                glm::mat4 model = glm::translate(glm::mat4(1.0f),
                    m_runners[m_editFeatureIndex].point);
                model = glm::scale(model, glm::vec3(kVentMarkerRadius * 1.8f));
                glUniformMatrix4fv(glGetUniformLocation(m_program, "uModel"), 1, GL_FALSE, &model[0][0]);
                glDrawElements(GL_TRIANGLES, m_sphereIndexCount, GL_UNSIGNED_INT, 0);

                glDepthMask(GL_TRUE);
                glDisable(GL_BLEND);
            }

            // Pass 2: normal markers on top
            const glm::vec3 runnerColor(0.10f, 0.40f, 0.95f);
            glUniform3fv(glGetUniformLocation(m_program, "uBaseColor"), 1, &runnerColor[0]);
            glUniform1f(glGetUniformLocation(m_program, "uAlpha"), 1.0f);

            for (int i = 0; i < (int)m_runners.size(); ++i)
            {
                glm::mat4 model = glm::translate(glm::mat4(1.0f), m_runners[i].point);
                model = glm::scale(model, glm::vec3(kVentMarkerRadius));
                glUniformMatrix4fv(glGetUniformLocation(m_program, "uModel"), 1, GL_FALSE, &model[0][0]);
                glDrawElements(GL_TRIANGLES, m_sphereIndexCount, GL_UNSIGNED_INT, 0);
            }
        }

        glBindVertexArray(0);
        glUseProgram(0);
    }

    // ---- Gate point markers (yellow spheres) --------------------------------
    if (m_transformMode == TransformMode::PlaceGate)
    {
        glm::vec3 hitPos, hitNormal;
        m_gateGhostActive = RayCastParting(m_gateGhostMousePos.x, m_gateGhostMousePos.y,
            hitPos, hitNormal);
        m_gateGhost.worldPos = hitPos;
        m_gateGhost.worldNormal = hitNormal;
    }
    if (m_program && m_sphereVAO && m_sphereIndexCount > 0 &&
        (!m_gates.empty() || m_gateGhostActive))
    {
        glEnable(GL_DEPTH_TEST);
        glUseProgram(m_program);

        glUniform3fv(glGetUniformLocation(m_program, "uCameraPos"), 1, &camPos[0]);
        glUniform3fv(glGetUniformLocation(m_program, "uLightDir"), 1, &lightDir[0]);
        glUniform3fv(glGetUniformLocation(m_program, "uLightColor"), 1, &lightColor[0]);
        glUniform1f(glGetUniformLocation(m_program, "uAmbient"), 0.35f);
        glUniform1f(glGetUniformLocation(m_program, "uDiffuse"), 0.75f);
        glUniform1f(glGetUniformLocation(m_program, "uSpecular"), 0.60f);
        glUniform1f(glGetUniformLocation(m_program, "uShininess"), 48.0f);
        glUniformMatrix4fv(glGetUniformLocation(m_program, "uView"), 1, GL_FALSE, &view[0][0]);
        glUniformMatrix4fv(glGetUniformLocation(m_program, "uProj"), 1, GL_FALSE, &proj[0][0]);

        glBindVertexArray(m_sphereVAO);

        // Ghost preview — translucent yellow
        if (m_gateGhostActive)
        {
            const glm::vec3 ghostColor(1.0f, 0.85f, 0.10f);   // yellow
            glUniform3fv(glGetUniformLocation(m_program, "uBaseColor"), 1, &ghostColor[0]);
            glUniform1f(glGetUniformLocation(m_program, "uAlpha"), 0.45f);
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            glDepthMask(GL_FALSE);

            glm::mat4 model = glm::translate(glm::mat4(1.0f), m_gateGhost.worldPos);
            model = glm::scale(model, glm::vec3(kVentMarkerRadius * 1.15f));
            glUniformMatrix4fv(glGetUniformLocation(m_program, "uModel"), 1, GL_FALSE, &model[0][0]);
            glDrawElements(GL_TRIANGLES, m_sphereIndexCount, GL_UNSIGNED_INT, 0);

            glDepthMask(GL_TRUE);
            glDisable(GL_BLEND);
        }

        // Confirmed gate points — highlight halo behind, then normal markers
        if (!m_gates.empty())
        {
            // Pass 1: draw highlight halo for the selected marker (behind)
            if (m_transformMode == TransformMode::EditGate &&
                m_editFeatureIndex >= 0 && m_editFeatureIndex < (int)m_gates.size())
            {
                const glm::vec3 haloColor(1.0f, 0.55f, 0.0f);
                glUniform3fv(glGetUniformLocation(m_program, "uBaseColor"), 1, &haloColor[0]);
                glUniform1f(glGetUniformLocation(m_program, "uAlpha"), 0.55f);
                glEnable(GL_BLEND);
                glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
                glDepthMask(GL_FALSE);

                glm::mat4 model = glm::translate(glm::mat4(1.0f),
                    m_gates[m_editFeatureIndex].point.worldPos);
                model = glm::scale(model, glm::vec3(kVentMarkerRadius * 1.8f));
                glUniformMatrix4fv(glGetUniformLocation(m_program, "uModel"), 1, GL_FALSE, &model[0][0]);
                glDrawElements(GL_TRIANGLES, m_sphereIndexCount, GL_UNSIGNED_INT, 0);

                glDepthMask(GL_TRUE);
                glDisable(GL_BLEND);
            }

            // Pass 2: normal markers on top
            const glm::vec3 gateColor(1.0f, 0.85f, 0.10f);
            glUniform3fv(glGetUniformLocation(m_program, "uBaseColor"), 1, &gateColor[0]);
            glUniform1f(glGetUniformLocation(m_program, "uAlpha"), 1.0f);

            for (int i = 0; i < (int)m_gates.size(); ++i)
            {
                glm::mat4 model = glm::translate(glm::mat4(1.0f), m_gates[i].point.worldPos);
                model = glm::scale(model, glm::vec3(kVentMarkerRadius));
                glUniformMatrix4fv(glGetUniformLocation(m_program, "uModel"), 1, GL_FALSE, &model[0][0]);
                glDrawElements(GL_TRIANGLES, m_sphereIndexCount, GL_UNSIGNED_INT, 0);
            }
        }

        glBindVertexArray(0);
        glUseProgram(0);
    }

    // ---- Ejector point markers (cyan spheres) ------------------------------
    // Mirrors the vent / gate marker pattern, but with an extended snap pick
    // (RayCastEjectorSnap) instead of the parting-line snap. The ghost
    // tracks whichever candidate currently wins (sprue parting pt / runner
    // segment / gate segment / object face).
    if (m_transformMode == TransformMode::PlaceEjector)
    {
        glm::vec3 hitPos;
        m_ejectorGhostActive = RayCastEjectorSnap(m_ejectorGhostMousePos.x,
            m_ejectorGhostMousePos.y, hitPos);
        m_ejectorGhostPos = hitPos;
    }
    if (m_program && m_sphereVAO && m_sphereIndexCount > 0 &&
        (!m_ejectors.empty() || m_ejectorGhostActive))
    {
        glEnable(GL_DEPTH_TEST);
        glUseProgram(m_program);

        glUniform3fv(glGetUniformLocation(m_program, "uCameraPos"), 1, &camPos[0]);
        glUniform3fv(glGetUniformLocation(m_program, "uLightDir"), 1, &lightDir[0]);
        glUniform3fv(glGetUniformLocation(m_program, "uLightColor"), 1, &lightColor[0]);
        glUniform1f(glGetUniformLocation(m_program, "uAmbient"), 0.35f);
        glUniform1f(glGetUniformLocation(m_program, "uDiffuse"), 0.75f);
        glUniform1f(glGetUniformLocation(m_program, "uSpecular"), 0.60f);
        glUniform1f(glGetUniformLocation(m_program, "uShininess"), 48.0f);
        glUniformMatrix4fv(glGetUniformLocation(m_program, "uView"), 1, GL_FALSE, &view[0][0]);
        glUniformMatrix4fv(glGetUniformLocation(m_program, "uProj"), 1, GL_FALSE, &proj[0][0]);

        glBindVertexArray(m_sphereVAO);

        // Ghost preview — translucent cyan, slightly oversized to read against
        // a runner / gate line that already lives at the same world location.
        if (m_ejectorGhostActive)
        {
            const glm::vec3 ghostColor(0.20f, 0.85f, 0.95f);   // cyan
            glUniform3fv(glGetUniformLocation(m_program, "uBaseColor"), 1, &ghostColor[0]);
            glUniform1f(glGetUniformLocation(m_program, "uAlpha"), 0.45f);
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            glDepthMask(GL_FALSE);

            glm::mat4 model = glm::translate(glm::mat4(1.0f), m_ejectorGhostPos);
            model = glm::scale(model, glm::vec3(kVentMarkerRadius * 1.15f));
            glUniformMatrix4fv(glGetUniformLocation(m_program, "uModel"), 1, GL_FALSE, &model[0][0]);
            glDrawElements(GL_TRIANGLES, m_sphereIndexCount, GL_UNSIGNED_INT, 0);

            glDepthMask(GL_TRUE);
            glDisable(GL_BLEND);
        }

        // Confirmed ejector points — halo pass (edit mode) then markers.
        if (!m_ejectors.empty())
        {
            // Pass 1: highlight halo for the selected marker (drawn behind
            // via the depth-mask-off blend, same convention as gates).
            // Orange against cyan markers — matches the gate halo so the
            // edit-mode visual idiom is consistent across feature types.
            if (m_transformMode == TransformMode::EditEjector &&
                m_editFeatureIndex >= 0 && m_editFeatureIndex < (int)m_ejectors.size())
            {
                const glm::vec3 haloColor(1.0f, 0.55f, 0.0f);
                glUniform3fv(glGetUniformLocation(m_program, "uBaseColor"), 1, &haloColor[0]);
                glUniform1f(glGetUniformLocation(m_program, "uAlpha"), 0.55f);
                glEnable(GL_BLEND);
                glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
                glDepthMask(GL_FALSE);

                glm::mat4 model = glm::translate(glm::mat4(1.0f),
                    m_ejectors[m_editFeatureIndex].point);
                model = glm::scale(model, glm::vec3(kVentMarkerRadius * 1.8f));
                glUniformMatrix4fv(glGetUniformLocation(m_program, "uModel"), 1, GL_FALSE, &model[0][0]);
                glDrawElements(GL_TRIANGLES, m_sphereIndexCount, GL_UNSIGNED_INT, 0);

                glDepthMask(GL_TRUE);
                glDisable(GL_BLEND);
            }

            // Pass 2: normal markers.
            const glm::vec3 ejectorColor(0.20f, 0.85f, 0.95f);
            glUniform3fv(glGetUniformLocation(m_program, "uBaseColor"), 1, &ejectorColor[0]);
            glUniform1f(glGetUniformLocation(m_program, "uAlpha"), 1.0f);

            for (const EjectorFeature& ef : m_ejectors)
            {
                glm::mat4 model = glm::translate(glm::mat4(1.0f), ef.point);
                model = glm::scale(model, glm::vec3(kVentMarkerRadius));
                glUniformMatrix4fv(glGetUniformLocation(m_program, "uModel"), 1, GL_FALSE, &model[0][0]);
                glDrawElements(GL_TRIANGLES, m_sphereIndexCount, GL_UNSIGNED_INT, 0);
            }
        }

        glBindVertexArray(0);
        glUseProgram(0);
    }

    // ---- Sprue sphere (purple) ---------------------------------------------
    if (m_program && m_sphereVAO && m_sphereIndexCount > 0 && m_sprue.hasPoint)
    {
        glEnable(GL_DEPTH_TEST);
        glUseProgram(m_program);

        glUniform3fv(glGetUniformLocation(m_program, "uCameraPos"), 1, &camPos[0]);
        glUniform3fv(glGetUniformLocation(m_program, "uLightDir"), 1, &lightDir[0]);
        glUniform3fv(glGetUniformLocation(m_program, "uLightColor"), 1, &lightColor[0]);
        glUniform1f(glGetUniformLocation(m_program, "uAmbient"), 0.35f);
        glUniform1f(glGetUniformLocation(m_program, "uDiffuse"), 0.75f);
        glUniform1f(glGetUniformLocation(m_program, "uSpecular"), 0.60f);
        glUniform1f(glGetUniformLocation(m_program, "uShininess"), 48.0f);
        glUniformMatrix4fv(glGetUniformLocation(m_program, "uView"), 1, GL_FALSE, &view[0][0]);
        glUniformMatrix4fv(glGetUniformLocation(m_program, "uProj"), 1, GL_FALSE, &proj[0][0]);

        const glm::vec3 sprueColor(0.65f, 0.10f, 0.90f);   // purple
        glUniform3fv(glGetUniformLocation(m_program, "uBaseColor"), 1, &sprueColor[0]);
        glUniform1f(glGetUniformLocation(m_program, "uAlpha"), 1.0f);

        glBindVertexArray(m_sphereVAO);
        glm::mat4 model = glm::translate(glm::mat4(1.0f), m_sprue.worldPos);
        model = glm::scale(model, glm::vec3(kVentMarkerRadius * 1.5f));  // slightly larger than vent spheres
        glUniformMatrix4fv(glGetUniformLocation(m_program, "uModel"), 1, GL_FALSE, &model[0][0]);
        glDrawElements(GL_TRIANGLES, m_sphereIndexCount, GL_UNSIGNED_INT, 0);
        glBindVertexArray(0);

        glUseProgram(0);
    }

    // ---- Sprue parting-plane intersection sphere (blue) ---------------------
    if (m_program && m_sphereVAO && m_sphereIndexCount > 0 && m_sprue.hasPartingPoint)
    {
        glEnable(GL_DEPTH_TEST);
        glUseProgram(m_program);

        glUniform3fv(glGetUniformLocation(m_program, "uCameraPos"), 1, &camPos[0]);
        glUniform3fv(glGetUniformLocation(m_program, "uLightDir"), 1, &lightDir[0]);
        glUniform3fv(glGetUniformLocation(m_program, "uLightColor"), 1, &lightColor[0]);
        glUniform1f(glGetUniformLocation(m_program, "uAmbient"), 0.35f);
        glUniform1f(glGetUniformLocation(m_program, "uDiffuse"), 0.75f);
        glUniform1f(glGetUniformLocation(m_program, "uSpecular"), 0.60f);
        glUniform1f(glGetUniformLocation(m_program, "uShininess"), 48.0f);
        glUniformMatrix4fv(glGetUniformLocation(m_program, "uView"), 1, GL_FALSE, &view[0][0]);
        glUniformMatrix4fv(glGetUniformLocation(m_program, "uProj"), 1, GL_FALSE, &proj[0][0]);

        const glm::vec3 partingColor(0.10f, 0.40f, 0.95f);   // blue
        glUniform3fv(glGetUniformLocation(m_program, "uBaseColor"), 1, &partingColor[0]);
        glUniform1f(glGetUniformLocation(m_program, "uAlpha"), 1.0f);

        glBindVertexArray(m_sphereVAO);
        glm::mat4 model = glm::translate(glm::mat4(1.0f), m_sprue.partingPos);
        model = glm::scale(model, glm::vec3(kVentMarkerRadius * 1.5f));
        glUniformMatrix4fv(glGetUniformLocation(m_program, "uModel"), 1, GL_FALSE, &model[0][0]);
        glDrawElements(GL_TRIANGLES, m_sphereIndexCount, GL_UNSIGNED_INT, 0);
        glBindVertexArray(0);

        glUseProgram(0);
    }

    // ---- Injection point selection markers (purple spheres) -----------------
    // Shown only in SelectInjectionPoint mode so the user can pick one.
    if (m_program && m_sphereVAO && m_sphereIndexCount > 0 &&
        m_transformMode == TransformMode::SelectInjectionPoint &&
        !m_injectionPoints.empty())
    {
        glEnable(GL_DEPTH_TEST);
        glUseProgram(m_program);

        glUniform3fv(glGetUniformLocation(m_program, "uCameraPos"), 1, &camPos[0]);
        glUniform3fv(glGetUniformLocation(m_program, "uLightDir"), 1, &lightDir[0]);
        glUniform3fv(glGetUniformLocation(m_program, "uLightColor"), 1, &lightColor[0]);
        glUniform1f(glGetUniformLocation(m_program, "uAmbient"), 0.35f);
        glUniform1f(glGetUniformLocation(m_program, "uDiffuse"), 0.75f);
        glUniform1f(glGetUniformLocation(m_program, "uSpecular"), 0.60f);
        glUniform1f(glGetUniformLocation(m_program, "uShininess"), 48.0f);
        glUniformMatrix4fv(glGetUniformLocation(m_program, "uView"), 1, GL_FALSE, &view[0][0]);
        glUniformMatrix4fv(glGetUniformLocation(m_program, "uProj"), 1, GL_FALSE, &proj[0][0]);

        const glm::vec3 ipColor(0.65f, 0.10f, 0.90f);   // purple
        glUniform3fv(glGetUniformLocation(m_program, "uBaseColor"), 1, &ipColor[0]);
        glUniform1f(glGetUniformLocation(m_program, "uAlpha"), 1.0f);

        glm::mat4 fixtureMatrix(1.0f);
        if (!m_fixtures.empty())
            fixtureMatrix = m_fixtures[0].BuildModelMatrix();

        glBindVertexArray(m_sphereVAO);
        for (const auto& ip : m_injectionPoints)
        {
            glm::vec3 worldPos = glm::vec3(fixtureMatrix *
                glm::vec4(ip.x, ip.y, ip.z, 1.0f));
            glm::mat4 model = glm::translate(glm::mat4(1.0f), worldPos);
            model = glm::scale(model, glm::vec3(kVentMarkerRadius * 0.9f));
            glUniformMatrix4fv(glGetUniformLocation(m_program, "uModel"), 1, GL_FALSE, &model[0][0]);
            glDrawElements(GL_TRIANGLES, m_sphereIndexCount, GL_UNSIGNED_INT, 0);
        }
        glBindVertexArray(0);

        glUseProgram(0);
    }

    // ---- Sprue path line (purple) ------------------------------------------
    if (m_flatProgram && m_sprue.pathVAO && m_sprue.pathVertexCount > 0)
    {
        glEnable(GL_DEPTH_TEST);
        glLineWidth(2.5f);
        glUseProgram(m_flatProgram);

        const glm::mat4 VP = proj * view;
        glUniformMatrix4fv(m_flat_uVP, 1, GL_FALSE, &VP[0][0]);

        const glm::vec4 sprueLineColor(0.65f, 0.10f, 0.90f, 1.0f);   // purple
        glUniform4fv(m_flat_uColor, 1, &sprueLineColor[0]);

        glBindVertexArray(m_sprue.pathVAO);
        glDrawArrays(GL_LINES, 0, m_sprue.pathVertexCount);
        glBindVertexArray(0);

        glLineWidth(1.0f);
        glUseProgram(0);
    }

    // ---- Sprue cross-section circle (purple) -------------------------------
    if (m_flatProgram && m_sprue.xsecVAO && m_sprue.xsecVertexCount > 0)
    {
        glEnable(GL_DEPTH_TEST);
        glLineWidth(2.0f);
        glUseProgram(m_flatProgram);

        const glm::mat4 VP = proj * view;
        glUniformMatrix4fv(m_flat_uVP, 1, GL_FALSE, &VP[0][0]);

        const glm::vec4 sprueCircleColor(0.65f, 0.10f, 0.90f, 1.0f);   // purple
        glUniform4fv(m_flat_uColor, 1, &sprueCircleColor[0]);

        glBindVertexArray(m_sprue.xsecVAO);
        glDrawArrays(GL_LINES, 0, m_sprue.xsecVertexCount);
        glBindVertexArray(0);

        glLineWidth(1.0f);
        glUseProgram(0);
    }

    // ---- Runner path lines (blue) ------------------------------------------
    if (m_flatProgram && m_runnerPathVAO && m_runnerPathVertexCount > 0)
    {
        glEnable(GL_DEPTH_TEST);
        glLineWidth(2.5f);
        glUseProgram(m_flatProgram);

        const glm::mat4 VP = proj * view;
        glUniformMatrix4fv(m_flat_uVP, 1, GL_FALSE, &VP[0][0]);

        const glm::vec4 runnerLineColor(0.10f, 0.40f, 0.95f, 1.0f);   // blue
        glUniform4fv(m_flat_uColor, 1, &runnerLineColor[0]);

        glBindVertexArray(m_runnerPathVAO);
        glDrawArrays(GL_LINES, 0, m_runnerPathVertexCount);
        glBindVertexArray(0);

        glLineWidth(1.0f);
        glUseProgram(0);
    }

    // ---- Gate path lines (yellow) -----------------------------------------
    if (m_flatProgram && m_gatePathVAO && m_gatePathVertexCount > 0)
    {
        glEnable(GL_DEPTH_TEST);
        glLineWidth(2.5f);
        glUseProgram(m_flatProgram);

        const glm::mat4 VP = proj * view;
        glUniformMatrix4fv(m_flat_uVP, 1, GL_FALSE, &VP[0][0]);

        const glm::vec4 gateLineColor(1.0f, 0.85f, 0.10f, 1.0f);   // yellow, matches gate spheres
        glUniform4fv(m_flat_uColor, 1, &gateLineColor[0]);

        glBindVertexArray(m_gatePathVAO);
        glDrawArrays(GL_LINES, 0, m_gatePathVertexCount);
        glBindVertexArray(0);

        glLineWidth(1.0f);
        glUseProgram(0);
    }

    // ---- Vent path lines ---------------------------------------------------
    if (m_flatProgram && m_pathVAO && m_pathVertexCount > 0)
    {
        glEnable(GL_DEPTH_TEST);
        glLineWidth(2.0f);
        glUseProgram(m_flatProgram);

        const glm::mat4 VP = proj * view;
        glUniformMatrix4fv(m_flat_uVP, 1, GL_FALSE, &VP[0][0]);

        const glm::vec4 lineColor(0.10f, 0.92f, 0.25f, 1.0f);
        glUniform4fv(m_flat_uColor, 1, &lineColor[0]);

        glBindVertexArray(m_pathVAO);
        glDrawArrays(GL_LINES, 0, m_pathVertexCount);
        glBindVertexArray(0);
        glLineWidth(1.0f);
        glUseProgram(0);
    }

    // ---- Vent cross-sections -----------------------------------------------
    if (m_flatProgram && m_xsecVAO && m_xsecVertexCount > 0)
    {
        glEnable(GL_DEPTH_TEST);
        glLineWidth(2.0f);
        glUseProgram(m_flatProgram);

        const glm::mat4 VP = proj * view;
        glUniformMatrix4fv(m_flat_uVP, 1, GL_FALSE, &VP[0][0]);

        // Slightly brighter yellow-green to distinguish from path lines
        const glm::vec4 xsecColor(0.60f, 1.00f, 0.20f, 1.0f);
        glUniform4fv(m_flat_uColor, 1, &xsecColor[0]);

        glBindVertexArray(m_xsecVAO);
        glDrawArrays(GL_LINES, 0, m_xsecVertexCount);
        glBindVertexArray(0);
        glLineWidth(1.0f);
        glUseProgram(0);
    }

    // ---- Vent solids (lit, semi-transparent so the path line shows through) -
    if (m_program && !m_vents.empty())
    {
        glEnable(GL_DEPTH_TEST);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glDepthMask(GL_FALSE);

        glUseProgram(m_program);

        const glm::mat4 identity(1.0f);
        const glm::vec3 ventSolidColor(0.20f, 0.85f, 0.35f);
        glUniform3fv(glGetUniformLocation(m_program, "uCameraPos"), 1, &camPos[0]);
        glUniform3fv(glGetUniformLocation(m_program, "uLightDir"), 1, &lightDir[0]);
        glUniform3fv(glGetUniformLocation(m_program, "uLightColor"), 1, &lightColor[0]);
        glUniform3fv(glGetUniformLocation(m_program, "uBaseColor"), 1, &ventSolidColor[0]);
        glUniform1f(glGetUniformLocation(m_program, "uAlpha"), 0.55f);
        glUniform1f(glGetUniformLocation(m_program, "uAmbient"), 0.30f);
        glUniform1f(glGetUniformLocation(m_program, "uDiffuse"), 0.80f);
        glUniform1f(glGetUniformLocation(m_program, "uSpecular"), 0.40f);
        glUniform1f(glGetUniformLocation(m_program, "uShininess"), 32.0f);
        glUniformMatrix4fv(glGetUniformLocation(m_program, "uView"), 1, GL_FALSE, &view[0][0]);
        glUniformMatrix4fv(glGetUniformLocation(m_program, "uProj"), 1, GL_FALSE, &proj[0][0]);
        glUniformMatrix4fv(glGetUniformLocation(m_program, "uModel"), 1, GL_FALSE, &identity[0][0]);

        for (const VentInstance& vi : m_vents)
        {
            if (!vi.solid.valid || vi.solid.vao == 0) continue;
            glBindVertexArray(vi.solid.vao);
            glDrawElements(GL_TRIANGLES, vi.solid.indexCount, GL_UNSIGNED_INT, 0);
        }

        glBindVertexArray(0);
        glUseProgram(0);
        glDepthMask(GL_TRUE);
        glDisable(GL_BLEND);
    }

    // ---- Runner solids (lit, semi-transparent blue) -------------------------
    if (m_program && !m_runners.empty())
    {
        glEnable(GL_DEPTH_TEST);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glDepthMask(GL_FALSE);

        glUseProgram(m_program);

        const glm::mat4 identity(1.0f);
        const glm::vec3 runnerSolidColor(0.10f, 0.40f, 0.95f);   // blue, matches runner markers
        glUniform3fv(glGetUniformLocation(m_program, "uCameraPos"), 1, &camPos[0]);
        glUniform3fv(glGetUniformLocation(m_program, "uLightDir"), 1, &lightDir[0]);
        glUniform3fv(glGetUniformLocation(m_program, "uLightColor"), 1, &lightColor[0]);
        glUniform3fv(glGetUniformLocation(m_program, "uBaseColor"), 1, &runnerSolidColor[0]);
        glUniform1f(glGetUniformLocation(m_program, "uAlpha"), 0.55f);
        glUniform1f(glGetUniformLocation(m_program, "uAmbient"), 0.30f);
        glUniform1f(glGetUniformLocation(m_program, "uDiffuse"), 0.80f);
        glUniform1f(glGetUniformLocation(m_program, "uSpecular"), 0.40f);
        glUniform1f(glGetUniformLocation(m_program, "uShininess"), 32.0f);
        glUniformMatrix4fv(glGetUniformLocation(m_program, "uView"), 1, GL_FALSE, &view[0][0]);
        glUniformMatrix4fv(glGetUniformLocation(m_program, "uProj"), 1, GL_FALSE, &proj[0][0]);
        glUniformMatrix4fv(glGetUniformLocation(m_program, "uModel"), 1, GL_FALSE, &identity[0][0]);

        for (const RunnerFeature& rf : m_runners)
        {
            if (!rf.solid.valid || rf.solid.vao == 0) continue;
            glBindVertexArray(rf.solid.vao);
            glDrawElements(GL_TRIANGLES, rf.solid.indexCount, GL_UNSIGNED_INT, 0);
        }

        // Cold plug wells — darker blue, slightly more transparent
        const glm::vec3 coldPlugColor(0.08f, 0.28f, 0.70f);
        glUniform3fv(glGetUniformLocation(m_program, "uBaseColor"), 1, &coldPlugColor[0]);
        glUniform1f(glGetUniformLocation(m_program, "uAlpha"), 0.45f);

        for (const RunnerFeature& rf : m_runners)
        {
            if (!rf.coldPlugSolid.valid || rf.coldPlugSolid.vao == 0) continue;
            glBindVertexArray(rf.coldPlugSolid.vao);
            glDrawElements(GL_TRIANGLES, rf.coldPlugSolid.indexCount, GL_UNSIGNED_INT, 0);
        }

        glBindVertexArray(0);
        glUseProgram(0);
        glDepthMask(GL_TRUE);
        glDisable(GL_BLEND);
    }

    // ---- Gate solids (lit, semi-transparent yellow) ------------------------
    if (m_program && !m_gates.empty())
    {
        glEnable(GL_DEPTH_TEST);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glDepthMask(GL_FALSE);
        glUseProgram(m_program);

        const glm::mat4 identity(1.0f);
        glUniform3fv(glGetUniformLocation(m_program, "uCameraPos"), 1, &camPos[0]);
        glUniform3fv(glGetUniformLocation(m_program, "uLightDir"), 1, &lightDir[0]);
        glUniform3fv(glGetUniformLocation(m_program, "uLightColor"), 1, &lightColor[0]);
        glUniform1f(glGetUniformLocation(m_program, "uAlpha"), 0.55f);
        glUniform1f(glGetUniformLocation(m_program, "uAmbient"), 0.30f);
        glUniform1f(glGetUniformLocation(m_program, "uDiffuse"), 0.80f);
        glUniform1f(glGetUniformLocation(m_program, "uSpecular"), 0.40f);
        glUniform1f(glGetUniformLocation(m_program, "uShininess"), 32.0f);
        glUniformMatrix4fv(glGetUniformLocation(m_program, "uView"), 1, GL_FALSE, &view[0][0]);
        glUniformMatrix4fv(glGetUniformLocation(m_program, "uProj"), 1, GL_FALSE, &proj[0][0]);
        glUniformMatrix4fv(glGetUniformLocation(m_program, "uModel"), 1, GL_FALSE, &identity[0][0]);

        // Gate frustums — bright yellow, matches gate sphere colour
        const glm::vec3 gateColor(1.0f, 0.85f, 0.10f);
        glUniform3fv(glGetUniformLocation(m_program, "uBaseColor"), 1, &gateColor[0]);
        for (const GateFeature& gf : m_gates)
        {
            if (!gf.solid.valid || gf.solid.vao == 0) continue;
            glBindVertexArray(gf.solid.vao);
            glDrawElements(GL_TRIANGLES, gf.solid.indexCount, GL_UNSIGNED_INT, 0);
        }

        // Sub-runner cylinders — slightly dimmer yellow, more transparent
        const glm::vec3 subRunnerColor(0.80f, 0.65f, 0.05f);
        glUniform3fv(glGetUniformLocation(m_program, "uBaseColor"), 1, &subRunnerColor[0]);
        glUniform1f(glGetUniformLocation(m_program, "uAlpha"), 0.45f);
        for (const GateFeature& gf : m_gates)
        {
            if (!gf.subRunnerSolid.valid || gf.subRunnerSolid.vao == 0) continue;
            glBindVertexArray(gf.subRunnerSolid.vao);
            glDrawElements(GL_TRIANGLES, gf.subRunnerSolid.indexCount, GL_UNSIGNED_INT, 0);
        }

        glBindVertexArray(0);
        glUseProgram(0);
        glDepthMask(GL_TRUE);
        glDisable(GL_BLEND);
    }

    // ---- Ejector solids (lit, semi-transparent cyan) -----------------------
    // Mirrors the gate-solid block exactly: same lighting setup, same blend
    // / depth-mask convention, only the colour and the source vector differ.
    // The cyan matches the ejector marker spheres so the geometry visually
    // groups with its placement point.
    if (m_program && !m_ejectors.empty())
    {
        glEnable(GL_DEPTH_TEST);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glDepthMask(GL_FALSE);
        glUseProgram(m_program);

        const glm::mat4 identity(1.0f);
        glUniform3fv(glGetUniformLocation(m_program, "uCameraPos"), 1, &camPos[0]);
        glUniform3fv(glGetUniformLocation(m_program, "uLightDir"), 1, &lightDir[0]);
        glUniform3fv(glGetUniformLocation(m_program, "uLightColor"), 1, &lightColor[0]);
        glUniform1f(glGetUniformLocation(m_program, "uAlpha"), 0.55f);
        glUniform1f(glGetUniformLocation(m_program, "uAmbient"), 0.30f);
        glUniform1f(glGetUniformLocation(m_program, "uDiffuse"), 0.80f);
        glUniform1f(glGetUniformLocation(m_program, "uSpecular"), 0.40f);
        glUniform1f(glGetUniformLocation(m_program, "uShininess"), 32.0f);
        glUniformMatrix4fv(glGetUniformLocation(m_program, "uView"), 1, GL_FALSE, &view[0][0]);
        glUniformMatrix4fv(glGetUniformLocation(m_program, "uProj"), 1, GL_FALSE, &proj[0][0]);
        glUniformMatrix4fv(glGetUniformLocation(m_program, "uModel"), 1, GL_FALSE, &identity[0][0]);

        const glm::vec3 ejectorColor(0.20f, 0.85f, 0.95f);   // cyan
        glUniform3fv(glGetUniformLocation(m_program, "uBaseColor"), 1, &ejectorColor[0]);
        for (const EjectorFeature& ef : m_ejectors)
        {
            if (!ef.solid.valid || ef.solid.vao == 0) continue;
            glBindVertexArray(ef.solid.vao);
            glDrawElements(GL_TRIANGLES, ef.solid.indexCount, GL_UNSIGNED_INT, 0);
        }

        glBindVertexArray(0);
        glUseProgram(0);
        glDepthMask(GL_TRUE);
        glDisable(GL_BLEND);
    }

    // ---- Sprue solid (lit, semi-transparent purple) ------------------------
    if (m_program && m_sprue.solid.valid && m_sprue.solid.vao != 0)
    {
        glEnable(GL_DEPTH_TEST);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glDepthMask(GL_FALSE);

        glUseProgram(m_program);

        const glm::mat4 identity(1.0f);
        const glm::vec3 sprueSolidColor(0.65f, 0.10f, 0.90f);   // purple
        glUniform3fv(glGetUniformLocation(m_program, "uCameraPos"), 1, &camPos[0]);
        glUniform3fv(glGetUniformLocation(m_program, "uLightDir"), 1, &lightDir[0]);
        glUniform3fv(glGetUniformLocation(m_program, "uLightColor"), 1, &lightColor[0]);
        glUniform3fv(glGetUniformLocation(m_program, "uBaseColor"), 1, &sprueSolidColor[0]);
        glUniform1f(glGetUniformLocation(m_program, "uAlpha"), 0.55f);
        glUniform1f(glGetUniformLocation(m_program, "uAmbient"), 0.30f);
        glUniform1f(glGetUniformLocation(m_program, "uDiffuse"), 0.80f);
        glUniform1f(glGetUniformLocation(m_program, "uSpecular"), 0.40f);
        glUniform1f(glGetUniformLocation(m_program, "uShininess"), 32.0f);
        glUniformMatrix4fv(glGetUniformLocation(m_program, "uView"), 1, GL_FALSE, &view[0][0]);
        glUniformMatrix4fv(glGetUniformLocation(m_program, "uProj"), 1, GL_FALSE, &proj[0][0]);
        glUniformMatrix4fv(glGetUniformLocation(m_program, "uModel"), 1, GL_FALSE, &identity[0][0]);

        glBindVertexArray(m_sprue.solid.vao);
        glDrawElements(GL_TRIANGLES, m_sprue.solid.indexCount, GL_UNSIGNED_INT, 0);
        glBindVertexArray(0);

        glUseProgram(0);
        glDepthMask(GL_TRUE);
        glDisable(GL_BLEND);
    }

    // ---- Cold slug solid (lit, semi-transparent purple) ---------------------
    if (m_program && m_sprue.coldSlugSolid.valid && m_sprue.coldSlugSolid.vao != 0)
    {
        glEnable(GL_DEPTH_TEST);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glDepthMask(GL_FALSE);

        glUseProgram(m_program);

        const glm::mat4 identity(1.0f);
        const glm::vec3 coldSlugColor(0.65f, 0.10f, 0.90f);   // purple, same as sprue
        glUniform3fv(glGetUniformLocation(m_program, "uCameraPos"), 1, &camPos[0]);
        glUniform3fv(glGetUniformLocation(m_program, "uLightDir"), 1, &lightDir[0]);
        glUniform3fv(glGetUniformLocation(m_program, "uLightColor"), 1, &lightColor[0]);
        glUniform3fv(glGetUniformLocation(m_program, "uBaseColor"), 1, &coldSlugColor[0]);
        glUniform1f(glGetUniformLocation(m_program, "uAlpha"), 0.55f);
        glUniform1f(glGetUniformLocation(m_program, "uAmbient"), 0.30f);
        glUniform1f(glGetUniformLocation(m_program, "uDiffuse"), 0.80f);
        glUniform1f(glGetUniformLocation(m_program, "uSpecular"), 0.40f);
        glUniform1f(glGetUniformLocation(m_program, "uShininess"), 32.0f);
        glUniformMatrix4fv(glGetUniformLocation(m_program, "uView"), 1, GL_FALSE, &view[0][0]);
        glUniformMatrix4fv(glGetUniformLocation(m_program, "uProj"), 1, GL_FALSE, &proj[0][0]);
        glUniformMatrix4fv(glGetUniformLocation(m_program, "uModel"), 1, GL_FALSE, &identity[0][0]);

        glBindVertexArray(m_sprue.coldSlugSolid.vao);
        glDrawElements(GL_TRIANGLES, m_sprue.coldSlugSolid.indexCount, GL_UNSIGNED_INT, 0);
        glBindVertexArray(0);

        glUseProgram(0);
        glDepthMask(GL_TRUE);
        glDisable(GL_BLEND);
    }

    // ---- DEBUG: fixture perimeter hull -------------------------------------
    if (m_flatProgram && !m_fixturePerimeter.empty())
    {
        // Pack hull vertices into a temporary float buffer (y = 0)
        std::vector<float> perimVerts;
        perimVerts.reserve(m_fixturePerimeter.size() * 3);
        for (const glm::vec2& p : m_fixturePerimeter)
        {
            perimVerts.push_back(p.x);
            perimVerts.push_back(0.0f);
            perimVerts.push_back(p.y);
        }

        GLuint tmpVAO = 0, tmpVBO = 0;
        glGenVertexArrays(1, &tmpVAO);
        glGenBuffers(1, &tmpVBO);
        glBindVertexArray(tmpVAO);
        glBindBuffer(GL_ARRAY_BUFFER, tmpVBO);
        glBufferData(GL_ARRAY_BUFFER,
            (GLsizeiptr)(perimVerts.size() * sizeof(float)),
            perimVerts.data(), GL_STREAM_DRAW);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void*)0);
        glEnableVertexAttribArray(0);

        glDisable(GL_DEPTH_TEST);   // always draw on top
        glLineWidth(2.0f);
        glUseProgram(m_flatProgram);

        const glm::mat4 VP = proj * view;
        glUniformMatrix4fv(m_flat_uVP, 1, GL_FALSE, &VP[0][0]);

        const glm::vec4 perimColor(1.0f, 0.4f, 0.0f, 1.0f);   // orange
        glUniform4fv(m_flat_uColor, 1, &perimColor[0]);

        glDrawArrays(GL_LINE_LOOP, 0,
            (GLsizei)m_fixturePerimeter.size());

        glLineWidth(1.0f);
        glEnable(GL_DEPTH_TEST);
        glUseProgram(0);
        glBindVertexArray(0);

        glDeleteBuffers(1, &tmpVBO);
        glDeleteVertexArrays(1, &tmpVAO);
    }

    SwapBuffers();
}

// ---------------------------------------------------------------------------
// Mouse
// ---------------------------------------------------------------------------
void GLCanvas::OnMouse(wxMouseEvent& evt)
{
    // Preview canvas: navigation only. Orbit (LMB), pan (MMB or Shift+LMB) and
    // dolly (RMB or Ctrl+LMB) match the main canvas, but there is no picking,
    // selection, transform, or feature placement.
    if (m_previewMode)
    {
        if (evt.LeftDown())   { m_lmb = true;  m_hasLast = false; }
        if (evt.LeftUp())     { m_lmb = false; if (HasCapture()) ReleaseMouse(); }
        if (evt.MiddleDown()) { m_mmb = true;  m_hasLast = false; CaptureMouse(); }
        if (evt.MiddleUp())   { m_mmb = false; if (HasCapture()) ReleaseMouse(); }
        if (evt.RightDown())  { m_rmb = true;  m_hasLast = false; CaptureMouse(); }
        if (evt.RightUp())    { m_rmb = false; if (HasCapture()) ReleaseMouse(); }

        if (evt.Dragging() && m_lmb && !HasCapture())
            CaptureMouse();

        if (!evt.Moving() && !evt.Dragging()) { evt.Skip(); return; }

        const wxPoint pos = evt.GetPosition();
        if (!m_hasLast) { m_lastPos = pos; m_hasLast = true; evt.Skip(); return; }

        const float dx = float(pos.x - m_lastPos.x);
        const float dy = float(pos.y - m_lastPos.y);
        m_lastPos = pos;

        const bool shift = evt.ShiftDown();
        const bool ctrl = evt.ControlDown();

        if (m_mmb)        { m_camera.Pan(dx, -dy);       Refresh(false); }
        else if (m_rmb)   { m_camera.Dolly(dy * 0.05f);  Refresh(false); }
        else if (m_lmb)
        {
            if (shift)      m_camera.Pan(dx, -dy);
            else if (ctrl)  m_camera.Dolly(dy * 0.05f);
            else            m_camera.Orbit(dx, dy);
            Refresh(false);
        }
        return;
    }

    if (evt.LeftDown())
    {
        m_lmb = true;
        m_hasLast = false;

        if (m_transformMode == TransformMode::Select)
        {
            const wxPoint p = evt.GetPosition();
            const int hit = PickObjectAt(p.x, p.y);
            const bool ctrl = evt.ControlDown();

            if (ctrl)
            {
                // Ctrl+LMB: toggle the hit object in the selection. A miss
                // is a no-op so users can drift the cursor mid-modifier
                // without losing selection.
                if (hit >= 0)
                {
                    auto it = std::find(m_selectedIndices.begin(),
                        m_selectedIndices.end(), hit);
                    if (it != m_selectedIndices.end())
                        m_selectedIndices.erase(it);
                    else
                        m_selectedIndices.push_back(hit);
                }
            }
            else
            {
                // Plain LMB:
                //   miss          -> clear selection
                //   hit unselected -> replace selection with {hit}
                //   hit already-selected -> leave the vector alone, so the
                //     follow-up drag in OnMotion moves the whole group as
                //     a rigid unit (without this, clicking any member of a
                //     multi-selection would collapse it to that one object).
                if (hit < 0)
                {
                    m_selectedIndices.clear();
                }
                else
                {
                    const bool already = std::find(m_selectedIndices.begin(),
                        m_selectedIndices.end(), hit) != m_selectedIndices.end();
                    if (!already)
                    {
                        m_selectedIndices.clear();
                        m_selectedIndices.push_back(hit);
                    }
                }
            }
            Refresh(false);
        }
        else if (m_transformMode == TransformMode::PlaceVent)
        {
            const wxPoint p = evt.GetPosition();
            glm::vec3 hitPos, hitNormal;
            int       hitObj = -1;
            if (RayCastParting(p.x, p.y, hitPos, hitNormal, &hitObj))
            {
                VentInstance vi;
                vi.point = VentPoint{ hitPos, hitNormal };

                // Capture parent association so the vent travels with its
                // object on subsequent transforms / patterning. localPos /
                // localNormal are computed from the parent's current model
                // matrix; transpose(model3x3) applied to a world normal
                // produces the matching local normal because the inverse
                // and transpose-inverse are themselves transposes for
                // rotations (and the magnitude is renormalised below).
                if (hitObj >= 0 && hitObj < (int)m_objects.size())
                {
                    const glm::mat4 m = m_objects[hitObj].BuildModelMatrix();
                    const glm::mat4 invM = glm::inverse(m);
                    vi.parentIndex = hitObj;
                    vi.localPos = glm::vec3(invM * glm::vec4(hitPos, 1.0f));
                    glm::vec3 ln = glm::transpose(glm::mat3(m)) * hitNormal;
                    const float lnLen = glm::length(ln);
                    vi.localNormal = (lnLen > 1e-6f) ? ln / lnLen : glm::vec3(0, 0, 1);
                }

                // Read dimensions from the left-panel UI
                float ventLength = 5.0f, ventWidth = 2.0f,
                    ventOverrunStart = 0.5f, ventOverrunEnd = 0.5f;
                if (auto* frame = dynamic_cast<MainFrame*>(wxGetTopLevelParent(this)))
                    frame->GetVentDimensions(ventLength, ventWidth,
                        ventOverrunStart, ventOverrunEnd);

                vi.path = ComputeVentPath(vi.point);
                vi.path.overrunStart = ventOverrunStart;
                vi.path.overrunEnd = ventOverrunEnd;

                vi.crossSection =
                    BuildVentCrossSection(vi.path, ventWidth, ventLength);

                vi.solid =
                    BuildBoxSweepMesh(vi.path, ventWidth, ventLength,
                        ventOverrunStart, ventOverrunEnd);

                m_vents.push_back(std::move(vi));

                RebuildPathVBO();
                RebuildCrossSectionVBO();
                Refresh(false);
                NotifySceneMutated();
            }
        }
        else if (m_transformMode == TransformMode::PlaceRunner)
        {
            const wxPoint p = evt.GetPosition();
            glm::vec3 hitPos;
            if (RayCastToPartingPlane(p.x, p.y, hitPos))
            {
                const glm::vec2 hitXZ(hitPos.x, hitPos.z);
                if (IsInsideConvexHull(m_fixturePerimeter, hitXZ))
                {
                    m_runners.push_back(RunnerFeature{ hitPos, {}, {} });
                    RebuildRunnerPathVBO();
                    RebuildRunnerSolids();
                    RebuildGatePathVBO();
                    RebuildGateSolids();
                    Refresh(false);
                    NotifySceneMutated();
                }
            }
        }
        else if (m_transformMode == TransformMode::PlaceGate)
        {
            const wxPoint p = evt.GetPosition();
            glm::vec3 hitPos, hitNormal;
            int       hitObj = -1;
            if (RayCastParting(p.x, p.y, hitPos, hitNormal, &hitObj))
            {
                GateFeature gf;
                gf.point = VentPoint{ hitPos, hitNormal };

                // Capture parent association — see PlaceVent for the rationale
                // behind the transpose(model3x3) used to take a world normal
                // back into local space.
                if (hitObj >= 0 && hitObj < (int)m_objects.size())
                {
                    const glm::mat4 m = m_objects[hitObj].BuildModelMatrix();
                    const glm::mat4 invM = glm::inverse(m);
                    gf.parentIndex = hitObj;
                    gf.localPos = glm::vec3(invM * glm::vec4(hitPos, 1.0f));
                    glm::vec3 ln = glm::transpose(glm::mat3(m)) * hitNormal;
                    const float lnLen = glm::length(ln);
                    gf.localNormal = (lnLen > 1e-6f) ? ln / lnLen : glm::vec3(0, 0, 1);
                }

                m_gates.push_back(std::move(gf));
                RebuildGatePathVBO();
                RebuildGateSolids();
                Refresh(false);
                NotifySceneMutated();
            }
        }
        else if (m_transformMode == TransformMode::PlaceEjector)
        {
            // Snap to the closest of: sprue parting point, any runner
            // segment, any gate segment, or any object face. Falls through
            // silently if none are in range — matching the other Place*
            // modes' behaviour for an out-of-bounds click.
            const wxPoint p = evt.GetPosition();
            glm::vec3 hitPos;
            if (RayCastEjectorSnap(p.x, p.y, hitPos))
            {
                EjectorFeature ef;
                ef.point = hitPos;
                m_ejectors.push_back(std::move(ef));
                // Rebuild every ejector's cylinder. Cheap with the small
                // counts we deal with here, and matches the all-at-once
                // pattern RebuildGateSolids uses (which lets a future
                // "live update on UI change" feature plug in cleanly).
                RebuildEjectorSolids();
                Refresh(false);
                NotifySceneMutated();
            }
        }
        else if (m_transformMode == TransformMode::RemoveVent)
        {
            const wxPoint p = evt.GetPosition();
            RemoveVentAtMouse(p.x, p.y);
        }
        else if (m_transformMode == TransformMode::RemoveRunner)
        {
            const wxPoint p = evt.GetPosition();
            RemoveRunnerAtMouse(p.x, p.y);
        }
        else if (m_transformMode == TransformMode::RemoveGate)
        {
            const wxPoint p = evt.GetPosition();
            RemoveGateAtMouse(p.x, p.y);
        }
        else if (m_transformMode == TransformMode::RemoveSprue)
        {
            const wxPoint p = evt.GetPosition();
            RemoveSprueAtMouse(p.x, p.y);
        }
        else if (m_transformMode == TransformMode::RemoveEjector)
        {
            const wxPoint p = evt.GetPosition();
            RemoveEjectorAtMouse(p.x, p.y);
        }
        else if (m_transformMode == TransformMode::AlignFace)
        {
            // Click commits whatever face is currently highlighted under the
            // cursor. We re-run the cast here rather than trusting the cached
            // hover state, in case a click arrives before OnPaint refreshes.
            const wxPoint p = evt.GetPosition();
            int objIdx = -1, triIdx = -1;
            if (RayCastFacePick(p.x, p.y, objIdx, triIdx))
            {
                std::vector<uint32_t> faceTris;
                glm::vec3 nLocal(0.0f);
                EnsureTriAdjacency(m_objects[objIdx]);
                GrowCoplanarFace(m_objects[objIdx], triIdx, faceTris, nLocal);
                if (!faceTris.empty())
                {
                    ApplyAlignFaceToObject(objIdx, nLocal, faceTris);

                    // After applying, the cached world-space triangles are
                    // stale and the new pose may not project under the cursor
                    // anymore — drop the highlight so OnPaint regenerates it
                    // from the next motion event.
                    m_alignHoverObject = -1;
                    m_alignSeedTri = -1;
                    m_alignFaceTris.clear();
                    m_alignHighlightVertexCount = 0;
                    Refresh(false);
                }
            }
        }
        else if (m_transformMode == TransformMode::AlignMidplane)
        {
            // Two-click flow:
            //   • No face locked yet → first click locks face 1.
            //   • Face 1 locked → second click computes the midplane between
            //     face 1 and the picked face 2, applies the alignment, and
            //     resets to "no lock" so the user can do another midplane
            //     alignment without leaving the mode.
            //   • Second click on a different object than face 1 is silently
            //     ignored (alignment only makes sense within one object).
            const wxPoint p = evt.GetPosition();
            int objIdx = -1, triIdx = -1;
            if (RayCastFacePick(p.x, p.y, objIdx, triIdx))
            {
                std::vector<uint32_t> faceTris;
                glm::vec3 nLocal(0.0f);
                EnsureTriAdjacency(m_objects[objIdx]);
                GrowCoplanarFace(m_objects[objIdx], triIdx, faceTris, nLocal);
                if (!faceTris.empty())
                {
                    if (!m_midplaneFaceLocked)
                    {
                        // First click: lock the face. Compute its centroid in
                        // local space now so we don't need the tri list later.
                        const SceneObject& obj = m_objects[objIdx];
                        glm::vec3 c1Local(0.0f);
                        for (uint32_t t : faceTris)
                        {
                            const uint32_t i0 = obj.cpuIndices[3 * t + 0];
                            const uint32_t i1 = obj.cpuIndices[3 * t + 1];
                            const uint32_t i2 = obj.cpuIndices[3 * t + 2];
                            const glm::vec3 v0(obj.cpuVerts[i0 * 3], obj.cpuVerts[i0 * 3 + 1], obj.cpuVerts[i0 * 3 + 2]);
                            const glm::vec3 v1(obj.cpuVerts[i1 * 3], obj.cpuVerts[i1 * 3 + 1], obj.cpuVerts[i1 * 3 + 2]);
                            const glm::vec3 v2(obj.cpuVerts[i2 * 3], obj.cpuVerts[i2 * 3 + 1], obj.cpuVerts[i2 * 3 + 2]);
                            c1Local += (v0 + v1 + v2) * (1.0f / 3.0f);
                        }
                        c1Local /= float(faceTris.size());

                        m_midplaneFaceLocked = true;
                        m_midplaneFaceObject = objIdx;
                        m_midplaneFaceTris = faceTris;
                        m_midplaneFaceNormalLocal = nLocal;
                        m_midplaneFaceCentroidLocal = c1Local;

                        RebuildMidplaneLockedVBO(obj, faceTris);
                        Refresh(false);
                    }
                    else if (objIdx == m_midplaneFaceObject)
                    {
                        // Second click on the same object: apply midplane alignment.
                        ApplyAlignMidplaneToObject(objIdx, nLocal, faceTris);

                        // Reset to "no lock" so the user can do another pair.
                        // Highlight state is also cleared since the object
                        // moved and the cached world-space tris are stale.
                        m_midplaneFaceLocked = false;
                        m_midplaneFaceObject = -1;
                        m_midplaneFaceTris.clear();
                        m_midplaneLockedVertexCount = 0;

                        m_alignHoverObject = -1;
                        m_alignSeedTri = -1;
                        m_alignFaceTris.clear();
                        m_alignHighlightVertexCount = 0;
                        Refresh(false);
                    }
                    // else: second click was on a different object — ignored.
                }
            }
        }
        else if (m_transformMode == TransformMode::SelectInjectionPoint)
        {
            if (m_injectionPoints.empty()) { /* nothing to pick */ }
            else
            {
                const wxPoint p = evt.GetPosition();
                glm::vec3 rayOrig, rayDir;
                BuildMouseRay(p.x, p.y, rayOrig, rayDir);

                glm::mat4 fixtureMatrix(1.0f);
                if (!m_fixtures.empty())
                    fixtureMatrix = m_fixtures[0].BuildModelMatrix();

                const float hitRadius = kVentMarkerRadius * 3.5f;
                float bestDist = hitRadius;
                int   bestIdx = -1;
                for (int i = 0; i < (int)m_injectionPoints.size(); ++i)
                {
                    const auto& ip = m_injectionPoints[i];
                    glm::vec3 worldPos = glm::vec3(fixtureMatrix *
                        glm::vec4(ip.x, ip.y, ip.z, 1.0f));
                    const float d = PointRayDistance(worldPos, rayOrig, rayDir);
                    if (d < bestDist) { bestDist = d; bestIdx = i; }
                }

                if (bestIdx >= 0)
                {
                    SetActiveInjectionPoint(m_injectionPoints[bestIdx]);

                    // Notify MainFrame BEFORE PlaceSprue so any UI fields
                    // whose value depends on injection-point type (currently
                    // just the Draft Angle) update first — PlaceSprue reads
                    // those fields back when building the sprue.
                    if (auto* frame = dynamic_cast<MainFrame*>(wxGetTopLevelParent(this)))
                        frame->OnInjectionPointSelected(m_injectionPoints[bestIdx]);

                    PlaceSprue();

                    // Exit selection mode back to Select
                    m_transformMode = TransformMode::Select;
                    if (auto* frame = dynamic_cast<MainFrame*>(wxGetTopLevelParent(this)))
                        frame->SetActiveTool(TransformMode::Select);
                    Refresh(false);
                }
            }
        }
        else if (m_transformMode == TransformMode::EditVent)
        {
            const wxPoint p = evt.GetPosition();
            glm::vec3 rayOrig, rayDir;
            BuildMouseRay(p.x, p.y, rayOrig, rayDir);

            const float hitRadius = kVentMarkerRadius * 2.0f;
            float bestDist = hitRadius;
            int   bestIdx = -1;
            for (int i = 0; i < (int)m_vents.size(); ++i)
            {
                const float d = PointRayDistance(m_vents[i].point.worldPos, rayOrig, rayDir);
                if (d < bestDist) { bestDist = d; bestIdx = i; }
            }
            m_editFeatureIndex = bestIdx;
            Refresh(false);
        }
        else if (m_transformMode == TransformMode::EditRunner)
        {
            const wxPoint p = evt.GetPosition();
            glm::vec3 rayOrig, rayDir;
            BuildMouseRay(p.x, p.y, rayOrig, rayDir);

            const float hitRadius = kVentMarkerRadius * 2.0f;
            float bestDist = hitRadius;
            int   bestIdx = -1;
            for (int i = 0; i < (int)m_runners.size(); ++i)
            {
                const float d = PointRayDistance(m_runners[i].point, rayOrig, rayDir);
                if (d < bestDist) { bestDist = d; bestIdx = i; }
            }
            m_editFeatureIndex = bestIdx;
            Refresh(false);
        }
        else if (m_transformMode == TransformMode::EditGate)
        {
            const wxPoint p = evt.GetPosition();
            glm::vec3 rayOrig, rayDir;
            BuildMouseRay(p.x, p.y, rayOrig, rayDir);

            const float hitRadius = kVentMarkerRadius * 2.0f;
            float bestDist = hitRadius;
            int   bestIdx = -1;
            for (int i = 0; i < (int)m_gates.size(); ++i)
            {
                const float d = PointRayDistance(m_gates[i].point.worldPos, rayOrig, rayDir);
                if (d < bestDist) { bestDist = d; bestIdx = i; }
            }
            m_editFeatureIndex = bestIdx;
            Refresh(false);
        }
        else if (m_transformMode == TransformMode::EditEjector)
        {
            // Same pick pattern as EditGate — closest marker within
            // 2× the marker radius wins, -1 if no marker is in range.
            const wxPoint p = evt.GetPosition();
            glm::vec3 rayOrig, rayDir;
            BuildMouseRay(p.x, p.y, rayOrig, rayDir);

            const float hitRadius = kVentMarkerRadius * 2.0f;
            float bestDist = hitRadius;
            int   bestIdx = -1;
            for (int i = 0; i < (int)m_ejectors.size(); ++i)
            {
                const float d = PointRayDistance(m_ejectors[i].point, rayOrig, rayDir);
                if (d < bestDist) { bestDist = d; bestIdx = i; }
            }
            m_editFeatureIndex = bestIdx;
            Refresh(false);
        }
    }

    if (evt.LeftUp()) { m_lmb = false; if (HasCapture()) ReleaseMouse(); }
    if (evt.MiddleDown()) { m_mmb = true;  m_hasLast = false; CaptureMouse(); }
    if (evt.MiddleUp()) { m_mmb = false; if (HasCapture()) ReleaseMouse(); }
    if (evt.RightDown()) { m_rmb = true;  m_hasLast = false; CaptureMouse(); }
    if (evt.RightUp()) { m_rmb = false; if (HasCapture()) ReleaseMouse(); }

    // RMB cancels the locked first face in AlignMidplane mode (without
    // leaving the mode). Mirrors the click-to-cancel idiom used elsewhere
    // for partial selections.
    if (evt.RightDown() &&
        m_transformMode == TransformMode::AlignMidplane &&
        m_midplaneFaceLocked)
    {
        m_midplaneFaceLocked = false;
        m_midplaneFaceObject = -1;
        m_midplaneFaceTris.clear();
        m_midplaneLockedVertexCount = 0;
        Refresh(false);
    }

    if (evt.Dragging() && m_lmb && !HasCapture())
        CaptureMouse();

    if (!evt.Moving() && !evt.Dragging()) { evt.Skip(); return; }

    const wxPoint pos = evt.GetPosition();
    if (!m_hasLast)
    {
        m_lastPos = pos;
        m_hasLast = true;
        if (m_transformMode == TransformMode::PlaceVent)
        {
            m_ghostMousePos = pos;
            Refresh(false);
        }
        if (m_transformMode == TransformMode::PlaceRunner)
        {
            m_runnerGhostMousePos = pos;
            Refresh(false);
        }
        if (m_transformMode == TransformMode::PlaceGate)
        {
            m_gateGhostMousePos = pos;
            Refresh(false);
        }
        if (m_transformMode == TransformMode::PlaceEjector)
        {
            m_ejectorGhostMousePos = pos;
            Refresh(false);
        }
        if (m_transformMode == TransformMode::AlignFace ||
            m_transformMode == TransformMode::AlignMidplane)
        {
            m_alignMousePos = pos;
            Refresh(false);
        }
        evt.Skip();
        return;
    }

    const float dx = float(pos.x - m_lastPos.x);
    const float dy = float(pos.y - m_lastPos.y);
    m_lastPos = pos;

    const bool shift = evt.ShiftDown();
    const bool ctrl = evt.ControlDown();

    // Camera controls always available via MMB / RMB
    if (m_mmb) {
        m_camera.Pan(dx, -dy);
        Refresh(false);
    }
    else if (m_rmb) {
        m_camera.Dolly(dy * 0.05f);
        Refresh(false);
    }
    else if (m_lmb && m_transformMode == TransformMode::Select)
    {
        if (!m_selectedIndices.empty())
        {
            const float dist = m_camera.GetDistance();
            const float unitsPerPx = dist * 0.0015f;

            glm::vec3 right = m_camera.Right();
            right.y = 0.0f;
            if (glm::length(right) > 1e-4f)
                right = glm::normalize(right);

            glm::vec3 forward = glm::normalize(
                glm::cross(glm::vec3(0, 1, 0), right));

            // Flip dy when camera is below the grid to keep drag direction consistent
            const float dyAdjusted = (m_camera.Position().y < 0.0f) ? -dy : dy;

            // Same delta applied to every selected object so they move as a
            // rigid group, preserving relative positions.
            const glm::vec3 delta = right * (dx * unitsPerPx)
                + forward * (-dyAdjusted * unitsPerPx);
            for (int idx : m_selectedIndices)
            {
                if (idx < 0 || idx >= (int)m_objects.size()) continue;
                m_objects[idx].pos += delta;
            }
            // Parented vents/gates ride along with their parents; unparented
            // features are independent and left alone.
            ReanchorFeaturesForObjects(m_selectedIndices);
            Refresh(false);
        }
        else
        {
            // Nothing selected — orbit as normal
            if (shift)
                m_camera.Pan(dx, -dy);
            else if (ctrl)
                m_camera.Dolly(dy * 0.05f);
            else
                m_camera.Orbit(dx, dy);
            Refresh(false);
        }
    }
    else if (m_lmb && m_transformMode == TransformMode::PlaceVent)
    {
        // Orbit while holding LMB in PlaceVent mode (no object to drag)
        if (shift)
            m_camera.Pan(dx, -dy);
        else if (ctrl)
            m_camera.Dolly(dy * 0.05f);
        else
            m_camera.Orbit(dx, dy);
        Refresh(false);
    }
    else if (m_lmb && m_transformMode == TransformMode::PlaceRunner)
    {
        // Orbit while holding LMB in PlaceRunner mode
        if (shift)
            m_camera.Pan(dx, -dy);
        else if (ctrl)
            m_camera.Dolly(dy * 0.05f);
        else
            m_camera.Orbit(dx, dy);
        Refresh(false);
    }
    else if (m_lmb && m_transformMode == TransformMode::PlaceGate)
    {
        // Orbit while holding LMB in PlaceGate mode
        if (shift)
            m_camera.Pan(dx, -dy);
        else if (ctrl)
            m_camera.Dolly(dy * 0.05f);
        else
            m_camera.Orbit(dx, dy);
        Refresh(false);
    }
    else if (m_lmb && (m_transformMode == TransformMode::RemoveVent ||
        m_transformMode == TransformMode::RemoveRunner ||
        m_transformMode == TransformMode::RemoveGate ||
        m_transformMode == TransformMode::RemoveSprue))
    {
        // Orbit while holding LMB in Remove modes
        if (shift)
            m_camera.Pan(dx, -dy);
        else if (ctrl)
            m_camera.Dolly(dy * 0.05f);
        else
            m_camera.Orbit(dx, dy);
        Refresh(false);
    }
    else if (m_lmb && (m_transformMode == TransformMode::EditVent ||
        m_transformMode == TransformMode::EditRunner ||
        m_transformMode == TransformMode::EditGate ||
        m_transformMode == TransformMode::EditEjector))
    {
        if (m_editFeatureIndex >= 0)
        {
            // Defer ray cast + geometry rebuild to OnPaint so only one
            // update runs per rendered frame regardless of queued events.
            m_editMousePos = pos;
            m_editNeedsUpdate = true;
            Refresh(false);
        }
        else
        {
            // No feature selected — orbit
            if (shift) m_camera.Pan(dx, -dy);
            else if (ctrl) m_camera.Dolly(dy * 0.05f);
            else m_camera.Orbit(dx, dy);
            Refresh(false);
        }
    }
    else if (m_lmb && (m_transformMode == TransformMode::AlignFace ||
        m_transformMode == TransformMode::AlignMidplane))
    {
        // Orbit while holding LMB in alignment modes (matches Place* modes).
        if (shift)
            m_camera.Pan(dx, -dy);
        else if (ctrl)
            m_camera.Dolly(dy * 0.05f);
        else
            m_camera.Orbit(dx, dy);
        Refresh(false);
    }

    // Update ghost preview whenever the mouse moves in PlaceVent mode.
    // The actual ray cast is deferred to OnPaint so only one cast runs per
    // rendered frame, regardless of how many motion events have queued up.
    if (m_transformMode == TransformMode::PlaceVent)
    {
        m_ghostMousePos = evt.GetPosition();
        Refresh(false);
    }
    else if (m_transformMode == TransformMode::PlaceRunner)
    {
        m_runnerGhostMousePos = evt.GetPosition();
        Refresh(false);
    }
    else if (m_transformMode == TransformMode::PlaceGate)
    {
        m_gateGhostMousePos = evt.GetPosition();
        Refresh(false);
    }
    else if (m_transformMode == TransformMode::PlaceEjector)
    {
        m_ejectorGhostMousePos = evt.GetPosition();
        Refresh(false);
    }
    else if (m_transformMode == TransformMode::AlignFace ||
        m_transformMode == TransformMode::AlignMidplane)
    {
        // Same deferred-cast pattern as Place* ghosts: store mouse, defer to OnPaint.
        m_alignMousePos = evt.GetPosition();
        Refresh(false);
    }
    else if (m_ventGhostActive || m_runnerGhostActive || m_gateGhostActive ||
        m_ejectorGhostActive)
    {
        m_ventGhostActive = false;
        m_runnerGhostActive = false;
        m_gateGhostActive = false;
        m_ejectorGhostActive = false;
        Refresh(false);
    }

    evt.Skip();
}

void GLCanvas::OnMouseWheel(wxMouseEvent& evt)
{
    const int rot = evt.GetWheelRotation();
    const int delta = evt.GetWheelDelta();
    if (delta == 0) return;
    m_camera.Dolly(float(rot) / float(delta));
    Refresh(false);
}

void GLCanvas::OnKeyDown(wxKeyEvent& evt)
{
    // Preview canvas has no editing state, so none of the shortcuts below
    // (Escape mode-exit, Ctrl+A/C/V, Delete) apply. Let the event propagate
    // so the host frame can still react (e.g. Escape to close).
    if (m_previewMode) { evt.Skip(); return; }

    if (evt.GetKeyCode() == WXK_ESCAPE)
    {
        if (m_transformMode != TransformMode::Select)
        {
            m_ventGhostActive = false;
            m_runnerGhostActive = false;
            m_gateGhostActive = false;
            m_ejectorGhostActive = false;
            SetTransformMode(TransformMode::Select);
            if (auto* frame = dynamic_cast<MainFrame*>(wxGetTopLevelParent(this)))
                frame->SetActiveTool(TransformMode::Select);
        }
    }
    else if (evt.GetKeyCode() == 'A' && evt.ControlDown()
        && m_transformMode == TransformMode::Select)
    {
        // Ctrl+A: select every imported object. Restricted to Select mode
        // so it doesn't fire while the user is mid-placement (vents,
        // runners, gates, etc.) — those modes shouldn't surface a
        // selection state at all.
        m_selectedIndices.clear();
        m_selectedIndices.reserve(m_objects.size());
        for (int i = 0; i < (int)m_objects.size(); ++i)
            m_selectedIndices.push_back(i);
        Refresh(false);
    }
    else if (evt.GetKeyCode() == 'C' && evt.ControlDown()
        && m_transformMode == TransformMode::Select)
    {
        // Ctrl+C: copy current selection into m_clipboard. No-op when
        // nothing is selected. Same Select-mode gate as Ctrl+A so
        // copy/paste can't fire mid-placement.
        CopySelectedToClipboard();
    }
    else if (evt.GetKeyCode() == 'V' && evt.ControlDown()
        && m_transformMode == TransformMode::Select)
    {
        // Ctrl+V: paste clipboard contents at the world origin. No-op
        // when the clipboard is empty.
        PasteFromClipboard();
    }
    else if (evt.GetKeyCode() == WXK_DELETE && !m_selectedIndices.empty())
    {
        // Sort selection in descending order so erasing each entry doesn't
        // shift the indices we still need to delete.
        std::vector<int> toDelete = m_selectedIndices;
        std::sort(toDelete.begin(), toDelete.end(), std::greater<int>());
        for (int idx : toDelete)
        {
            if (idx < 0 || idx >= (int)m_objects.size()) continue;

            // Drop any vents/gates parented to this object, then decrement
            // parentIndex on every surviving feature whose parent index is
            // above idx (those parents just shifted down by one). Doing this
            // inside the descending loop keeps the renumbering local — by
            // the time we reach a smaller idx, indices we already processed
            // (which were higher) have all been erased, so any remaining
            // parentIndex > idx is still valid in the now-shrunk m_objects.
            m_vents.erase(std::remove_if(m_vents.begin(), m_vents.end(),
                [idx](VentInstance& vi) {
                    if (vi.parentIndex == idx) { vi.Destroy(); return true; }
                    return false;
                }), m_vents.end());
            m_gates.erase(std::remove_if(m_gates.begin(), m_gates.end(),
                [idx](GateFeature& gf) {
                    if (gf.parentIndex == idx) { gf.Destroy(); return true; }
                    return false;
                }), m_gates.end());
            for (auto& vi : m_vents) if (vi.parentIndex > idx) --vi.parentIndex;
            for (auto& gf : m_gates) if (gf.parentIndex > idx) --gf.parentIndex;

            m_objects[idx].mesh.Destroy();
            m_objects.erase(m_objects.begin() + idx);
        }
        m_selectedIndices.clear();
        // VBOs that aggregate vent / gate state need refreshing in case
        // we deleted any.
        RebuildPathVBO();
        RebuildCrossSectionVBO();
        RebuildGatePathVBO();
        RebuildGateSolids();
        Refresh(false);
    }
    else
    {
        evt.Skip();
    }
}

// ---------------------------------------------------------------------------
// Copy / Paste (Ctrl+C / Ctrl+V)
//
// Internal-only clipboard — does not touch the OS clipboard. Each entry
// captures the CPU-side mesh and pose of one selected object; paste
// rebuilds GPU resources from scratch via the same normal/crease pipeline
// used at import time, so the new object owns its own VAO/VBO/EBO.
//
// Per spec: rotation, scale, and mirror flags are preserved on paste;
// position is reset to the world origin. Repeated pastes therefore stack
// at (0,0,0); the user is expected to translate them apart afterwards.
// ---------------------------------------------------------------------------
void GLCanvas::CopySelectedToClipboard()
{
    if (m_selectedIndices.empty()) return;

    // Walk the selection in ascending index order so multi-paste preserves
    // the on-screen layering (lowest index first). m_selectedIndices is in
    // click order, which is fine for "primary" semantics but not for
    // deterministic paste ordering — sort a local copy.
    std::vector<int> indices = m_selectedIndices;
    std::sort(indices.begin(), indices.end());

    m_clipboard.clear();
    m_clipboard.reserve(indices.size());
    for (int idx : indices)
    {
        if (idx < 0 || idx >= (int)m_objects.size()) continue;
        const SceneObject& src = m_objects[idx];

        ClipboardEntry e;
        e.cpuVerts = src.cpuVerts;
        e.cpuIndices = src.cpuIndices;
        e.sourcePath = src.sourcePath;
        e.sourceShape = src.sourceShape;
        e.hasSourceShape = src.hasSourceShape;
        e.role = src.role;
        e.yawDeg = src.yawDeg;
        e.pitchDeg = src.pitchDeg;
        e.rollDeg = src.rollDeg;
        e.scale = src.scale;
        e.mirrorX = src.mirrorX;
        e.mirrorZ = src.mirrorZ;
        m_clipboard.push_back(std::move(e));
    }
}

void GLCanvas::PasteFromClipboard()
{
    if (m_clipboard.empty()) return;

    // GL context must be current for UploadMeshToGPU. Mirrors the prelude
    // used by ApplyCircularPattern / ApplyGridPattern.
    SetCurrent(*m_context);
    InitGLOnce();

    // Indices of the new objects, captured as we go. After the loop we
    // make these the new selection so the user immediately sees what
    // they just pasted (and can drag it off the origin without an extra
    // click). m_objects may reallocate during emplace_back, so we record
    // indices rather than pointers.
    std::vector<int> newIndices;
    newIndices.reserve(m_clipboard.size());

    for (const ClipboardEntry& e : m_clipboard)
    {
        // Skip empty meshes defensively — shouldn't occur because copy
        // only stores objects that already imported successfully, but
        // UploadMeshToGPU requires non-empty buffers.
        if (e.cpuVerts.empty() || e.cpuIndices.empty()) continue;

        m_objects.emplace_back();
        SceneObject& obj = m_objects.back();

        obj.role = e.role;
        obj.sourcePath = e.sourcePath;
        obj.sourceShape = e.sourceShape;
        obj.hasSourceShape = e.hasSourceShape;
        obj.cpuVerts = e.cpuVerts;
        obj.cpuIndices = e.cpuIndices;
        // triNeighbors / adjacencyBuilt left at defaults — rebuilt lazily
        // on first use, identically to the original (same convention as
        // pattern operations).

        // Pose: world origin, but keep rotation, scale, and mirror flags.
        obj.pos = glm::vec3(0.0f);
        obj.yawDeg = e.yawDeg;
        obj.pitchDeg = e.pitchDeg;
        obj.rollDeg = e.rollDeg;
        obj.scale = e.scale;
        obj.mirrorX = e.mirrorX;
        obj.mirrorZ = e.mirrorZ;
        // mouldShape / hasMould intentionally left unset — pasted copies
        // are fresh objects without a generated mould (matches pattern-op
        // behavior; the user re-runs Generate Mould as needed).

        // Rebuild GPU mesh from the cached CPU vertices, mirroring the
        // post-import pipeline in ImportFile() (and the pattern ops).
        FileImporter::MeshData md;
        md.vertices = obj.cpuVerts;
        md.indices = obj.cpuIndices;
        ComputeVertexNormals_Pos3(md.vertices, md.indices, md.posNorm);
        auto split = SplitByCreaseAngle_Pos3(md.vertices, md.indices, 35.0f);
        md.posNorm = std::move(split.posNorm);
        md.indices = std::move(split.indices);
        UploadMeshToGPU(md, obj);

        newIndices.push_back((int)m_objects.size() - 1);
    }

    if (!newIndices.empty())
    {
        // Replace selection with the freshly-pasted set. Any prior
        // selection is dropped — feels right for a paste action and
        // matches the conventional behavior of paste in editors that
        // have a notion of selection.
        m_selectedIndices = std::move(newIndices);
        Refresh(false);
    }
}

void GLCanvas::OnResize(wxSizeEvent& evt)
{
    Refresh(false);
    evt.Skip();
}

// ---------------------------------------------------------------------------
// Picking FBO
// ---------------------------------------------------------------------------
void GLCanvas::DestroyPickFBO()
{
    if (m_pickDepthRb) { glDeleteRenderbuffers(1, &m_pickDepthRb); m_pickDepthRb = 0; }
    if (m_pickColorTex) { glDeleteTextures(1, &m_pickColorTex);     m_pickColorTex = 0; }
    if (m_pickFBO) { glDeleteFramebuffers(1, &m_pickFBO);      m_pickFBO = 0; }
    m_pickW = m_pickH = 0;
}

void GLCanvas::EnsurePickFBO(int w, int h)
{
    w = std::max(1, w); h = std::max(1, h);
    if (m_pickFBO && w == m_pickW && h == m_pickH) return;

    DestroyPickFBO();
    m_pickW = w; m_pickH = h;

    glGenFramebuffers(1, &m_pickFBO);
    glBindFramebuffer(GL_FRAMEBUFFER, m_pickFBO);

    glGenTextures(1, &m_pickColorTex);
    glBindTexture(GL_TEXTURE_2D, m_pickColorTex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R32UI, m_pickW, m_pickH, 0,
        GL_RED_INTEGER, GL_UNSIGNED_INT, nullptr);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
        GL_TEXTURE_2D, m_pickColorTex, 0);

    glGenRenderbuffers(1, &m_pickDepthRb);
    glBindRenderbuffer(GL_RENDERBUFFER, m_pickDepthRb);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, m_pickW, m_pickH);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT,
        GL_RENDERBUFFER, m_pickDepthRb);

    const GLenum drawBufs[1] = { GL_COLOR_ATTACHMENT0 };
    glDrawBuffers(1, drawBufs);

    const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    if (status != GL_FRAMEBUFFER_COMPLETE) {
        wxLogError("Picking FBO incomplete (status=0x%X).", (unsigned)status);
        DestroyPickFBO();
    }
}

// Returns scene index of clicked object, or -1 for miss
int GLCanvas::PickObjectAt(int mouseX, int mouseY)
{
    SetCurrent(*m_context);
    InitGLOnce();

    const auto sz = GetClientSize();
    const int w = std::max(1, sz.x);
    const int h = std::max(1, sz.y);
    EnsurePickFBO(w, h);

    if (!m_pickFBO || !m_pickProgram || m_objects.empty()) return -1;

    const int x = mouseX;
    const int y = (m_pickH - 1) - mouseY;
    if (x < 0 || y < 0 || x >= m_pickW || y >= m_pickH) return -1;

    glBindFramebuffer(GL_FRAMEBUFFER, m_pickFBO);
    glViewport(0, 0, m_pickW, m_pickH);

    GLuint clearId = 0;
    glClearBufferuiv(GL_COLOR, 0, &clearId);
    glClear(GL_DEPTH_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);

    glUseProgram(m_pickProgram);
    m_camera.SetAspect(float(w) / float(h));

    // Draw each object with its unique ID
    for (int i = 0; i < (int)m_objects.size(); ++i)
    {
        const SceneObject& obj = m_objects[i];
        if (obj.mesh.vao == 0 || obj.mesh.indexCount == 0) continue;

        const glm::mat4 mvp = m_camera.Projection()
            * m_camera.View()
            * obj.BuildModelMatrix();

        glUniformMatrix4fv(m_pick_uMVP, 1, GL_FALSE, &mvp[0][0]);
        glUniform1ui(m_pick_uObjectId, (uint32_t)(i + 1));   // 1-based

        glBindVertexArray(obj.mesh.vao);
        glDrawElements(GL_TRIANGLES, obj.mesh.indexCount, GL_UNSIGNED_INT, 0);
        glBindVertexArray(0);
    }

    uint32_t id = 0;
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    glReadPixels(x, y, 1, 1, GL_RED_INTEGER, GL_UNSIGNED_INT, &id);

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, w, h);
    glUseProgram(0);

    return (id == 0) ? -1 : (int)(id - 1);
}

void GLCanvas::RenderPickPass_NoRead(int w, int h)
{
    EnsurePickFBO(w, h);
    if (!m_pickFBO || !m_pickProgram || m_objects.empty()) return;

    glBindFramebuffer(GL_FRAMEBUFFER, m_pickFBO);
    glViewport(0, 0, m_pickW, m_pickH);

    GLuint clearId = 0;
    glClearBufferuiv(GL_COLOR, 0, &clearId);
    glClear(GL_DEPTH_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);

    glUseProgram(m_pickProgram);
    m_camera.SetAspect(float(w) / float(h));

    for (int i = 0; i < (int)m_objects.size(); ++i)
    {
        const SceneObject& obj = m_objects[i];
        if (obj.mesh.vao == 0 || obj.mesh.indexCount == 0) continue;

        const glm::mat4 mvp = m_camera.Projection()
            * m_camera.View()
            * obj.BuildModelMatrix();

        glUniformMatrix4fv(m_pick_uMVP, 1, GL_FALSE, &mvp[0][0]);
        glUniform1ui(m_pick_uObjectId, (uint32_t)(i + 1));

        glBindVertexArray(obj.mesh.vao);
        glDrawElements(GL_TRIANGLES, obj.mesh.indexCount, GL_UNSIGNED_INT, 0);
        glBindVertexArray(0);
    }

    glUseProgram(0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, w, h);
}

void GLCanvas::ClearFixtures()
{
    SetCurrent(*m_context);
    for (auto& fix : m_fixtures)
        fix.mesh.Destroy();
    m_fixtures.clear();
    m_fixturePerimeter.clear();
    for (auto& v : m_vents) v.Destroy(); m_vents.clear(); RebuildPathVBO(); RebuildCrossSectionVBO();
    Refresh(false);
}

// ---------------------------------------------------------------------------
// Project restore helpers
// ---------------------------------------------------------------------------

void GLCanvas::ClearAll()
{
    SetCurrent(*m_context);

    // Fixtures
    for (auto& fix : m_fixtures) fix.mesh.Destroy();
    m_fixtures.clear();
    m_fixturePerimeter.clear();

    // Objects
    for (auto& obj : m_objects) obj.mesh.Destroy();
    m_objects.clear();
    m_selectedIndices.clear();

    // Vents
    for (auto& v : m_vents) v.Destroy();
    m_vents.clear();

    // Runners
    for (auto& rf : m_runners) rf.Destroy();
    m_runners.clear();

    // Gates
    for (auto& gf : m_gates) gf.Destroy();
    m_gates.clear();

    // Ejectors
    for (auto& ef : m_ejectors) ef.Destroy();
    m_ejectors.clear();

    // Sprue
    m_sprue.Clear();
    m_sprue.DestroyGL();
    m_hasActiveInjectionPoint = false;
    m_injectionPoints.clear();

    // Ghost state
    m_ventGhostActive = false;
    m_runnerGhostActive = false;
    m_gateGhostActive = false;
    m_ejectorGhostActive = false;
    m_editFeatureIndex = -1;

    // Rebuild all path/cross-section VBOs so stale highlights are cleared
    RebuildPathVBO();
    RebuildCrossSectionVBO();
    RebuildSpruePathVBO();
    RebuildSprueXsecVBO();
    RebuildRunnerPathVBO();
    RebuildGatePathVBO();

    Refresh(false);
}

void GLCanvas::RestoreObject(const std::string& path, const glm::vec3& pos,
    float yaw, float pitch, float roll, float scl,
    bool mirrorX, bool mirrorZ)
{
    // Import the model normally (dispatches on extension, uploads GPU mesh)
    ImportFile(path);

    // Apply the saved transform to the last-added object
    if (!m_objects.empty())
    {
        SceneObject& obj = m_objects.back();
        obj.pos = pos;
        obj.yawDeg = yaw;
        obj.pitchDeg = pitch;
        obj.rollDeg = roll;
        obj.scale = scl;
        obj.mirrorX = mirrorX;
        obj.mirrorZ = mirrorZ;
    }
}

void GLCanvas::RestoreSprue(const ProjectSprueData& data)
{
    SetCurrent(*m_context);

    // Set the injection point so future PlaceSprue calls work
    m_activeInjectionPoint = data.injectionPoint;
    m_hasActiveInjectionPoint = true;

    // Copy placement state
    m_sprue.worldPos = data.worldPos;
    m_sprue.hasPoint = true;
    m_sprue.pathStart = data.pathStart;
    m_sprue.pathEnd = data.pathEnd;
    m_sprue.partingPos = data.partingPos;
    m_sprue.hasPartingPoint = data.hasPartingPoint;
    m_sprue.isDirectInjection = data.isDirectInjection;
    m_sprue.radius = data.radius;
    m_sprue.draftAngleDeg = data.draftAngleDeg;
    m_sprue.coldSlugDepth = data.coldSlugDepth;

    // Build the swept cylinder preview mesh
    m_sprue.solid.Destroy();
    m_sprue.solid = BuildCylinderMesh(m_sprue.pathStart, m_sprue.pathEnd,
        m_sprue.radius, m_sprue.draftAngleDeg);

    // Build cold slug well (same logic as PlaceSprue)
    m_sprue.coldSlugSolid.Destroy();
    if (!m_sprue.isDirectInjection && m_sprue.coldSlugDepth > 1e-6f)
    {
        const glm::vec3 sprueDir = glm::normalize(m_sprue.pathEnd - m_sprue.pathStart);
        const float sprueLen = glm::length(m_sprue.pathEnd - m_sprue.pathStart);
        const float draftRad = glm::radians(glm::clamp(m_sprue.draftAngleDeg, 0.0f, 45.0f));
        const float endRadius = m_sprue.radius + sprueLen * std::tan(draftRad);

        const glm::vec3 slugStart = m_sprue.pathEnd;
        const glm::vec3 slugEnd = m_sprue.pathEnd + sprueDir * m_sprue.coldSlugDepth;
        m_sprue.coldSlugSolid = BuildCylinderMesh(slugStart, slugEnd, endRadius, 0.0f);
    }
}

void GLCanvas::RestoreRunner(const glm::vec3& point)
{
    m_runners.push_back(RunnerFeature{ point, {}, {} });
}

void GLCanvas::RestoreGate(const glm::vec3& pos, const glm::vec3& normal,
    int parentIndex,
    const glm::vec3& localPos,
    const glm::vec3& localNormal)
{
    GateFeature gf;
    gf.point = VentPoint{ pos, normal };
    gf.parentIndex = parentIndex;
    gf.localPos = localPos;
    gf.localNormal = localNormal;
    m_gates.push_back(std::move(gf));
}

void GLCanvas::RestoreVent(const glm::vec3& pos, const glm::vec3& normal,
    float ventWidth, float ventLength,
    float overrunStart, float overrunEnd,
    int parentIndex,
    const glm::vec3& localPos,
    const glm::vec3& localNormal)
{
    SetCurrent(*m_context);

    VentInstance vi;
    vi.point = VentPoint{ pos, normal };
    vi.path = ComputeVentPath(vi.point);
    vi.path.overrunStart = overrunStart;
    vi.path.overrunEnd = overrunEnd;
    vi.crossSection = BuildVentCrossSection(vi.path, ventWidth, ventLength);
    vi.solid = BuildBoxSweepMesh(vi.path, ventWidth, ventLength,
        overrunStart, overrunEnd);
    vi.parentIndex = parentIndex;
    vi.localPos = localPos;
    vi.localNormal = localNormal;
    m_vents.push_back(std::move(vi));
}

void GLCanvas::RestoreEjector(const glm::vec3& point)
{
    // Programmatic placement during project load. Mirrors RestoreGate /
    // RestoreVent — no rebuild here; the caller batches a single
    // RebuildAllFeatures() at the end of the load.
    EjectorFeature ef;
    ef.point = point;
    m_ejectors.push_back(std::move(ef));
}

// ---------------------------------------------------------------------------
// ReanchorVent / ReanchorGate — re-derive a parented feature's world-space
// data from its parent object's current transform plus its stored local
// placement, then rebuild GPU geometry.  No-op when parentIndex is out of
// range (the feature is unparented or its parent has been deleted).
//
// This is what makes vents/gates "stick" to their parent objects:
//   - Apply* transforms call ReanchorFeaturesForObjects on the affected
//     selection, so moving an object drags its features along.
//   - Patterning clones the parented features onto each new clone object
//     and then reanchors so the clone's features land on the clone's
//     surface (mirrored / rotated as appropriate).
//
// Vent geometry is rebuilt with current UI dimensions, matching the
// existing edit-drag convention.  Gate geometry is rebuilt by the global
// RebuildGateSolids pass invoked by callers; this helper only refreshes
// the gate's world point + the gate-path VBO seed.
// ---------------------------------------------------------------------------
void GLCanvas::ReanchorVent(VentInstance& vi)
{
    if (vi.parentIndex < 0 || vi.parentIndex >= (int)m_objects.size())
        return;

    const SceneObject& parent = m_objects[vi.parentIndex];
    const glm::mat4 m = parent.BuildModelMatrix();
    const glm::mat3 normMat = glm::transpose(glm::inverse(glm::mat3(m)));

    // World point + normal from the stored local placement.  Normal is
    // re-normalised because non-uniform scaling (or reflection) could
    // alter its magnitude.
    vi.point.worldPos = glm::vec3(m * glm::vec4(vi.localPos, 1.0f));
    glm::vec3 wn = normMat * vi.localNormal;
    const float wnLen = glm::length(wn);
    vi.point.worldNormal = (wnLen > 1e-6f) ? wn / wnLen : glm::vec3(0, 1, 0);

    // Read current UI dimensions, same convention as edit-drag.
    float ventLength = 5.0f, ventWidth = 2.0f,
        ventOverrunStart = vi.path.overrunStart,
        ventOverrunEnd = vi.path.overrunEnd;
    if (auto* frame = dynamic_cast<MainFrame*>(wxGetTopLevelParent(this)))
        frame->GetVentDimensions(ventLength, ventWidth,
            ventOverrunStart, ventOverrunEnd);

    // Destroy and rebuild the GPU mesh — vi.solid currently references the
    // pre-reanchor geometry which is now stale.
    vi.Destroy();
    vi.path = ComputeVentPath(vi.point);
    vi.path.overrunStart = ventOverrunStart;
    vi.path.overrunEnd = ventOverrunEnd;
    vi.crossSection = BuildVentCrossSection(vi.path, ventWidth, ventLength);
    vi.solid = BuildBoxSweepMesh(vi.path, ventWidth, ventLength,
        ventOverrunStart, ventOverrunEnd);
}

void GLCanvas::ReanchorGate(GateFeature& gf)
{
    if (gf.parentIndex < 0 || gf.parentIndex >= (int)m_objects.size())
        return;

    const SceneObject& parent = m_objects[gf.parentIndex];
    const glm::mat4 m = parent.BuildModelMatrix();
    const glm::mat3 normMat = glm::transpose(glm::inverse(glm::mat3(m)));

    gf.point.worldPos = glm::vec3(m * glm::vec4(gf.localPos, 1.0f));
    glm::vec3 wn = normMat * gf.localNormal;
    const float wnLen = glm::length(wn);
    gf.point.worldNormal = (wnLen > 1e-6f) ? wn / wnLen : glm::vec3(0, 1, 0);

    // Gate solids are rebuilt globally by RebuildGateSolids() (it always
    // reads current UI dimensions and processes every gate), so we just
    // refresh the world point here. Callers invoke RebuildGatePathVBO()
    // and RebuildGateSolids() once after the batch reanchor finishes.
}

void GLCanvas::ReanchorFeaturesForObjects(const std::vector<int>& objIndices)
{
    if (objIndices.empty()) return;

    // Quick membership lookup. Selection sizes are tiny in practice, but a
    // set keeps the per-feature check O(log n) and avoids surprises if the
    // caller passes a large selection (e.g. after Ctrl+A).
    std::set<int> affected(objIndices.begin(), objIndices.end());

    bool touchedVent = false;
    bool touchedGate = false;

    for (auto& vi : m_vents)
    {
        if (vi.parentIndex < 0) continue;
        if (affected.find(vi.parentIndex) == affected.end()) continue;
        ReanchorVent(vi);
        touchedVent = true;
    }
    for (auto& gf : m_gates)
    {
        if (gf.parentIndex < 0) continue;
        if (affected.find(gf.parentIndex) == affected.end()) continue;
        ReanchorGate(gf);
        touchedGate = true;
    }

    if (touchedVent)
    {
        RebuildPathVBO();
        RebuildCrossSectionVBO();
    }
    if (touchedGate)
    {
        RebuildGatePathVBO();
        RebuildGateSolids();
    }
}

void GLCanvas::RebuildAllFeatures()
{
    SetCurrent(*m_context);

    RebuildSpruePathVBO();
    RebuildSprueXsecVBO();
    RebuildPathVBO();
    RebuildCrossSectionVBO();
    RebuildRunnerPathVBO();
    RebuildRunnerSolids();
    RebuildGatePathVBO();
    RebuildGateSolids();
    RebuildEjectorSolids();
    Refresh(false);
}