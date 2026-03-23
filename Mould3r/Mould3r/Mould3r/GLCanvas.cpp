#define _CRT_SECURE_NO_WARNINGS

#include <glad/glad.h>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include "GLCanvas.h"
#include <wx/dcclient.h>
#include <wx/log.h>
#include <wx/msgdlg.h>
#include <wx/progdlg.h>

#include <opencascade/STEPControl_Reader.hxx>
#include <opencascade/STEPControl_Writer.hxx>
#include <opencascade/BRepBuilderAPI_Transform.hxx>
#include <opencascade/gp_Trsf.hxx>
#include <opencascade/gp_Mat.hxx>
#include <opencascade/gp_XYZ.hxx>
#include <opencascade/BRepAlgoAPI_Cut.hxx>
#include <opencascade/BRepMesh_IncrementalMesh.hxx>
#include <opencascade/BRepBuilderAPI_MakeEdge.hxx>
#include <opencascade/BRepBuilderAPI_MakeWire.hxx>
#include <opencascade/BRepBuilderAPI_MakeFace.hxx>
#include <opencascade/BRepPrimAPI_MakePrism.hxx>
#include <opencascade/gp_Vec.hxx>

#include <algorithm>
#include <array>
#include <string>
#include <vector>

#include "camera.h"
#include "FileImporter.h"
#include "GridRenderer.h"
#include "shaders.h"
#include "MeshUtils.h"
#include "MeshOps.h"
#include "MouldFeature.h"

// Radius of the green sphere drawn at each vent placement point (world units)
static constexpr float kVentMarkerRadius = 1.5f;

// How close (world units) the cursor's parting-plane hit must be to a parting
// segment before it snaps — gives a generous click target on the parting line.
static constexpr float kVentSnapRadius = 8.0f;

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
        SetCursor(wxCursor(wxCURSOR_ARROW));    break;
    case TransformMode::Translate:
        SetCursor(wxCursor(wxCURSOR_SIZING));   break;
    case TransformMode::Rotate:
        SetCursor(wxCursor(wxCURSOR_CROSS));    break;
    case TransformMode::Scale:
        SetCursor(wxCursor(wxCURSOR_SIZENS));   break;
    case TransformMode::PlaceVent:
        SetCursor(wxCursor(wxCURSOR_CROSS));    break;
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
    for (auto& s : m_ventSolids) s.Destroy(); m_ventSolids.clear(); m_ventPoints.clear(); m_ventPaths.clear(); m_ventCrossSections.clear(); RebuildPathVBO(); RebuildCrossSectionVBO();
    Refresh(false);
}

void GLCanvas::ApplyTranslation(float x, float y, float z)
{
    if (!HasSelection()) return;
    m_objects[m_selectedIndex].pos += glm::vec3(x, y, z);
    for (auto& s : m_ventSolids) s.Destroy(); m_ventSolids.clear(); m_ventPoints.clear(); m_ventPaths.clear(); m_ventCrossSections.clear(); RebuildPathVBO(); RebuildCrossSectionVBO();
    Refresh(false);
}

void GLCanvas::ApplyScale(float factor)
{
    if (!HasSelection()) return;
    m_objects[m_selectedIndex].scale =
        std::max(0.001f, m_objects[m_selectedIndex].scale * factor);
    for (auto& s : m_ventSolids) s.Destroy(); m_ventSolids.clear(); m_ventPoints.clear(); m_ventPaths.clear(); m_ventCrossSections.clear(); RebuildPathVBO(); RebuildCrossSectionVBO();
    Refresh(false);
}

void GLCanvas::CenterSelectedObject()
{
    if (!HasSelection()) return;
    m_objects[m_selectedIndex].pos = glm::vec3(0.0f);
    for (auto& s : m_ventSolids) s.Destroy(); m_ventSolids.clear(); m_ventPoints.clear(); m_ventPaths.clear(); m_ventCrossSections.clear(); RebuildPathVBO(); RebuildCrossSectionVBO();
    Refresh(false);
}

void GLCanvas::ClearVentPoints()
{
    for (auto& s : m_ventSolids) s.Destroy();
    m_ventSolids.clear();
    m_ventPoints.clear();
    m_ventPaths.clear();
    m_ventCrossSections.clear();
    RebuildPathVBO();
    RebuildCrossSectionVBO();
    Refresh(false);
}

