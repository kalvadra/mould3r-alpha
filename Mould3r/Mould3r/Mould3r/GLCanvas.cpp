#define _CRT_SECURE_NO_WARNINGS

#include <glad/glad.h>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include "GLCanvas.h"
#include <wx/dcclient.h>
#include <wx/log.h>
#include <wx/msgdlg.h>

#include <algorithm>
#include <string>
#include <vector>

#include "camera.h"
#include "FileImporter.h"
#include "GridRenderer.h"
#include "shaders.h"
#include "MeshUtils.h"
#include "MeshOps.h"

#include "TranslateDialog.h"
#include "ScaleDialog.h"

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

static int glArgs[] = {
    WX_GL_RGBA,
    WX_GL_DOUBLEBUFFER,
    WX_GL_DEPTH_SIZE, 24,
    WX_GL_STENCIL_SIZE, 8,
    WX_GL_SAMPLE_BUFFERS, 1,
    WX_GL_SAMPLES, 4,
    0
};

// ---------------------------------------------------------------------------
// Constructor / Destructor
// ---------------------------------------------------------------------------
GLCanvas::GLCanvas(wxWindow* parent)
    : wxGLCanvas(parent,
        wxID_ANY,
        glArgs,
        wxDefaultPosition,
        wxDefaultSize,
        wxFULL_REPAINT_ON_RESIZE,
        "GLCanvas")
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
// SetTransformMode (called by ribbon)
// ---------------------------------------------------------------------------
void GLCanvas::SetTransformMode(TransformMode mode)
{
    m_transformMode = mode;
    // Give a visual cue: change cursor
    switch (mode)
    {
    case TransformMode::Select:
        SetCursor(wxCursor(wxCURSOR_ARROW));    break;
    case TransformMode::Translate:
        SetCursor(wxCursor(wxCURSOR_SIZING));   break;
    case TransformMode::Rotate:
        SetCursor(wxCursor(wxCURSOR_BULLSEYE)); break;
    case TransformMode::Scale:
        SetCursor(wxCursor(wxCURSOR_SIZENS));   break;
    }
}

// ---------------------------------------------------------------------------
// BuildModelMatrix
// ---------------------------------------------------------------------------
glm::mat4 GLCanvas::BuildModelMatrix() const
{
    glm::mat4 T = glm::translate(glm::mat4(1.0f), m_modelPos);
    glm::mat4 RY = glm::rotate(glm::mat4(1.0f), glm::radians(m_modelYawDeg), glm::vec3(0, 1, 0));
    glm::mat4 RX = glm::rotate(glm::mat4(1.0f), glm::radians(m_modelPitchDeg), glm::vec3(1, 0, 0));
    glm::mat4 RZ = glm::rotate(glm::mat4(1.0f), glm::radians(m_modelRollDeg), glm::vec3(0, 0, 1));
    glm::mat4 S = glm::scale(glm::mat4(1.0f), glm::vec3(m_modelScale));

    return T * RY * RX * RZ * S;
}

void GLCanvas::ApplyTranslation(float x, float y, float z)
{
    m_modelPos += glm::vec3(x, y, z);
    Refresh(false);
}

void GLCanvas::ApplyRotation(float xDeg, float yDeg, float zDeg)
{
    m_modelPitchDeg += xDeg;
    m_modelYawDeg += yDeg;
    m_modelRollDeg += zDeg;
    Refresh(false);
}

void GLCanvas::ApplyScale(float factor)
{
    m_modelScale = std::max(0.001f, m_modelScale * factor);
    Refresh(false);
}

// ---------------------------------------------------------------------------
// ApplyTransformDelta – converts mouse dx/dy into the active transform
// ---------------------------------------------------------------------------
void GLCanvas::ApplyTransformDelta(float dx, float dy)
{
    switch (m_transformMode)
    {
    case TransformMode::Translate:
    {
        // Move in the camera's right/up plane, scaled by camera distance
        const float dist = m_camera.GetDistance();   // see note below
        const float unitsPerPx = dist * 0.0015f;

        glm::vec3 right = m_camera.Right();
        glm::vec3 up = m_camera.Up();

        // Suppress Y component of 'right' so horizontal drag stays on XZ
        right.y = 0.0f;
        if (glm::length(right) > 1e-4f)
            right = glm::normalize(right);

        m_modelPos += right * (dx * unitsPerPx);
        m_modelPos += -up * (dy * unitsPerPx);   // dy: screen Y increases downward
        break;
    }

    case TransformMode::Rotate:
        // dx → spin around world Y,  dy → tilt around local X
        m_modelYawDeg += dx * 0.5f;
        m_modelPitchDeg += dy * 0.5f;
        break;

    case TransformMode::Scale:
    {
        // Drag up (negative dy) = grow,  drag down = shrink
        const float factor = 1.0f - dy * 0.005f;
        m_modelScale = std::max(0.001f, m_modelScale * factor);
        break;
    }

    default:
        break;
    }
}

