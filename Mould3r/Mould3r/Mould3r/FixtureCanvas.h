#pragma once

#include <glad/glad.h>
#include <wx/glcanvas.h>

#include <string>

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
// dollies, wheel zooms — so muscle memory carries over. Picking, selection,
// and transform-mode logic don't exist yet; those land alongside the
// toolbar wiring in a follow-up change.
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

private:
    void OnPaint(wxPaintEvent&);
    void OnResize(wxSizeEvent&);
    void OnMouse(wxMouseEvent&);
    void OnMouseWheel(wxMouseEvent&);

    // Lazy GL setup. Called from OnPaint (and from LoadHalf) after
    // SetCurrent so we know a context is bound. m_inited guards re-runs.
    void InitGLOnce();
    void DestroyGL();

    // Compile + link the lit shader program. Idempotent — guards on
    // m_litProgram. Same shader source as the main canvas (shaders.h)
    // so visual treatment of the imported halves matches.
    void EnsureLitProgram();

    // GPU-side state for one fixture half. Position+normal interleaved
    // VBO, indexed triangle EBO, plus the local-space AABB stashed at
    // upload time so framing can include all loaded halves.
    struct FixtureMesh
    {
        GLuint  vao = 0;
        GLuint  vbo = 0;
        GLuint  ebo = 0;
        GLsizei indexCount = 0;
        bool    valid = false;

        glm::vec3 aabbMin{ 0.0f };
        glm::vec3 aabbMax{ 0.0f };

        void Destroy();
    };

    // Move the importer's MeshData (post-normal-compute, post-crease-split)
    // onto the GPU. Mesh src is read-only; dst is replaced in place — any
    // prior GL handles it owned are destroyed first.
    void UploadMesh(const FileImporter::MeshData& src, FixtureMesh& dst);

    // Re-frame the camera to fit the combined AABB of every valid half.
    // No-op when nothing is loaded.
    void FrameLoadedHalves();

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
