#define _CRT_SECURE_NO_WARNINGS

#include <glad/glad.h>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include "GLCanvas.h"
#include <wx/dcclient.h>     // wxPaintDC
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
    // Request a modern core profile context.
    wxGLContextAttrs ctxAttrs;
#if defined(__APPLE__)
    ctxAttrs.PlatformDefaults().CoreProfile().OGLVersion(3, 2).EndList();
#else
    ctxAttrs.PlatformDefaults().CoreProfile().OGLVersion(3, 3).EndList();
#endif

    m_context = new wxGLContext(this, nullptr, &ctxAttrs);

    if (!m_context->IsOK()) {
        // Fallback: default context if core profile fails.
        delete m_context;
        m_context = new wxGLContext(this);
        wxLogWarning("Core profile context request failed; using default GL context.");
    }
#else
    // Older wxWidgets: no wxGLContextAttrs.
    m_context = new wxGLContext(this);
#endif

    m_camera.SetOrbitSensitivity(0.15f); // deg/pixel
    m_camera.SetPanSensitivity(0.0025f); // units/pixel at distance=1
    m_camera.SetDollySensitivity(0.08f); // exponential strength

    //Bind painting and frame resize events
    Bind(wxEVT_PAINT, &GLCanvas::OnPaint, this);
    Bind(wxEVT_SIZE, &GLCanvas::OnResize, this);

    //Bind mouse events
    Bind(wxEVT_LEFT_DOWN, &GLCanvas::OnMouse, this);
    Bind(wxEVT_LEFT_UP, &GLCanvas::OnMouse, this);
    Bind(wxEVT_MIDDLE_DOWN, &GLCanvas::OnMouse, this);
    Bind(wxEVT_MIDDLE_UP, &GLCanvas::OnMouse, this);
    Bind(wxEVT_RIGHT_DOWN, &GLCanvas::OnMouse, this);
    Bind(wxEVT_RIGHT_UP, &GLCanvas::OnMouse, this);
    Bind(wxEVT_MOTION, &GLCanvas::OnMouse, this);
    Bind(wxEVT_MOUSEWHEEL, &GLCanvas::OnMouseWheel, this);

    // helps ensure wheel/key modifiers are received
    SetFocus();
}

GLCanvas::~GLCanvas()
{
    // Ensure GL objects are deleted while context is current.
    if (m_context) {
        SetCurrent(*m_context);
        DestroyGL();
    }
    delete m_context;
}

void GLCanvas::OnResize(wxSizeEvent& evt)
{
    Refresh(false);
    evt.Skip();
}

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

    // Picking program (object-id rendering)
    {
        GLuint pvs = Compile(GL_VERTEX_SHADER, m_shaders.vsPick);
        GLuint pfs = Compile(GL_FRAGMENT_SHADER, m_shaders.fsPick);
        m_pickProgram = Link(pvs, pfs);

        if (!m_pickProgram)
            wxLogError("Pick program failed to link.");

        // Cache uniform locations once
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

    // Triangle data: pos(x,y), color(r,g,b)
    float verts[] = {
        // position              // color
         0.0f,  0.8f,  0.0f,      1.f, 1.f, 1.f,   // 0 apex
        -0.5f, -0.5f,  0.5f,      1.f, 0.f, 0.f,   // 1 base front-left
         0.5f, -0.5f,  0.5f,      0.f, 1.f, 0.f,   // 2 base front-right
         0.5f, -0.5f, -0.5f,      0.f, 0.f, 1.f,   // 3 base back-right
        -0.5f, -0.5f, -0.5f,      1.f, 1.f, 0.f    // 4 base back-left
    };

    unsigned int idx[] = 
    {
        // sides
        0, 1, 2,
        0, 2, 3,
        0, 3, 4,
        0, 4, 1,

        // base (two triangles)
        1, 4, 3,
        1, 3, 2
    };

    glGenVertexArrays(1, &m_vao);
    glGenBuffers(1, &m_vbo);
    glGenBuffers(1, &m_ebo);

    glBindVertexArray(m_vao);

    glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);

    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(idx), idx, GL_STATIC_DRAW);

    // position (location 0): 3 floats
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);

    // color (location 1): 3 floats
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)(3 * sizeof(float)));
    glEnableVertexAttribArray(1);

    glBindVertexArray(0);
}