// ---------------------------------------------------------------------------
// Generate Mould Operation — Cuts objects, vents, runners, and sprues from blank mold halves
// ---------------------------------------------------------------------------
void GLCanvas::GenerateMould()
{
    if (m_fixtures.empty())
    {
        wxMessageBox("No fixtures loaded.",
            "Generate Mould", wxOK | wxICON_WARNING, this);
        return;
    }
    if (m_objects.empty())
    {
        wxMessageBox("No imported objects to subtract.",
            "Generate Mould", wxOK | wxICON_WARNING, this);
        return;
    }

    // Steps per fixture: 1 read + 1 transform + (1 per object subtract) + (1 per vent subtract) + 1 tessellate + 1 upload
    const int stepsPerFixture = 3 + (int)m_objects.size() + (int)m_ventCrossSections.size();
    const int totalSteps = stepsPerFixture * (int)m_fixtures.size();

    wxProgressDialog progress(
        "Generating Mould",
        "Initialising...",
        totalSteps,
        nullptr,
        wxPD_APP_MODAL | wxPD_AUTO_HIDE | wxPD_SMOOTH | wxPD_ELAPSED_TIME
    );

    int step = 0;

    SetCurrent(*m_context);
    InitGLOnce();

    for (int fi = 0; fi < (int)m_fixtures.size(); ++fi)
    {
        SceneObject& fix = m_fixtures[fi];
        if (fix.sourcePath.empty()) continue;

        const std::string fixLabel = "Fixture " + std::to_string(fi + 1);

        // Step: read fixture
        progress.Update(step++, fixLabel + ": reading source file...");

        STEPControl_Reader fixReader;
        if (fixReader.ReadFile(fix.sourcePath.c_str()) != IFSelect_RetDone)
        {
            wxMessageBox("Failed to re-read fixture: " + fix.sourcePath,
                "Generate Mould", wxOK | wxICON_ERROR, this);
            step += stepsPerFixture - 1;  // skip remaining steps for this fixture
            continue;
        }
        fixReader.TransferRoots();
        TopoDS_Shape fixtureShape = fixReader.OneShape();
        if (fixtureShape.IsNull()) continue;

        // Step: apply fixture transform
        progress.Update(step++, fixLabel + ": applying transform...");

        gp_Trsf fixTrsf;
        glm::mat4 fm = fix.BuildModelMatrix();
        fixTrsf.SetValues(
            fm[0][0], fm[1][0], fm[2][0], fm[3][0],
            fm[0][1], fm[1][1], fm[2][1], fm[3][1],
            fm[0][2], fm[1][2], fm[2][2], fm[3][2]
        );
        BRepBuilderAPI_Transform fixXform(fixtureShape, fixTrsf, true);
        TopoDS_Shape result = fixXform.Shape();

        // Steps: subtract each object
        for (int oi = 0; oi < (int)m_objects.size(); ++oi)
        {
            const SceneObject& obj = m_objects[oi];
            progress.Update(step++,
                fixLabel + ": subtracting object " +
                std::to_string(oi + 1) + " of " +
                std::to_string((int)m_objects.size()) + "...");

            if (obj.sourcePath.empty()) continue;

            STEPControl_Reader objReader;
            if (objReader.ReadFile(obj.sourcePath.c_str()) != IFSelect_RetDone)
                continue;
            objReader.TransferRoots();
            TopoDS_Shape objShape = objReader.OneShape();
            if (objShape.IsNull()) continue;

            gp_Trsf objTrsf;
            glm::mat4 om = obj.BuildModelMatrix();
            objTrsf.SetValues(
                om[0][0], om[1][0], om[2][0], om[3][0],
                om[0][1], om[1][1], om[2][1], om[3][1],
                om[0][2], om[1][2], om[2][2], om[3][2]
            );
            BRepBuilderAPI_Transform objXform(objShape, objTrsf, true);
            TopoDS_Shape transformedObj = objXform.Shape();

            BRepAlgoAPI_Cut cut(result, transformedObj);
            cut.Build();
            if (!cut.IsDone() || cut.Shape().IsNull())
            {
                wxMessageBox("Boolean subtract failed for object " +
                    std::to_string(oi + 1),
                    "Generate Mould", wxOK | wxICON_WARNING, this);
                continue;
            }
            result = cut.Shape();
        }

        // Steps: subtract each vent
        for (int vi = 0; vi < (int)m_ventCrossSections.size(); ++vi)
        {
            const VentCrossSection& xs = m_ventCrossSections[vi];
            const VentPath& vp = m_ventPaths[vi];

            progress.Update(step++,
                fixLabel + ": cutting vent " +
                std::to_string(vi + 1) + " of " +
                std::to_string((int)m_ventCrossSections.size()) + "...");

            if (!xs.valid || !vp.valid) continue;

            // Build a planar wire from the 4 cross-section corners
            auto toOCC = [](const glm::vec3& v) { return gp_Pnt(v.x, v.y, v.z); };

            const gp_Pnt p0 = toOCC(xs.corners[0]);
            const gp_Pnt p1 = toOCC(xs.corners[1]);
            const gp_Pnt p2 = toOCC(xs.corners[2]);
            const gp_Pnt p3 = toOCC(xs.corners[3]);

            BRepBuilderAPI_MakeEdge e0(p0, p1);
            BRepBuilderAPI_MakeEdge e1(p1, p2);
            BRepBuilderAPI_MakeEdge e2(p2, p3);
            BRepBuilderAPI_MakeEdge e3(p3, p0);

            if (!e0.IsDone() || !e1.IsDone() || !e2.IsDone() || !e3.IsDone()) continue;

            BRepBuilderAPI_MakeWire wire;
            wire.Add(e0.Edge());
            wire.Add(e1.Edge());
            wire.Add(e2.Edge());
            wire.Add(e3.Edge());
            if (!wire.IsDone()) continue;

            BRepBuilderAPI_MakeFace face(wire.Wire(), /*onlyPlane=*/true);
            if (!face.IsDone()) continue;

            // Extrude face along the vent path vector, extended by the stored overruns.
            // The cross-section sits at path.start; shift it back by overrunStart,
            // then extend the sweep by overrunStart + overrunEnd so both ends clear
            // any surface co-planarity artifacts.
            const glm::vec3 rawSweep = vp.end - vp.start;
            const float     rawLen = glm::length(rawSweep);
            if (rawLen < 1e-6f) continue;
            const glm::vec3 sweepDir = rawSweep / rawLen;

            // Translate the face back by overrunStart before extruding
            const glm::vec3 originOffset = -sweepDir * vp.overrunStart;
            const gp_Trsf   offsetTrsf = [&]() {
                gp_Trsf t;
                t.SetTranslation(gp_Vec(originOffset.x, originOffset.y, originOffset.z));
                return t;
                }();
            const TopoDS_Shape offsetFace =
                BRepBuilderAPI_Transform(face.Face(), offsetTrsf, /*copy=*/true).Shape();

            const float     totalLen = rawLen + vp.overrunStart + vp.overrunEnd;
            const glm::vec3 totalSweep = sweepDir * totalLen;
            const gp_Vec    sweepVec(totalSweep.x, totalSweep.y, totalSweep.z);

            BRepPrimAPI_MakePrism prism(offsetFace, sweepVec);
            if (!prism.IsDone() || prism.Shape().IsNull()) continue;

            BRepAlgoAPI_Cut ventCut(result, prism.Shape());
            ventCut.Build();
            if (!ventCut.IsDone() || ventCut.Shape().IsNull())
            {
                wxMessageBox("Vent cut failed for vent " + std::to_string(vi + 1),
                    "Generate Mould", wxOK | wxICON_WARNING, this);
                continue;
            }
            result = ventCut.Shape();
        }

        // Step: tessellate + upload
        progress.Update(step++, fixLabel + ": tessellating result...");

        fix.mouldShape = result;
        fix.hasMould = true;

        BRepMesh_IncrementalMesh mesher(result, 0.05, false, 0.5, true);

        FileImporter::MeshData meshData;
        meshData.aabbMin = glm::vec3(std::numeric_limits<float>::infinity());
        meshData.aabbMax = glm::vec3(-std::numeric_limits<float>::infinity());

        for (TopExp_Explorer ex(result, TopAbs_FACE); ex.More(); ex.Next())
        {
            const TopoDS_Face face = TopoDS::Face(ex.Current());
            TopLoc_Location loc;
            Handle(Poly_Triangulation) tri = BRep_Tool::Triangulation(face, loc);
            if (tri.IsNull()) continue;

            const gp_Trsf tr = loc.Transformation();
            const uint32_t baseIndex = (uint32_t)(meshData.vertices.size() / 3);

            for (int i = 1; i <= tri->NbNodes(); ++i)
            {
                gp_Pnt p = tri->Node(i);
                p.Transform(tr);
                meshData.vertices.push_back((float)p.X());
                meshData.vertices.push_back((float)p.Y());
                meshData.vertices.push_back((float)p.Z());
            }
            for (int t = 1; t <= tri->NbTriangles(); ++t)
            {
                int n1, n2, n3;
                tri->Triangle(t).Get(n1, n2, n3);
                meshData.indices.push_back(baseIndex + (uint32_t)(n1 - 1));
                meshData.indices.push_back(baseIndex + (uint32_t)(n2 - 1));
                meshData.indices.push_back(baseIndex + (uint32_t)(n3 - 1));
            }
        }

        if (meshData.vertices.empty() || meshData.indices.empty())
            continue;

        ComputeVertexNormals_Pos3(meshData.vertices, meshData.indices, meshData.posNorm);
        auto split = SplitByCreaseAngle_Pos3(meshData.vertices, meshData.indices, 35.0f);
        meshData.posNorm = std::move(split.posNorm);
        meshData.indices = std::move(split.indices);

        fix.pos = glm::vec3(0.0f);
        fix.yawDeg = 0.0f;
        fix.pitchDeg = 0.0f;
        fix.rollDeg = 0.0f;
        fix.scale = 1.0f;

        UploadMeshToGPU(meshData, fix);
    }

    progress.Update(totalSteps, "Done.");
    Refresh(false);
    wxMessageBox("Mould generated successfully.",
        "Generate Mould", wxOK | wxICON_INFORMATION, this);
}

// ---------------------------------------------------------------------------
// BuildSphereGPU — generates a UV sphere and uploads it to the GPU.
// Vertex layout: [pos(3), normal(3)] — compatible with vsLit/fsLit.
// ---------------------------------------------------------------------------
void GLCanvas::BuildSphereGPU(float radius, int stacks, int slices)
{
    std::vector<float>    verts;
    std::vector<uint32_t> idx;

    for (int i = 0; i <= stacks; ++i)
    {
        const float phi = glm::pi<float>() * float(i) / float(stacks);
        const float sinPhi = sinf(phi);
        const float cosPhi = cosf(phi);

        for (int j = 0; j <= slices; ++j)
        {
            const float theta = 2.0f * glm::pi<float>() * float(j) / float(slices);
            const float x = sinPhi * cosf(theta);
            const float y = cosPhi;
            const float z = sinPhi * sinf(theta);

            verts.push_back(x * radius);  verts.push_back(y * radius);  verts.push_back(z * radius);
            verts.push_back(x);           verts.push_back(y);           verts.push_back(z);
        }
    }

    for (int i = 0; i < stacks; ++i)
    {
        for (int j = 0; j < slices; ++j)
        {
            const uint32_t a = uint32_t(i * (slices + 1) + j);
            const uint32_t b = uint32_t(a + slices + 1);
            idx.push_back(a);     idx.push_back(b);     idx.push_back(a + 1);
            idx.push_back(b);     idx.push_back(b + 1); idx.push_back(a + 1);
        }
    }

    glGenVertexArrays(1, &m_sphereVAO);
    glGenBuffers(1, &m_sphereVBO);
    glGenBuffers(1, &m_sphereEBO);

    glBindVertexArray(m_sphereVAO);
    glBindBuffer(GL_ARRAY_BUFFER, m_sphereVBO);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(verts.size() * sizeof(float)), verts.data(), GL_STATIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_sphereEBO);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)(idx.size() * sizeof(uint32_t)), idx.data(), GL_STATIC_DRAW);

    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)(3 * sizeof(float)));
    glEnableVertexAttribArray(1);
    glBindVertexArray(0);

    m_sphereIndexCount = (GLsizei)idx.size();
}

