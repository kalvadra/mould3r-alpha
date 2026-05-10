#include "FixtureCanvas.h"

#include <wx/dcclient.h>
#include <wx/log.h>
#include <wx/msgdlg.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <unordered_map>
#include <utility>

#include <glm/gtc/matrix_transform.hpp>

// Pulled in for the ESC-key callback into the parent FixtureEditor's
// SetActiveTool. Same MainFrame-lookup pattern GLCanvas uses for its own
// Escape handler.
#include "FixtureEditor.h"

#include "GLLoader.h"
#include "MeshUtils.h"   // ComputeVertexNormals_Pos3
#include "MeshOps.h"     // SplitByCreaseAngle_Pos3

// glArgs[] and GetAnyGLFuncAddress() now live in GLLoader.h/.cpp — shared
// with the main GLCanvas. Adding a third viewport will reuse the same
// helpers without forking either.

namespace
{
    // Local Compile/Link helpers — same approach the main GLCanvas uses,
    // intentionally kept file-static here rather than promoted to a shared
    // header (the helpers exist in GLCanvas.cpp too; if a third caller
    // appears, lifting all three copies into a GLShaderUtils.h is an easy
    // sweep).
    GLuint Compile(GLenum type, const char* src)
    {
        GLuint s = glCreateShader(type);
        glShaderSource(s, 1, &src, nullptr);
        glCompileShader(s);

        GLint ok = 0;
        glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
        if (!ok)
        {
            GLint len = 0;
            glGetShaderiv(s, GL_INFO_LOG_LENGTH, &len);
            std::string log(len, '\0');
            glGetShaderInfoLog(s, len, &len, log.data());
            wxLogError("FixtureCanvas shader compile failed: %s",
                wxString::FromUTF8(log));
            glDeleteShader(s);
            return 0;
        }
        return s;
    }

    GLuint Link(GLuint vs, GLuint fs)
    {
        GLuint p = glCreateProgram();
        glAttachShader(p, vs);
        glAttachShader(p, fs);
        glLinkProgram(p);

        GLint ok = 0;
        glGetProgramiv(p, GL_LINK_STATUS, &ok);
        glDeleteShader(vs);
        glDeleteShader(fs);
        if (!ok)
        {
            GLint len = 0;
            glGetProgramiv(p, GL_INFO_LOG_LENGTH, &len);
            std::string log(len, '\0');
            glGetProgramInfoLog(p, len, &len, log.data());
            wxLogError("FixtureCanvas program link failed: %s",
                wxString::FromUTF8(log));
            glDeleteProgram(p);
            return 0;
        }
        return p;
    }

    // Möller–Trumbore ray-triangle test. Front-face only (rejects a < EPS),
    // matching GLCanvas's RayTriangle so opaque cavity geometry doesn't
    // produce hits on its far back-face. Direct port — see GLCanvas.cpp for
    // the rationale on the front-face-only choice.
    bool RayTriangle(const glm::vec3& orig, const glm::vec3& dir,
        const glm::vec3& v0, const glm::vec3& v1, const glm::vec3& v2,
        float& outT)
    {
        constexpr float EPS = 1e-7f;
        const glm::vec3 e1 = v1 - v0;
        const glm::vec3 e2 = v2 - v0;
        const glm::vec3 h = glm::cross(dir, e2);
        const float     a = glm::dot(e1, h);
        if (a < EPS) return false;
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

    // Extract YXZ Euler angles from a rotation matrix R that was built as
    // Ry(yaw) * Rx(pitch) * Rz(roll). Direct port of GLCanvas::DecomposeYXZ
    // — same convention is mandatory because AlignFace below feeds the
    // result back into FixtureMesh's pose, whose BuildModelMatrix uses YXZ.
    // Gimbal-lock convention (cos(pitch) ≈ 0): pin roll = 0, recover yaw
    // from the X column.
    void DecomposeYXZ(const glm::mat3& R,
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
            roll = 0.0f;
            yaw = std::atan2(-R[0][2], R[0][0]);
        }

        yawDeg = glm::degrees(yaw);
        pitchDeg = glm::degrees(pitch);
        rollDeg = glm::degrees(roll);
    }
}

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------
FixtureCanvas::FixtureCanvas(wxWindow* parent)
    : wxGLCanvas(parent, wxID_ANY, GLLoader::glArgs,
        wxDefaultPosition, wxDefaultSize,
        wxFULL_REPAINT_ON_RESIZE, "FixtureCanvas")
{
#if WX_CHECK_VERSION(3, 1, 0)
    wxGLContextAttrs ctxAttrs;
#if defined(__APPLE__)
    ctxAttrs.PlatformDefaults().CoreProfile().OGLVersion(3, 2).EndList();
#else
    ctxAttrs.PlatformDefaults().CoreProfile().OGLVersion(3, 3).EndList();
#endif
    m_context = new wxGLContext(this, nullptr, &ctxAttrs);
    if (!m_context->IsOK())
    {
        // Fall back to the default context — same defensive recovery the
        // main canvas does. The grid shader only needs 3.3 features, so
        // this fallback may yield a non-functional viewport; better than
        // crashing on context creation.
        delete m_context;
        m_context = new wxGLContext(this);
    }
#else
    m_context = new wxGLContext(this);
#endif

    // Match the main canvas's interaction feel so the user's muscle memory
    // carries over between the two viewports.
    m_camera.SetOrbitSensitivity(0.15f);
    m_camera.SetPanSensitivity(0.0025f);
    m_camera.SetDollySensitivity(0.08f);

    Bind(wxEVT_PAINT, &FixtureCanvas::OnPaint, this);
    Bind(wxEVT_SIZE, &FixtureCanvas::OnResize, this);
    Bind(wxEVT_LEFT_DOWN, &FixtureCanvas::OnMouse, this);
    Bind(wxEVT_LEFT_UP, &FixtureCanvas::OnMouse, this);
    Bind(wxEVT_MIDDLE_DOWN, &FixtureCanvas::OnMouse, this);
    Bind(wxEVT_MIDDLE_UP, &FixtureCanvas::OnMouse, this);
    Bind(wxEVT_RIGHT_DOWN, &FixtureCanvas::OnMouse, this);
    Bind(wxEVT_RIGHT_UP, &FixtureCanvas::OnMouse, this);
    Bind(wxEVT_MOTION, &FixtureCanvas::OnMouse, this);
    Bind(wxEVT_MOUSEWHEEL, &FixtureCanvas::OnMouseWheel, this);
    // ESC drops AlignFace mode back to Select (mirrors GLCanvas::OnKeyDown).
    // wxGLCanvas accepts focus on its own when clicked, so the handler
    // fires once the user has interacted with the viewport — same behaviour
    // as the main canvas.
    Bind(wxEVT_KEY_DOWN, &FixtureCanvas::OnKeyDown, this);
}

