#include "FixtureCanvas.h"

#include <wx/dcclient.h>
#include <wx/log.h>
#include <wx/msgdlg.h>

#include <algorithm>
#include <cstdint>
#include <string>

#include <glm/gtc/matrix_transform.hpp>

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
    // instead of smearing across them. cpuVerts/cpuIndices snapshotting
    // (which the main canvas keeps for ray-cast picking) is omitted —
    // FixtureCanvas has no picking yet.
    auto& mesh = res.meshes[0];
    ComputeVertexNormals_Pos3(mesh.vertices, mesh.indices, mesh.posNorm);
    auto split = SplitByCreaseAngle_Pos3(mesh.vertices, mesh.indices, 35.0f);
    mesh.posNorm = std::move(split.posNorm);
    mesh.indices = std::move(split.indices);

    UploadMesh(mesh, target);

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
    // Both halves currently render at identity transform. When the toolbar
    // gets wired up, per-half pose state will live in this class and
    // multiply through uModel here — same pattern MainFrame's GLCanvas
    // uses for its SceneObject list.
    if (m_litProgram)
    {
        glUseProgram(m_litProgram);

        // Lighting / material — match the main canvas's defaults so an
        // imported half looks identical between the two viewports.
        const glm::vec3 lightDir = glm::normalize(glm::vec3(0.4f, 0.8f, 0.2f));
        const glm::vec3 lightColor = glm::vec3(1.0f);
        const glm::vec3 baseColor = glm::vec3(0.80f, 0.80f, 0.85f);

        glUniformMatrix4fv(m_uView, 1, GL_FALSE, &view[0][0]);
        glUniformMatrix4fv(m_uProj, 1, GL_FALSE, &proj[0][0]);
        glUniform3fv(m_uCameraPos, 1, &camPos[0]);
        glUniform3fv(m_uLightDir, 1, &lightDir[0]);
        glUniform3fv(m_uLightColor, 1, &lightColor[0]);
        glUniform3fv(m_uBaseColor, 1, &baseColor[0]);
        glUniform1f(m_uAlpha, 1.0f);
        glUniform1f(m_uAmbient, 0.25f);
        glUniform1f(m_uDiffuse, 0.85f);
        glUniform1f(m_uSpecular, 0.20f);
        glUniform1f(m_uShininess, 64.0f);

        const glm::mat4 identity(1.0f);
        glUniformMatrix4fv(m_uModel, 1, GL_FALSE, &identity[0][0]);

        for (const FixtureMesh* m : { &m_meshA, &m_meshB })
        {
            if (!m->valid || m->indexCount == 0) continue;
            glBindVertexArray(m->vao);
            glDrawElements(GL_TRIANGLES, m->indexCount, GL_UNSIGNED_INT, 0);
        }
        glBindVertexArray(0);
        glUseProgram(0);
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
// Mouse — orbit/pan/dolly camera, no model interaction yet
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
}

void FixtureCanvas::OnMouseWheel(wxMouseEvent& evt)
{
    const int rot = evt.GetWheelRotation();
    const int delta = evt.GetWheelDelta();
    if (delta == 0) return;
    m_camera.Dolly(float(rot) / float(delta));
    Refresh(false);
}
