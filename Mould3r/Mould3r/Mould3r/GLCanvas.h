#pragma once

#include <glad/glad.h>
#include <wx/glcanvas.h>

#include "camera.h"
#include "FileImporter.h"
#include "GridRenderer.h"
#include "shaders.h"
#include "MainFrame.h"   // TransformMode enum

struct GPUMesh
{
    GLuint vao = 0;
    GLuint vbo = 0;
    GLuint ebo = 0;
    GLsizei indexCount = 0;

    void Destroy()
    {
        if (ebo) { glDeleteBuffers(1, &ebo); ebo = 0; }
        if (vbo) { glDeleteBuffers(1, &vbo); vbo = 0; }
        if (vao) { glDeleteVertexArrays(1, &vao); vao = 0; }
        indexCount = 0;
    }
};

class GLCanvas : public wxGLCanvas
{
public:
    GLCanvas(wxWindow* parent);
    ~GLCanvas() override;

    void ImportStepFile(const std::string& path);

    // Called by MainFrame ribbon buttons
    void SetTransformMode(TransformMode mode);

    // Called by dialogs
    void ApplyRotation(float xDeg, float yDeg, float zDeg);
    void ApplyTranslation(float x, float y, float z);
    void ApplyScale(float factor);
    bool HasSelection() const { return m_selectedObjectId != 0; }
private:
    void OnPaint(wxPaintEvent& evt);
    void OnResize(wxSizeEvent& evt);

    // Input
    void OnMouse(wxMouseEvent& evt);
    void OnMouseWheel(wxMouseEvent& evt);

    void InitGLOnce();
    void DestroyGL();

    void UploadMeshToGPU(const FileImporter::MeshData& mesh, GPUMesh* reuse);

    void EnsurePickFBO(int w, int h);
    void DestroyPickFBO();

    uint32_t PickObjectAt(int mouseX, int mouseY);

    // Build the model matrix from current transform state
    glm::mat4 BuildModelMatrix() const;

    // Apply mouse delta to current transform mode
    void ApplyTransformDelta(float dx, float dy);

private:
    wxGLContext* m_context = nullptr;
    bool m_inited = false;

    OrbitCamera m_camera;
    GPUMesh m_modelMesh;
    GridRenderer m_grid;
    shaders m_shaders;

    // GL objects (legacy test pyramid – kept for fallback)
    unsigned int m_vao = 0;
    unsigned int m_vbo = 0;
    unsigned int m_ebo = 0;
    unsigned int m_program = 0;

    // Mouse states
    bool m_lmb = false;
    bool m_mmb = false;
    bool m_rmb = false;

    bool    m_hasLast = false;
    wxPoint m_lastPos;

    // -----------------------------------------------------------------------
    // Transform tool state
    // -----------------------------------------------------------------------
    TransformMode m_transformMode = TransformMode::Select;

    glm::vec3 m_modelPos{ 0.0f, 0.0f, 0.0f };
    float     m_modelYawDeg = 0.0f;   // rotation around world Y
    float     m_modelPitchDeg = 0.0f;   // rotation around local X
    float     m_modelRollDeg = 0.0f;   // rotation around local Z
    float     m_modelScale = 1.0f;

    // -----------------------------------------------------------------------
    // Selection / picking
    // -----------------------------------------------------------------------
    GLuint   m_pickFBO = 0;
    GLuint   m_pickColorTex = 0;
    GLuint   m_pickDepthRb = 0;
    int      m_pickW = 0;
    int      m_pickH = 0;

    uint32_t m_selectedObjectId = 0;

    GLuint m_pickProgram = 0;
    GLint  m_pick_uMVP = -1;
    GLint  m_pick_uObjectId = -1;

    GLuint m_outlineProgram = 0;
    GLint  m_outline_uIdTex = -1;
    GLint  m_outline_uTargetId = -1;
    GLint  m_outline_uTexSize = -1;
    GLint  m_outline_uAlpha = -1;
    GLint  m_outline_uThickness = -1;

    GLuint m_fullscreenVAO = 0;

private:
    void RenderPickPass_NoRead(int w, int h);
};
