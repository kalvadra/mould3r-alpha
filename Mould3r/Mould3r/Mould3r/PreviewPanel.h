#pragma once

#include <wx/wx.h>
#include <wx/tglbtn.h>
#include <wx/checkbox.h>
#include <vector>
#include <unordered_map>

#include <opencascade/TopoDS_Shape.hxx>  // shot BREP, stored for face checks

#include "FileImporter.h"   // FileImporter::MeshData
#include "DesignChecks.h"   // DesignChecks::FaceDraftStats / DraftSample
#include "FlowMesh.h"       // Flow::FlowMesh / FlowMeshStats (Hele-Shaw P1)
#include "FeedNetwork.h"    // Flow::FeedNetwork — 1D feed-system snapshot
#include "Midplane.h"       // Flow::PartSurface / MidplaneMesh — planform midplane
#include "CoupledFill.h"    // Flow::CoupledFillResult — feed + cavity fill
#include "GridSettings.h"   // GridSettings — forwarded to the preview canvas
#include "FixtureFile.h"    // FixtureKind — gates cast generation

#include <glm/glm.hpp>      // cached half bounds (perimeter of the cast bases)

class GLCanvas;
class wxSpinCtrlDouble;
namespace MeshBoolean { struct Mesh; }   // travel-volume return type (defined in MeshBoolean.h)

// Bundle of the shot artefacts handed to the preview. All pointers may be null
// (no shot). The panel copies what it keeps; the caller need not preserve the
// pointees afterwards.
struct ShotPreviewInput
{
    const FileImporter::MeshData* mesh = nullptr;     // display mesh
    const TopoDS_Shape*           shape = nullptr;    // BREP for face checks
    const std::vector<int>*       faceIds = nullptr;  // per display tri -> face
    double                        volumeMm3 = 0.0;

    // The "Cast Shot Body" (standard shot + vents + scaled inserts + ejector
    // pins), used only by cast-mould base generation. May be null (no cast shot
    // was built); the base generation then falls back to `mesh`. `castShape` is
    // the matching BREP solid (null in a mesh scene) so the bases can be built
    // as STEP-exportable solids.
    const FileImporter::MeshData* castMesh = nullptr;
    const TopoDS_Shape*           castShape = nullptr;

    // True when the last generation was a mesh-toolpath scene. The BREP design
    // checks can't run on it, so the panel refuses them (see OnStartSimulation).
    // Carried here so it's set even when there's no BREP shot to attach.
    bool sceneIsMesh = false;

    // Which kind of mould produced this generation. Cast generation is locked
    // to procedural moulds — Parametric (fixed box) and Dynamic (adaptive box)
    // — whose perimeter is a clean rectangle; a Library mould can't be cast.
    // Carried here so PreviewPanel can gate the Generate Mould Casts flow.
    FixtureKind mouldKind = FixtureKind::Library;

    // The feed system (sprue / runners / gates) + vents + one node per moulded
    // object, snapshotted as a 1D nodal network at Generate Mould
    // (GLCanvas::BuildFeedNetwork). Drives the Hele-Shaw flow analysis and its
    // "Flow network" debug view. May be null (no network built).
    const Flow::FeedNetwork* feedNetwork = nullptr;

    // Each moulded object's own surface (world space), snapshotted at Generate
    // Mould (GLCanvas::BuildPartSurfaces) — the source for the part midplane
    // meshes. objectIndex matches the network's part nodes. May be null.
    const std::vector<Flow::PartSurface>* partSurfaces = nullptr;
};

// ===========================================================================
// PreviewPanel
//
// The "Preview" workflow perspective, embedded as a page inside MainFrame
// (previously the standalone PreviewFrame top-level window). It hosts a
// GLCanvas running in preview mode (grid + halves, full orbit/pan/dolly
// navigation, no editing), a show/hide toggle bar across the top, a left
// panel of runnable simulations, and a right information panel.
//
// Lifecycle: a single instance lives for the lifetime of MainFrame. The grid
// renders immediately, even with no data. Each successful Generate Mould calls
// SetData() to (re)seed the post-cut halves + shot; the GL upload is deferred
// via FlushIfDirty() until the page is actually visible, since a canvas on a
// hidden book page may not yet have a valid drawable.
// ===========================================================================
class PreviewPanel : public wxPanel
{
public:
    explicit PreviewPanel(wxWindow* parent);