// ---------------------------------------------------------------------------
// RayCastObjects — Möller–Trumbore ray-mesh intersection.
// Tests all triangles of all imported objects (CPU side) and returns the
// closest hit in world space.  Returns false if no surface was hit.
// ---------------------------------------------------------------------------
static bool RayTriangle(const glm::vec3& orig, const glm::vec3& dir,
    const glm::vec3& v0, const glm::vec3& v1, const glm::vec3& v2,
    float& outT)
{
    constexpr float EPS = 1e-7f;
    const glm::vec3 e1 = v1 - v0;
    const glm::vec3 e2 = v2 - v0;
    const glm::vec3 h = glm::cross(dir, e2);
    const float     a = glm::dot(e1, h);
    // a < EPS rejects back-facing triangles (standard front-face-only MT).
    // This prevents hits on the far side of a cavity when the near face is
    // occluded by the fixture (which has no CPU geometry to block the ray).
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

bool GLCanvas::RayCastObjects(int mouseX, int mouseY,
    glm::vec3& outPos, glm::vec3& outNormal)
{
    const wxSize sz = GetClientSize();
    const int    w = std::max(1, sz.x);
    const int    h = std::max(1, sz.y);

    // Build world-space ray from NDC mouse position
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

    float     bestWorldT = std::numeric_limits<float>::max();
    bool      hit = false;

    for (const auto& obj : m_objects)
    {
        if (obj.cpuVerts.empty() || obj.cpuIndices.empty()) continue;

        const glm::mat4 model = obj.BuildModelMatrix();
        const glm::mat4 invModel = glm::inverse(model);

        // Transform ray into object (local) space for intersection test
        const glm::vec3 localOrig = glm::vec3(invModel * glm::vec4(rayOrig, 1.0f));
        const glm::vec3 localDir = glm::normalize(glm::vec3(invModel * glm::vec4(rayDir, 0.0f)));

        for (size_t i = 0; i + 2 < obj.cpuIndices.size(); i += 3)
        {
            const uint32_t i0 = obj.cpuIndices[i];
            const uint32_t i1 = obj.cpuIndices[i + 1];
            const uint32_t i2 = obj.cpuIndices[i + 2];

            const glm::vec3 v0(obj.cpuVerts[i0 * 3], obj.cpuVerts[i0 * 3 + 1], obj.cpuVerts[i0 * 3 + 2]);
            const glm::vec3 v1(obj.cpuVerts[i1 * 3], obj.cpuVerts[i1 * 3 + 1], obj.cpuVerts[i1 * 3 + 2]);
            const glm::vec3 v2(obj.cpuVerts[i2 * 3], obj.cpuVerts[i2 * 3 + 1], obj.cpuVerts[i2 * 3 + 2]);

            float localT = 0.0f;
            if (!RayTriangle(localOrig, localDir, v0, v1, v2, localT))
                continue;

            // Convert local hit back to world space to get a consistent depth
            const glm::vec3 localHit = localOrig + localDir * localT;
            const glm::vec3 worldHit = glm::vec3(model * glm::vec4(localHit, 1.0f));
            const float     worldT = glm::length(worldHit - rayOrig);

            if (worldT < bestWorldT)
            {
                bestWorldT = worldT;
                outPos = worldHit;

                // Face normal → transform to world space
                const glm::vec3 faceN = glm::normalize(glm::cross(v1 - v0, v2 - v0));
                const glm::mat3 normMat = glm::transpose(glm::inverse(glm::mat3(model)));
                outNormal = glm::normalize(normMat * faceN);

                // Flip normal to face the camera
                if (glm::dot(outNormal, rayDir) > 0.0f)
                    outNormal = -outNormal;

                hit = true;
            }
        }
    }

    return hit;
}

// ---------------------------------------------------------------------------
// RayCastParting — snaps to the nearest point on the mesh's parting line.
//
// Steps:
//  1. Cast mouse ray to world y=0 plane → planeHit (x, 0, z)
//  2. Walk every triangle of every imported object in world space.
//     Triangles that straddle y=0 contribute an intersection segment.
//  3. Find the closest point on any segment to planeHit.
//  4. If within kVentSnapRadius, set outPos (y forced to 0) and outNormal
//     (face normal projected onto the XZ plane so it lies on the parting
//     plane surface) and return true.
// ---------------------------------------------------------------------------

// Helper: closest point on segment [a,b] to point p (all in 2D XZ plane)
static glm::vec3 ClosestPointOnSegment(const glm::vec3& a, const glm::vec3& b, const glm::vec3& p)
{
    const glm::vec3 ab = b - a;
    const float     len2 = glm::dot(ab, ab);
    if (len2 < 1e-10f) return a;
    const float t = glm::clamp(glm::dot(p - a, ab) / len2, 0.0f, 1.0f);
    return a + t * ab;
}

bool GLCanvas::RayCastParting(int mouseX, int mouseY,
    glm::vec3& outPos, glm::vec3& outNormal)
{
    const wxSize sz = GetClientSize();
    const int    w = std::max(1, sz.x);
    const int    h = std::max(1, sz.y);

    // Build world-space ray
    const float ndcX = (2.0f * float(mouseX)) / float(w) - 1.0f;
    const float ndcY = 1.0f - (2.0f * float(mouseY)) / float(h);

    m_camera.SetAspect(float(w) / float(h));
    const glm::mat4 invVP = glm::inverse(m_camera.Projection() * m_camera.View());

    glm::vec4 nearH = invVP * glm::vec4(ndcX, ndcY, -1.0f, 1.0f);
    glm::vec4 farH = invVP * glm::vec4(ndcX, ndcY, 1.0f, 1.0f);
    nearH /= nearH.w;
    farH /= farH.w;

    const glm::vec3 rayOrig = glm::vec3(nearH);
    const glm::vec3 rayDir = glm::normalize(glm::vec3(farH) - glm::vec3(nearH));

    // Intersect ray with world y=0
    if (fabsf(rayDir.y) < 1e-6f) return false;   // ray parallel to parting plane
    const float t = -rayOrig.y / rayDir.y;
    if (t < 0.0f) return false;                   // plane is behind camera
    const glm::vec3 planeHit = rayOrig + rayDir * t;   // y == 0

    float     bestDist = kVentSnapRadius;
    glm::vec3 bestPos(0.0f);
    glm::vec3 bestNormal(0.0f, 1.0f, 0.0f);
    bool      found = false;

    for (const auto& obj : m_objects)
    {
        if (obj.cpuVerts.empty() || obj.cpuIndices.empty()) continue;

        const glm::mat4 model = obj.BuildModelMatrix();
        const glm::mat3 normMat = glm::transpose(glm::inverse(glm::mat3(model)));

        for (size_t i = 0; i + 2 < obj.cpuIndices.size(); i += 3)
        {
            const uint32_t i0 = obj.cpuIndices[i];
            const uint32_t i1 = obj.cpuIndices[i + 1];
            const uint32_t i2 = obj.cpuIndices[i + 2];

            // Transform triangle to world space
            const glm::vec3 lv0(obj.cpuVerts[i0 * 3], obj.cpuVerts[i0 * 3 + 1], obj.cpuVerts[i0 * 3 + 2]);
            const glm::vec3 lv1(obj.cpuVerts[i1 * 3], obj.cpuVerts[i1 * 3 + 1], obj.cpuVerts[i1 * 3 + 2]);
            const glm::vec3 lv2(obj.cpuVerts[i2 * 3], obj.cpuVerts[i2 * 3 + 1], obj.cpuVerts[i2 * 3 + 2]);

            const glm::vec3 wv0 = glm::vec3(model * glm::vec4(lv0, 1.0f));
            const glm::vec3 wv1 = glm::vec3(model * glm::vec4(lv1, 1.0f));
            const glm::vec3 wv2 = glm::vec3(model * glm::vec4(lv2, 1.0f));

            // Does this triangle touch the parting band [-0.1, +0.1]?
            const float minY = std::min({ wv0.y, wv1.y, wv2.y });
            const float maxY = std::max({ wv0.y, wv1.y, wv2.y });
            static constexpr float kBand = 0.1f;
            if (minY >= kBand || maxY <= -kBand) continue;

            // Find up to two points on or near y=0 for this triangle.
            // Vertices inside the band are projected directly; edges crossing
            // fully (below<->above) are interpolated to y=0.
            const glm::vec3 verts[3] = { wv0, wv1, wv2 };
            glm::vec3 seg[3];   // up to 3 candidates
            int       segCount = 0;

            for (int v = 0; v < 3; ++v)
                if (fabsf(verts[v].y) <= kBand)
                    seg[segCount++] = glm::vec3(verts[v].x, 0.0f, verts[v].z);

            for (int e = 0; e < 3 && segCount < 3; ++e)
            {
                const glm::vec3& a = verts[e];
                const glm::vec3& b = verts[(e + 1) % 3];
                if (!((a.y < -kBand && b.y > kBand) || (a.y > kBand && b.y < -kBand))) continue;
                const float alpha = -a.y / (b.y - a.y);
                glm::vec3 cross = a + alpha * (b - a);
                cross.y = 0.0f;
                seg[segCount++] = cross;
            }
            if (segCount < 2) continue;

            // Closest point on this parting segment to the plane hit
            const glm::vec3 closest = ClosestPointOnSegment(seg[0], seg[1], planeHit);
            const float     dist = glm::length(glm::vec3(closest.x - planeHit.x,
                0.0f,
                closest.z - planeHit.z));

            if (dist < bestDist)
            {
                bestDist = dist;
                bestPos = closest;
                bestPos.y = 0.0f;   // clamp exactly onto parting plane

                // Face normal projected onto XZ so it lies on the parting surface
                const glm::vec3 localFaceN = glm::normalize(
                    glm::cross(lv1 - lv0, lv2 - lv0));
                glm::vec3 worldFaceN = glm::normalize(normMat * localFaceN);
                worldFaceN.y = 0.0f;
                const float nlen = glm::length(worldFaceN);
                bestNormal = (nlen > 1e-4f) ? worldFaceN / nlen : glm::vec3(0.0f, 0.0f, 1.0f);
                found = true;
            }
        }
    }

    if (found)
    {
        outPos = bestPos;
        outNormal = bestNormal;
    }
    return found;
}

// ---------------------------------------------------------------------------
// BuildFixturePerimeter — collects all y=0 XZ crossing points from every
// fixture triangle, then computes their 2D convex hull (Graham scan).
// Result is cached in m_fixturePerimeter and is only rebuilt when fixtures
// change, not per-frame.
// ---------------------------------------------------------------------------

// Convex hull helpers (2D in glm::vec2 = XZ plane)
static float Cross2D(const glm::vec2& O, const glm::vec2& A, const glm::vec2& B)
{
    return (A.x - O.x) * (B.y - O.y) - (A.y - O.y) * (B.x - O.x);
}

static std::vector<glm::vec2> ConvexHull(std::vector<glm::vec2> pts)
{
    const int n = (int)pts.size();
    if (n < 3) return pts;

    // Sort by x, then y
    std::sort(pts.begin(), pts.end(), [](const glm::vec2& a, const glm::vec2& b) {
        return a.x < b.x || (a.x == b.x && a.y < b.y);
        });

    std::vector<glm::vec2> hull;
    hull.reserve(2 * n);

    // Lower hull
    for (int i = 0; i < n; ++i)
    {
        while (hull.size() >= 2 &&
            Cross2D(hull[hull.size() - 2], hull[hull.size() - 1], pts[i]) <= 0.0f)
            hull.pop_back();
        hull.push_back(pts[i]);
    }

    // Upper hull
    const int lower_size = (int)hull.size() + 1;
    for (int i = n - 2; i >= 0; --i)
    {
        while ((int)hull.size() >= lower_size &&
            Cross2D(hull[hull.size() - 2], hull[hull.size() - 1], pts[i]) <= 0.0f)
            hull.pop_back();
        hull.push_back(pts[i]);
    }

    hull.pop_back();   // last point == first point
    return hull;
}

void GLCanvas::BuildFixturePerimeter()
{
    m_fixturePerimeter.clear();

    // Half-thickness of the virtual parting band (world units).
    // Triangles whose Y extent overlaps [-kBand, +kBand] contribute points.
    static constexpr float kBand = 0.1f;

    // Vertex classification relative to the parting band.
    enum class Side { Below, Band, Above };
    auto classify = [](float y) -> Side {
        if (y < -kBand) return Side::Below;
        if (y > kBand) return Side::Above;
        return Side::Band;
        };

    std::vector<glm::vec2> pts;

    for (const auto& fix : m_fixtures)
    {
        if (fix.cpuVerts.empty() || fix.cpuIndices.empty()) continue;

        const glm::mat4 model = fix.BuildModelMatrix();

        for (size_t i = 0; i + 2 < fix.cpuIndices.size(); i += 3)
        {
            const uint32_t i0 = fix.cpuIndices[i];
            const uint32_t i1 = fix.cpuIndices[i + 1];
            const uint32_t i2 = fix.cpuIndices[i + 2];

            const glm::vec3 lv0(fix.cpuVerts[i0 * 3], fix.cpuVerts[i0 * 3 + 1], fix.cpuVerts[i0 * 3 + 2]);
            const glm::vec3 lv1(fix.cpuVerts[i1 * 3], fix.cpuVerts[i1 * 3 + 1], fix.cpuVerts[i1 * 3 + 2]);
            const glm::vec3 lv2(fix.cpuVerts[i2 * 3], fix.cpuVerts[i2 * 3 + 1], fix.cpuVerts[i2 * 3 + 2]);

            const glm::vec3 wv[3] = {
                glm::vec3(model * glm::vec4(lv0, 1.0f)),
                glm::vec3(model * glm::vec4(lv1, 1.0f)),
                glm::vec3(model * glm::vec4(lv2, 1.0f))
            };

            const Side s[3] = { classify(wv[0].y), classify(wv[1].y), classify(wv[2].y) };

            // Skip triangles entirely above or entirely below the band
            if (s[0] == Side::Above && s[1] == Side::Above && s[2] == Side::Above) continue;
            if (s[0] == Side::Below && s[1] == Side::Below && s[2] == Side::Below) continue;

            // Any vertex inside the band is projected directly onto y=0
            for (int v = 0; v < 3; ++v)
                if (s[v] == Side::Band)
                    pts.emplace_back(wv[v].x, wv[v].z);

            // Edges that cross fully (Below<->Above, skipping Band vertices since
            // those are already included) get an exact y=0 intersection point
            for (int e = 0; e < 3; ++e)
            {
                const int      next = (e + 1) % 3;
                const Side     sa = s[e];
                const Side     sb = s[next];
                const glm::vec3& a = wv[e];
                const glm::vec3& b = wv[next];

                // Only interpolate across a full Below<->Above crossing
                if ((sa == Side::Below && sb == Side::Above) ||
                    (sa == Side::Above && sb == Side::Below))
                {
                    const float alpha = -a.y / (b.y - a.y);
                    const glm::vec3 crossing = a + alpha * (b - a);
                    pts.emplace_back(crossing.x, crossing.z);
                }
            }
        }
    }

    if (pts.size() >= 3)
        m_fixturePerimeter = ConvexHull(pts);
}

// ---------------------------------------------------------------------------
// ComputeVentPath — finds the closest point on the fixture perimeter hull
// to the vent origin and draws a straight line to it on the parting plane.
// ---------------------------------------------------------------------------
VentPath GLCanvas::ComputeVentPath(const VentPoint& vp)
{
    VentPath result;
    result.start = vp.worldPos;
    result.valid = false;

    if (m_fixturePerimeter.size() < 2)
        return result;

    const glm::vec2 origin(vp.worldPos.x, vp.worldPos.z);

    float     bestDist = std::numeric_limits<float>::max();
    glm::vec2 bestPt(0.0f);

    const int n = (int)m_fixturePerimeter.size();
    for (int i = 0; i < n; ++i)
    {
        const glm::vec2& A = m_fixturePerimeter[i];
        const glm::vec2& B = m_fixturePerimeter[(i + 1) % n];

        // Closest point on edge AB to origin
        const glm::vec2 AB = B - A;
        const float     len2 = glm::dot(AB, AB);
        float           t = 0.0f;
        if (len2 > 1e-10f)
            t = glm::clamp(glm::dot(origin - A, AB) / len2, 0.0f, 1.0f);

        const glm::vec2 closest = A + t * AB;
        const float     dist = glm::length(closest - origin);

        if (dist < bestDist)
        {
            bestDist = dist;
            bestPt = closest;
        }
    }

    result.end = glm::vec3(bestPt.x, 0.0f, bestPt.y);
    result.valid = true;
    return result;
}

// ---------------------------------------------------------------------------
// RebuildPathVBO — uploads all vent path line vertices to the GPU.
// Each path = 2 vertices of 3 floats = 6 floats.
// ---------------------------------------------------------------------------
void GLCanvas::RebuildPathVBO()
{
    if (!m_pathVAO) return;

    std::vector<float> verts;
    verts.reserve(m_ventPaths.size() * 6);
    for (const VentPath& p : m_ventPaths)
    {
        if (!p.valid) continue;
        verts.push_back(p.start.x); verts.push_back(p.start.y); verts.push_back(p.start.z);
        verts.push_back(p.end.x);   verts.push_back(p.end.y);   verts.push_back(p.end.z);
    }

    m_pathVertexCount = (GLsizei)verts.size() / 3;

    glBindVertexArray(m_pathVAO);
    glBindBuffer(GL_ARRAY_BUFFER, m_pathVBO);
    glBufferData(GL_ARRAY_BUFFER,
        (GLsizeiptr)(verts.size() * sizeof(float)),
        verts.empty() ? nullptr : verts.data(),
        GL_DYNAMIC_DRAW);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);
    glBindVertexArray(0);
}