void GLCanvas::DestroyGL()
{
    if (m_outlineProgram) { glDeleteProgram(m_outlineProgram); m_outlineProgram = 0; }
    if (m_fullscreenVAO) { glDeleteVertexArrays(1, &m_fullscreenVAO); m_fullscreenVAO = 0; }
    if (m_vbo) { glDeleteBuffers(1, &m_vbo); m_vbo = 0; }
    if (m_vao) { glDeleteVertexArrays(1, &m_vao); m_vao = 0; }
    if (m_ebo) { glDeleteBuffers(1, &m_ebo); m_ebo = 0; }
    if (m_program) { glDeleteProgram(m_program); m_program = 0; }
    if (m_pickProgram) { glDeleteProgram(m_pickProgram); m_pickProgram = 0; }
    m_pick_uMVP = -1;
    m_pick_uObjectId = -1;

    DestroyPickFBO();

}

void GLCanvas::UploadMeshToGPU(const FileImporter::MeshData& mesh, GPUMesh* reuse)
{
    // pick vertex source
    const std::vector<float>& vtx = !mesh.posNorm.empty() ? mesh.posNorm : mesh.vertices;
    const int strideFloats = !mesh.posNorm.empty() ? 6 : 3;

    if (vtx.empty() || mesh.indices.empty())
        return;

    if ((vtx.size() % strideFloats) != 0)
        return;

    // IMPORTANT: don't destroy the current mesh until we're sure new data is valid
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

    // pos
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, strideFloats * (GLsizei)sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);

    if (strideFloats == 6) {
        // normal
        glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 6 * (GLsizei)sizeof(float), (void*)(3 * sizeof(float)));
        glEnableVertexAttribArray(1);
    }
    else {
        // no normals provided — use a constant normal so shader stays sane
        glDisableVertexAttribArray(1);
        glVertexAttrib3f(1, 0.0f, 1.0f, 0.0f);
    }

    glBindVertexArray(0);

    newMesh.indexCount = (GLsizei)mesh.indices.size();

    // swap in only after success
    m_modelMesh.Destroy();
    m_modelMesh = newMesh;
}

void GLCanvas::ImportStepFile(const std::string& path)
{
    // Make sure GL context is current before *any* GL calls
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

    UploadMeshToGPU(res.meshes[0],nullptr);

    Refresh(false);

    return;
}