    // (Re)seed the preview with a fresh set of post-cut halves and an optional
    // shot. Clears any previously loaded parts, rebuilds the toggle bar and the
    // information panel, and marks the GL data dirty. The actual upload happens
    // in FlushIfDirty once the panel is shown on screen. `halves` are the
    // world-space post-cut meshes (one per fixture, in fixture order); `shot`
    // bundles the shot model (display mesh, BREP, per-triangle face map, volume)
    // and, when its mesh is present, gets its own show/hide toggle, renders in a
    // distinct colour, and is analysable by the design checks.
    void SetData(const std::vector<FileImporter::MeshData>& halves,
        const ShotPreviewInput& shot = {},
        const std::vector<FileImporter::MeshData>& inserts = {});

    // Drop all loaded parts and reset the panel to its empty (grid-only) state.
    void ClearData();

    // If a SetData is pending and the panel is now visible, push the captured
    // meshes into the canvas's GL context. Called by MainFrame right after it
    // switches the active perspective to Preview (and harmlessly a no-op when
    // nothing is dirty or the panel is still hidden). Idempotent.
    void FlushIfDirty();

    // Push the ground-plane grid configuration to the preview's own canvas so
    // it matches the Prepare perspective. MainFrame calls this on entry to the
    // Preview perspective (the Grid menu lives only in Prepare, so settings
    // can't change while Preview is showing — syncing on entry is enough).
    void SetGridSettings(const GridSettings& s);

    // One exportable cast body: a filename-safe suffix, the display mesh (always
    // present, used for STL export), and — in a BREP scene — the exact OCC solid
    // (used for STEP export). `hasShape` gates the STEP path.
    struct CastExportBody
    {
        std::string            suffix;
        FileImporter::MeshData mesh;
        TopoDS_Shape           shape;
        bool                   hasShape = false;
    };

    // Cast bodies retained for export (populated by Generate Mould Casts): the
    // Top Base and Bottom Base, plus ONE half of each wall (the two halves are
    // symmetric about y = 0, so only one need be saved). Empty until casts have
    // been generated. MainFrame's "Export Mould Casts" writes one file per entry
    // — STEP when the body carries a shape (BREP scene), else STL.
    bool HasCastBodies() const { return !m_castExports.empty(); }
    const std::vector<CastExportBody>& GetCastExportBodies() const
    {
        return m_castExports;
    }

private:
    // Build one visibility checkbox per part: a checkbox for each mould half,
    // then a "Shot" checkbox when a shot model is present, into m_visPanel (in
    // the left column, above the Simulations section). Re-runnable: call
    // ClearVisibilityChecks first to drop the previous set.
    void BuildVisibilityChecks(int halfCount, bool hasShot, int insertCount);
    void ClearVisibilityChecks();

    // One child body inside a cast group (Top Cast / Bottom Cast).
    struct CastChild
    {
        FileImporter::MeshData mesh;
        wxString               label;
        glm::vec3              color{ 0.5f };
        wxString               tip;
    };

    // Upload one cast body to the canvas at the next free preview-part index
    // (no UI) and return that index. Used by AddCastGroup.
    int  AddCastPart(const FileImporter::MeshData& mesh, const glm::vec3& color,
        const wxString& label);

    // Add a collapsible cast GROUP to the Preview Output Bodies card: a parent
    // "master" checkbox that shows/hides the whole group, plus a chevron on the
    // right that expands to per-child checkboxes. Uploads each child's mesh to
    // the canvas. The group panels are tracked (m_castGroupPanels) so a cast
    // re-generation can drop them via ClearCastChecks without disturbing the
    // half / shot / insert toggles.
    void AddCastGroup(const wxString& groupLabel, std::vector<CastChild>& children);
    void ClearCastChecks();

    // Left panel: a list of runnable simulations, each with its own Start
    // button. Right panel: read-only information about the shot. Both are built
    // once in the constructor; the info panel's value labels are updated in
    // place via UpdateInfoPanel as data changes.
    wxPanel* BuildSimPanel(wxWindow* parent);
    wxPanel* BuildInfoPanel(wxWindow* parent);
    // The "Physical Setup" bar across the top of the centre column (beneath the
    // perspective/generate toolbar): injection + mould material dropdowns.
    // Groundwork for material-dependent simulations.
    wxPanel* BuildPhysicalSetupBar(wxWindow* parent);
    void     UpdateInfoPanel();

    // Entry point for a simulation's Start button. "Draft Angle Checks" runs the
    // face-by-face draft check (RunFaceDraftCheck); "Separation Test" runs the
    // collision check; any other name reports it isn't implemented.
    void OnStartSimulation(const wxString& simName);