FixtureCanvas::~FixtureCanvas()
{
    if (m_context)
    {
        SetCurrent(*m_context);
        DestroyGL();
    }
    delete m_context;
}

// ---------------------------------------------------------------------------
// GL setup / teardown
// ---------------------------------------------------------------------------
void FixtureCanvas::InitGLOnce()
{
    if (m_inited) return;
    SetCurrent(*m_context);

    // glad's function pointers are global. If MainFrame's GLCanvas already
    // populated them, calling again is a harmless re-populate; if this
    // canvas runs first (the common case — the editor opens before the
    // main frame ever exists), we own initial loading. Either way, the
    // pointers obtained against a 3.3 core context here remain valid for
    // the main canvas's 3.3 core context too on Windows.
    if (!gladLoadGLLoader((GLADloadproc)GLLoader::GetAnyGLFuncAddress))
        return;

    glEnable(GL_DEPTH_TEST);
    glEnable(GL_MULTISAMPLE);

    m_inited = true;
}

void FixtureCanvas::DestroyGL()
{
    m_meshA.Destroy();
    m_meshB.Destroy();
    if (m_litProgram) { glDeleteProgram(m_litProgram); m_litProgram = 0; }
    if (m_flatProgram) { glDeleteProgram(m_flatProgram); m_flatProgram = 0; }
    if (m_alignHighlightVBO) { glDeleteBuffers(1, &m_alignHighlightVBO); m_alignHighlightVBO = 0; }
    if (m_alignHighlightVAO) { glDeleteVertexArrays(1, &m_alignHighlightVAO); m_alignHighlightVAO = 0; }
    m_alignHighlightVertexCount = 0;
    m_grid.Destroy();
    m_inited = false;
}

// ---------------------------------------------------------------------------
// FixtureMesh
// ---------------------------------------------------------------------------
void FixtureCanvas::FixtureMesh::Destroy()
{
    if (ebo) { glDeleteBuffers(1, &ebo); ebo = 0; }
    if (vbo) { glDeleteBuffers(1, &vbo); vbo = 0; }
    if (vao) { glDeleteVertexArrays(1, &vao); vao = 0; }
    indexCount = 0;
    valid = false;
    aabbMin = glm::vec3(0.0f);
    aabbMax = glm::vec3(0.0f);

    // Reset CPU mirror + adjacency. Re-importing into a slot replaces the
    // geometry wholesale — keeping stale CPU verts or a half-built
    // neighbour map would corrupt picking/AlignFace against the new mesh.
    cpuVerts.clear();
    cpuVerts.shrink_to_fit();
    cpuIndices.clear();
    cpuIndices.shrink_to_fit();
    triNeighbors.clear();
    triNeighbors.shrink_to_fit();
    adjacencyBuilt = false;

    // Reset pose. Re-importing should not preserve the previous half's
    // transform — that pose was derived from different geometry, and
    // applying it to a fresh import is rarely what the user wants.
    pos = glm::vec3(0.0f);
    yawDeg = 0.0f;
    pitchDeg = 0.0f;
    rollDeg = 0.0f;
    scale = 1.0f;
}

glm::mat4 FixtureCanvas::FixtureMesh::BuildModelMatrix() const
{
    // YXZ Euler order — must match SceneObject::BuildModelMatrix exactly so
    // the AlignFace math (which composes a delta rotation against the
    // existing model's R then decomposes back via DecomposeYXZ) round-trips
    // cleanly.
    glm::mat4 T = glm::translate(glm::mat4(1.0f), pos);
    glm::mat4 RY = glm::rotate(glm::mat4(1.0f), glm::radians(yawDeg), glm::vec3(0, 1, 0));
    glm::mat4 RX = glm::rotate(glm::mat4(1.0f), glm::radians(pitchDeg), glm::vec3(1, 0, 0));
    glm::mat4 RZ = glm::rotate(glm::mat4(1.0f), glm::radians(rollDeg), glm::vec3(0, 0, 1));
    glm::mat4 S = glm::scale(glm::mat4(1.0f), glm::vec3(scale));
    return T * RY * RX * RZ * S;
}

// ---------------------------------------------------------------------------
// Lit program — compiled lazily on first use. Mirrors the main canvas's
// shader setup so an imported half looks the same in both viewports.
// ---------------------------------------------------------------------------
void FixtureCanvas::EnsureLitProgram()
{
    if (m_litProgram) return;

    GLuint vs = Compile(GL_VERTEX_SHADER, m_shaders.vsLit);
    GLuint fs = Compile(GL_FRAGMENT_SHADER, m_shaders.fsLit);
    if (!vs || !fs) return;
    m_litProgram = Link(vs, fs);
    if (!m_litProgram) return;

    // Cache uniform locations once — a -1 here just means the uniform was
    // optimised out, which is fine; glUniform* on -1 is a documented no-op.
    m_uModel = glGetUniformLocation(m_litProgram, "uModel");
    m_uView = glGetUniformLocation(m_litProgram, "uView");
    m_uProj = glGetUniformLocation(m_litProgram, "uProj");
    m_uCameraPos = glGetUniformLocation(m_litProgram, "uCameraPos");
    m_uLightDir = glGetUniformLocation(m_litProgram, "uLightDir");
    m_uLightColor = glGetUniformLocation(m_litProgram, "uLightColor");
    m_uBaseColor = glGetUniformLocation(m_litProgram, "uBaseColor");
    m_uAlpha = glGetUniformLocation(m_litProgram, "uAlpha");
    m_uAmbient = glGetUniformLocation(m_litProgram, "uAmbient");
    m_uDiffuse = glGetUniformLocation(m_litProgram, "uDiffuse");
    m_uSpecular = glGetUniformLocation(m_litProgram, "uSpecular");
    m_uShininess = glGetUniformLocation(m_litProgram, "uShininess");
}