// ---------------------------------------------------------------------------
// BuildVentCrossSection — constructs a rectangular profile at the vent origin,
// centred on the parting plane, perpendicular to the path direction.
//
// Orientation:
//   pathDir  = normalised XZ direction from start to end (lies on y=0 plane)
//   sideAxis = pathDir rotated 90° in XZ  (the "width" axis)
//   upAxis   = world Y                    (the "depth" axis, into each half)
//
// The rectangle has:
//   half-width  = width / 2  along sideAxis
//   half-depth  = depth / 2  along ±Y
// ---------------------------------------------------------------------------
VentCrossSection GLCanvas::BuildVentCrossSection(const VentPath& path,
    float width, float depth)
{
    VentCrossSection xs;
    xs.valid = false;

    if (!path.valid) return xs;

    const glm::vec3 start = path.start;
    const glm::vec3 diff = path.end - path.start;
    const float     len = glm::length(glm::vec2(diff.x, diff.z));
    if (len < 1e-6f) return xs;

    // Path direction in XZ (y=0)
    const glm::vec3 pathDir(diff.x / len, 0.0f, diff.z / len);

    // Perpendicular in XZ: rotate pathDir 90° around Y
    const glm::vec3 sideAxis(-pathDir.z, 0.0f, pathDir.x);
    const glm::vec3 upAxis(0.0f, 1.0f, 0.0f);

    const float hw = width * 0.5f;
    const float hd = depth * 0.5f;

    // BL, BR, TR, TL  (B=below parting plane, T=above, L=left, R=right)
    xs.corners[0] = start - sideAxis * hw - upAxis * hd;
    xs.corners[1] = start + sideAxis * hw - upAxis * hd;
    xs.corners[2] = start + sideAxis * hw + upAxis * hd;
    xs.corners[3] = start - sideAxis * hw + upAxis * hd;
    xs.valid = true;
    return xs;
}

