#pragma once

#include <glad/glad.h>
#include <wx/glcanvas.h>
#include <vector>
#include <array>
#include <functional>   // std::function for scene-mutation callback

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
#include "ProjectFile.h"

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

    // Cached BREP shape from import (native for STEP, faceted shell/solid
    // for mesh formats). Populated at import time so boolean operations and
    // export don't have to re-read the source file.
    TopoDS_Shape sourceShape;
    bool         hasSourceShape = false;

    // CPU-side geometry for ray casting (position-only, object space)
    std::vector<float>    cpuVerts;    // 3 floats per vertex
    std::vector<uint32_t> cpuIndices;  // triangle indices

    // Per-triangle edge adjacency for face-region growing (Align Face).
    // triNeighbors[t][k] = the triangle that shares edge k of triangle t
    // (where edge k is between local vertex k and (k+1)%3), or -1 if none.
    // Built lazily on first use; persists for the object's lifetime since
    // local mesh topology never changes after import.
    std::vector<std::array<int, 3>> triNeighbors;
    bool                            adjacencyBuilt = false;

    glm::vec3 pos{ 0.0f, 0.0f, 0.0f };
    float     yawDeg = 0.0f;
    float     pitchDeg = 0.0f;
    float     rollDeg = 0.0f;
    float     scale = 1.0f;

    // Optional reflections about the local YZ / XY planes, applied as a
    // negative component in the scale matrix. Used by the grid-pattern
    // tool to mirror clones placed on the opposite side of the world X
    // or Z axis. With both flags set the mesh is reflected through the
    // local Y axis (parity preserved). The lit shader uses the
    // transpose-inverse normal matrix so reflections light correctly,
    // and face culling is off so reversed winding renders fine.
    bool mirrorX = false;
    bool mirrorZ = false;

    glm::mat4 BuildModelMatrix() const
    {
        glm::mat4 T = glm::translate(glm::mat4(1.0f), pos);
        glm::mat4 RY = glm::rotate(glm::mat4(1.0f), glm::radians(yawDeg), glm::vec3(0, 1, 0));
        glm::mat4 RX = glm::rotate(glm::mat4(1.0f), glm::radians(pitchDeg), glm::vec3(1, 0, 0));
        glm::mat4 RZ = glm::rotate(glm::mat4(1.0f), glm::radians(rollDeg), glm::vec3(0, 0, 1));
        // Per-axis scale: uniform `scale`, with optional mirror flipping
        // X and/or Z. Applied at the innermost (local) step so the
        // reflection acts on the un-rotated mesh; the rotation +
        // translation that follow place the mirrored shape into world
        // space.
        const glm::vec3 sv(scale * (mirrorX ? -1.0f : 1.0f),
            scale,
            scale * (mirrorZ ? -1.0f : 1.0f));
        glm::mat4 S = glm::scale(glm::mat4(1.0f), sv);
        return T * RY * RX * RZ * S;
    }
};

class GLCanvas : public wxGLCanvas
{
public:
    GLCanvas(wxWindow* parent);
    ~GLCanvas() override;

    // Imports any supported format (STEP, STL, OBJ) based on file extension.
    void ImportFile(const std::string& path);

    // Import a single fixture half. The optional `xform` carries the per-half
    // pose authored in the FixtureEditor and persisted to the .fixture file
    // ([half_a_transform] / [half_b_transform]). It is applied to the new
    // SceneObject before BuildFixturePerimeter runs, so the parting-line
    // perimeter sees the transformed mesh. Defaulting to an identity
    // HalfTransform makes this a pure no-op for callers that don't have
    // pose data — fixtures from older files (no transform sections) load
    // exactly as they did before.
    void ImportFileAsFixture(const std::string& path,
        const HalfTransform& xform = HalfTransform{});

    // Called by MainFrame ribbon buttons
    void SetTransformMode(TransformMode mode);

    // Called by dialogs
    void ApplyRotation(float xDeg, float yDeg, float zDeg);
    void ApplyTranslation(float x, float y, float z);
    void ApplyScale(float factor);
    void CenterSelectedObject();

