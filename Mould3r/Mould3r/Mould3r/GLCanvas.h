#pragma once

#include <glad/glad.h>

#include <wx/glcanvas.h>

#include "camera.h"
#include "FileImporter.h"
#include "GridRenderer.h"
#include "shaders.h"

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

private:
    wxGLContext* m_context = nullptr;
    bool m_inited = false;

    OrbitCamera m_camera;
    GPUMesh m_modelMesh;
    GridRenderer m_grid;
    shaders m_shaders;

    // GL objects
    unsigned int m_vao = 0;
    unsigned int m_vbo = 0;
    unsigned int m_ebo = 0;
    unsigned int m_program = 0;

    //Mouse states
    bool m_lmb = false;
    bool m_mmb = false;
    bool m_rmb = false;

    bool m_hasLast = false;
    wxPoint m_lastPos;

//All selection members here
private:
    // Picking FBO
    GLuint m_pickFBO = 0;
    GLuint m_pickColorTex = 0;
    GLuint m_pickDepthRb = 0;
    int    m_pickW = 0;
    int    m_pickH = 0;

    // Selection state
    uint32_t m_selectedObjectId = 0;

    // Picking shader (simple program that outputs uint ID)
    GLuint m_pickProgram = 0;
    GLint  m_pick_uMVP = -1;
    GLint  m_pick_uObjectId = -1;

    // Outline program + fullscreen VAO
    GLuint m_outlineProgram = 0;
    GLint  m_outline_uIdTex = -1;
    GLint  m_outline_uTargetId = -1;
    GLint  m_outline_uTexSize = -1;
    GLint  m_outline_uAlpha = -1;
    GLint  m_outline_uThickness = -1;

    GLuint m_fullscreenVAO = 0; // empty VAO for gl_VertexID fullscreen triangle

private:
    void RenderPickPass_NoRead(int w, int h);

};