    // Open the "Generate Mould Casts" dialog (wall + base characteristics for
    // silicone / sand casting). UI scaffolding only for now — it collects the
    // user's settings; building the wall + shot-cavity base geometry and adding
    // them to the preview scene is a later step.
    void OnGenerateMouldCasts();

    // Run the separation/collision demoldability check: lift each mould half
    // off the shot and test for interference. Reports the verdict and shows the
    // interference region as a red overlay.
    void RunSeparationCheck();

    // Run the Hele-Shaw 2.5D flow analysis on the feed network captured at
    // Generate Mould: resolve the Physical Setup materials and process fields,
    // solve the steady feed system (Cross-WLF at melt temp, sprue inlet -> part
    // nodes) for the feed pressure drop and gate flow split, report it, and
    // show the "Flow network" debug view. Each part is one lumped node for now;
    // its cavity flow field is the next step (see HeleShaw2D_Plan.md).
    // Dispatched from the "Hele-Shaw 2.5D Flow" card.
    void RunFlowCheck();

    // Run the Draft Angle Checks: split the shot at the parting plane, assign
    // each facet's owning mould half by casting a ray along its outward normal
    // into the generated half meshes, then measure signed draft against that
    // half's pull. Reports per-facet pass/warn/fail counts (gated by the
    // minimum-significant-area filter) and drives the "Draft (ray)" overlay.
    // Remesh-free; runs on the shot display mesh, on BREP and mesh scenes alike.
    void RunFaceDraftCheck();

    // Ensure the parting-split, ownership-assigned shot mesh exists (the shared
    // foundation for the Draft Angle Checks and the Separation Test): splits the
    // shot at the parting plane and casts the ownership ray, caching the result
    // in m_faceDraft*. Cheap no-op when already built for this shot. Returns
    // false when there is no shot or no generated mould halves to work from.
    bool EnsureFaceDraftAnalysis();

    // Ensure the Hele-Shaw flow mesh exists (P1): the shot surface soup with a
    // per-facet wall thickness from dual-domain opposite-wall pairing, cached in
    // m_flowMesh / m_flowMeshStats. Cheap no-op when already built for this shot.
    // Returns false when there is no shot mesh to build from.
    bool EnsureFlowMesh();

    // Ensure every part's planform midplane mesh exists at the card's target
    // triangle area (rebuilt when the area changes or after a new generation).
    // Returns true if at least one part produced a mesh.
    bool EnsureMidplanes();

    // Build one mould half's "travel volume": the shot surface that half owns,
    // swept toward the parting plane by the half's height (see the Separation
    // Test). `side` is 0 (+draw) or 1 (-draw). Returns an empty mesh when that
    // side owns no surface or the sweep can't be formed. `startEps` lifts the
    // swept prism off the coincident cavity wall (mm) to suppress contact noise.
    // Assumes the ownership analysis is current (call EnsureFaceDraftAnalysis).
    MeshBoolean::Mesh BuildSideTravelVolume(int side, float startEps) const;

    // Apply or clear the Draft Angle Checks overlay per the Debug View dropdown:
    // None (clear, optional wireframe) or "Draft (ray)" (the parting-split
    // analysis mesh coloured by per-facet signed draft).
    void UpdateDraftOverlay();

    // Show or hide the Separation Test mould overlay (the red interference
    // solid produced by the last run) according to that card's "Show mould
    // overlay" checkbox. A no-op when no interference solid is available.
    void UpdateSeparationOverlay();

    // Upload the captured meshes into the canvas's context (halves first, then
    // the shot) and enable the toggles. Run via CallAfter so the canvas window
    // is fully realized (its GL context valid) before any GL call is issued.
    void LoadHalves();

    GLCanvas* m_canvas = nullptr;

    // Visibility (show/hide) controls, in the "Preview Output Bodies" card in
    // the left column above the Simulations section. m_visPanel is the card
    // body; m_visEmptyLabel shows "No bodies generated" when nothing is loaded;
    // one checkbox per loaded part (mould halves + shot) otherwise, rebuilt by
    // BuildVisibilityChecks on each SetData.
    wxPanel* m_visPanel = nullptr;
    wxStaticText* m_visEmptyLabel = nullptr;
    std::vector<wxCheckBox*> m_halfChecks;

    // Cast-body visibility widgets, kept apart from m_halfChecks so a cast
    // re-generation can drop and rebuild just these (their preview-part indices
    // start at m_castAnchorCount). Cast bodies are organised into collapsible
    // "Top Cast" / "Bottom Cast" groups: m_castGroupPanels holds each group's
    // container panel (destroying it frees the child checkboxes too), and
    // m_castChecks references every child checkbox for convenience.
    std::vector<wxCheckBox*> m_castChecks;
    std::vector<wxPanel*>    m_castGroupPanels;