    // Circular pattern around the world origin in the XZ plane.
    //   count          - total instances including the original (no-op if <= 1)
    //   overrideRadius - if true, place clones at `radius` instead of the
    //                    original's current XZ distance from the origin
    //   radius         - the override radius, in world units (mm)
    //   rotateCopies   - if true, each clone's local yaw is rotated by its
    //                    angular offset (gear-tooth style — every instance
    //                    faces outward like the original). If false, every
    //                    clone keeps the original's local rotation verbatim.
    // The original keeps its position; (count - 1) clones are placed at
    // equally-spaced angular offsets around the origin.
    void ApplyCircularPattern(int count, bool overrideRadius, float radius,
        bool rotateCopies);

    // Grid pattern in the y=0 plane, centred on the world origin.
    //   numH               - cell count along X (>= 1)
    //   numV               - cell count along Z (>= 1)
    //   mirrorH            - if true, clones whose X coordinate sits on the
    //                        opposite side of x=0 from the original have
    //                        their mesh reflected about the local YZ plane
    //   mirrorV            - same for Z (reflection about local XY plane)
    //   overrideLengthWidth- if true, use `length`/`width` (interpreted as
    //                        full grid extents) instead of deriving from
    //                        the original's (x,z), which is treated as
    //                        half the full grid extent.
    //   length             - full X extent of the grid (mm), override only
    //   width              - full Z extent of the grid (mm), override only
    // The grid spans [-halfX, +halfX] x [-halfZ, +halfZ] in the y=0 plane,
    // with halfX,halfZ derived per above. The original is anchored to the
    // corner cell matching its current XZ-quadrant (and is moved there if
    // override is on or if its position no longer matches the anchor).
    void ApplyGridPattern(int numH, int numV, bool mirrorH, bool mirrorV,
        bool overrideLengthWidth, float length, float width);

    bool HasSelection() const { return !m_selectedIndices.empty(); }
    TransformMode GetTransformMode() const { return m_transformMode; }

    // Build the moulds for every fixture in the scene. Returns true when the
    // generation pipeline ran to completion, false when an up-front validation
    // failed (no fixtures loaded, or no imported objects to subtract). Used by
    // MainFrame to gate the Export button — Export stays disabled until at
    // least one successful run. Per-fixture failures inside the loop don't
    // affect the return: the function still falls through and returns true,
    // matching the existing success-message-box behaviour.
    bool GenerateMould();
    void ExportFixtures(const std::string& pathA, const std::string& pathB);

    // Scene-mutation callback. MainFrame registers a callback that
    // invalidates the Export button when anything that would stale a
    // previously generated mould happens — transforms, feature place /
    // remove, sprue placement, etc. Fired synchronously from every
    // mutating method on this class that I've identified; project-
    // level changes (import, fixture swap, project load) are handled
    // on the MainFrame side directly since some of them route around
    // canvas methods entirely.
    //
    // Known gap as of this iteration: Edit* modes commit their drag
    // updates via OnPaint's m_editNeedsUpdate path, which doesn't
    // currently emit through here. Tracked as a follow-up.
    void SetOnSceneMutated(std::function<void()> cb)
    {
        m_onSceneMutated = std::move(cb);
    }

    void ClearFixtures();

    // Vent point placement
    const std::vector<VentInstance>& GetVents() const { return m_vents; }
    void ClearVentPoints();

    // Sprue placement
    void SetActiveInjectionPoint(const InjectionPoint& ip);
    void SetInjectionPoints(const std::vector<InjectionPoint>& pts);
    void PlaceSprue();
    void ClearSprue();
    bool IsDirectInjection() const { return m_sprue.isDirectInjection; }

    // Point where the sprue path crosses the y=0 parting plane
    bool              HasSpruePartingPoint() const { return m_sprue.hasPartingPoint; }
    const glm::vec3& GetSpruePartingPoint() const { return m_sprue.partingPos; }

    // Runner placement
    const std::vector<RunnerFeature>& GetRunners() const { return m_runners; }
    void ClearRunnerPoints();

    // Gate placement
    const std::vector<GateFeature>& GetGates() const { return m_gates; }
    void ClearGatePoints();

