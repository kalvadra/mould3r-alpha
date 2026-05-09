#pragma once

#include <glad/glad.h>
#include <wx/glcanvas.h>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "camera.h"
#include "FileImporter.h"
#include "GridRenderer.h"
#include "shaders.h"

// =============================================================================
// FixtureCanvas
//
// 3D viewport for the FixtureEditor. Intentionally separate from the main
// GLCanvas — that class is heavily coupled to MainFrame's TransformMode and
// the full mould-design state model (fixtures, vents, sprues, runners,
// gates, ejectors, picking FBO, outline pass…), none of which apply to
// fixture authoring. This canvas is a focused subset: an OpenGL context,
// an orbit camera, the shared GridRenderer, a lit shader, and storage for
// up to two fixture halves loaded from STEP.
//
// Camera convention matches the main canvas — LMB orbits, MMB pans, RMB
// dollies, wheel zooms — so muscle memory carries over.
//
// Selection / transform model:
//   * Single-half selection (None / A / B). Multi-select isn't meaningful
//     for two-object fixture authoring, and Center on "both halves
//     simultaneously" has no obvious answer.
//   * Picking is a CPU ray-vs-mesh test — for two halves the cost is well
//     below what a picking FBO would buy, and avoiding the FBO keeps this
//     class lean.
//   * Per-half pose (pos, yaw/pitch/roll, scale) lives on FixtureMesh and
//     multiplies through the lit shader's uModel.
//   * Transform mode is just { Select, AlignFace } — Move/Rotate/Scale in
//     the editor are dialog-based (mirroring MainFrame), so they don't
//     need a persistent canvas mode. AlignFace is the only true persistent
//     pick-mode, and ESC drops back to Select.
//
// This canvas does not need (and so doesn't carry) the main GLCanvas's
// outline FBO, picking FBO, multi-select bookkeeping, parented-feature
// reanchoring, or shape-level (BREP) machinery. AlignFace is ported from
// the main canvas as instance-private helpers operating on FixtureMesh.
// =============================================================================
class FixtureCanvas : public wxGLCanvas
{
public:
    explicit FixtureCanvas(wxWindow* parent);
    ~FixtureCanvas() override;

    // No copy/move — wxGLCanvas owns native handles, and the GL context
    // lifecycle is tied to this instance.
    FixtureCanvas(const FixtureCanvas&) = delete;
    FixtureCanvas& operator=(const FixtureCanvas&) = delete;

    // Identifies which fixture half a load operation targets. The two
    // halves are treated symmetrically by this canvas — A is conventionally
    // the "core" side and B the "cavity" side, but nothing here cares
    // which is which.
    enum class HalfSlot { A, B };

    // Load a STEP/STL/OBJ file into the given slot. Replaces any geometry
    // previously occupying that slot. On import failure, shows a message
    // box, clears the slot, and returns false. On success returns true and
    // re-frames the camera to fit the combined extents of both halves so
    // small or off-origin imports remain visible.
    //
    // GL context is made current internally; safe to call before the
    // canvas has had its first paint.
    bool LoadHalf(HalfSlot slot, const std::string& path);

    // ---- Selection / transform mode ----------------------------------------
    // Subset of TransformMode from MainFrame.h. Move/Rotate/Scale are dialog-
    // driven in the fixture editor (matching MainFrame's UX), so they need no
    // persistent mode of their own. AlignFace is the only persistent picking
    // mode here.
    enum class TransformMode { Select, AlignFace };

    void SetTransformMode(TransformMode m);
    TransformMode GetTransformMode() const { return m_transformMode; }

    // True when exactly one half is currently selected. Mirrors
    // GLCanvas::HasSelection(); same role — gates the dialog handlers in
    // FixtureEditor so they no-op silently rather than applying to nothing.
    bool HasSelection() const { return m_selectedHalf != Selection::None; }

    // Apply transforms to the currently selected half. No-op when nothing
    // is selected. Same semantics as the GLCanvas equivalents:
    //   * ApplyTranslation: additive on pos.
    //   * ApplyRotation:    additive on Euler angles (YXZ order).
    //   * ApplyScale:       multiplicative, with a 0.001 floor.
    //   * CenterSelected:   sets pos to the world origin (degenerate case
    //                       of GLCanvas::CenterSelectedObject for a one-
    //                       object selection — no centroid math needed).
    void ApplyTranslation(float x, float y, float z);
    void ApplyRotation(float xDeg, float yDeg, float zDeg);
    void ApplyScale(float factor);
    void CenterSelected();

private:
    void OnPaint(wxPaintEvent&);
    void OnResize(wxSizeEvent&);
    void OnMouse(wxMouseEvent&);
    void OnMouseWheel(wxMouseEvent&);
    // ESC drops AlignFace mode back to Select and notifies the parent
    // FixtureEditor so the toolbar toggle visual stays in sync. Same
    // pattern GLCanvas uses to call MainFrame::SetActiveTool from its
    // own ESC handler.
    void OnKeyDown(wxKeyEvent&);