    // Exportable cast bodies (survive past the GPU upload). Rebuilt on each
    // Generate Mould Casts; consumed by MainFrame's "Export Mould Casts".
    std::vector<CastExportBody> m_castExports;

    // Insert preview bodies. Unlike halves and the shot, ALL inserts share a
    // SINGLE show/hide checkbox (m_insertCheck) rather than one each — the user
    // treats "the inserts" as one category. m_pendingInserts stages the meshes
    // until the panel is visible (same deferral as m_pendingHalves);
    // [m_insertFirstIndex, m_insertFirstIndex + m_insertCount) is the contiguous
    // block of preview-part indices they occupy once loaded, so the one checkbox
    // can drive the whole range.
    std::vector<FileImporter::MeshData> m_pendingInserts;
    wxCheckBox* m_insertCheck = nullptr;
    int m_insertFirstIndex = -1;
    int m_insertCount = 0;

    // Physical Setup bar (top of the centre column): material selections shared
    // across simulations. Groundwork — no behaviour wired yet.
    wxChoice* m_injMaterialChoice = nullptr;    // injection material
    wxChoice* m_mouldMaterialChoice = nullptr;  // mould material

    // Design-check parameter fields (left panel) and the verdict read-outs
    // (right panel). Plain text fields styled like the mould-feature inputs:
    // label + field + separate unit label.
    wxTextCtrl* m_failDraftCtrl = nullptr;    // draft fail threshold (deg)
    wxTextCtrl* m_warnDraftCtrl = nullptr;    // draft warn threshold (deg)
    wxTextCtrl* m_backdraftEpsCtrl = nullptr; // back-draft epsilon (deg)
    wxChoice*   m_sigModeChoice = nullptr;    // significance: % of surface vs absolute mm^2
    wxTextCtrl* m_sigValueCtrl = nullptr;     // significance threshold value
    wxStaticText* m_sigUnitLbl = nullptr;     // significance unit label (tracks the dropdown)
    wxTextCtrl* m_sepMinOverlapCtrl = nullptr; // separation: per-region min overlap volume (mm^3)
    wxTextCtrl* m_sepStartEpsCtrl = nullptr;    // separation: start-offset epsilon off the wall (mm)

    // Hele-Shaw 2.5D Flow process + mesh fields.
    wxTextCtrl* m_flowFillTimeCtrl = nullptr;  // injection fill time (s)
    wxTextCtrl* m_flowMeltTempCtrl = nullptr;  // melt temperature (deg C)
    wxTextCtrl* m_flowMouldTempCtrl = nullptr; // mould-wall temperature (deg C)
    wxTextCtrl* m_flowMeshAreaCtrl = nullptr;  // midplane target triangle area (mm^2)
    wxTextCtrl* m_flowMaxPressureCtrl = nullptr; // machine injection-pressure limit (MPa)
    wxCheckBox* m_flowThermalCheck = nullptr;    // thermal fill (frozen layer) vs isothermal

    wxStaticText* m_draftStatus = nullptr;    // "Draft Angle Checks" verdict
    wxStaticText* m_demouldStatus = nullptr;  // "Separation Test" verdict
    wxStaticText* m_flowStatus = nullptr;     // "Flow Analysis" verdict

    // Debug view controls + whether the separation run has produced an
    // interference solid to show (m_hasSepOverlay gates the separation toggle).
    wxChoice*   m_debugModeChoice = nullptr;  // debug view: None / Draft (ray)
    wxCheckBox* m_debugWireCheck = nullptr;   // debug view: wireframe toggle
    wxCheckBox* m_sepOverlayCheck = nullptr;
    bool        m_hasSepOverlay = false;

    // Right-hand information panel (outer), relaid out by UpdateInfoPanel, and
    // its shot-volume value labels (cm³ primary, in³ secondary).
    wxPanel* m_infoPanel = nullptr;
    wxStaticText* m_volPrimary = nullptr;    // "12.345 cm³"
    wxStaticText* m_volSecondary = nullptr;  // "0.753 in³"

    // Draft Angle Checks (ray-assigned half ownership): the per-facet samples,
    // the last classification, and the parting-plane-split analysis mesh the
    // check runs on and the "Draft (ray)" overlay renders (6 floats/vertex +
    // its own indices).
    std::vector<DesignChecks::DraftSample> m_faceDraftSamples;
    DesignChecks::FaceDraftStats           m_lastFaceDraftStats;
    std::vector<float>        m_faceDraftPosNorm;
    std::vector<unsigned int> m_faceDraftIdx;
    int                       m_faceDraftFallback = 0;  // facets that hit no half (cached)