// ---------------------------------------------------------------------------
// Resize
// ---------------------------------------------------------------------------
void GLCanvas::OnResize(wxSizeEvent& evt)
{
    Refresh(false);
    evt.Skip();
}

// ---------------------------------------------------------------------------
// GL init helpers
// ---------------------------------------------------------------------------
static void* GetAnyGLFuncAddress(const char* name)
{
    void* p = (void*)wglGetProcAddress(name);
    if (p) return p;
    static HMODULE module = LoadLibraryA("opengl32.dll");
    if (!module) return nullptr;
    return (void*)GetProcAddress(module, name);
}

void GLCanvas::InitGLOnce()
{
    if (m_inited) return;

    SetCurrent(*m_context);

    if (!gladLoadGLLoader((GLADloadproc)GetAnyGLFuncAddress)) {
        wxLogError("Failed to load OpenGL functions");
        return;
    }

    glEnable(GL_DEPTH_TEST);
    glEnable(GL_MULTISAMPLE);

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

    // Fallback test pyramid
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

    m_inited = true;
}

void GLCanvas::DestroyGL()
{
    if (m_outlineProgram) { glDeleteProgram(m_outlineProgram); m_outlineProgram = 0; }
    if (m_fullscreenVAO) { glDeleteVertexArrays(1, &m_fullscreenVAO); m_fullscreenVAO = 0; }
    if (m_vbo) { glDeleteBuffers(1, &m_vbo);          m_vbo = 0; }
    if (m_vao) { glDeleteVertexArrays(1, &m_vao);     m_vao = 0; }
    if (m_ebo) { glDeleteBuffers(1, &m_ebo);          m_ebo = 0; }
    if (m_program) { glDeleteProgram(m_program);     m_program = 0; }
    if (m_pickProgram) { glDeleteProgram(m_pickProgram); m_pickProgram = 0; }
    m_pick_uMVP = m_pick_uObjectId = -1;
    DestroyPickFBO();
}

// ---------------------------------------------------------------------------
// GPU upload
// ---------------------------------------------------------------------------
void GLCanvas::UploadMeshToGPU(const FileImporter::MeshData& mesh, GPUMesh* /*reuse*/)
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
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(vtx.size() * sizeof(float)), vtx.data(), GL_STATIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, newMesh.ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)(mesh.indices.size() * sizeof(uint32_t)),
        mesh.indices.data(), GL_STATIC_DRAW);

    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, strideFloats * (GLsizei)sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);

    if (strideFloats == 6) {
        glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 6 * (GLsizei)sizeof(float), (void*)(3 * sizeof(float)));
        glEnableVertexAttribArray(1);
    }
    else {
        glDisableVertexAttribArray(1);
        glVertexAttrib3f(1, 0.0f, 1.0f, 0.0f);
    }

    glBindVertexArray(0);
    newMesh.indexCount = (GLsizei)mesh.indices.size();

    m_modelMesh.Destroy();
    m_modelMesh = newMesh;
}

