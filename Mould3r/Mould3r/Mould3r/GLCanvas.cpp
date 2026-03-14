#define _CRT_SECURE_NO_WARNINGS

#include <glad/glad.h>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include "GLCanvas.h"
#include <wx/dcclient.h>
#include <wx/log.h>
#include <wx/msgdlg.h>

#include <opencascade/STEPControl_Reader.hxx>
#include <opencascade/STEPControl_Writer.hxx>
#include <opencascade/BRepBuilderAPI_Transform.hxx>
#include <opencascade/gp_Trsf.hxx>
#include <opencascade/gp_Mat.hxx>
#include <opencascade/gp_XYZ.hxx>

#include <algorithm>
#include <string>
#include <vector>

#include "camera.h"
#include "FileImporter.h"
#include "GridRenderer.h"
#include "shaders.h"
#include "MeshUtils.h"
#include "MeshOps.h"

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
    WX_GL_RGBA, WX_GL_DOUBLEBUFFER,
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
    : wxGLCanvas(parent, wxID_ANY, glArgs,
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
    m_transformMode = mode;
    switch (mode)
    {
    case TransformMode::Select:
        SetCursor(wxCursor(wxCURSOR_ARROW));   break;
    case TransformMode::Translate:
        SetCursor(wxCursor(wxCURSOR_SIZING));  break;
    case TransformMode::Rotate:
        SetCursor(wxCursor(wxCURSOR_CROSS));   break;
    case TransformMode::Scale:
        SetCursor(wxCursor(wxCURSOR_SIZENS));  break;
    }
}

// ---------------------------------------------------------------------------
// Transform methods — operate on selected object
// ---------------------------------------------------------------------------
void GLCanvas::ApplyRotation(float xDeg, float yDeg, float zDeg)
{
    if (!HasSelection()) return;
    m_objects[m_selectedIndex].pitchDeg += xDeg;
    m_objects[m_selectedIndex].yawDeg += yDeg;
    m_objects[m_selectedIndex].rollDeg += zDeg;
    Refresh(false);
}

void GLCanvas::ApplyTranslation(float x, float y, float z)
{
    if (!HasSelection()) return;
    m_objects[m_selectedIndex].pos += glm::vec3(x, y, z);
    Refresh(false);
}

void GLCanvas::ApplyScale(float factor)
{
    if (!HasSelection()) return;
    m_objects[m_selectedIndex].scale =
        std::max(0.001f, m_objects[m_selectedIndex].scale * factor);
    Refresh(false);
}

void GLCanvas::CenterSelectedObject()
{
    if (!HasSelection()) return;
    m_objects[m_selectedIndex].pos = glm::vec3(0.0f);
    Refresh(false);
}

// ---------------------------------------------------------------------------
// GL init
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

    m_inited = true;
}

void GLCanvas::DestroyGL()
{
    for (auto& obj : m_fixtures) obj.mesh.Destroy();
    m_fixtures.clear();
    for (auto& obj : m_objects)  obj.mesh.Destroy();
    m_objects.clear();

    if (m_outlineProgram) { glDeleteProgram(m_outlineProgram);        m_outlineProgram = 0; }
    if (m_fullscreenVAO) { glDeleteVertexArrays(1, &m_fullscreenVAO); m_fullscreenVAO = 0; }
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
        if (fix.sourcePath.empty() || outPaths[i].empty()) continue;

        // Re-read original STEP
        STEPControl_Reader reader;
        if (reader.ReadFile(fix.sourcePath.c_str()) != IFSelect_RetDone)
        {
            wxMessageBox("Failed to re-read: " + fix.sourcePath,
                "Export Failed", wxOK | wxICON_ERROR, this);
            continue;
        }
        reader.TransferRoots();
        TopoDS_Shape shape = reader.OneShape();
        if (shape.IsNull()) continue;

        // Build OCC transform from SceneObject state
        glm::mat4 m = fix.BuildModelMatrix();

        gp_Mat occMat(
            m[0][0], m[1][0], m[2][0],
            m[0][1], m[1][1], m[2][1],
            m[0][2], m[1][2], m[2][2]
        );
        gp_XYZ occTrans(m[3][0], m[3][1], m[3][2]);

        gp_Trsf trsf;
        trsf.SetValues(
            m[0][0], m[1][0], m[2][0], m[3][0],
            m[0][1], m[1][1], m[2][1], m[3][1],
            m[0][2], m[1][2], m[2][2], m[3][2]
        );

        BRepBuilderAPI_Transform xform(shape, trsf, true);
        TopoDS_Shape transformed = xform.Shape();

        // Write STEP
        STEPControl_Writer writer;
        writer.Transfer(transformed, STEPControl_AsIs);
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
// Import — appends a new SceneObject
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

    ComputeVertexNormals_Pos3(res.meshes[0].vertices,
        res.meshes[0].indices,
        res.meshes[0].posNorm);

    auto split = SplitByCreaseAngle_Pos3(res.meshes[0].vertices,
        res.meshes[0].indices, 35.0f);
    res.meshes[0].posNorm = std::move(split.posNorm);
    res.meshes[0].indices = std::move(split.indices);

    // Append a fresh SceneObject
    m_objects.emplace_back();
    UploadMeshToGPU(res.meshes[0], m_objects.back());

    Refresh(false);
}