// ---------------------------------------------------------------------------
// RebuildCrossSectionVBO — packs all cross-section rectangles as line loops
// into a single VBO (4 verts × 3 floats per cross-section, drawn as
// GL_LINES with explicit pairs so one Draw call covers all of them).
// ---------------------------------------------------------------------------
void GLCanvas::RebuildCrossSectionVBO()
{
    if (!m_xsecVAO) return;

    // Each rectangle = 4 edges = 8 verts (pairs: 0-1, 1-2, 2-3, 3-0)
    std::vector<float> verts;
    verts.reserve(m_ventCrossSections.size() * 24);

    for (const VentCrossSection& xs : m_ventCrossSections)
    {
        if (!xs.valid) continue;
        auto push = [&](const glm::vec3& v) {
            verts.push_back(v.x); verts.push_back(v.y); verts.push_back(v.z);
            };
        push(xs.corners[0]); push(xs.corners[1]);
        push(xs.corners[1]); push(xs.corners[2]);
        push(xs.corners[2]); push(xs.corners[3]);
        push(xs.corners[3]); push(xs.corners[0]);
    }

    m_xsecVertexCount = (GLsizei)verts.size() / 3;

    glBindVertexArray(m_xsecVAO);
    glBindBuffer(GL_ARRAY_BUFFER, m_xsecVBO);
    glBufferData(GL_ARRAY_BUFFER,
        (GLsizeiptr)(verts.size() * sizeof(float)),
        verts.empty() ? nullptr : verts.data(),
        GL_DYNAMIC_DRAW);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);
    glBindVertexArray(0);
}