void GLCanvas::OnPaint(wxPaintEvent&)
{
    wxPaintDC dc(this);  // REQUIRED
    SetCurrent(*m_context);
    InitGLOnce();
    m_grid.Init();

    const wxSize sz = GetClientSize();
    const int w = std::max(1, sz.x);
    const int h = std::max(1, sz.y);

    glViewport(0, 0, w, h);

    EnsurePickFBO(w, h);

    //Selection method
    //glTexImage2D(GL_TEXTURE_2D, 0, GL_R32UI, w, h, 0, GL_RED_INTEGER, GL_UNSIGNED_INT, nullptr);
    //glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, w, h);

    glEnable(GL_DEPTH_TEST);

    glClearColor(0.12f, 0.14f, 0.18f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    m_camera.SetAspect(float(w) / float(h));

    glm::mat4 view = m_camera.View();
    glm::mat4 proj = m_camera.Projection();

    glm::vec3 camPos = m_camera.Position();

    //Create grid
    m_grid.Draw(view, proj);


    if (m_program && m_modelMesh.vao && m_modelMesh.indexCount > 0)
    {
        glUseProgram(m_program);

        // simple camera-like view: pull back on Z
        glm::mat4 model(1.0f);


        glUseProgram(m_program);

        glBindVertexArray(m_modelMesh.vao);

        // upload uniforms
        glUniformMatrix4fv(glGetUniformLocation(m_program, "uModel"), 1, GL_FALSE, &model[0][0]);
        glUniformMatrix4fv(glGetUniformLocation(m_program, "uView"), 1, GL_FALSE, &view[0][0]);
        glUniformMatrix4fv(glGetUniformLocation(m_program, "uProj"), 1, GL_FALSE, &proj[0][0]);

        // lighting
        glm::vec3 lightDir = glm::normalize(glm::vec3(0.4f, 0.8f, 0.2f)); // tweak
        glUniform3fv(glGetUniformLocation(m_program, "uCameraPos"), 1, &camPos[0]);
        glUniform3fv(glGetUniformLocation(m_program, "uLightDir"), 1, &lightDir[0]);

        glm::vec3 lightColor(1.0f, 1.0f, 1.0f);
        glm::vec3 baseColor(0.80f, 0.80f, 0.85f);
        glUniform3fv(glGetUniformLocation(m_program, "uLightColor"), 1, &lightColor[0]);
        glUniform3fv(glGetUniformLocation(m_program, "uBaseColor"), 1, &baseColor[0]);

        glUniform1f(glGetUniformLocation(m_program, "uAmbient"), 0.25f);
        glUniform1f(glGetUniformLocation(m_program, "uDiffuse"), 0.85f);
        glUniform1f(glGetUniformLocation(m_program, "uSpecular"), 0.20f);
        glUniform1f(glGetUniformLocation(m_program, "uShininess"), 64.0f);

        glBindVertexArray(m_modelMesh.vao);

        glDrawElements(GL_TRIANGLES, m_modelMesh.indexCount, GL_UNSIGNED_INT, 0);

        if (m_selectedObjectId == 1)
        {
            // Keep ID buffer updated for current frame
            RenderPickPass_NoRead(w, h);

            // Outline pass (blend over the already-rendered scene)
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
                glUniform1i(m_outline_uThickness, 2); // 1..4

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

void GLCanvas::OnMouse(wxMouseEvent& evt)
{
    if (evt.LeftDown())
    {
        m_lmb = true;
        m_hasLast = false;

        // Selection on click
        const wxPoint p = evt.GetPosition();
        m_selectedObjectId = PickObjectAt(p.x, p.y);
        Refresh(false);
    }


    
    if (evt.LeftUp()) { m_lmb = false; if (HasCapture()) ReleaseMouse(); }

    if (evt.MiddleDown()) { m_mmb = true;  m_hasLast = false; CaptureMouse(); }
    if (evt.MiddleUp()) { m_mmb = false; if (HasCapture()) ReleaseMouse(); }

    if (evt.RightDown()) { m_rmb = true;  m_hasLast = false; CaptureMouse(); }
    if (evt.RightUp()) { m_rmb = false; if (HasCapture()) ReleaseMouse(); }

    if (evt.Dragging() && m_lmb && !HasCapture())
        CaptureMouse();

    // Only act on motion/drag
    if (!evt.Moving() && !evt.Dragging()) {
        evt.Skip();
        return;
    }

    const wxPoint pos = evt.GetPosition();

    if (!m_hasLast) {
        m_lastPos = pos;
        m_hasLast = true;
        evt.Skip();
        return;
    }

    const float dx = float(pos.x - m_lastPos.x);
    const float dy = float(pos.y - m_lastPos.y);
    m_lastPos = pos;

    // Modifiers
    const bool shift = evt.ShiftDown();
    const bool ctrl = evt.ControlDown();

    // CAD-ish controls:
    // - MMB drag => pan
    // - LMB drag => orbit (shift => pan, ctrl => dolly)
    // - RMB drag (optional) => dolly
    if (m_mmb) {
        // Pan: many CAD apps invert dy so dragging up moves scene up
        m_camera.Pan(dx, -dy);
        Refresh(false);
    }
    else if (m_lmb) 
    {

        if (shift) {
            m_camera.Pan(dx, -dy);
        }
        else if (ctrl) {
            // Use vertical motion as dolly input
            m_camera.Dolly(dy * 0.05f);
        }
        else {
            // Orbit: invert dy if you want natural feel
            m_camera.Orbit(dx, dy);
        }
        Refresh(false);
    }
    else if (m_rmb) {
        // Optional: RMB dolly
        m_camera.Dolly(dy * 0.05f);
        Refresh(false);
    }

    evt.Skip();
}

void GLCanvas::OnMouseWheel(wxMouseEvent& evt)
{
    // wheel rotation is in "notches" scaled by delta
    const int rot = evt.GetWheelRotation();
    const int delta = evt.GetWheelDelta();
    if (delta == 0) return;

    const float steps = float(rot) / float(delta); // typically +/-1 per notch

    // If direction feels backwards, flip the sign here
    m_camera.Dolly(steps);

    Refresh(false);
}

void GLCanvas::DestroyPickFBO()
{
    if (m_pickDepthRb) { glDeleteRenderbuffers(1, &m_pickDepthRb); m_pickDepthRb = 0; }
    if (m_pickColorTex) { glDeleteTextures(1, &m_pickColorTex);     m_pickColorTex = 0; }
    if (m_pickFBO) { glDeleteFramebuffers(1, &m_pickFBO);      m_pickFBO = 0; }
    m_pickW = 0;
    m_pickH = 0;
}

void GLCanvas::EnsurePickFBO(int w, int h)
{
    w = std::max(1, w);
    h = std::max(1, h);

    // Already correct size
    if (m_pickFBO && w == m_pickW && h == m_pickH)
        return;

    // Recreate
    DestroyPickFBO();

    m_pickW = w;
    m_pickH = h;

    glGenFramebuffers(1, &m_pickFBO);
    glBindFramebuffer(GL_FRAMEBUFFER, m_pickFBO);

    // Integer ID color texture (1 uint per pixel)
    glGenTextures(1, &m_pickColorTex);
    glBindTexture(GL_TEXTURE_2D, m_pickColorTex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST); // important: no filtering
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glTexImage2D(
        GL_TEXTURE_2D,
        0,
        GL_R32UI,                 // internal format
        m_pickW, m_pickH,
        0,
        GL_RED_INTEGER,           // format must be *_INTEGER for integer textures
        GL_UNSIGNED_INT,
        nullptr
    );

    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_pickColorTex, 0);

    // Depth+stencil renderbuffer (so picking respects depth)
    glGenRenderbuffers(1, &m_pickDepthRb);
    glBindRenderbuffer(GL_RENDERBUFFER, m_pickDepthRb);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, m_pickW, m_pickH);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, m_pickDepthRb);

    // Single draw buffer
    const GLenum drawBufs[1] = { GL_COLOR_ATTACHMENT0 };
    glDrawBuffers(1, drawBufs);

    const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    if (status != GL_FRAMEBUFFER_COMPLETE)
    {
        wxLogError("Picking FBO incomplete (status=0x%X). Picking disabled.", (unsigned)status);
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

    // wx: (0,0) top-left, OpenGL readback: (0,0) bottom-left
    const int x = mouseX;
    const int y = (m_pickH - 1) - mouseY;

    if (x < 0 || y < 0 || x >= m_pickW || y >= m_pickH)
        return 0;

    glBindFramebuffer(GL_FRAMEBUFFER, m_pickFBO);
    glViewport(0, 0, m_pickW, m_pickH);

    // Clear ID to 0 and depth
    GLuint clearId = 0;
    glClearBufferuiv(GL_COLOR, 0, &clearId);
    glClear(GL_DEPTH_BUFFER_BIT);

    glEnable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);

    glUseProgram(m_pickProgram);

    glm::mat4 model(1.0f);

    m_camera.SetAspect(float(w) / float(h));
    glm::mat4 view = m_camera.View();
    glm::mat4 proj = m_camera.Projection();

    glm::mat4 mvp = proj * view * model;
    glUniformMatrix4fv(m_pick_uMVP, 1, GL_FALSE, &mvp[0][0]);

    // Single-object for now
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
    if (!m_pickFBO || !m_pickProgram || m_modelMesh.vao == 0 || m_modelMesh.indexCount <= 0)
        return;

    glBindFramebuffer(GL_FRAMEBUFFER, m_pickFBO);
    glViewport(0, 0, m_pickW, m_pickH);

    GLuint clearId = 0;
    glClearBufferuiv(GL_COLOR, 0, &clearId);
    glClear(GL_DEPTH_BUFFER_BIT);

    glEnable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);

    glUseProgram(m_pickProgram);

    glm::mat4 model(1.0f);

    m_camera.SetAspect(float(w) / float(h));
    glm::mat4 view = m_camera.View();
    glm::mat4 proj = m_camera.Projection();

    glm::mat4 mvp = proj * view * model;

    glUniformMatrix4fv(m_pick_uMVP, 1, GL_FALSE, &mvp[0][0]);
    glUniform1ui(m_pick_uObjectId, 1u);

    glBindVertexArray(m_modelMesh.vao);
    glDrawElements(GL_TRIANGLES, m_modelMesh.indexCount, GL_UNSIGNED_INT, 0);
    glBindVertexArray(0);

    glUseProgram(0);

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, w, h);
}