// ---------------------------------------------------------------------------
// Import
// ---------------------------------------------------------------------------
void GLCanvas::ImportStepFile(const std::string& path)
{
    SetCurrent(*m_context);
    InitGLOnce();

    FileImporter importer;
    auto res = importer.ImportSTEP(path, 0.05, 0.5);

    if (!res.ok()) {
        wxMessageBox(res.error, "Import failed", wxOK | wxICON_ERROR, this);
        return;
    }

    ComputeVertexNormals_Pos3(res.meshes[0].vertices, res.meshes[0].indices, res.meshes[0].posNorm);

    auto split = SplitByCreaseAngle_Pos3(res.meshes[0].vertices, res.meshes[0].indices, 35.0f);
    res.meshes[0].posNorm = std::move(split.posNorm);
    res.meshes[0].indices = std::move(split.indices);

    UploadMeshToGPU(res.meshes[0], nullptr);

    // Reset transforms when a new file is loaded
    m_modelPos = glm::vec3(0.0f);
    m_modelYawDeg = 0.0f;
    m_modelPitchDeg = 0.0f;
    m_modelRollDeg = 0.0f;
    m_modelScale = 1.0f;

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

    glm::mat4 view = m_camera.View();
    glm::mat4 proj = m_camera.Projection();
    glm::vec3 camPos = m_camera.Position();

    m_grid.Draw(view, proj);

    if (m_program && m_modelMesh.vao && m_modelMesh.indexCount > 0)
    {
        glm::mat4 model = BuildModelMatrix();   // <-- uses transform state

        glUseProgram(m_program);
        glBindVertexArray(m_modelMesh.vao);

        glUniformMatrix4fv(glGetUniformLocation(m_program, "uModel"), 1, GL_FALSE, &model[0][0]);
        glUniformMatrix4fv(glGetUniformLocation(m_program, "uView"), 1, GL_FALSE, &view[0][0]);
        glUniformMatrix4fv(glGetUniformLocation(m_program, "uProj"), 1, GL_FALSE, &proj[0][0]);

        glm::vec3 lightDir = glm::normalize(glm::vec3(0.4f, 0.8f, 0.2f));
        glm::vec3 lightColor = glm::vec3(1.0f);
        glm::vec3 baseColor = glm::vec3(0.80f, 0.80f, 0.85f);

        glUniform3fv(glGetUniformLocation(m_program, "uCameraPos"), 1, &camPos[0]);
        glUniform3fv(glGetUniformLocation(m_program, "uLightDir"), 1, &lightDir[0]);
        glUniform3fv(glGetUniformLocation(m_program, "uLightColor"), 1, &lightColor[0]);
        glUniform3fv(glGetUniformLocation(m_program, "uBaseColor"), 1, &baseColor[0]);
        glUniform1f(glGetUniformLocation(m_program, "uAmbient"), 0.25f);
        glUniform1f(glGetUniformLocation(m_program, "uDiffuse"), 0.85f);
        glUniform1f(glGetUniformLocation(m_program, "uSpecular"), 0.20f);
        glUniform1f(glGetUniformLocation(m_program, "uShininess"), 64.0f);

        glDrawElements(GL_TRIANGLES, m_modelMesh.indexCount, GL_UNSIGNED_INT, 0);

        // Outline when selected
        if (m_selectedObjectId == 1)
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
                glUniform1i(m_outline_uIdTex, 0);
                glUniform1ui(m_outline_uTargetId, 1u);
                glUniform2i(m_outline_uTexSize, m_pickW, m_pickH);
                glUniform1f(m_outline_uAlpha, 0.9f);
                glUniform1i(m_outline_uThickness, 2);

                glBindVertexArray(m_fullscreenVAO);
                glDrawArrays(GL_TRIANGLES, 0, 3);
                glBindVertexArray(0);
                glBindTexture(GL_TEXTURE_2D, 0);
                glUseProgram(0);
                glDisable(GL_BLEND);
                glEnable(GL_DEPTH_TEST);
            }
        }

        glBindVertexArray(0);
        glUseProgram(0);
    }

    SwapBuffers();
}