// ---------------------------------------------------------------------------
// BuildVentSolid — sweeps the rectangular cross-section along the vent path
// to produce a closed prismatic mesh.
//
// overrunStart / overrunEnd extend the solid beyond path.start and path.end
// respectively (along the path direction) so the cut geometry clears any
// surface artifacts at both terminations.
//
// The prism has 6 faces:
//   - Start cap  (at extended start, facing -pathDir)
//   - End cap    (at extended end,   facing +pathDir)
//   - 4 side quads connecting corresponding edges of start and end caps
//
// Vertex layout per vertex: [px, py, pz, nx, ny, nz]  (compatible with vsLit)
// ---------------------------------------------------------------------------
VentSolid GLCanvas::BuildVentSolid(const VentPath& path, float width, float depth,
    float overrunStart, float overrunEnd)
{
    VentSolid solid;
    solid.valid = false;

    if (!path.valid) return solid;

    const glm::vec3 diff = path.end - path.start;
    const float     len = glm::length(glm::vec2(diff.x, diff.z));
    if (len < 1e-6f) return solid;

    // Basis vectors (same as BuildVentCrossSection)
    const glm::vec3 pathDir(-diff.x / len, 0.0f, -diff.z / len);  // points start→end, reversed for cap normal
    const glm::vec3 sideAxis(-pathDir.z, 0.0f, pathDir.x);
    const glm::vec3 upAxis(0.0f, 1.0f, 0.0f);

    const float hw = width * 0.5f;
    const float hd = depth * 0.5f;

    // Sweep direction (start → end).  pathDir was built reversed, so sweep = -pathDir.
    const glm::vec3 sweepDir = -pathDir;

    // Extend the cap positions outward by the requested overrun amounts.
    // overrunStart pushes the start cap back (opposite sweepDir).
    // overrunEnd   pushes the end   cap forward (along sweepDir).
    const glm::vec3 extendedStart = path.start - sweepDir * overrunStart;
    const glm::vec3 extendedEnd = path.end + sweepDir * overrunEnd;

    // Build start and end corner sets (BL, BR, TR, TL)
    auto makeCorners = [&](const glm::vec3& centre) -> std::array<glm::vec3, 4>
        {
            return { {
                centre - sideAxis * hw - upAxis * hd,   // 0 BL
                centre + sideAxis * hw - upAxis * hd,   // 1 BR
                centre + sideAxis * hw + upAxis * hd,   // 2 TR
                centre - sideAxis * hw + upAxis * hd    // 3 TL
            } };
        };

    const auto startC = makeCorners(extendedStart);
    const auto endC = makeCorners(extendedEnd);

    // pathDir currently points start←end; sweep direction is start→end

    // ---- Accumulate interleaved [pos(3) norm(3)] verts and indices ----
    std::vector<float>    verts;
    std::vector<uint32_t> idx;

    auto addQuad = [&](const glm::vec3& a, const glm::vec3& b,
        const glm::vec3& c, const glm::vec3& d,
        const glm::vec3& n)
        {
            const uint32_t base = (uint32_t)(verts.size() / 6);
            for (const glm::vec3& p : { a, b, c, d })
            {
                verts.push_back(p.x); verts.push_back(p.y); verts.push_back(p.z);
                verts.push_back(n.x); verts.push_back(n.y); verts.push_back(n.z);
            }
            // Two CCW triangles: a-b-c and a-c-d
            idx.push_back(base + 0); idx.push_back(base + 1); idx.push_back(base + 2);
            idx.push_back(base + 0); idx.push_back(base + 2); idx.push_back(base + 3);
        };

    // Start cap — normal faces away from path (−sweepDir)
    addQuad(startC[0], startC[3], startC[2], startC[1], -sweepDir);

    // End cap — normal faces along sweepDir
    addQuad(endC[0], endC[1], endC[2], endC[3], sweepDir);

    // Side faces: bottom, right, top, left
    // bottom (normal = -upAxis)
    addQuad(startC[0], startC[1], endC[1], endC[0], -upAxis);
    // right (normal = +sideAxis)
    addQuad(startC[1], startC[2], endC[2], endC[1], sideAxis);
    // top (normal = +upAxis)
    addQuad(startC[2], startC[3], endC[3], endC[2], upAxis);
    // left (normal = -sideAxis)
    addQuad(startC[3], startC[0], endC[0], endC[3], -sideAxis);

    // ---- Upload to GPU ----
    glGenVertexArrays(1, &solid.vao);
    glGenBuffers(1, &solid.vbo);
    glGenBuffers(1, &solid.ebo);

    glBindVertexArray(solid.vao);
    glBindBuffer(GL_ARRAY_BUFFER, solid.vbo);
    glBufferData(GL_ARRAY_BUFFER,
        (GLsizeiptr)(verts.size() * sizeof(float)),
        verts.data(), GL_STATIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, solid.ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER,
        (GLsizeiptr)(idx.size() * sizeof(uint32_t)),
        idx.data(), GL_STATIC_DRAW);

    // position (location 0)
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);
    // normal (location 1)
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float),
        (void*)(3 * sizeof(float)));
    glEnableVertexAttribArray(1);

    glBindVertexArray(0);

    solid.indexCount = (GLsizei)idx.size();
    solid.valid = true;
    return solid;
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

    // Build vent-point marker sphere
    BuildSphereGPU(1.0f, 12, 16);

    // Flat shader for vent path lines
    {
        GLuint fvs = Compile(GL_VERTEX_SHADER, m_shaders.vsFlat);
        GLuint ffs = Compile(GL_FRAGMENT_SHADER, m_shaders.fsFlat);
        m_flatProgram = Link(fvs, ffs);
        m_flat_uVP = glGetUniformLocation(m_flatProgram, "uVP");
        m_flat_uColor = glGetUniformLocation(m_flatProgram, "uColor");
    }

    // Vent path line VBO (dynamic, rebuilt whenever paths change)
    glGenVertexArrays(1, &m_pathVAO);
    glGenBuffers(1, &m_pathVBO);
    glBindVertexArray(m_pathVAO);
    glBindBuffer(GL_ARRAY_BUFFER, m_pathVBO);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);
    glBindVertexArray(0);

    // Vent cross-section VBO (dynamic)
    glGenVertexArrays(1, &m_xsecVAO);
    glGenBuffers(1, &m_xsecVBO);
    glBindVertexArray(m_xsecVAO);
    glBindBuffer(GL_ARRAY_BUFFER, m_xsecVBO);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);
    glBindVertexArray(0);

    m_inited = true;
}

void GLCanvas::DestroyGL()
{
    for (auto& obj : m_fixtures) obj.mesh.Destroy();
    m_fixtures.clear();
    for (auto& obj : m_objects)  obj.mesh.Destroy();
    m_objects.clear();
    for (auto& s : m_ventSolids) s.Destroy();
    m_ventSolids.clear();

    if (m_outlineProgram) { glDeleteProgram(m_outlineProgram);        m_outlineProgram = 0; }
    if (m_flatProgram) { glDeleteProgram(m_flatProgram);           m_flatProgram = 0; }
    if (m_fullscreenVAO) { glDeleteVertexArrays(1, &m_fullscreenVAO); m_fullscreenVAO = 0; }
    if (m_pathVBO) { glDeleteBuffers(1, &m_pathVBO);              m_pathVBO = 0; }
    if (m_pathVAO) { glDeleteVertexArrays(1, &m_pathVAO);         m_pathVAO = 0; }
    if (m_xsecVBO) { glDeleteBuffers(1, &m_xsecVBO);              m_xsecVBO = 0; }
    if (m_xsecVAO) { glDeleteVertexArrays(1, &m_xsecVAO);         m_xsecVAO = 0; }
    if (m_sphereEBO) { glDeleteBuffers(1, &m_sphereEBO);          m_sphereEBO = 0; }
    if (m_sphereVBO) { glDeleteBuffers(1, &m_sphereVBO);          m_sphereVBO = 0; }
    if (m_sphereVAO) { glDeleteVertexArrays(1, &m_sphereVAO);     m_sphereVAO = 0; }
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
        if (outPaths[i].empty()) continue;

        TopoDS_Shape shapeToExport;

        if (fix.hasMould)
        {
            // Use the post-cut shape directly — transform already baked in
            shapeToExport = fix.mouldShape;
        }
        else
        {
            // Fall back to re-reading source and applying current transform
            if (fix.sourcePath.empty()) continue;

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

            gp_Trsf trsf;
            glm::mat4 m = fix.BuildModelMatrix();
            trsf.SetValues(
                m[0][0], m[1][0], m[2][0], m[3][0],
                m[0][1], m[1][1], m[2][1], m[3][1],
                m[0][2], m[1][2], m[2][2], m[3][2]
            );
            BRepBuilderAPI_Transform xform(shape, trsf, true);
            shapeToExport = xform.Shape();
        }

        STEPControl_Writer writer;
        writer.Transfer(shapeToExport, STEPControl_AsIs);
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

    wxProgressDialog progress(
        "Importing File",
        "Reading STEP file...",
        5,
        nullptr,
        wxPD_APP_MODAL | wxPD_AUTO_HIDE | wxPD_SMOOTH | wxPD_ELAPSED_TIME
    );

    int step = 0;

    progress.Update(step++, "Reading STEP file...");
    FileImporter importer;
    auto res = importer.ImportSTEP(path, 0.05, 0.5);

    if (!res.ok()) {
        wxMessageBox(res.error, "Import failed", wxOK | wxICON_ERROR, this);
        return;
    }

    progress.Update(step++, "Computing vertex normals...");

    // Snapshot position-only geometry for CPU ray casting BEFORE the crease
    // split duplicates vertices and replaces the index buffer.
    std::vector<float>    cpuVerts = res.meshes[0].vertices;
    std::vector<uint32_t> cpuIndices = res.meshes[0].indices;

    ComputeVertexNormals_Pos3(res.meshes[0].vertices,
        res.meshes[0].indices,
        res.meshes[0].posNorm);

    progress.Update(step++, "Splitting by crease angle...");
    auto split = SplitByCreaseAngle_Pos3(res.meshes[0].vertices,
        res.meshes[0].indices, 35.0f);
    res.meshes[0].posNorm = std::move(split.posNorm);
    res.meshes[0].indices = std::move(split.indices);

    progress.Update(step++, "Uploading to GPU...");
    m_objects.emplace_back();
    m_objects.back().sourcePath = path;
    m_objects.back().cpuVerts = std::move(cpuVerts);
    m_objects.back().cpuIndices = std::move(cpuIndices);
    UploadMeshToGPU(res.meshes[0], m_objects.back());

    progress.Update(step++, "Done.");
    Refresh(false);
}