    // Lazy GL setup. Called from OnPaint (and from LoadHalf) after
    // SetCurrent so we know a context is bound. m_inited guards re-runs.
    void InitGLOnce();
    void DestroyGL();

    // Compile + link the lit shader program. Idempotent — guards on
    // m_litProgram. Same shader source as the main canvas (shaders.h)
    // so visual treatment of the imported halves matches.
    void EnsureLitProgram();

    // Compile + link the unlit/flat program used by the AlignFace highlight
    // overlay. Same shader source as the main canvas's m_flatProgram, just
    // owned locally so this class doesn't depend on GLCanvas being alive.
    // Idempotent — guards on m_flatProgram.
    void EnsureFlatProgram();

    // GPU- and CPU-side state for one fixture half.
    //
    // GPU buffers store a position+normal interleaved VBO + indexed EBO,
    // matching the shared lit shader's attribute layout (location 0 =
    // position, location 1 = normal). Same convention as the main canvas's
    // GPUMesh.
    //
    // CPU buffers (cpuVerts/cpuIndices) keep a position-only mirror of the
    // pre-crease-split geometry — same role as SceneObject::cpuVerts on the
    // main canvas. They power the ray-vs-mesh pick (Select mode) and the
    // adjacency / region-grow pipeline (AlignFace). Cheap to retain (one
    // float per component, one uint per index), and avoids a CPU readback
    // every time we want to ray-cast.
    //
    // Adjacency (triNeighbors) is built lazily on first AlignFace use, the
    // same way SceneObject::triNeighbors is — local mesh topology never
    // changes after import, so the build amortises across every alignment
    // attempt on the same half.
    //
    // Pose fields and BuildModelMatrix mirror SceneObject's layout exactly
    // (YXZ Euler, uniform scale) so AlignFace's plane-alignment math —
    // ported from the main canvas — works against the same conventions.
    // No mirror flags here: those are pattern-tool concerns, and the
    // fixture editor has no patterning.
    struct FixtureMesh
    {
        // GPU
        GLuint  vao = 0;
        GLuint  vbo = 0;
        GLuint  ebo = 0;
        GLsizei indexCount = 0;
        bool    valid = false;

        glm::vec3 aabbMin{ 0.0f };
        glm::vec3 aabbMax{ 0.0f };

        // CPU mirror (position-only, object space) — populated by LoadHalf.
        std::vector<float>    cpuVerts;
        std::vector<uint32_t> cpuIndices;

        // Per-tri edge adjacency for AlignFace's BFS. Built on demand by
        // EnsureTriAdjacency. triNeighbors[t][k] = the triangle that shares
        // edge k of triangle t (k between local vertex k and (k+1)%3), or
        // -1 if none. Identical layout to SceneObject::triNeighbors so the
        // same BFS code ports across unchanged.
        std::vector<std::array<int, 3>> triNeighbors;
        bool                            adjacencyBuilt = false;

        // Pose (YXZ Euler, uniform scale). Default = identity.
        glm::vec3 pos{ 0.0f };
        float     yawDeg = 0.0f;
        float     pitchDeg = 0.0f;
        float     rollDeg = 0.0f;
        float     scale = 1.0f;

        // T * Ry(yaw) * Rx(pitch) * Rz(roll) * S(scale). YXZ order matches
        // SceneObject::BuildModelMatrix and the YXZ decomposition used by
        // the AlignFace alignment math.
        glm::mat4 BuildModelMatrix() const;

        void Destroy();
    };

    // Move the importer's MeshData (post-normal-compute, post-crease-split)
    // onto the GPU. Mesh src is read-only; dst is replaced in place — any
    // prior GL handles it owned are destroyed first.
    void UploadMesh(const FileImporter::MeshData& src, FixtureMesh& dst);

    // Re-frame the camera to fit the combined AABB of every valid half.
    // No-op when nothing is loaded.
    void FrameLoadedHalves();

    // ---- Selection helpers -------------------------------------------------
    // Return the FixtureMesh corresponding to m_selectedHalf, or nullptr
    // when no half is selected. Both overloads behave the same; the
    // const overload exists so const-context callers don't have to cast.
    FixtureMesh* SelectedMesh();
    const FixtureMesh* SelectedMesh() const;

    // Single-half pick. Returns 0 for half A, 1 for half B, or -1 on miss.
    // CPU ray-vs-mesh test against each half's cpuVerts/cpuIndices in its
    // own local space (model matrix inverted onto the ray). For two halves
    // the per-frame cost is negligible compared to standing up a picking
    // FBO, which is why we don't have one here.
    int PickHalf(int mouseX, int mouseY);

    // ---- AlignFace pipeline (ported from GLCanvas) -------------------------
    // Same shape as GLCanvas's RayCastFacePick / EnsureTriAdjacency /
    // GrowCoplanarFace / ApplyPlaneAlignmentToObject / ApplyAlignFaceToObject,
    // but operating on FixtureMesh in place of SceneObject. Comments here
    // are deliberately terse — see the originals in GLCanvas.cpp for the
    // detailed rationale on each step (cosine tolerance, manifold-edge
    // pairing, smallest-rotation target on ±Y, etc.).

