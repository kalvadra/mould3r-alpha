#pragma once

#include <glad/glad.h>
#include <wx/glcanvas.h>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include "camera.h"
#include "FileImporter.h"
#include "GridRenderer.h"
#include "shaders.h"
#include "MainFrame.h"

struct GPUMesh
{
    GLuint vao = 0;
    GLuint vbo = 0;
    GLuint ebo = 0;
    GLsizei indexCount = 0;

    void Destroy()
    {
        if (ebo) { glDeleteBuffers(1, &ebo);         ebo = 0; }
        if (vbo) { glDeleteBuffers(1, &vbo);         vbo = 0; }
        if (vao) { glDeleteVertexArrays(1, &vao);    vao = 0; }
        indexCount = 0;
    }
};

enum class ObjectRole { Fixture, Imported };

struct SceneObject
{
    GPUMesh   mesh;
    ObjectRole role = ObjectRole::Imported;
    std::string sourcePath;   // original file path, used for STEP export

    glm::vec3 pos{ 0.0f, 0.0f, 0.0f };
    float     yawDeg = 0.0f;
    float     pitchDeg = 0.0f;
    float     rollDeg = 0.0f;
    float     scale = 1.0f;

    glm::mat4 BuildModelMatrix() const
    {
        glm::mat4 T = glm::translate(glm::mat4(1.0f), pos);
        glm::mat4 RY = glm::rotate(glm::mat4(1.0f), glm::radians(yawDeg), glm::vec3(0, 1, 0));
        glm::mat4 RX = glm::rotate(glm::mat4(1.0f), glm::radians(pitchDeg), glm::vec3(1, 0, 0));
        glm::mat4 RZ = glm::rotate(glm::mat4(1.0f), glm::radians(rollDeg), glm::vec3(0, 0, 1));
        glm::mat4 S = glm::scale(glm::mat4(1.0f), glm::vec3(scale));
        return T * RY * RX * RZ * S;
    }
};

class GLCanvas : public wxGLCanvas
{
public:
    GLCanvas(wxWindow* parent);
    ~GLCanvas() override;

    void ImportStepFile(const std::string& path);
    void ImportStepFileAsFixture(const std::string& path);

    // Called by MainFrame ribbon buttons
    void SetTransformMode(TransformMode mode);

    // Called by dialogs
    void ApplyRotation(float xDeg, float yDeg, float zDeg);
    void ApplyTranslation(float x, float y, float z);
    void ApplyScale(float factor);
    void CenterSelectedObject();

    bool HasSelection() const { return m_selectedIndex >= 0; }

    void ExportFixtures(const std::string& pathA, const std::string& pathB);

private:
    void OnPaint(wxPaintEvent& evt);
    void OnResize(wxSizeEvent& evt);

    void OnMouse(wxMouseEvent& evt);
    void OnMouseWheel(wxMouseEvent& evt);

    void OnKeyDown(wxKeyEvent& evt);

    void InitGLOnce();
    void DestroyGL();

    void UploadMeshToGPU(const FileImporter::MeshData& mesh, SceneObject& obj);

    void EnsurePickFBO(int w, int h);
    void DestroyPickFBO();

    int  PickObjectAt(int mouseX, int mouseY);   // returns index, -1 = miss

    void RenderPickPass_NoRead(int w, int h);

private:
    wxGLContext* m_context = nullptr;
    bool         m_inited = false;

    OrbitCamera  m_camera;
    GridRenderer m_grid;
    shaders      m_shaders;

    // Scene
    std::vector<SceneObject> m_fixtures;    // Model A + B from startup
    std::vector<SceneObject> m_objects;
    int                      m_selectedIndex = -1;

    // Fallback test geometry (pyramid)
    unsigned int m_vao = 0;
    unsigned int m_vbo = 0;
    unsigned int m_ebo = 0;

    // Shader programs
    unsigned int m_program = 0;
    GLuint       m_pickProgram = 0;
    GLuint       m_outlineProgram = 0;

    // Uniform locations — picking
    GLint m_pick_uMVP = -1;
    GLint m_pick_uObjectId = -1;

    // Uniform locations — outline
    GLint m_outline_uIdTex = -1;
    GLint m_outline_uTargetId = -1;
    GLint m_outline_uTexSize = -1;
    GLint m_outline_uAlpha = -1;
    GLint m_outline_uThickness = -1;

    GLuint m_fullscreenVAO = 0;

    // Mouse state
    bool    m_lmb = false;
    bool    m_mmb = false;
    bool    m_rmb = false;
    bool    m_hasLast = false;
    wxPoint m_lastPos;

    // Transform mode
    TransformMode m_transformMode = TransformMode::Select;

    // Picking FBO
    GLuint m_pickFBO = 0;
    GLuint m_pickColorTex = 0;
    GLuint m_pickDepthRb = 0;
    int    m_pickW = 0;
    int    m_pickH = 0;
};