void GLCanvas::ImportStepFileAsFixture(const std::string& path)
{
    SetCurrent(*m_context);
    InitGLOnce();

    wxProgressDialog progress(
        "Importing Fixture",
        "Reading STEP file...",
        5,
        nullptr,
        wxPD_APP_MODAL | wxPD_AUTO_HIDE | wxPD_SMOOTH | wxPD_ELAPSED_TIME
    );

    int step = 0;

    progress.Update(step++, "Reading STEP file...");
    FileImporter importer;
    auto res = importer.ImportSTEP(path, 0.05, 0.5);

    if (!res.ok()) {
        wxMessageBox(res.error, "Import failed", wxOK | wxICON_ERROR, this);
        return;
    }

    progress.Update(step++, "Computing vertex normals...");

    // Snapshot geometry before crease split (same pattern as ImportStepFile)
    std::vector<float>    cpuVerts = res.meshes[0].vertices;
    std::vector<uint32_t> cpuIndices = res.meshes[0].indices;

    ComputeVertexNormals_Pos3(res.meshes[0].vertices,
        res.meshes[0].indices,
        res.meshes[0].posNorm);

    progress.Update(step++, "Splitting by crease angle...");
    auto split = SplitByCreaseAngle_Pos3(res.meshes[0].vertices,
        res.meshes[0].indices, 35.0f);
    res.meshes[0].posNorm = std::move(split.posNorm);
    res.meshes[0].indices = std::move(split.indices);

    progress.Update(step++, "Uploading to GPU...");
    m_fixtures.emplace_back();
    m_fixtures.back().role = ObjectRole::Fixture;
    m_fixtures.back().sourcePath = path;
    m_fixtures.back().cpuVerts = std::move(cpuVerts);
    m_fixtures.back().cpuIndices = std::move(cpuIndices);
    UploadMeshToGPU(res.meshes[0], m_fixtures.back());

    progress.Update(step++, "Done.");
    BuildFixturePerimeter();
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
    glDepthMask(GL_FALSE);   // don't write depth — keeps vent markers visible through the tint

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

    glDepthMask(GL_TRUE);
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

    // ---- Vent point markers (green spheres) --------------------------------
    // Resolve ghost position here — one ray cast per rendered frame regardless
    // of how many motion events queued up since the last paint.
    if (m_transformMode == TransformMode::PlaceVent)
    {
        glm::vec3 hitPos, hitNormal;
        m_ventGhostActive = RayCastParting(m_ghostMousePos.x, m_ghostMousePos.y, hitPos, hitNormal);
        m_ventGhost.worldPos = hitPos;
        m_ventGhost.worldNormal = hitNormal;
    }
    if (m_program && m_sphereVAO && m_sphereIndexCount > 0 &&
        (!m_ventPoints.empty() || m_ventGhostActive))
    {
        glEnable(GL_DEPTH_TEST);
        glUseProgram(m_program);

        glUniform3fv(glGetUniformLocation(m_program, "uCameraPos"), 1, &camPos[0]);
        glUniform3fv(glGetUniformLocation(m_program, "uLightDir"), 1, &lightDir[0]);
        glUniform3fv(glGetUniformLocation(m_program, "uLightColor"), 1, &lightColor[0]);
        glUniform1f(glGetUniformLocation(m_program, "uAmbient"), 0.35f);
        glUniform1f(glGetUniformLocation(m_program, "uDiffuse"), 0.75f);
        glUniform1f(glGetUniformLocation(m_program, "uSpecular"), 0.60f);
        glUniform1f(glGetUniformLocation(m_program, "uShininess"), 48.0f);
        glUniformMatrix4fv(glGetUniformLocation(m_program, "uView"), 1, GL_FALSE, &view[0][0]);
        glUniformMatrix4fv(glGetUniformLocation(m_program, "uProj"), 1, GL_FALSE, &proj[0][0]);

        glBindVertexArray(m_sphereVAO);

        // Ghost preview — translucent, slightly larger, pulsing not needed but distinct
        if (m_ventGhostActive)
        {
            const glm::vec3 ghostColor(0.10f, 0.92f, 0.25f);
            glUniform3fv(glGetUniformLocation(m_program, "uBaseColor"), 1, &ghostColor[0]);
            glUniform1f(glGetUniformLocation(m_program, "uAlpha"), 0.45f);
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            glDepthMask(GL_FALSE);

            glm::mat4 model = glm::translate(glm::mat4(1.0f), m_ventGhost.worldPos);
            model = glm::scale(model, glm::vec3(kVentMarkerRadius * 1.15f));
            glUniformMatrix4fv(glGetUniformLocation(m_program, "uModel"), 1, GL_FALSE, &model[0][0]);
            glDrawElements(GL_TRIANGLES, m_sphereIndexCount, GL_UNSIGNED_INT, 0);

            glDepthMask(GL_TRUE);
            glDisable(GL_BLEND);
        }

        // Confirmed vent points — fully opaque
        if (!m_ventPoints.empty())
        {
            const glm::vec3 ventColor(0.10f, 0.92f, 0.25f);
            glUniform3fv(glGetUniformLocation(m_program, "uBaseColor"), 1, &ventColor[0]);
            glUniform1f(glGetUniformLocation(m_program, "uAlpha"), 1.0f);

            for (const VentPoint& vp : m_ventPoints)
            {
                glm::mat4 model = glm::translate(glm::mat4(1.0f), vp.worldPos);
                model = glm::scale(model, glm::vec3(kVentMarkerRadius));
                glUniformMatrix4fv(glGetUniformLocation(m_program, "uModel"), 1, GL_FALSE, &model[0][0]);
                glDrawElements(GL_TRIANGLES, m_sphereIndexCount, GL_UNSIGNED_INT, 0);
            }
        }

        glBindVertexArray(0);
        glUseProgram(0);
    }

    // ---- Vent path lines ---------------------------------------------------
    if (m_flatProgram && m_pathVAO && m_pathVertexCount > 0)
    {
        glEnable(GL_DEPTH_TEST);
        glLineWidth(2.0f);
        glUseProgram(m_flatProgram);

        const glm::mat4 VP = proj * view;
        glUniformMatrix4fv(m_flat_uVP, 1, GL_FALSE, &VP[0][0]);

        const glm::vec4 lineColor(0.10f, 0.92f, 0.25f, 1.0f);
        glUniform4fv(m_flat_uColor, 1, &lineColor[0]);

        glBindVertexArray(m_pathVAO);
        glDrawArrays(GL_LINES, 0, m_pathVertexCount);
        glBindVertexArray(0);
        glLineWidth(1.0f);
        glUseProgram(0);
    }

    // ---- Vent cross-sections -----------------------------------------------
    if (m_flatProgram && m_xsecVAO && m_xsecVertexCount > 0)
    {
        glEnable(GL_DEPTH_TEST);
        glLineWidth(2.0f);
        glUseProgram(m_flatProgram);

        const glm::mat4 VP = proj * view;
        glUniformMatrix4fv(m_flat_uVP, 1, GL_FALSE, &VP[0][0]);

        // Slightly brighter yellow-green to distinguish from path lines
        const glm::vec4 xsecColor(0.60f, 1.00f, 0.20f, 1.0f);
        glUniform4fv(m_flat_uColor, 1, &xsecColor[0]);

        glBindVertexArray(m_xsecVAO);
        glDrawArrays(GL_LINES, 0, m_xsecVertexCount);
        glBindVertexArray(0);
        glLineWidth(1.0f);
        glUseProgram(0);
    }

    // ---- Vent solids (lit, semi-transparent so the path line shows through) -
    if (m_program && !m_ventSolids.empty())
    {
        glEnable(GL_DEPTH_TEST);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glDepthMask(GL_FALSE);

        glUseProgram(m_program);

        const glm::mat4 identity(1.0f);
        const glm::vec3 ventSolidColor(0.20f, 0.85f, 0.35f);
        glUniform3fv(glGetUniformLocation(m_program, "uCameraPos"), 1, &camPos[0]);
        glUniform3fv(glGetUniformLocation(m_program, "uLightDir"), 1, &lightDir[0]);
        glUniform3fv(glGetUniformLocation(m_program, "uLightColor"), 1, &lightColor[0]);
        glUniform3fv(glGetUniformLocation(m_program, "uBaseColor"), 1, &ventSolidColor[0]);
        glUniform1f(glGetUniformLocation(m_program, "uAlpha"), 0.55f);
        glUniform1f(glGetUniformLocation(m_program, "uAmbient"), 0.30f);
        glUniform1f(glGetUniformLocation(m_program, "uDiffuse"), 0.80f);
        glUniform1f(glGetUniformLocation(m_program, "uSpecular"), 0.40f);
        glUniform1f(glGetUniformLocation(m_program, "uShininess"), 32.0f);
        glUniformMatrix4fv(glGetUniformLocation(m_program, "uView"), 1, GL_FALSE, &view[0][0]);
        glUniformMatrix4fv(glGetUniformLocation(m_program, "uProj"), 1, GL_FALSE, &proj[0][0]);
        glUniformMatrix4fv(glGetUniformLocation(m_program, "uModel"), 1, GL_FALSE, &identity[0][0]);

        for (const VentSolid& vs : m_ventSolids)
        {
            if (!vs.valid || vs.vao == 0) continue;
            glBindVertexArray(vs.vao);
            glDrawElements(GL_TRIANGLES, vs.indexCount, GL_UNSIGNED_INT, 0);
        }

        glBindVertexArray(0);
        glUseProgram(0);
        glDepthMask(GL_TRUE);
        glDisable(GL_BLEND);
    }

    // ---- DEBUG: fixture perimeter hull -------------------------------------
    if (m_flatProgram && !m_fixturePerimeter.empty())
    {
        // Pack hull vertices into a temporary float buffer (y = 0)
        std::vector<float> perimVerts;
        perimVerts.reserve(m_fixturePerimeter.size() * 3);
        for (const glm::vec2& p : m_fixturePerimeter)
        {
            perimVerts.push_back(p.x);
            perimVerts.push_back(0.0f);
            perimVerts.push_back(p.y);
        }

        GLuint tmpVAO = 0, tmpVBO = 0;
        glGenVertexArrays(1, &tmpVAO);
        glGenBuffers(1, &tmpVBO);
        glBindVertexArray(tmpVAO);
        glBindBuffer(GL_ARRAY_BUFFER, tmpVBO);
        glBufferData(GL_ARRAY_BUFFER,
            (GLsizeiptr)(perimVerts.size() * sizeof(float)),
            perimVerts.data(), GL_STREAM_DRAW);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void*)0);
        glEnableVertexAttribArray(0);

        glDisable(GL_DEPTH_TEST);   // always draw on top
        glLineWidth(2.0f);
        glUseProgram(m_flatProgram);

        const glm::mat4 VP = proj * view;
        glUniformMatrix4fv(m_flat_uVP, 1, GL_FALSE, &VP[0][0]);

        const glm::vec4 perimColor(1.0f, 0.4f, 0.0f, 1.0f);   // orange
        glUniform4fv(m_flat_uColor, 1, &perimColor[0]);

        glDrawArrays(GL_LINE_LOOP, 0,
            (GLsizei)m_fixturePerimeter.size());

        glLineWidth(1.0f);
        glEnable(GL_DEPTH_TEST);
        glUseProgram(0);
        glBindVertexArray(0);

        glDeleteBuffers(1, &tmpVBO);
        glDeleteVertexArrays(1, &tmpVAO);
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
        else if (m_transformMode == TransformMode::PlaceVent)
        {
            const wxPoint p = evt.GetPosition();
            glm::vec3 hitPos, hitNormal;
            if (RayCastParting(p.x, p.y, hitPos, hitNormal))
            {
                const VentPoint vp{ hitPos, hitNormal };
                m_ventPoints.push_back(vp);

                // Read dimensions from the left-panel UI
                float ventLength = 5.0f, ventWidth = 2.0f,
                    ventOverrunStart = 0.5f, ventOverrunEnd = 0.5f;
                if (auto* frame = dynamic_cast<MainFrame*>(wxGetTopLevelParent(this)))
                    frame->GetVentDimensions(ventLength, ventWidth,
                        ventOverrunStart, ventOverrunEnd);

                VentPath path = ComputeVentPath(vp);
                path.overrunStart = ventOverrunStart;
                path.overrunEnd = ventOverrunEnd;
                m_ventPaths.push_back(path);

                m_ventCrossSections.push_back(
                    BuildVentCrossSection(path, ventWidth, ventLength));

                m_ventSolids.push_back(
                    BuildVentSolid(path, ventWidth, ventLength,
                        ventOverrunStart, ventOverrunEnd));

                RebuildPathVBO();
                RebuildCrossSectionVBO();
                Refresh(false);
            }
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
    if (!m_hasLast)
    {
        m_lastPos = pos;
        m_hasLast = true;
        if (m_transformMode == TransformMode::PlaceVent)
        {
            m_ghostMousePos = pos;
            Refresh(false);
        }
        evt.Skip();
        return;
    }

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
            for (auto& s : m_ventSolids) s.Destroy(); m_ventSolids.clear(); m_ventPoints.clear(); m_ventPaths.clear(); m_ventCrossSections.clear(); RebuildPathVBO(); RebuildCrossSectionVBO();
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
    else if (m_lmb && m_transformMode == TransformMode::PlaceVent)
    {
        // Orbit while holding LMB in PlaceVent mode (no object to drag)
        if (shift)
            m_camera.Pan(dx, -dy);
        else if (ctrl)
            m_camera.Dolly(dy * 0.05f);
        else
            m_camera.Orbit(dx, dy);
        Refresh(false);
    }

    // Update ghost preview whenever the mouse moves in PlaceVent mode.
    // The actual ray cast is deferred to OnPaint so only one cast runs per
    // rendered frame, regardless of how many motion events have queued up.
    if (m_transformMode == TransformMode::PlaceVent)
    {
        m_ghostMousePos = evt.GetPosition();
        Refresh(false);
    }
    else if (m_ventGhostActive)
    {
        m_ventGhostActive = false;
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

void GLCanvas::OnKeyDown(wxKeyEvent& evt)
{
    if (evt.GetKeyCode() == WXK_ESCAPE)
    {
        if (m_transformMode != TransformMode::Select)
        {
            m_ventGhostActive = false;
            SetTransformMode(TransformMode::Select);
            if (auto* frame = dynamic_cast<MainFrame*>(wxGetTopLevelParent(this)))
                frame->SetActiveTool(TransformMode::Select);
        }
    }
    else if (evt.GetKeyCode() == WXK_DELETE && m_selectedIndex >= 0)
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

void GLCanvas::ClearFixtures()
{
    SetCurrent(*m_context);
    for (auto& fix : m_fixtures)
        fix.mesh.Destroy();
    m_fixtures.clear();
    m_fixturePerimeter.clear();
    for (auto& s : m_ventSolids) s.Destroy(); m_ventSolids.clear(); m_ventPoints.clear(); m_ventPaths.clear(); m_ventCrossSections.clear(); RebuildPathVBO(); RebuildCrossSectionVBO();
    Refresh(false);
}