    // Like PickHalf but also returns the hit triangle index.
    bool RayCastFacePick(int mouseX, int mouseY, int& outHalf, int& outTri);

    // Build edge → neighbour-tri map on first AlignFace use, then never again
    // for the lifetime of the half (topology is immutable post-import).
    void EnsureTriAdjacency(FixtureMesh& m);

    // BFS from seedTri across edge-shared neighbours within ~1° of the seed
    // normal (same kCosTol = 0.99985 the main canvas uses).
    void GrowCoplanarFace(const FixtureMesh& m, int seedTri,
        std::vector<uint32_t>& outTris,
        glm::vec3& outNormalLocal);

    // Snap a local-space plane (normal + anchor on the plane) onto world Y=0.
    // Anchor is held fixed laterally (X/Z); only Y and the rotation change.
    void ApplyPlaneAlignmentToHalf(FixtureMesh& m,
        const glm::vec3& planeNormalLocal,
        const glm::vec3& anchorLocal);

    // Compute the face's local-space centroid and delegate to
    // ApplyPlaneAlignmentToHalf.
    void ApplyAlignFaceToHalf(FixtureMesh& m, const glm::vec3& nLocal,
        const std::vector<uint32_t>& faceTris);

    // Upload the highlighted face's triangles to the overlay VBO with the
    // half's current model matrix baked in (so the overlay doesn't need its
    // own uModel). Pass an empty tri list to clear.
    void RebuildAlignHighlightVBO(const FixtureMesh& m,
        const std::vector<uint32_t>& tris);

    wxGLContext* m_context = nullptr;
    bool         m_inited = false;

    OrbitCamera  m_camera;
    GridRenderer m_grid;

    // Lit program — compiled lazily by EnsureLitProgram on first load /
    // first paint. Cached uniform locations let the per-frame uniform
    // sets in OnPaint stay terse.
    shaders m_shaders;
    GLuint  m_litProgram = 0;
    GLint   m_uModel = -1;
    GLint   m_uView = -1;
    GLint   m_uProj = -1;
    GLint   m_uCameraPos = -1;
    GLint   m_uLightDir = -1;
    GLint   m_uLightColor = -1;
    GLint   m_uBaseColor = -1;
    GLint   m_uAlpha = -1;
    GLint   m_uAmbient = -1;
    GLint   m_uDiffuse = -1;
    GLint   m_uSpecular = -1;
    GLint   m_uShininess = -1;

    // Up to one mesh per half slot. Either may be empty (.valid == false)
    // until the corresponding Import button has been used.
    FixtureMesh m_meshA;
    FixtureMesh m_meshB;

    // ---- Selection / transform mode ---------------------------------------
    // None / A / B — at most one half is selected at a time. Internal-only
    // counterpart to HalfSlot; Selection::None lets the same enum stand in
    // for "nothing selected" without an extra has_value bool. The public
    // API never exposes this enum directly; HasSelection() is the surface.
    enum class Selection { None, A, B };
    Selection     m_selectedHalf = Selection::None;
    TransformMode m_transformMode = TransformMode::Select;

    // ---- AlignFace hover state --------------------------------------------
    // Same deferred-cast pattern the main canvas uses: motion events stash
    // the mouse position, OnPaint runs at most one ray-cast per frame and
    // grows the region only when the hover changed. Coalesces a burst of
    // wxEVT_MOTION events into one cast.
    wxPoint               m_alignMousePos;
    int                   m_alignHoverHalf = -1;     // 0=A, 1=B, -1=miss
    int                   m_alignSeedTri = -1;       // last grown seed
    std::vector<uint32_t> m_alignFaceTris;           // tris in current hover region
    glm::vec3             m_alignFaceNormalLocal{ 0.0f };

    // GPU resources for the dark-grey hover overlay. World-space verts
    // (model matrix baked in at upload time) so the draw doesn't need a
    // per-half uModel uniform — same approach as GLCanvas's m_alignHighlight*.
    GLuint  m_alignHighlightVAO = 0;
    GLuint  m_alignHighlightVBO = 0;
    GLsizei m_alignHighlightVertexCount = 0;

    // Flat (unlit) program for the hover overlay. Shader source is shared
    // with the main canvas via shaders.h; uniform locations cached on link.
    GLuint m_flatProgram = 0;
    GLint  m_flat_uVP = -1;
    GLint  m_flat_uColor = -1;

    // Mouse state for camera controls. Tracked the same way the main
    // GLCanvas does — m_hasLast=false on each button-down so the first
    // motion frame just records the anchor position rather than emitting
    // a spurious large delta.
    bool    m_lmb = false;
    bool    m_mmb = false;
    bool    m_rmb = false;
    bool    m_hasLast = false;
    wxPoint m_lastPos;
};