// ---------------------------------------------------------------------------
// Flat program — used by the AlignFace hover overlay. Compiled lazily on
// first paint that actually needs it (i.e. first time the user enters
// AlignFace mode and hovers a face). Source comes from the same shaders.h
// the main canvas uses, so the look matches.
// ---------------------------------------------------------------------------
void FixtureCanvas::EnsureFlatProgram()
{
    if (m_flatProgram) return;

    GLuint vs = Compile(GL_VERTEX_SHADER, m_shaders.vsFlat);
    GLuint fs = Compile(GL_FRAGMENT_SHADER, m_shaders.fsFlat);
    if (!vs || !fs) return;
    m_flatProgram = Link(vs, fs);
    if (!m_flatProgram) return;

    m_flat_uVP = glGetUniformLocation(m_flatProgram, "uVP");
    m_flat_uColor = glGetUniformLocation(m_flatProgram, "uColor");
}

// ---------------------------------------------------------------------------
// UploadMesh — interleaved position+normal VBO, indexed EBO. Same layout
// the main canvas uses (location 0 = pos, location 1 = normal), so the
// shared lit shader binds correctly.
// ---------------------------------------------------------------------------
void FixtureCanvas::UploadMesh(const FileImporter::MeshData& src,
    FixtureMesh& dst)
{
    // Defensive: the main canvas's pipeline always populates posNorm
    // (ComputeVertexNormals_Pos3 + SplitByCreaseAngle_Pos3 produce one);
    // bail without trashing the slot if somehow we got handed bare
    // positions instead. Result: the previous geometry is kept, which is
    // less surprising than a silently-empty viewport.
    if (src.posNorm.empty() || src.indices.empty()) return;
    if ((src.posNorm.size() % 6) != 0) return;

    dst.Destroy();   // free any prior GL handles before re-creating

    glGenVertexArrays(1, &dst.vao);
    glGenBuffers(1, &dst.vbo);
    glGenBuffers(1, &dst.ebo);

    glBindVertexArray(dst.vao);

    glBindBuffer(GL_ARRAY_BUFFER, dst.vbo);
    glBufferData(GL_ARRAY_BUFFER,
        (GLsizeiptr)(src.posNorm.size() * sizeof(float)),
        src.posNorm.data(), GL_STATIC_DRAW);

    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, dst.ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER,
        (GLsizeiptr)(src.indices.size() * sizeof(uint32_t)),
        src.indices.data(), GL_STATIC_DRAW);

    // location 0 — position, location 1 — normal. Stride = 6 floats.
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE,
        6 * (GLsizei)sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE,
        6 * (GLsizei)sizeof(float), (void*)(3 * sizeof(float)));
    glEnableVertexAttribArray(1);

    glBindVertexArray(0);

    dst.indexCount = (GLsizei)src.indices.size();
    dst.valid = true;
    dst.aabbMin = src.aabbMin;
    dst.aabbMax = src.aabbMax;
}

// ---------------------------------------------------------------------------
// LoadHalf — public entry point used by FixtureEditor's Import buttons.
// ---------------------------------------------------------------------------
bool FixtureCanvas::LoadHalf(HalfSlot slot, const std::string& path)
{
    SetCurrent(*m_context);
    InitGLOnce();
    EnsureLitProgram();

    FixtureMesh& target = (slot == HalfSlot::A) ? m_meshA : m_meshB;

    // Use the shared importer — same path the main GLCanvas uses for its
    // ImportFile pipeline. ImportAuto dispatches on file extension; the
    // FixtureEditor's picker filters to .step/.stp, but ImportAuto
    // gracefully handles the case where someone drags an STL or OBJ in
    // through a future drag-and-drop hookup.
    FileImporter importer;
    auto res = importer.ImportAuto(path, 0.05, 0.5);
    if (!res.ok())
    {
        wxMessageBox(res.error, "Import failed", wxOK | wxICON_ERROR, this);
        target.Destroy();
        Refresh(false);
        return false;
    }
    if (res.meshes.empty())
    {
        wxMessageBox("Imported file contained no usable geometry.",
            "Import Failed", wxOK | wxICON_WARNING, this);
        target.Destroy();
        Refresh(false);
        return false;
    }

    // Same post-import processing the main canvas does for SceneObjects:
    // vertex normals, then a crease split so hard edges shade crisply
    // instead of smearing across them. We snapshot the position-only
    // geometry (cpuVerts/cpuIndices) BEFORE the crease split, since that
    // step duplicates vertices and rewrites the index buffer — the
    // pre-split mesh is what we want for ray-vs-mesh picking and for the
    // adjacency map used by AlignFace's BFS.
    auto& mesh = res.meshes[0];
    std::vector<float>    cpuVerts = mesh.vertices;
    std::vector<uint32_t> cpuIndices = mesh.indices;

    ComputeVertexNormals_Pos3(mesh.vertices, mesh.indices, mesh.posNorm);
    auto split = SplitByCreaseAngle_Pos3(mesh.vertices, mesh.indices, 35.0f);
    mesh.posNorm = std::move(split.posNorm);
    mesh.indices = std::move(split.indices);

    UploadMesh(mesh, target);

    // Stash the pre-split CPU buffers on the slot. UploadMesh's call to
    // target.Destroy() above already cleared any prior CPU mirror; we
    // populate the fresh copy here.
    target.cpuVerts = std::move(cpuVerts);
    target.cpuIndices = std::move(cpuIndices);

    // Re-importing into a slot invalidates any selection/hover that
    // pointed at the old geometry. Cheap to clear unconditionally rather
    // than try to be clever about which slot was just replaced.
    if ((slot == HalfSlot::A && m_selectedHalf == Selection::A) ||
        (slot == HalfSlot::B && m_selectedHalf == Selection::B))
    {
        m_selectedHalf = Selection::None;
    }
    m_alignHoverHalf = -1;
    m_alignSeedTri = -1;
    m_alignFaceTris.clear();
    m_alignHighlightVertexCount = 0;

    // Frame the camera around everything that's loaded so a single import
    // doesn't end up off-screen, and so loading B doesn't tunnel the user
    // away from A.
    FrameLoadedHalves();

    Refresh(false);
    return true;
}