    // Ejector placement
    const std::vector<EjectorFeature>& GetEjectors() const { return m_ejectors; }
    void ClearEjectors();
    // Rebuild every ejector's preview cylinder. Called after place / clear /
    // any operation that changes the ejector list. Reads diameter and
    // length from the MainFrame UI at call time, same convention as
    // RebuildGateSolids — so changing the dimension fields and placing a
    // new ejector picks up fresh values, but existing ejectors keep
    // whatever dimensions they were built with.
    void RebuildEjectorSolids();

    // Remove individual features by clicking their marker
    void RemoveVentAtMouse(int mouseX, int mouseY);
    void RemoveRunnerAtMouse(int mouseX, int mouseY);
    void RemoveGateAtMouse(int mouseX, int mouseY);
    void RemoveSprueAtMouse(int mouseX, int mouseY);
    void RemoveEjectorAtMouse(int mouseX, int mouseY);

    // ---- Project save/load support -----------------------------------------

    // Read-only accessors for serialization
    const std::vector<SceneObject>& GetObjects()  const { return m_objects; }
    const std::vector<SceneObject>& GetFixtures() const { return m_fixtures; }
    const SprueFeature& GetSprue()    const { return m_sprue; }
    bool  HasActiveInjectionPoint()               const { return m_hasActiveInjectionPoint; }
    const InjectionPoint& GetActiveInjectionPoint() const { return m_activeInjectionPoint; }

    // Restore helpers — programmatic placement from saved data (no mouse/ray cast)
    void RestoreObject(const std::string& path, const glm::vec3& pos,
        float yaw, float pitch, float roll, float scale,
        bool mirrorX, bool mirrorZ);

    void RestoreSprue(const ProjectSprueData& data);

    void RestoreRunner(const glm::vec3& point);

    void RestoreGate(const glm::vec3& pos, const glm::vec3& normal,
        int parentIndex = -1,
        const glm::vec3& localPos = glm::vec3(0.0f),
        const glm::vec3& localNormal = glm::vec3(0.0f, 0.0f, 1.0f));

    void RestoreVent(const glm::vec3& pos, const glm::vec3& normal,
        float ventWidth, float ventLength,
        float overrunStart, float overrunEnd,
        int parentIndex = -1,
        const glm::vec3& localPos = glm::vec3(0.0f),
        const glm::vec3& localNormal = glm::vec3(0.0f, 0.0f, 1.0f));

    // Restore an ejector during project load. No batch rebuild is performed
    // here — the caller is expected to invoke RebuildAllFeatures() once at
    // the end of the load to materialise GPU resources for every restored
    // feature in one pass.
    void RestoreEjector(const glm::vec3& point);

    // Rebuild all derived geometry after a batch restore (call once at end)
    void RebuildAllFeatures();

    // Clear everything (fixtures, objects, features) for a fresh load
    void ClearAll();

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

    // Parting-plane snap: finds closest point on the mesh's y=0 intersection.
    // outObjectIndex (optional): if non-null and the cast succeeds, receives
    // the index into m_objects of the object whose parting segment won the
    // snap. -1 means no nearby object (cast still fails as before).
    bool RayCastParting(int mouseX, int mouseY,
        glm::vec3& outPos, glm::vec3& outNormal,
        int* outObjectIndex = nullptr);

    // Simple ray–plane intersection with y=0 (no mesh snapping)
    bool RayCastToPartingPlane(int mouseX, int mouseY, glm::vec3& outPos);

    // Snap-pick for ejector placement. Considers four candidate sources:
    //   1. Sprue path's intersection with y=0 (m_sprue.partingPos),
    //   2. Any runner segment (sprue parting pos -> runner pt, on y=0),
    //   3. Any gate segment (gate worldPos -> gate.pathEnd),
    //   4. Any object face (full mesh ray-cast).
    // Path candidates win when within kEjectorSnapRadiusPx screen-space
    // pixels of the cursor; otherwise falls through to the face hit.
    // Returns false if nothing is in range. See implementation for the
    // ranking rationale.
    bool RayCastEjectorSnap(int mouseX, int mouseY, glm::vec3& outPos);