    // Hele-Shaw flow mesh (P1): the shot surface with a per-facet wall thickness
    // from dual-domain pairing, plus the pairing/thickness summary. Cached per
    // shot; built on demand by EnsureFlowMesh and drawn by the "Flow (thickness)"
    // debug mode. Cleared on reset alongside the draft analysis.
    Flow::FlowMesh      m_flowMesh;
    Flow::FlowMeshStats m_flowMeshStats;

    // Feed network snapshot from the last Generate Mould (see ShotPreviewInput)
    // and the last steady feed solve over it (pressure drop + gate split). The
    // "Flow network" debug view draws the node tree; the part nodes are where
    // the cavity mid-surface mesh (rebuilt from source geometry) will attach.
    Flow::FeedNetwork     m_feedNetwork;
    bool                  m_hasFeedNetwork = false;
    Flow::FeedSolveResult m_feedSolve;
    bool                  m_hasFeedSolve = false;

    // Part surfaces (from Generate Mould) and the planform midplane built from
    // each at m_midplaneAreaMm2 (-1 = not built). One MidplaneMesh per surface,
    // kept even when a build fails so the report can say why.
    std::vector<Flow::PartSurface>  m_partSurfaces;
    std::vector<Flow::MidplaneMesh> m_midplanes;
    float                           m_midplaneAreaMm2 = -1.0f;

    // Last coupled fill (feed network + part midplanes filled together). Drives
    // the "Flow fill time" / "Flow pressure" views and, for thermal runs, "Flow
    // front temp" / "Flow frozen layer"; cleared whenever the midplanes are
    // rebuilt (its per-node arrays index them).
    Flow::CoupledFillResult m_fill;
    bool                    m_hasFill = false;

    // Which preview part is the shot (index into the canvas's parts).
    int m_shotHalfIndex = -1;

    // Cast generation state. m_mouldKind gates the flow (only Parametric /
    // Dynamic can be cast). m_castAnchorCount is the number of non-cast preview
    // parts (halves + shot + inserts) — cast bodies append at and above this
    // index, and a re-generation truncates the canvas back to it. m_halves*
    // cache the combined bounding box of the mould halves (computed in SetData
    // before the CPU meshes are dropped) so the bases can match their XZ
    // perimeter.
    FixtureKind m_mouldKind = FixtureKind::Library;
    int         m_castAnchorCount = 0;
    glm::vec3   m_halvesMin{ 0.0f };
    glm::vec3   m_halvesMax{ 0.0f };
    bool        m_hasHalvesBounds = false;

    // Mould-half meshes are uploaded then dropped; the shot artefacts are
    // RETAINED (CPU-side) because the design checks analyse them on demand:
    // the display mesh (for the overlay), the BREP shape (for face analysis),
    // and the per-display-triangle face-index map (to colour flagged faces).
    std::vector<FileImporter::MeshData> m_pendingHalves;
    FileImporter::MeshData              m_shotMesh;
    TopoDS_Shape                        m_shotShape;
    std::vector<int>                    m_shotFaceIds;

    // Mould-half surface soups (xyz positions + indices, one entry per half),
    // retained from the half display meshes in SetData. Used by the Draft Angle
    // Checks ownership ray and by the Separation Test's sweep/overlap. The half
    // MeshData themselves are uploaded then dropped, so these lightweight copies
    // are kept explicitly.
    std::vector<std::vector<float>>        m_halfMeshPos;
    std::vector<std::vector<unsigned int>> m_halfMeshIdx;
    bool                                m_hasShot = false;
    double                              m_shotVolumeMm3 = 0.0;

    // The Cast Shot Body (augmented shot) retained for base generation: the
    // standard shot plus vents, scaled inserts and ejector pins. Empty when the
    // last generation didn't build one — base generation then uses m_shotMesh.
    FileImporter::MeshData              m_castShotMesh;
    TopoDS_Shape                        m_castShotShape;   // BREP (BREP scenes)
    bool                                m_hasCastShot = false;
    bool                                m_hasCastShotShape = false;

    // Last generation was a mesh-toolpath scene — the BREP design checks refuse.
    bool                                m_sceneIsMesh = false;

    // True when SetData has staged new meshes that have not yet been uploaded
    // to the canvas's GL context (the upload waits until the panel is visible).
    bool                                m_dataDirty = false;
};