// ---------------------------------------------------------------------------
// FrameLoadedHalves — fit camera to the union of every valid half's AABB.
// ---------------------------------------------------------------------------
void FixtureCanvas::FrameLoadedHalves()
{
    glm::vec3 mn(0.0f), mx(0.0f);
    bool any = false;
    for (const FixtureMesh* m : { &m_meshA, &m_meshB })
    {
        if (!m->valid) continue;
        if (!any)
        {
            mn = m->aabbMin;
            mx = m->aabbMax;
            any = true;
        }
        else
        {
            mn = glm::min(mn, m->aabbMin);
            mx = glm::max(mx, m->aabbMax);
        }
    }
    if (!any) return;

    const glm::vec3 center = 0.5f * (mn + mx);
    const float radius = 0.5f * glm::length(mx - mn);
    // Tiny floor on radius — degenerate AABBs (a single point, or a flat
    // 2D shape) would otherwise frame to ~0 distance and clip.
    m_camera.FrameSphere(center, std::max(radius, 1.0f));
}

// ---------------------------------------------------------------------------
// Paint
// ---------------------------------------------------------------------------
void FixtureCanvas::OnPaint(wxPaintEvent&)
{
    // wxPaintDC is required even when we're rendering with GL — wxWidgets
    // uses its presence to decide a paint event has been validated.
    wxPaintDC dc(this);
    SetCurrent(*m_context);
    InitGLOnce();
    EnsureLitProgram();
    m_grid.Init();   // safe to call repeatedly; internal m_ready guards setup

    const wxSize sz = GetClientSize();
    const int w = std::max(1, sz.x);
    const int h = std::max(1, sz.y);

    glViewport(0, 0, w, h);

    glEnable(GL_DEPTH_TEST);
    // Same clear color as the main canvas (0.12, 0.14, 0.18) so the two
    // viewports read as the same scene from a colour-sampling standpoint.
    glClearColor(0.12f, 0.14f, 0.18f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    m_camera.SetAspect(float(w) / float(h));
    const glm::mat4 view = m_camera.View();
    const glm::mat4 proj = m_camera.Projection();
    const glm::vec3 camPos = m_camera.Position();

    // ---- Lit pass: imported halves --------------------------------------
    // Each half renders with its own model matrix, picking up any pose
    // changes the toolbar applied. The selected half (if any) gets a warm
    // yellow-tinted base colour as the selection indicator — keeps the
    // class lean (no outline FBO needed) while still being unambiguous.
    if (m_litProgram)
    {
        glUseProgram(m_litProgram);

        // Lighting / material — match the main canvas's defaults so an
        // imported half looks identical between the two viewports.
        const glm::vec3 lightDir = glm::normalize(glm::vec3(0.4f, 0.8f, 0.2f));
        const glm::vec3 lightColor = glm::vec3(1.0f);
        const glm::vec3 unselectedColor = glm::vec3(0.80f, 0.80f, 0.85f);
        const glm::vec3 selectedColor = glm::vec3(1.00f, 0.85f, 0.50f);

        glUniformMatrix4fv(m_uView, 1, GL_FALSE, &view[0][0]);
        glUniformMatrix4fv(m_uProj, 1, GL_FALSE, &proj[0][0]);
        glUniform3fv(m_uCameraPos, 1, &camPos[0]);
        glUniform3fv(m_uLightDir, 1, &lightDir[0]);
        glUniform3fv(m_uLightColor, 1, &lightColor[0]);
        glUniform1f(m_uAlpha, 1.0f);
        glUniform1f(m_uAmbient, 0.25f);
        glUniform1f(m_uDiffuse, 0.85f);
        glUniform1f(m_uSpecular, 0.20f);
        glUniform1f(m_uShininess, 64.0f);

        const FixtureMesh* halves[2] = { &m_meshA, &m_meshB };
        const Selection    halfTags[2] = { Selection::A, Selection::B };
        for (int i = 0; i < 2; ++i)
        {
            const FixtureMesh* m = halves[i];
            if (!m->valid || m->indexCount == 0) continue;

            const bool selected = (m_selectedHalf == halfTags[i]);
            glUniform3fv(m_uBaseColor, 1,
                selected ? &selectedColor[0] : &unselectedColor[0]);

            const glm::mat4 model = m->BuildModelMatrix();
            glUniformMatrix4fv(m_uModel, 1, GL_FALSE, &model[0][0]);

            glBindVertexArray(m->vao);
            glDrawElements(GL_TRIANGLES, m->indexCount, GL_UNSIGNED_INT, 0);
        }
        glBindVertexArray(0);
        glUseProgram(0);
    }

    // ---- AlignFace hover highlight --------------------------------------
    // Same deferred-cast pattern the main canvas uses: motion events stash
    // the cursor; we run at most one ray-cast per frame here and rebuild
    // the highlight VBO only when the hovered face changed. This coalesces
    // a burst of wxEVT_MOTION events down to one ray-cast.
    if (m_transformMode == TransformMode::AlignFace)
    {
        EnsureFlatProgram();

        int hoverHalf = -1, hoverTri = -1;
        const bool hovered = RayCastFacePick(m_alignMousePos.x,
            m_alignMousePos.y, hoverHalf, hoverTri);

        if (!hovered)
        {
            if (m_alignHoverHalf != -1 || m_alignSeedTri != -1)
            {
                m_alignHoverHalf = -1;
                m_alignSeedTri = -1;
                m_alignFaceTris.clear();
                m_alignHighlightVertexCount = 0;
            }
        }
        else if (hoverHalf != m_alignHoverHalf || hoverTri != m_alignSeedTri)
        {
            FixtureMesh& hm = (hoverHalf == 0) ? m_meshA : m_meshB;
            EnsureTriAdjacency(hm);
            GrowCoplanarFace(hm, hoverTri,
                m_alignFaceTris, m_alignFaceNormalLocal);
            RebuildAlignHighlightVBO(hm, m_alignFaceTris);
            m_alignHoverHalf = hoverHalf;
            m_alignSeedTri = hoverTri;
        }

        // Draw the overlay if we have any tris cached. glPolygonOffset
        // wins the depth fight against the underlying lit half so the
        // highlight sits cleanly on top instead of z-fighting.
        if (m_flatProgram &&
            m_alignHighlightVAO != 0 &&
            m_alignHighlightVertexCount > 0)
        {
            glEnable(GL_DEPTH_TEST);
            glDepthFunc(GL_LEQUAL);
            glEnable(GL_POLYGON_OFFSET_FILL);
            glPolygonOffset(-1.0f, -1.0f);

            glUseProgram(m_flatProgram);
            const glm::mat4 VP = proj * view;
            glUniformMatrix4fv(m_flat_uVP, 1, GL_FALSE, &VP[0][0]);
            const glm::vec4 hoverColor(0.18f, 0.18f, 0.18f, 1.0f);
            glUniform4fv(m_flat_uColor, 1, &hoverColor[0]);
            glBindVertexArray(m_alignHighlightVAO);
            glDrawArrays(GL_TRIANGLES, 0, m_alignHighlightVertexCount);
            glBindVertexArray(0);

            glDisable(GL_POLYGON_OFFSET_FILL);
            glDepthFunc(GL_LESS);
            glUseProgram(0);
        }
    }

    // ---- Grid (drawn last because the grid shader uses blending) --------
    m_grid.Draw(view, proj);

    SwapBuffers();
}

void FixtureCanvas::OnResize(wxSizeEvent& evt)
{
    Refresh(false);
    evt.Skip();
}

// ---------------------------------------------------------------------------
// Mouse — orbit/pan/dolly camera plus mode-specific click handling
// ---------------------------------------------------------------------------
void FixtureCanvas::OnMouse(wxMouseEvent& evt)
{
    const wxPoint pos = evt.GetPosition();

    // Button transitions: reset m_hasLast so the first subsequent motion
    // frame just records the anchor position. Without this, swapping
    // buttons mid-drag would emit a single huge delta as the cursor
    // jumped from the last button's position to the current one.
    if (evt.LeftDown()) { m_lmb = true;  m_hasLast = false; }
    else if (evt.LeftUp()) { m_lmb = false; }
    else if (evt.MiddleDown()) { m_mmb = true;  m_hasLast = false; }
    else if (evt.MiddleUp()) { m_mmb = false; }
    else if (evt.RightDown()) { m_rmb = true;  m_hasLast = false; }
    else if (evt.RightUp()) { m_rmb = false; }

    // ---- Mode-specific click handling on LMB-down -------------------------
    // Runs before the camera-drag logic below so picking happens on the
    // press, not on release. Subsequent drag still orbits the camera —
    // matches the pattern MainFrame's GLCanvas uses for its Select mode.
    if (evt.LeftDown())
    {
        if (m_transformMode == TransformMode::Select)
        {
            const int hit = PickHalf(pos.x, pos.y);
            if (hit < 0)
            {
                m_selectedHalf = Selection::None;
            }
            else
            {
                m_selectedHalf = (hit == 0) ? Selection::A : Selection::B;
            }
            Refresh(false);
        }
        else if (m_transformMode == TransformMode::AlignFace)
        {
            // Re-run the face-pick rather than trusting the cached hover —
            // a click can arrive before OnPaint has refreshed the cache,
            // and re-casting is cheap. Mirrors GLCanvas's AlignFace click
            // path.
            int hitHalf = -1, hitTri = -1;
            if (RayCastFacePick(pos.x, pos.y, hitHalf, hitTri))
            {
                FixtureMesh& fm = (hitHalf == 0) ? m_meshA : m_meshB;
                std::vector<uint32_t> faceTris;
                glm::vec3 nLocal(0.0f);
                EnsureTriAdjacency(fm);
                GrowCoplanarFace(fm, hitTri, faceTris, nLocal);
                if (!faceTris.empty())
                {
                    ApplyAlignFaceToHalf(fm, nLocal, faceTris);

                    // Pose changed → cached world-space highlight verts are
                    // stale, and the new pose may not project under the
                    // cursor anymore. Drop hover state; OnPaint will
                    // regenerate from the next motion event.
                    m_alignHoverHalf = -1;
                    m_alignSeedTri = -1;
                    m_alignFaceTris.clear();
                    m_alignHighlightVertexCount = 0;
                    Refresh(false);
                }
            }
        }
    }

    if (!m_hasLast)
    {
        m_lastPos = pos;
        m_hasLast = true;
        evt.Skip();
        return;
    }

    const float dx = float(pos.x - m_lastPos.x);
    const float dy = float(pos.y - m_lastPos.y);
    m_lastPos = pos;

    // Same convention as the main canvas: MMB always pans, RMB always
    // dollies, LMB orbits when nothing is selected. With no selection
    // model in this canvas yet, LMB is the default orbit.
    if (m_mmb)
    {
        m_camera.Pan(dx, -dy);
        Refresh(false);
    }
    else if (m_rmb)
    {
        m_camera.Dolly(dy * 0.05f);
        Refresh(false);
    }
    else if (m_lmb)
    {
        m_camera.Orbit(dx, dy);
        Refresh(false);
    }
    else if (m_transformMode == TransformMode::AlignFace)
    {
        // No buttons held but in AlignFace: bare cursor motion updates the
        // hover highlight. Ray-cast itself is deferred to OnPaint; we just
        // stash the cursor and request a redraw.
        m_alignMousePos = pos;
        Refresh(false);
    }
}

void FixtureCanvas::OnMouseWheel(wxMouseEvent& evt)
{
    const int rot = evt.GetWheelRotation();
    const int delta = evt.GetWheelDelta();
    if (delta == 0) return;
    m_camera.Dolly(float(rot) / float(delta));
    Refresh(false);
}

// ---------------------------------------------------------------------------
// Keyboard — Escape drops AlignFace back to Select. Same shape as
// GLCanvas::OnKeyDown, with a callback into the parent FixtureEditor so
// the toolbar toggle visual stays in sync. We don't replicate GLCanvas's
// Ctrl+A / Ctrl+C / Ctrl+V / Delete shortcuts: the fixture editor only
// has two halves and no clipboard concept, so those would have nothing
// to act on.
// ---------------------------------------------------------------------------
void FixtureCanvas::OnKeyDown(wxKeyEvent& evt)
{
    if (evt.GetKeyCode() == WXK_ESCAPE)
    {
        if (m_transformMode != TransformMode::Select)
        {
            SetTransformMode(TransformMode::Select);

            // Notify the editor so the toolbar toggle untoggles. Mirrors
            // GLCanvas's call into MainFrame::SetActiveTool.
            if (auto* editor = dynamic_cast<FixtureEditor*>(wxGetTopLevelParent(this)))
                editor->SetActiveTool(wxID_NONE);
        }
        return;
    }
    evt.Skip();
}

// ---------------------------------------------------------------------------
// Mode / selection plumbing
// ---------------------------------------------------------------------------
void FixtureCanvas::SetTransformMode(TransformMode m)
{
    if (m_transformMode == m) return;
    m_transformMode = m;

    // Leaving AlignFace: discard any cached hover so re-entering doesn't
    // flash a stale highlight before the next motion event refreshes it.
    if (m != TransformMode::AlignFace)
    {
        m_alignHoverHalf = -1;
        m_alignSeedTri = -1;
        m_alignFaceTris.clear();
        m_alignHighlightVertexCount = 0;
    }
    Refresh(false);
}

FixtureCanvas::FixtureMesh* FixtureCanvas::SelectedMesh()
{
    switch (m_selectedHalf)
    {
    case Selection::A: return &m_meshA;
    case Selection::B: return &m_meshB;
    default:           return nullptr;
    }
}

const FixtureCanvas::FixtureMesh* FixtureCanvas::SelectedMesh() const
{
    switch (m_selectedHalf)
    {
    case Selection::A: return &m_meshA;
    case Selection::B: return &m_meshB;
    default:           return nullptr;
    }
}

// ---------------------------------------------------------------------------
// Transform application — direct ports of the GLCanvas equivalents,
// degenerated to a single-target selection. ReanchorFeaturesForObjects is
// not called here because the fixture editor has no parented features.
// ---------------------------------------------------------------------------
void FixtureCanvas::ApplyTranslation(float x, float y, float z)
{
    FixtureMesh* m = SelectedMesh();
    if (!m) return;
    m->pos += glm::vec3(x, y, z);
    Refresh(false);
}

void FixtureCanvas::ApplyRotation(float xDeg, float yDeg, float zDeg)
{
    FixtureMesh* m = SelectedMesh();
    if (!m) return;
    m->pitchDeg += xDeg;
    m->yawDeg += yDeg;
    m->rollDeg += zDeg;
    Refresh(false);
}

void FixtureCanvas::ApplyScale(float factor)
{
    FixtureMesh* m = SelectedMesh();
    if (!m) return;
    // 0.001 floor matches GLCanvas::ApplyScale — guards against runaway
    // shrinking that would make the half un-selectable / un-pickable.
    m->scale = std::max(0.001f, m->scale * factor);
    Refresh(false);
}

void FixtureCanvas::CenterSelected()
{
    FixtureMesh* m = SelectedMesh();
    if (!m) return;
    // Single-object centring degenerates to "set pos = origin" — same end
    // state GLCanvas::CenterSelectedObject reaches when its selection has
    // exactly one entry.
    m->pos = glm::vec3(0.0f);
    Refresh(false);
}

// ---------------------------------------------------------------------------
// GetHalfPose — public read-only window onto a half's pose. Used by the
// FixtureEditor's Generate-Fixture flow to pull pose state out of the
// canvas at save time. Returns valid=false for a slot with no geometry,
// which the caller treats as "no half here yet".
// ---------------------------------------------------------------------------
FixtureCanvas::HalfPose FixtureCanvas::GetHalfPose(HalfSlot slot) const
{
    const FixtureMesh& m = (slot == HalfSlot::A) ? m_meshA : m_meshB;

    HalfPose out;
    if (!m.valid) return out;   // valid stays false on the default-constructed pose

    out.valid = true;
    out.pos = m.pos;
    out.yawDeg = m.yawDeg;
    out.pitchDeg = m.pitchDeg;
    out.rollDeg = m.rollDeg;
    out.scale = m.scale;
    return out;
}

// ---------------------------------------------------------------------------
// PickHalf — single-half ray-vs-mesh test. Returns 0 (A), 1 (B), or -1.
// Identical math to RayCastFacePick below; we expose the simpler two-line
// signature separately because Select-mode picking doesn't need the
// triangle index.
// ---------------------------------------------------------------------------
int FixtureCanvas::PickHalf(int mouseX, int mouseY)
{
    int outHalf = -1, outTri = -1;
    return RayCastFacePick(mouseX, mouseY, outHalf, outTri) ? outHalf : -1;
}

// ---------------------------------------------------------------------------
// RayCastFacePick — port of GLCanvas::RayCastFacePick across both halves.
// Builds a world-space ray from (mouseX, mouseY) via inverse VP, transforms
// it into each half's local space using the half's model matrix, runs
// front-face-only Möller-Trumbore against every triangle in cpuVerts /
// cpuIndices, and returns the closest hit by world-space distance.
// ---------------------------------------------------------------------------
bool FixtureCanvas::RayCastFacePick(int mouseX, int mouseY, int& outHalf, int& outTri)
{
    outHalf = -1;
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

    const FixtureMesh* halves[2] = { &m_meshA, &m_meshB };
    for (int hi = 0; hi < 2; ++hi)
    {
        const FixtureMesh* m = halves[hi];
        if (!m->valid) continue;
        if (m->cpuVerts.empty() || m->cpuIndices.empty()) continue;

        const glm::mat4 model = m->BuildModelMatrix();
        const glm::mat4 invModel = glm::inverse(model);

        const glm::vec3 localOrig = glm::vec3(invModel * glm::vec4(rayOrig, 1.0f));
        const glm::vec3 localDir = glm::normalize(glm::vec3(invModel * glm::vec4(rayDir, 0.0f)));

        const size_t triCount = m->cpuIndices.size() / 3;
        for (size_t t = 0; t < triCount; ++t)
        {
            const uint32_t i0 = m->cpuIndices[3 * t + 0];
            const uint32_t i1 = m->cpuIndices[3 * t + 1];
            const uint32_t i2 = m->cpuIndices[3 * t + 2];

            const glm::vec3 v0(m->cpuVerts[i0 * 3], m->cpuVerts[i0 * 3 + 1], m->cpuVerts[i0 * 3 + 2]);
            const glm::vec3 v1(m->cpuVerts[i1 * 3], m->cpuVerts[i1 * 3 + 1], m->cpuVerts[i1 * 3 + 2]);
            const glm::vec3 v2(m->cpuVerts[i2 * 3], m->cpuVerts[i2 * 3 + 1], m->cpuVerts[i2 * 3 + 2]);

            float localT = 0.0f;
            if (!RayTriangle(localOrig, localDir, v0, v1, v2, localT)) continue;

            const glm::vec3 localHit = localOrig + localDir * localT;
            const glm::vec3 worldHit = glm::vec3(model * glm::vec4(localHit, 1.0f));
            const float     worldT = glm::length(worldHit - rayOrig);

            if (worldT < bestWorldT)
            {
                bestWorldT = worldT;
                outHalf = hi;
                outTri = (int)t;
            }
        }
    }

    return outHalf >= 0;
}

// ---------------------------------------------------------------------------
// EnsureTriAdjacency — port of GLCanvas::EnsureTriAdjacency. Manifold edges
// (exactly two triangles meeting) are linked both ways; non-manifold edges
// (3+ triangles, possible on imperfect imported meshes) leave the third
// and beyond unlinked rather than overwriting an existing pairing. See the
// original in GLCanvas.cpp for the rationale.
// ---------------------------------------------------------------------------
void FixtureCanvas::EnsureTriAdjacency(FixtureMesh& m)
{
    if (m.adjacencyBuilt) return;

    const size_t triCount = m.cpuIndices.size() / 3;
    m.triNeighbors.assign(triCount, std::array<int, 3>{ -1, -1, -1 });

    constexpr int kPaired = -2;

    auto pack = [](uint32_t a, uint32_t b) -> uint64_t {
        if (a > b) std::swap(a, b);
        return (uint64_t(a) << 32) | uint64_t(b);
        };

    std::unordered_map<uint64_t, std::pair<int, int>> edgeMap;
    edgeMap.reserve(triCount * 3);

    for (size_t t = 0; t < triCount; ++t)
    {
        const uint32_t verts[3] = {
            m.cpuIndices[3 * t + 0],
            m.cpuIndices[3 * t + 1],
            m.cpuIndices[3 * t + 2]
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
                m.triNeighbors[t][s] = otherT;
                m.triNeighbors[otherT][otherS] = (int)t;
                it->second.first = kPaired;
            }
        }
    }

    m.adjacencyBuilt = true;
}