    // Sticky placement helpers — re-derive a parented vent's / gate's
    // world-space data from its parent object's current transform plus the
    // stored local-space placement, then rebuild GPU geometry. No-op when
    // parentIndex is out of range. ReanchorFeaturesForObjects walks both
    // feature lists once and re-anchors anything whose parent is in the
    // supplied set; called from Apply* transforms, drag-translate, and
    // patterning so features track their parent objects automatically.
    void ReanchorVent(VentInstance& vi);
    void ReanchorGate(GateFeature& gf);
    void ReanchorFeaturesForObjects(const std::vector<int>& objIndices);

    // Vent path computation and GPU upload
    VentPath         ComputeVentPath(const VentPoint& vp);
    void             RebuildPathVBO();

    // Vent cross-section geometry
    VentCrossSection BuildVentCrossSection(const VentPath& path,
        float width, float depth);
    void             RebuildCrossSectionVBO();

    // Vent solid is now built via free function BuildBoxSweepMesh() in MouldFeature.h

    // World-space ray cast against imported objects (no mouse unprojection).
    // Fires from 'origin' along 'dir' up to 'maxDist' world units.
    // Returns true and fills outPos with the closest hit; outPos is undefined on miss.
    bool RayCastWorldRay(const glm::vec3& origin, const glm::vec3& dir,
        float maxDist, glm::vec3& outPos);

    // Sprue path GPU upload
    void RebuildSpruePathVBO();

    // Sprue cross-section circle GPU upload (N-segment line-loop approximation)
    void RebuildSprueXsecVBO();

    // Runner path lines GPU upload (sprue parting point → each runner point)
    void RebuildRunnerPathVBO();

    // Runner solid geometry — swept cylinders from sprue parting point to each runner point
    void RebuildRunnerSolids();

    // Gate path lines GPU upload (gate point → nearest feed point)
    void RebuildGatePathVBO();

    // Gate and sub-runner solid geometry
    void RebuildGateSolids();

    // Cylinder/frustum mesh is now built via free function BuildCylinderMesh() in MouldFeature.h

    // Build a world-space ray from mouse coordinates (shared by remove modes)
    void BuildMouseRay(int mouseX, int mouseY,
        glm::vec3& outOrigin, glm::vec3& outDir);

    // Fixture outer perimeter on the parting plane (convex hull in XZ)
    void                   BuildFixturePerimeter();
    std::vector<glm::vec2> m_fixturePerimeter;   // hull vertices in CCW order

    // Sphere mesh for vent point markers
    void BuildSphereGPU(float radius, int stacks, int slices);

    // ---- Align Face ---------------------------------------------------------
    // Pick a triangle on an imported model and grow a coplanar region. Used
    // for hover highlighting and click-to-align. The seed triangle index lets
    // OnPaint detect when the hover changed and avoid redundant region growth.
    bool RayCastFacePick(int mouseX, int mouseY, int& outObj, int& outTri);

    // Ctrl+C / Ctrl+V: in-process clipboard for selected objects. Copy
    // captures the CPU mesh + transform of every currently-selected object
    // (no GPU handles, no mould, no parented features). Paste appends a
    // fresh SceneObject per clipboard entry at the world origin, retaining
    // rotation, scale, and mirror flags; the GPU mesh is rebuilt via the
    // same pipeline used by ImportFile and the pattern operations. After
    // pasting, m_selectedIndices is set to the newly-pasted objects so the
    // user can immediately drag them off the origin.
    void CopySelectedToClipboard();
    void PasteFromClipboard();

    // Build edge adjacency for the object's CPU mesh if not already built.
    void EnsureTriAdjacency(SceneObject& obj);

    // BFS from seedTri across edge-shared neighbors whose triangle normal is
    // within ~1° of the seed's normal. Output is in local (object) space.
    void GrowCoplanarFace(const SceneObject& obj, int seedTri,
        std::vector<uint32_t>& outTris, glm::vec3& outNormalLocal);

    // Upload world-space triangles for the highlighted face to m_alignHighlight*.
    // Pass an empty triangle list to clear the highlight.
    void RebuildAlignHighlightVBO(const SceneObject& obj,
        const std::vector<uint32_t>& tris);

    // Apply the rotation+translation that snaps an arbitrary plane (defined
    // by a local-space normal and a local-space anchor point on the plane)
    // onto the world Y=0 parting plane. The anchor is held fixed laterally
    // (X/Z) so the picked geometry stays put in screen space and only the
    // pose changes. Used by both AlignFace and AlignMidplane.
    void ApplyPlaneAlignmentToObject(int objIdx,
        const glm::vec3& planeNormalLocal,
        const glm::vec3& anchorLocal);