void GLCanvas::ImportStepFileAsFixture(const std::string& path)
{
    SetCurrent(*m_context);
    InitGLOnce();

    FileImporter importer;
    auto res = importer.ImportSTEP(path, 0.05, 0.5);

    if (!res.ok()) {
        wxMessageBox(res.error, "Import failed", wxOK | wxICON_ERROR, this);
        return;
    }

    ComputeVertexNormals_Pos3(res.meshes[0].vertices,
        res.meshes[0].indices,
        res.meshes[0].posNorm);

    auto split = SplitByCreaseAngle_Pos3(res.meshes[0].vertices,
        res.meshes[0].indices, 35.0f);
    res.meshes[0].posNorm = std::move(split.posNorm);
    res.meshes[0].indices = std::move(split.indices);

    m_fixtures.emplace_back();
    m_fixtures.back().role = ObjectRole::Fixture;
    m_fixtures.back().sourcePath = path;   // store for export
    UploadMeshToGPU(res.meshes[0], m_fixtures.back());

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

    glDisable(GL_BLEND);

    // Restore alpha for regular objects
    glUniform1f(glGetUniformLocation(m_program, "uAlpha"), 1.0f);


    glUseProgram(0);

    m_grid.Draw(view, proj);

    // Outline for selected object
    if (m_selectedIndex >= 0 && m_selectedIndex < (int)m_objects.size())
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

            const uint32_t targetId = (uint32_t)(m_selectedIndex + 1);
            glUniform1i(m_outline_uIdTex, 0);
            glUniform1ui(m_outline_uTargetId, targetId);
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

    SwapBuffers();
}

// ---------------------------------------------------------------------------
// Mouse
// ---------------------------------------------------------------------------
void GLCanvas::OnMouse(wxMouseEvent& evt)
{
    if (evt.LeftDown())
    {
        m_lmb = true;
        m_hasLast = false;

        if (m_transformMode == TransformMode::Select)
        {
            const wxPoint p = evt.GetPosition();
            m_selectedIndex = PickObjectAt(p.x, p.y);
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

    const wxPoint pos = evt.GetPosition();
    if (!m_hasLast) { m_lastPos = pos; m_hasLast = true; evt.Skip(); return; }

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
        if (m_selectedIndex >= 0)
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

            m_objects[m_selectedIndex].pos += right * (dx * unitsPerPx);
            m_objects[m_selectedIndex].pos += forward * (-dyAdjusted * unitsPerPx);

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
    if (evt.GetKeyCode() == WXK_DELETE && m_selectedIndex >= 0)
    {
        m_objects[m_selectedIndex].mesh.Destroy();
        m_objects.erase(m_objects.begin() + m_selectedIndex);
        m_selectedIndex = -1;
        Refresh(false);
    }
    else
    {
        evt.Skip();
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