// ---------------------------------------------------------------------------
// GrowCoplanarFace — BFS from seedTri across edge-shared neighbours whose
// triangle normal is within ~1° of the seed's normal (cos > 0.99985). Same
// "same-side coplanar" constraint as GLCanvas::GrowCoplanarFace — back-side
// faces of thin shells are excluded.
// ---------------------------------------------------------------------------
void FixtureCanvas::GrowCoplanarFace(const FixtureMesh& m, int seedTri,
    std::vector<uint32_t>& outTris, glm::vec3& outNormalLocal)
{
    outTris.clear();
    outNormalLocal = glm::vec3(0.0f);
    if (seedTri < 0 || seedTri >= (int)m.triNeighbors.size()) return;

    auto triNormal = [&](int t) -> glm::vec3 {
        const uint32_t i0 = m.cpuIndices[3 * t + 0];
        const uint32_t i1 = m.cpuIndices[3 * t + 1];
        const uint32_t i2 = m.cpuIndices[3 * t + 2];
        const glm::vec3 v0(m.cpuVerts[i0 * 3], m.cpuVerts[i0 * 3 + 1], m.cpuVerts[i0 * 3 + 2]);
        const glm::vec3 v1(m.cpuVerts[i1 * 3], m.cpuVerts[i1 * 3 + 1], m.cpuVerts[i1 * 3 + 2]);
        const glm::vec3 v2(m.cpuVerts[i2 * 3], m.cpuVerts[i2 * 3 + 1], m.cpuVerts[i2 * 3 + 2]);
        const glm::vec3 n = glm::cross(v1 - v0, v2 - v0);
        const float     len = glm::length(n);
        return (len > 1e-12f) ? (n / len) : glm::vec3(0.0f, 1.0f, 0.0f);
        };

    const glm::vec3 seedNormal = triNormal(seedTri);
    outNormalLocal = seedNormal;
    constexpr float kCosTol = 0.99985f;

    const size_t triCount = m.triNeighbors.size();
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
            const int nb = m.triNeighbors[t][s];
            if (nb < 0 || visited[nb]) continue;

            const glm::vec3 nNb = triNormal(nb);
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
// ApplyPlaneAlignmentToHalf — direct port of GLCanvas::ApplyPlaneAlignmentToObject,
// targeting a FixtureMesh in place of a SceneObject. See the original for
// the full derivation; the steps are:
//   1. Strip uniform scale from the model 3x3 to recover R_old.
//   2. Map the local plane normal into world space.
//   3. Pick whichever of ±Y is the smaller rotation away from the world
//      normal (so we respect the user's existing orientation).
//   4. Build R_delta from world normal → target axis.
//   5. R_new = R_delta * R_old, decompose back to YXZ Euler.
//   6. Solve translation so the anchor stays put laterally and lands on Y=0.
// ---------------------------------------------------------------------------
void FixtureCanvas::ApplyPlaneAlignmentToHalf(FixtureMesh& m,
    const glm::vec3& planeNormalLocal,
    const glm::vec3& anchorLocal)
{
    // ---- 1. Rotation pieces ------------------------------------------------
    const glm::mat4 modelOld = m.BuildModelMatrix();
    glm::mat3 R_old(modelOld);
    if (m.scale > 1e-12f)
        R_old /= m.scale;

    const glm::vec3 nLocalUnit = glm::normalize(planeNormalLocal);
    const glm::vec3 nWorld = glm::normalize(R_old * nLocalUnit);

    // ---- 2. Smallest-rotation target on ±Y --------------------------------
    const glm::vec3 targetY =
        (nWorld.y >= 0.0f) ? glm::vec3(0, 1, 0) : glm::vec3(0, -1, 0);

    // ---- 3. R_delta from nWorld to targetY --------------------------------
    glm::mat3 R_delta(1.0f);
    const float d = glm::clamp(glm::dot(nWorld, targetY), -1.0f, 1.0f);
    if (d < 0.99999f)
    {
        if (d < -0.99999f)
        {
            // Anti-parallel: 180° rotation about any axis perpendicular to
            // nWorld. Pick the world axis least parallel to nWorld for
            // numerical stability.
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

    // ---- 4. Compose and decompose -----------------------------------------
    const glm::mat3 R_new = R_delta * R_old;
    DecomposeYXZ(R_new, m.yawDeg, m.pitchDeg, m.rollDeg);

    // ---- 5. Solve translation ---------------------------------------------
    // a_w = pos_old + R_old * (s * a_local), and after alignment we want
    //   a_w'.y = 0 and a_w'.{x,z} = a_w.{x,z}. With pos_new + R_delta *
    //   (a_w - pos_old) = a_w', and R_delta only acting on the rotated
    //   anchor offset v_old, the closed-form fall-out below works.
    const glm::vec3 v_old = R_old * (m.scale * anchorLocal);
    const glm::vec3 a_w = m.pos + v_old;
    const glm::vec3 v_new = R_delta * v_old;

    m.pos.x = a_w.x - v_new.x;
    m.pos.y = -v_new.y;
    m.pos.z = a_w.z - v_new.z;
}

// ---------------------------------------------------------------------------
// ApplyAlignFaceToHalf — thin wrapper around ApplyPlaneAlignmentToHalf that
// computes the face's local-space centroid as the anchor point.
// ---------------------------------------------------------------------------
void FixtureCanvas::ApplyAlignFaceToHalf(FixtureMesh& m, const glm::vec3& nLocal,
    const std::vector<uint32_t>& faceTris)
{
    if (faceTris.empty()) return;

    glm::vec3 centroidLocal(0.0f);
    int       triCounted = 0;
    for (uint32_t t : faceTris)
    {
        const uint32_t i0 = m.cpuIndices[3 * t + 0];
        const uint32_t i1 = m.cpuIndices[3 * t + 1];
        const uint32_t i2 = m.cpuIndices[3 * t + 2];
        const glm::vec3 v0(m.cpuVerts[i0 * 3], m.cpuVerts[i0 * 3 + 1], m.cpuVerts[i0 * 3 + 2]);
        const glm::vec3 v1(m.cpuVerts[i1 * 3], m.cpuVerts[i1 * 3 + 1], m.cpuVerts[i1 * 3 + 2]);
        const glm::vec3 v2(m.cpuVerts[i2 * 3], m.cpuVerts[i2 * 3 + 1], m.cpuVerts[i2 * 3 + 2]);
        centroidLocal += (v0 + v1 + v2) * (1.0f / 3.0f);
        ++triCounted;
    }
    centroidLocal /= float(triCounted);

    ApplyPlaneAlignmentToHalf(m, nLocal, centroidLocal);
}

// ---------------------------------------------------------------------------
// RebuildAlignHighlightVBO — upload the highlighted face's triangles in
// world space (model matrix baked in here on the CPU). Lazy VAO/VBO
// creation; reuse on subsequent calls. Same approach as GLCanvas's
// equivalent.
// ---------------------------------------------------------------------------
void FixtureCanvas::RebuildAlignHighlightVBO(const FixtureMesh& m,
    const std::vector<uint32_t>& tris)
{
    if (tris.empty())
    {
        m_alignHighlightVertexCount = 0;
        return;
    }

    const glm::mat4 model = m.BuildModelMatrix();

    std::vector<float> verts;
    verts.reserve(tris.size() * 9);

    for (uint32_t t : tris)
    {
        for (int k = 0; k < 3; ++k)
        {
            const uint32_t vi = m.cpuIndices[3 * t + k];
            const glm::vec3 vL(m.cpuVerts[vi * 3],
                m.cpuVerts[vi * 3 + 1],
                m.cpuVerts[vi * 3 + 2]);
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