    // AlignFace: thin wrapper around ApplyPlaneAlignmentToObject that uses
    // the picked face's own normal and centroid.
    void ApplyAlignFaceToObject(int objIdx, const glm::vec3& nLocal,
        const std::vector<uint32_t>& faceTris);

    // AlignMidplane: combine the locked first face with a freshly-picked
    // second face into a midplane, then apply alignment.
    void ApplyAlignMidplaneToObject(int objIdx,
        const glm::vec3& n2Local,
        const std::vector<uint32_t>& faceTris2);

    // Build/clear the persistent locked-face overlay used in midplane mode.
    void RebuildMidplaneLockedVBO(const SceneObject& obj,
        const std::vector<uint32_t>& tris);

    // Decompose a YXZ-Euler rotation matrix back to (yaw, pitch, roll).
    // Handles the gimbal-lock case where pitch ≈ ±90°.
    static void DecomposeYXZ(const glm::mat3& R,
        float& yawDeg, float& pitchDeg, float& rollDeg);

private:
    wxGLContext* m_context = nullptr;
    bool         m_inited = false;

    OrbitCamera  m_camera;
    GridRenderer m_grid;
    shaders      m_shaders;

    // Scene
    std::vector<SceneObject> m_fixtures;
    std::vector<SceneObject> m_objects;
    // Selected object indices into m_objects, in click order.
    // Last entry is the "primary" selection (used by single-seed operations
    // such as Pattern). Empty when nothing is selected.
    //   - Plain LMB on an unselected object: replaces the vector with {hit}.
    //   - Plain LMB on an already-selected object: leaves the vector alone
    //     (so a subsequent drag moves the whole group).
    //   - Plain LMB miss: clears the vector.
    //   - Ctrl+LMB on an object: toggles that index in the vector.
    //   - Ctrl+A: fills the vector with every object index.
    std::vector<int>         m_selectedIndices;

    // In-process clipboard populated by Ctrl+C and consumed by Ctrl+V.
    // Stores only the data needed to rebuild a SceneObject from scratch:
    // CPU mesh (so the paste can re-run the normal/crease pipeline and
    // upload its own GPU buffers), source path/shape (so future operations
    // like mould generation and export still see a real BREP), role, and
    // pose minus position. Position is intentionally not captured — paste
    // always places the new object at the world origin per spec.
    //
    // Notably absent: GPUMesh handles (each paste owns its own GPU
    // resources, and stashing live handles here would risk double-free if
    // the source object got deleted before paste); mouldShape/hasMould
    // (matches pattern-op behavior — user re-generates the mould after
    // duplicating); and any parented vents/gates (also matches pattern-op
    // conservatism — the original retains its features, the copy starts
    // clean).
    struct ClipboardEntry
    {
        std::vector<float>    cpuVerts;
        std::vector<uint32_t> cpuIndices;
        std::string           sourcePath;
        TopoDS_Shape          sourceShape;
        bool                  hasSourceShape = false;
        ObjectRole            role = ObjectRole::Imported;

        float yawDeg = 0.0f;
        float pitchDeg = 0.0f;
        float rollDeg = 0.0f;
        float scale = 1.0f;
        bool  mirrorX = false;
        bool  mirrorZ = false;
    };
    std::vector<ClipboardEntry> m_clipboard;

    // Vent features (consolidated: point + path + cross-section + solid)
    std::vector<VentInstance> m_vents;

    // Sprue state (consolidated)
    InjectionPoint m_activeInjectionPoint;         // set from fixture on load
    bool           m_hasActiveInjectionPoint = false;
    std::vector<InjectionPoint> m_injectionPoints; // all points from fixture
    SprueFeature   m_sprue;

    // Ghost preview for vent placement (follows mouse in PlaceVent mode)
    VentPoint m_ventGhost;
    bool      m_ventGhostActive = false;
    wxPoint   m_ghostMousePos;          // last known cursor pos, ray cast deferred to OnPaint

    // Runner features (consolidated: point + solid + cold plug solid)
    std::vector<RunnerFeature> m_runners;

