#pragma once

#include <glad/glad.h>
#include <wx/glcanvas.h>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <opencascade/TopoDS.hxx>
#include <opencascade/TopoDS_Face.hxx>
#include <opencascade/TopExp_Explorer.hxx>
#include <opencascade/BRep_Tool.hxx>
#include <opencascade/Poly_Triangulation.hxx>
#include <opencascade/TopLoc_Location.hxx>
#include <opencascade/gp_Pnt.hxx>
#include <opencascade/gp_Trsf.hxx>
#include <opencascade/BRepBuilderAPI_Transform.hxx>
#include <opencascade/STEPControl_Reader.hxx>
#include <opencascade/STEPControl_Writer.hxx>
#include <opencascade/IFSelect_ReturnStatus.hxx>
#include <opencascade/TopoDS_Shape.hxx>

#include "camera.h"
#include "FileImporter.h"
#include "GridRenderer.h"
#include "shaders.h"
#include "MainFrame.h"
#include "MouldFeature.h"

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
    GPUMesh    mesh;
    ObjectRole role = ObjectRole::Imported;
    std::string sourcePath;
    TopoDS_Shape mouldShape;
    bool         hasMould = false;

    // CPU-side geometry for ray casting (position-only, object space)
    std::vector<float>    cpuVerts;    // 3 floats per vertex
    std::vector<uint32_t> cpuIndices;  // triangle indices

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
    TransformMode GetTransformMode() const { return m_transformMode; }

    void GenerateMould();
    void ExportFixtures(const std::string& pathA, const std::string& pathB);

    void ClearFixtures();

    // Vent point placement
    const std::vector<VentPoint>& GetVentPoints() const { return m_ventPoints; }
    void ClearVentPoints();

    // Sprue placement
    void SetActiveInjectionPoint(const InjectionPoint& ip);
    void PlaceSprue();
    void ClearSprue();
    bool IsDirectInjection() const { return m_isDirectInjection; }

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

    int  PickObjectAt(int mouseX, int mouseY);
    void RenderPickPass_NoRead(int w, int h);

    // Vent point ray casting
    bool RayCastObjects(int mouseX, int mouseY,
        glm::vec3& outPos, glm::vec3& outNormal);

    // Parting-plane snap: finds closest point on the mesh's y=0 intersection
    bool RayCastParting(int mouseX, int mouseY,
        glm::vec3& outPos, glm::vec3& outNormal);

    // Vent path computation and GPU upload
    VentPath         ComputeVentPath(const VentPoint& vp);
    void             RebuildPathVBO();

    // Vent cross-section geometry
    VentCrossSection BuildVentCrossSection(const VentPath& path,
        float width, float depth);
    void             RebuildCrossSectionVBO();

    // Vent solid (swept mesh)
    VentSolid BuildVentSolid(const VentPath& path, float width, float depth,
        float overrunStart = 0.0f, float overrunEnd = 0.0f);

    // World-space ray cast against imported objects (no mouse unprojection).
    // Fires from 'origin' along 'dir' up to 'maxDist' world units.
    // Returns true and fills outPos with the closest hit; outPos is undefined on miss.
    bool RayCastWorldRay(const glm::vec3& origin, const glm::vec3& dir,
        float maxDist, glm::vec3& outPos);

    // Sprue path GPU upload
    void RebuildSpruePathVBO();

    // Sprue cross-section circle GPU upload (N-segment line-loop approximation)
    void RebuildSprueXsecVBO();

    // Sprue solid — swept cylinder mesh (same GPU layout as VentSolid)
    VentSolid BuildSprueSolid(const glm::vec3& start, const glm::vec3& end,
        float radius, int segments = 32);

    // Fixture outer perimeter on the parting plane (convex hull in XZ)
    void                   BuildFixturePerimeter();
    std::vector<glm::vec2> m_fixturePerimeter;   // hull vertices in CCW order

    // Sphere mesh for vent point markers
    void BuildSphereGPU(float radius, int stacks, int slices);

private:
    wxGLContext* m_context = nullptr;
    bool         m_inited = false;

    OrbitCamera  m_camera;
    GridRenderer m_grid;
    shaders      m_shaders;

    // Scene
    std::vector<SceneObject> m_fixtures;
    std::vector<SceneObject> m_objects;
    int                      m_selectedIndex = -1;

    // Vent placement points
    std::vector<VentPoint>        m_ventPoints;
    std::vector<VentPath>         m_ventPaths;          // parallel to m_ventPoints
    std::vector<VentCrossSection> m_ventCrossSections;  // parallel to m_ventPoints
    std::vector<VentSolid>        m_ventSolids;         // parallel to m_ventPoints

    // Sprue state
    InjectionPoint m_activeInjectionPoint;         // set from fixture on load
    bool           m_hasActiveInjectionPoint = false;
    bool           m_isDirectInjection = false;    // true when sprue ray hits a body directly
    float          m_sprueRadius = 2.5f;           // radius of the sprue cylinder (mm)
    glm::vec3      m_sprueWorldPos{ 0.0f };        // world-space position of placed sphere
    bool           m_hasSpruePoint = false;
    glm::vec3      m_spruePathStart{ 0.0f };       // injection point (= m_sprueWorldPos)
    glm::vec3      m_spruePathEnd{ 0.0f };         // object hit or y=0 projection
    VentSolid      m_sprueSolid;                   // swept cylinder mesh for preview

    // Ghost preview for vent placement (follows mouse in PlaceVent mode)
    VentPoint m_ventGhost;
    bool      m_ventGhostActive = false;
    wxPoint   m_ghostMousePos;          // last known cursor pos, ray cast deferred to OnPaint

    // Fallback test geometry (pyramid)
    unsigned int m_vao = 0;
    unsigned int m_vbo = 0;
    unsigned int m_ebo = 0;

    // Shader programs
    unsigned int m_program = 0;
    GLuint       m_pickProgram = 0;
    GLuint       m_outlineProgram = 0;

    // Sphere GPU resources (vent point markers)
    GLuint  m_sphereVAO = 0;
    GLuint  m_sphereVBO = 0;
    GLuint  m_sphereEBO = 0;
    GLsizei m_sphereIndexCount = 0;

    // Vent path line GPU resources
    GLuint  m_pathVAO = 0;
    GLuint  m_pathVBO = 0;
    GLsizei m_pathVertexCount = 0;

    // Sprue path line GPU resources
    GLuint  m_spruePathVAO = 0;
    GLuint  m_spruePathVBO = 0;
    GLsizei m_spruePathVertexCount = 0;

    // Sprue cross-section circle GPU resources
    GLuint  m_sprueXsecVAO = 0;
    GLuint  m_sprueXsecVBO = 0;
    GLsizei m_sprueXsecVertexCount = 0;

    // Vent cross-section GPU resources
    GLuint  m_xsecVAO = 0;
    GLuint  m_xsecVBO = 0;
    GLsizei m_xsecVertexCount = 0;

    // Flat (unlit) shader for vent path lines
    GLuint m_flatProgram = 0;
    GLint  m_flat_uVP = -1;
    GLint  m_flat_uColor = -1;

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
