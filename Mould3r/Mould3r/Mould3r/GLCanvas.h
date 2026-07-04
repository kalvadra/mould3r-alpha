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

// PathEditTool (the EditVent sub-tool enum) is declared in MainFrame.h next to
// TransformMode so the lightweight VentEditToolbar header can use it without
// pulling in this heavy canvas header.

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

    // Precision Place — move the current selection to an absolute world XZ
    // position, preserving each member's Y (height) and the selection's
    // internal arrangement. For a single object this is a direct
    // "pos.x = x, pos.z = z"; for a multi-selection the whole group is
    // shifted so its XZ centroid lands on the target (same convention as
    // CenterSelectedObject).
    void MoveSelectionToXZ(float x, float z);

    // Report the XZ centroid of the current selection (the value the
    // Precision Place dialog pre-fills). Returns false (leaving outputs
    // untouched) when nothing is selected.
    bool GetSelectionCenterXZ(float& outX, float& outZ) const;

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

    // ---- Preview perspective ------------------------------------------------
    // A second GLCanvas instance (hosted by PreviewPanel) is put into preview
    // mode to show the post-cut mould halves on their own. Preview mode strips
    // the canvas down to: grid + the loaded mould halves, with the standard
    // orbit / pan / dolly navigation but no picking, selection, transform
    // modes, feature placement, or keyboard shortcuts. The main canvas is
    // never in preview mode.

    // Switch this canvas into (or out of) preview mode. Idempotent.
    void SetPreviewMode(bool on) { m_previewMode = on; }
    bool IsPreviewMode() const { return m_previewMode; }

    // Append one part to the preview from a world-space mesh (position +
    // normal interleaved, plus indices — exactly the MeshData produced by
    // GenerateMould). The mesh is uploaded to this canvas's own GL context and
    // stored with an identity pose, since the fixture transform is already
    // baked into the world-space vertices. `label` is shown on the part's
    // show/hide toggle (e.g. "Half A", "Shot"). `baseColor` lets the shot
    // model read distinctly from the grey mould halves. Parts start visible.
    // Must be called with this canvas's GL context current — PreviewPanel
    // arranges that.
    void AddPreviewHalf(const FileImporter::MeshData& mesh,
        const std::string& label,
        const glm::vec3& baseColor = glm::vec3(0.80f, 0.80f, 0.85f));

    // Number of preview parts (mould halves + shot) currently loaded.
    int  GetPreviewHalfCount() const { return (int)m_previewHalves.size(); }

    // Label of part `index` (empty string if out of range).
    std::string GetPreviewHalfLabel(int index) const;

    // Show / hide an individual part. Out-of-range indices are ignored.
    void SetPreviewHalfVisible(int index, bool visible);
    bool IsPreviewHalfVisible(int index) const;

    // Drop every loaded preview part (frees their GPU resources). Safe to call
    // with no context current only if nothing has been uploaded yet.
    void ClearPreviewHalves();

    // ---- Design-check debug colouring --------------------------------------
    // One flat-coloured group of facets for the debug overlay: every triangle
    // in `indices` (vertex-index triples into the part's own vertex buffer)
    // draws in `color`. The caller partitions the part's triangles into groups.
    struct ShotDebugGroup
    {
        glm::vec3             color{ 0.5f };
        std::vector<uint32_t> indices;
        bool                  emissive = false;  // draw flat (highlight) vs lit
    };

    // Recolour a preview part's facets using the given groups, for debugging
    // (each group drawn over a dedicated debug VAO that reuses the part's
    // existing vertices). Replaces any previous debug colouring. Used to
    // visualise check categories or, e.g., draft sign across the shot. A group
    // flagged `emissive` draws at full intensity (lighting flattened to its
    // flat colour) so it reads as a highlight; other groups stay shaded by the
    // normal lit pass, preserving the surface's 3D form.
    void SetShotDebugGroups(int halfIndex,
        const std::vector<ShotDebugGroup>& groups);

    // Turn debug colouring off — the shot returns to its normal single colour.
    void ClearShotDebugColoring();

    // ---- Design-check debug rays -------------------------------------------
    // Upload accessibility-ray debug geometry for the preview: `rayLineVerts`
    // is GL_LINES vertex pairs (world space) for the ray segments, and
    // `contactVerts` is GL_POINTS world positions for the contact markers.
    // Rendered with the flat (unlit) shader. Replaces any previous ray geometry.
    void SetShotDebugRays(const std::vector<glm::vec3>& rayLineVerts,
        const std::vector<glm::vec3>& contactVerts);

    // Toggle visibility of the ray segments / contact markers independently.
    void ShowShotDebugRays(bool on);
    void ShowShotDebugContacts(bool on);

    // Forget ray geometry and hide both overlays.
    void ClearShotDebugRays();

    // ---- Design-check debug solid ------------------------------------------
    // Upload a free-standing solid (e.g. the interference region from the
    // separation test) and show it in the preview, lit, in `color`. Replaces
    // any previous debug solid. Rendered at identity pose (world space).
    void SetShotDebugSolid(const TopoDS_Shape& shape, const glm::vec3& color);
    void ShowShotDebugSolid(bool on);
    void ClearShotDebugSolid();

    // Read-only access to the meshes produced by the most recent successful
    // GenerateMould run, one per fixture, in fixture order. World-space,
    // interleaved position+normal with indices. PreviewPanel consumes these
    // to populate a fresh preview window. Cleared at the start of every
    // GenerateMould call.
    const std::vector<FileImporter::MeshData>& GetLastMouldMeshes() const
    {
        return m_lastMouldMeshes;
    }

    // The "shot" model from the most recent successful GenerateMould: the
    // boolean union of every imported object and the feed-system features
    // (sprue, runners, gates and their sub-parts) — i.e. all placed features
    // except vents and ejectors. World-space, interleaved position+normal
    // with indices. This is the material body that fills the cavity; it is
    // also the geometry that downstream simulation will operate on. Valid only
    // when HasLastShotMesh() returns true (the union can legitimately be empty
    // when no objects/feed features exist or the fuse failed).
    bool HasLastShotMesh() const { return m_hasLastShotMesh; }
    const FileImporter::MeshData& GetLastShotMesh() const
    {
        return m_lastShotMesh;
    }

    // Volume of the shot solid from the most recent successful GenerateMould,
    // in cubic millimetres (the scene's modelling unit). Computed from the
    // fused BREP — overlaps between fused primitives are counted once — so it
    // is the true material volume, not a mesh approximation. Zero when no shot
    // was built (HasLastShotMesh() == false).
    double GetLastShotVolumeMm3() const { return m_lastShotVolumeMm3; }

    // The shot solid as a BREP, for design-check analysis at the face level.
    // Valid only when HasLastShotMesh() is true.
    const TopoDS_Shape& GetLastShotShape() const { return m_lastShotShape; }

    // Per display-triangle source-face index (1-based, into a
    // TopExp::MapShapes(shot, TopAbs_FACE) map of GetLastShotShape()). One
    // entry per triangle of GetLastShotMesh(); lets a face-level result be
    // mapped back to the shot's display triangles for colouring.
    const std::vector<int>& GetLastShotFaceIds() const { return m_lastShotFaceIds; }

    // The post-cut mould-half solids (BREP) from the most recent successful
    // GenerateMould, in fixture order. Used by the separation-based
    // demoldability check (translate each half off the shot and test for
    // interference). Empty when no mould was generated.
    const std::vector<TopoDS_Shape>& GetLastHalfShapes() const
    {
        return m_lastHalfShapes;
    }

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

    // ---- Part 5: complex vent-path authoring hooks -------------------------
    // Fired whenever the EditVent selection or path-edit state changes (a vent
    // is picked / deselected, a Simple<->Complex conversion happens, the
    // smooth flag flips, or a node is added / removed). The MainFrame uses it
    // to reconfigure and reposition the floating VentEditToolbar.
    void SetOnPathEditChanged(std::function<void()> cb)
    {
        m_onPathEditChanged = std::move(cb);
    }

    // Register the floating toolbar window (a child of this canvas). The canvas
    // treats it as an opaque wxWindow* — it only needs to reposition it on
    // resize so it stays pinned to the top-centre of the viewport.
    void SetPathToolbar(wxWindow* w) { m_pathToolbar = w; RepositionPathToolbar(); }

    // State queried by the floating toolbar.
    bool         IsEditingVent()       const { return m_transformMode == TransformMode::EditVent; }
    bool         HasEditVentSelected() const
    {
        return m_transformMode == TransformMode::EditVent &&
            m_editFeatureIndex >= 0 && m_editFeatureIndex < (int)m_vents.size();
    }
    bool         IsEditVentComplex()   const;
    bool         IsEditVentSmooth()    const;
    int          EditVentNodeCount()   const;
    PathEditTool GetPathEditTool()     const { return m_pathEditTool; }

    // Commands invoked by the floating toolbar.
    void SetPathEditTool(PathEditTool t);
    void ConvertEditVentToComplex();   // seed nodes from the Simple path; no-op if already Complex
    void ConvertEditVentToSimple();    // drop nodes, re-derive the Simple path
    void SetEditVentSmooth(bool smooth);

    // Runner-path authoring (Part 7 / R5) — parallels the vent API above and is
    // driven by the SAME shared floating toolbar while in EditRunner mode. The
    // deltas vs vents: node[0] is pinned to the sprue feed point (only the free
    // endpoint moves) and the endpoint is NOT snapped to the perimeter.
    bool         IsEditingRunner()       const { return m_transformMode == TransformMode::EditRunner; }
    bool         HasEditRunnerSelected() const
    {
        return m_transformMode == TransformMode::EditRunner &&
            m_editFeatureIndex >= 0 && m_editFeatureIndex < (int)m_runners.size();
    }
    bool         IsEditRunnerComplex()   const;
    bool         IsEditRunnerSmooth()    const;
    int          EditRunnerNodeCount()   const;

    void ConvertEditRunnerToComplex();  // seed [feed point, endpoint]; no-op if already Complex
    void ConvertEditRunnerToSimple();   // drop nodes, collapse to the straight runner
    void SetEditRunnerSmooth(bool smooth);

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

    // Restore a runner whose path was authored as a complex (multi-node) path.
    // Like RestoreVentComplex, the path is rebuilt verbatim from `nodes` (>= 2,
    // on the parting plane) and `smooth` rather than re-derived; `point` is the
    // runner endpoint (== nodes.back()). Solids are built by the following
    // RebuildRunnerSolids (ComputeRunnerPath preserves the authored path).
    void RestoreRunnerComplex(const glm::vec3& point,
        const std::vector<PathNode>& nodes, bool smooth);

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

    // Restore a vent whose path was authored as a complex (multi-node) path.
    // Unlike RestoreVent, the path is NOT re-derived — it is rebuilt verbatim
    // from `nodes` (>= 2, on the parting plane) and `smooth`. pos/normal are
    // the vent origin (matches nodes.front()).
    void RestoreVentComplex(const glm::vec3& pos, const glm::vec3& normal,
        const std::vector<PathNode>& nodes, bool smooth,
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
    void OnMouseDClick(wxMouseEvent& evt);
    void OnMouseWheel(wxMouseEvent& evt);
    void OnKeyDown(wxKeyEvent& evt);

    // Preview-mode render path: grid + loaded mould halves only, all opaque,
    // honouring each half's visibility flag. Called from OnPaint after the
    // clear + camera setup when m_previewMode is true; the normal scene/
    // feature passes are skipped entirely.
    void RenderPreview(const glm::mat4& view, const glm::mat4& proj,
        const glm::vec3& camPos);

    // Build the "shot" solid: the boolean union of every imported object
    // (transformed to world space) and the feed-system features — sprue +
    // cold slug, runners + cold plugs, gates + sub-runners — but NOT vents or
    // ejectors. All contributing primitives are constructed in world space
    // with the same geometry the cut loop uses, then fused pairwise. Returns
    // true and fills `out` when a non-null union results; returns false when
    // nothing contributed or the fuse failed outright. Read by GenerateMould.
    bool BuildShotModel(TopoDS_Shape& out);

    // Tessellate an OCC shape into a render-ready MeshData (vertices meshed,
    // per-vertex normals computed, crease-split applied so the interleaved
    // posNorm + indices upload cleanly), with orientation-aware winding.
    // `mesh` is cleared first. When `faceIds` is non-null it is filled with one
    // entry per output triangle: the 1-based index of that triangle's source
    // face in a TopExp::MapShapes(shape, TopAbs_FACE) map (the crease split
    // preserves triangle order, so the map stays aligned with mesh.indices).
    void TessellateShapeToMesh(const TopoDS_Shape& shape,
        FileImporter::MeshData& mesh,
        std::vector<int>* faceIds = nullptr);

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

    // ---- Part 5: complex vent-path authoring internals ---------------------
    // Rebuild the currently edited vent's cross-section, preview solid and the
    // start/end mirror from its (already mutated) path.nodes, reading width /
    // length / overrun from the MainFrame UI. Recomputes auto handles first
    // when the path is smooth. Refreshes path + cross-section VBOs.
    void RebuildEditVentGeometry();

    // Closest point (XZ, y=0) on the fixture perimeter polygon to p. Returns p
    // unchanged when no perimeter is available.
    glm::vec3 SnapToFixturePerimeter(const glm::vec3& p) const;

    // Pick the nearest node marker of the currently edited Complex vent to the
    // mouse ray; returns its node index or -1 if none is within the marker hit
    // radius.
    int  PickEditVentNode(int mouseX, int mouseY) const;

    // ---- Part 6: tangent-handle editing (smooth complex vents, Move tool) ---
    // Pick the nearest tangent-handle endpoint of the edited smooth vent to the
    // mouse ray. Returns the owning node index (and sets outIsOut = true for the
    // outgoing arm, false for the incoming arm) or -1 if none is in range.
    int  PickEditVentHandle(int mouseX, int mouseY, bool& outIsOut) const;

    // Drag a tangent handle to follow the cursor on the parting plane. Marks the
    // node hand-edited. breakLink (Alt) moves only the dragged arm and unlinks
    // the node; otherwise a linked node mirrors the opposite arm (symmetric).
    void MoveEditVentHandle(int node, bool isOut, int mouseX, int mouseY, bool breakLink);

    // Rebuild the GL line buffer that draws node->handle stems for the edited
    // smooth vent (called per frame while the handles are visible).
    void RebuildHandleLineVBO();

    // Insert a new node into the path of vent `ventIndex` at world point
    // `worldPt` (which the caller has snapped onto that vent's path). Selects
    // the vent, auto-converts Simple -> Complex, and splices the node into the
    // nearest segment so it lands between the two nodes that section spans.
    void InsertNodeOnVentAt(int ventIndex, const glm::vec3& worldPt);

    // Screen-space snap of the cursor onto an existing vent path (Add Node
    // tool). Mirrors RayCastEjectorSnap's runner snapping: returns the closest
    // point on any vent's RENDERED polyline within kEjectorSnapRadiusPx, plus
    // which vent it belongs to. Lets the user only place nodes on existing
    // paths, associating each new node with the path it snapped to.
    bool RayCastPathNodeSnap(int mouseX, int mouseY,
        glm::vec3& outPos, int& outVentIndex) const;

    // Remove node `idx` from the edited Complex vent. Origin (0) and endpoint
    // (last) are protected; interior nodes only. No-op if it would drop below
    // two nodes or idx is out of range.
    void RemoveEditVentNode(int idx);

    // Move node `idx` of the edited Complex vent to follow the mouse on the
    // parting plane. Origin re-snaps to a part edge (and recaptures parent),
    // the endpoint snaps to the fixture perimeter, interior nodes move freely.
    void MoveEditVentNode(int idx, int mouseX, int mouseY);

    // ---- Part 7 / R5b: runner node authoring (parallels the vent methods) ----
    // The runner deltas: node[0] is PINNED to the sprue feed point (not
    // grabbable), the endpoint is FREE inside the perimeter (no snap), and all
    // nodes must stay inside the fixture hull.
    int  PickEditRunnerNode(int mouseX, int mouseY) const;   // nearest node marker, -1 if none
    void MoveEditRunnerNode(int idx, int mouseX, int mouseY);// drag endpoint/interior (node[0] pinned)
    void RemoveEditRunnerNode(int idx);                      // interior only; feed(0)+endpoint protected
    void InsertNodeOnRunnerAt(int runnerIndex, const glm::vec3& worldPt); // splice on the snapped path
    // Screen-space snap onto any runner's RENDERED polyline (Add Node tool).
    bool RayCastRunnerNodeSnap(int mouseX, int mouseY,
        glm::vec3& outPos, int& outRunnerIndex) const;

    // ---- Part 7 / R6: runner tangent-handle editing (mirrors the vent Part-6
    // handle machinery, reusing m_editHandle* state + the handle-line VAO). The
    // feed node (0) exposes only its outgoing arm and the endpoint only its
    // incoming arm, exactly as for vents; the feed POSITION stays pinned.
    int  PickEditRunnerHandle(int mouseX, int mouseY, bool& outIsOut) const;
    void MoveEditRunnerHandle(int node, bool isOut, int mouseX, int mouseY, bool breakLink);

    // Reposition the floating toolbar to the top-centre of the viewport.
    void RepositionPathToolbar();

    void NotifyPathEditChanged() { if (m_onPathEditChanged) m_onPathEditChanged(); }

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

    // Part 7 (R1): derive a runner's Simple FeaturePath (start = sprue feed
    // point, end = runner point) into rf.path.  Pure bookkeeping — it keeps the
    // stored path in sync as the sprue or runner point move so later steps can
    // author a Complex route in its place; it drives no geometry yet.
    void ComputeRunnerPath(RunnerFeature& rf) const;

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

    // ---- Preview perspective state -----------------------------------------
    // True only for the dedicated preview canvas hosted by PreviewPanel. The
    // main editing canvas leaves this false and behaves exactly as before.
    bool m_previewMode = false;

    // One entry per part shown in the preview (mould halves + the shot). The
    // SceneObject carries its own GPU mesh (uploaded into the preview canvas's
    // context) at an identity pose — the fixture transform is already baked
    // into the world-space vertices handed to AddPreviewHalf. `label` drives
    // the part's show/hide toggle text; `visible` is flipped by that toggle;
    // `baseColor` lets the shot read distinctly from the grey mould halves.
    struct PreviewHalf
    {
        SceneObject obj;
        std::string label;
        bool        visible = true;
        glm::vec3   baseColor{ 0.80f, 0.80f, 0.85f };
    };
    std::vector<PreviewHalf> m_previewHalves;

    // Debug colouring overlay for one preview part (the shot). When active, the
    // named part is drawn as a set of flat-coloured groups instead of its
    // normal single colour. The debug VAO references the part's existing vertex
    // buffer, so only the per-group index buffers are owned here. GPU objects
    // are freed on the next SetShotDebugGroups call or by context teardown.
    struct DebugGroupGPU
    {
        GLuint    ebo = 0;
        GLsizei   count = 0;
        glm::vec3 color{ 0.5f };
        bool      emissive = false;
    };
    struct ShotDebugView
    {
        bool    active = false;
        int     halfIndex = -1;
        GLuint  vao = 0;
        std::vector<DebugGroupGPU> groups;
    };
    ShotDebugView m_shotDebug;

    // Accessibility-ray debug overlay (preview): ray segments drawn as GL_LINES
    // and contact points as GL_POINTS via the flat shader. Geometry is world
    // space; visibility of each is toggled independently.
    GLuint  m_debugRayVao = 0;
    GLuint  m_debugRayVbo = 0;
    GLsizei m_debugRayVertCount = 0;
    GLuint  m_debugContactVao = 0;
    GLuint  m_debugContactVbo = 0;
    GLsizei m_debugContactVertCount = 0;
    bool    m_showDebugRays = false;
    bool    m_showDebugContacts = false;

    // Free-standing lit debug solid (the separation test's interference region).
    // Stored as a SceneObject so the normal mesh upload/render path applies.
    SceneObject m_debugSolidObj;
    bool        m_showDebugSolid = false;
    glm::vec3   m_debugSolidColor{ 0.90f, 0.15f, 0.15f };

    // Meshes from the most recent successful GenerateMould (one per fixture,
    // world-space, position+normal interleaved with indices). Populated in
    // GenerateMould and consumed by PreviewPanel via GetLastMouldMeshes().
    // The main canvas no longer swaps these into its own fixtures — they live
    // here purely to seed the preview window.
    std::vector<FileImporter::MeshData> m_lastMouldMeshes;

    // The shot model from the most recent successful GenerateMould: union of
    // all imported objects + feed features (sprue / runners / gates), minus
    // vents and ejectors. World-space, position+normal interleaved with
    // indices. m_hasLastShotMesh is false when no shot could be built (no
    // contributing geometry, or the boolean union produced nothing usable).
    FileImporter::MeshData m_lastShotMesh;
    bool                   m_hasLastShotMesh = false;

    // Volume of the shot solid (cubic mm), computed from the fused BREP at
    // generation time. Zero when no shot was built.
    double                 m_lastShotVolumeMm3 = 0.0;

    // The shot solid (BREP) and a per-display-triangle face-index map, kept so
    // design checks can run at the face level and map results back to the
    // display triangles. Face indices are 1-based into a TopExp::MapShapes
    // face map of m_lastShotShape, matching what DesignChecks rebuilds.
    TopoDS_Shape           m_lastShotShape;
    std::vector<int>       m_lastShotFaceIds;

    // The post-cut mould-half solids (BREP), in fixture order, retained for the
    // separation-based demoldability check.
    std::vector<TopoDS_Shape> m_lastHalfShapes;

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

    // ---- Part 5: complex vent-path authoring state -------------------------
    PathEditTool m_pathEditTool = PathEditTool::Move;  // active EditVent sub-tool
    int          m_editVentNode = -1;   // node being dragged (Move tool), -1 = none
    int          m_editRunnerNode = -1; // runner node being dragged (Move tool), -1 = none
    wxWindow*    m_pathToolbar  = nullptr;  // floating toolbar overlay (opaque)
    std::function<void()> m_onPathEditChanged;  // toolbar reconfigure hook

    // Add Node ghost — snaps onto an existing vent path under the cursor.
    glm::vec3 m_pathNodeGhostPos{ 0.0f };
    bool      m_pathNodeGhostActive = false;
    wxPoint   m_pathNodeGhostMousePos;

    // ---- Part 6: tangent-handle drag state ---------------------------------
    int    m_editHandleNode = -1;     // node whose handle is grabbed (-1 = none)
    bool   m_editHandleIsOut = false; // true = outgoing arm, false = incoming
    bool   m_editHandleBreak = false; // Alt held during the current handle drag
    GLuint  m_handleLineVAO = 0;      // node->handle stems (flat program)
    GLuint  m_handleLineVBO = 0;
    GLsizei m_handleLineVertexCount = 0;

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