    // Ghost preview for runner placement (follows mouse in PlaceRunner mode)
    glm::vec3 m_runnerGhostPos{ 0.0f };
    bool      m_runnerGhostActive = false;
    wxPoint   m_runnerGhostMousePos;

    // Gate features
    std::vector<GateFeature> m_gates;

    // Ghost preview for gate placement (follows mouse in PlaceGate mode)
    VentPoint m_gateGhost;
    bool      m_gateGhostActive = false;
    wxPoint   m_gateGhostMousePos;

    // Ejector features (placement points only, geometry TBD)
    std::vector<EjectorFeature> m_ejectors;

    // Ghost preview for ejector placement (follows mouse in PlaceEjector mode).
    // No normal field — see EjectorFeature comment in MouldFeature.h.
    glm::vec3 m_ejectorGhostPos{ 0.0f };
    bool      m_ejectorGhostActive = false;
    wxPoint   m_ejectorGhostMousePos;

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

    // Sprue path/xsec GPU resources now live inside m_sprue (SprueFeature)

    // Runner path line GPU resources (sprue parting point → each runner point)
    GLuint  m_runnerPathVAO = 0;
    GLuint  m_runnerPathVBO = 0;
    GLsizei m_runnerPathVertexCount = 0;

    // Gate path line GPU resources (gate point → nearest feed point)
    GLuint  m_gatePathVAO = 0;
    GLuint  m_gatePathVBO = 0;
    GLsizei m_gatePathVertexCount = 0;

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

    // Edit mode state — index of the currently selected feature (-1 = none)
    int     m_editFeatureIndex = -1;
    wxPoint m_editMousePos;              // deferred mouse position for edit drag
    bool    m_editNeedsUpdate = false;   // true when edit drag needs processing in OnPaint

    // Picking FBO
    GLuint m_pickFBO = 0;
    GLuint m_pickColorTex = 0;
    GLuint m_pickDepthRb = 0;
    int    m_pickW = 0;
    int    m_pickH = 0;

    // ---- Align Face state ---------------------------------------------------
    // Hover state for AlignFace mode. Mouse position is captured in OnMouse
    // and the ray cast is deferred to OnPaint (one cast per frame).
    wxPoint               m_alignMousePos;
    int                   m_alignHoverObject = -1;
    int                   m_alignSeedTri = -1;       // last grown seed; -1 = no hover
    std::vector<uint32_t> m_alignFaceTris;           // tris in current hover region
    glm::vec3             m_alignFaceNormalLocal{ 0.0f };

    // GPU resources for the dark-grey highlight overlay (world-space triangles).
    GLuint  m_alignHighlightVAO = 0;
    GLuint  m_alignHighlightVBO = 0;
    GLsizei m_alignHighlightVertexCount = 0;

    // ---- Align Midplane state ----------------------------------------------
    // Locked first-face state for two-click midplane alignment. Reuses the
    // hover state above for the current cursor highlight, and adds a separate
    // VBO for the persistent locked-face overlay (yellow, matches selection
    // outline). Object-local data is captured at click time so subsequent
    // mouse motion or transforms don't invalidate it.
    bool                  m_midplaneFaceLocked = false;
    int                   m_midplaneFaceObject = -1;
    std::vector<uint32_t> m_midplaneFaceTris;
    glm::vec3             m_midplaneFaceNormalLocal{ 0.0f };
    glm::vec3             m_midplaneFaceCentroidLocal{ 0.0f };

    GLuint  m_midplaneLockedVAO = 0;
    GLuint  m_midplaneLockedVBO = 0;
    GLsizei m_midplaneLockedVertexCount = 0;

    // ---- Scene mutation observer ------------------------------------------
    // Registered by MainFrame to invalidate the Export button. Fired from
    // mutation methods via NotifySceneMutated() below — see SetOnSceneMutated
    // (public, near GenerateMould) for the user-facing contract.
    std::function<void()> m_onSceneMutated;

    // Helper: fire the scene-mutation callback if one is registered. Called
    // from every mutating method on this class. Synchronous — the callback
    // runs on the wxWidgets main thread before the mutating method returns.
    void NotifySceneMutated()
    {
        if (m_onSceneMutated) m_onSceneMutated();
    }
};