// ---------------------------------------------------------------------------
// Mouse – orbit/pan/dolly in Select mode; transform in other modes
// ---------------------------------------------------------------------------
void GLCanvas::OnMouse(wxMouseEvent& evt)
{
    // ---- Button down/up bookkeeping ----------------------------------------
    if (evt.LeftDown())
    {
        m_lmb = true;
        m_hasLast = false;

        if (m_transformMode == TransformMode::Select)
        {
            const wxPoint p = evt.GetPosition();
            m_selectedObjectId = PickObjectAt(p.x, p.y);
            Refresh(false);
        }
    }
    if (evt.LeftUp()) { m_lmb = false; if (HasCapture()) ReleaseMouse(); }
    if (evt.MiddleDown()) { m_mmb = true;  m_hasLast = false; CaptureMouse(); }
    if (evt.MiddleUp()) { m_mmb = false; if (HasCapture()) ReleaseMouse(); }
    if (evt.RightDown()) { m_rmb = true;  m_hasLast = false; CaptureMouse(); }
    if (evt.RightUp()) { m_rmb = false; if (HasCapture()) ReleaseMouse(); }

    if (evt.Dragging() && m_lmb && !HasCapture())
        CaptureMouse();

    if (!evt.Moving() && !evt.Dragging()) { evt.Skip(); return; }

    // ---- Delta calculation -------------------------------------------------
    const wxPoint pos = evt.GetPosition();
    if (!m_hasLast) { m_lastPos = pos; m_hasLast = true; evt.Skip(); return; }

    const float dx = float(pos.x - m_lastPos.x);
    const float dy = float(pos.y - m_lastPos.y);
    m_lastPos = pos;

    const bool shift = evt.ShiftDown();
    const bool ctrl = evt.ControlDown();

    // ---- Transform modes: LMB drag applies transform (only if object selected) ---
    if (m_lmb && m_transformMode != TransformMode::Select && m_selectedObjectId != 0)
    {
        ApplyTransformDelta(dx, dy);
        Refresh(false);
        evt.Skip();
        return;
    }

    // ---- Select mode (or MMB/RMB): camera controls -------------------------
    if (m_mmb)
    {
        m_camera.Pan(dx, -dy);
        Refresh(false);
    }
    else if (m_lmb)   // Select mode only reaches here
    {
        if (shift)
            m_camera.Pan(dx, -dy);
        else if (ctrl)
            m_camera.Dolly(dy * 0.05f);
        else
            m_camera.Orbit(dx, dy);
        Refresh(false);
    }
    else if (m_rmb)
    {
        m_camera.Dolly(dy * 0.05f);
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

// ---------------------------------------------------------------------------
// Picking FBO helpers
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
    w = std::max(1, w);
    h = std::max(1, h);
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
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_pickColorTex, 0);

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

uint32_t GLCanvas::PickObjectAt(int mouseX, int mouseY)
{
    SetCurrent(*m_context);
    InitGLOnce();

    const auto sz = GetClientSize();
    const int w = std::max(1, sz.x);
    const int h = std::max(1, sz.y);
    EnsurePickFBO(w, h);

    if (!m_pickFBO || !m_pickProgram || m_modelMesh.vao == 0 || m_modelMesh.indexCount <= 0)
        return 0;

    const int x = mouseX;
    const int y = (m_pickH - 1) - mouseY;
    if (x < 0 || y < 0 || x >= m_pickW || y >= m_pickH) return 0;

    glBindFramebuffer(GL_FRAMEBUFFER, m_pickFBO);
    glViewport(0, 0, m_pickW, m_pickH);

    GLuint clearId = 0;
    glClearBufferuiv(GL_COLOR, 0, &clearId);
    glClear(GL_DEPTH_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);

    glUseProgram(m_pickProgram);
    m_camera.SetAspect(float(w) / float(h));

    glm::mat4 mvp = m_camera.Projection() * m_camera.View() * BuildModelMatrix();
    glUniformMatrix4fv(m_pick_uMVP, 1, GL_FALSE, &mvp[0][0]);
    glUniform1ui(m_pick_uObjectId, 1u);

    glBindVertexArray(m_modelMesh.vao);
    glDrawElements(GL_TRIANGLES, m_modelMesh.indexCount, GL_UNSIGNED_INT, 0);
    glBindVertexArray(0);

    uint32_t id = 0;
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    glReadPixels(x, y, 1, 1, GL_RED_INTEGER, GL_UNSIGNED_INT, &id);

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, w, h);
    glUseProgram(0);
    return id;
}

void GLCanvas::RenderPickPass_NoRead(int w, int h)
{
    EnsurePickFBO(w, h);
    if (!m_pickFBO || !m_pickProgram || m_modelMesh.vao == 0 || m_modelMesh.indexCount <= 0) return;

    glBindFramebuffer(GL_FRAMEBUFFER, m_pickFBO);
    glViewport(0, 0, m_pickW, m_pickH);

    GLuint clearId = 0;
    glClearBufferuiv(GL_COLOR, 0, &clearId);
    glClear(GL_DEPTH_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);

    glUseProgram(m_pickProgram);
    m_camera.SetAspect(float(w) / float(h));
    glm::mat4 mvp = m_camera.Projection() * m_camera.View() * BuildModelMatrix();
    glUniformMatrix4fv(m_pick_uMVP, 1, GL_FALSE, &mvp[0][0]);
    glUniform1ui(m_pick_uObjectId, 1u);

    glBindVertexArray(m_modelMesh.vao);
    glDrawElements(GL_TRIANGLES, m_modelMesh.indexCount, GL_UNSIGNED_INT, 0);
    glBindVertexArray(0);
    glUseProgram(0);

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, w, h);
}
