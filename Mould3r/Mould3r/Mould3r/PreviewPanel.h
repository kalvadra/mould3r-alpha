#pragma once

#include <wx/wx.h>
#include <wx/tglbtn.h>
#include <wx/checkbox.h>
#include <vector>
#include <unordered_map>

#include <opencascade/TopoDS_Shape.hxx>  // shot BREP, stored for face checks

#include "FileImporter.h"   // FileImporter::MeshData
#include "DesignChecks.h"   // DesignChecks::DemoldabilityResult
#include "GridSettings.h"   // GridSettings — forwarded to the preview canvas

class GLCanvas;
class wxSpinCtrlDouble;

// Bundle of the shot artefacts handed to the preview. All pointers may be null
// (no shot). The panel copies what it keeps; the caller need not preserve the
// pointees afterwards.
struct ShotPreviewInput
{
    const FileImporter::MeshData* mesh = nullptr;     // display mesh
    const TopoDS_Shape*           shape = nullptr;    // BREP for face checks
    const std::vector<int>*       faceIds = nullptr;  // per display tri -> face
    double                        volumeMm3 = 0.0;
    const std::vector<TopoDS_Shape>* halves = nullptr;  // half solids (separation)
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
        const ShotPreviewInput& shot = {});

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

private:
    // Build one visibility checkbox per part: a checkbox for each mould half,
    // then a "Shot" checkbox when a shot model is present, into m_visPanel (in
    // the left column, above the Simulations section). Re-runnable: call
    // ClearVisibilityChecks first to drop the previous set.
    void BuildVisibilityChecks(int halfCount, bool hasShot);
    void ClearVisibilityChecks();

    // Left panel: a list of runnable simulations, each with its own Start
    // button. Right panel: read-only information about the shot. Both are built
    // once in the constructor; the info panel's value labels are updated in
    // place via UpdateInfoPanel as data changes.
    wxPanel* BuildSimPanel(wxWindow* parent);
    wxPanel* BuildInfoPanel(wxWindow* parent);
    void     UpdateInfoPanel();

    // Stub entry point for kicking off a simulation. "Design Checks" runs the
    // demoldability assessment (see RunDemoldabilityCheck); any other named
    // simulation reports that it isn't implemented yet.
    void OnStartSimulation(const wxString& simName);

    // Run the demoldability check on the retained shot mesh using the draft
    // thresholds from the left-panel fields, then report the verdict (results
    // dialog + the status line in the information panel).
    void RunDemoldabilityCheck();

    // Run the alternative separation/collision demoldability check: lift each
    // mould half off the shot and test for interference. Reports the verdict
    // and shows the interference region as a red overlay for comparison with
    // the analytic undercut faces.
    void RunSeparationCheck();

    // Compute (and cache) the demoldability result from the current thresholds,
    // without any UI. Returns false when there is no shot to analyse.
    bool ComputeDemoldability();

    // Apply or clear the Draft Angle Checks mould overlay according to the
    // "Show mould overlay" checkbox on that card. When shown, the shot is
    // recoloured (against the current thresholds) into one combined view:
    // fail faces red, warn faces yellow, everything else its normal colour,
    // drawn as highlights (flat/full-intensity) rather than shaded.
    void UpdateDraftOverlay();

    // Show or hide the Separation Test mould overlay (the red interference
    // solid produced by the last run) according to that card's "Show mould
    // overlay" checkbox. A no-op when no interference solid is available.
    void UpdateSeparationOverlay();

    // Debug visualisation: recolour the shot so the facets flagged by one
    // category draw red and all others green. category: 0 = undercuts,
    // 1 = warnings, 2 = fails. Pressing the active category again clears it.
    void ShowDebugCategory(int category);

    // Debug visualisation: recolour the shot by draft sign relative to the
    // pull axis (up / down / vertical / mixed), using the analytic normals the
    // checks use — to expose any inverted normals. Pressing again clears it.
    void ShowDraftSign();

    // Debug visualisation: toggle the accessibility-ray overlay (yellow ray
    // segments, first 10 mm) and the contact markers (red points where failing
    // rays struck the shot). Independent on/off toggles.
    void ToggleDebugRays();
    void ToggleDebugContacts();

    // Recompute the demoldability result (capturing undercut rays) and push the
    // ray-segment + contact geometry to the canvas. Called before showing
    // either ray overlay so the geometry matches the current shot.
    void RefreshRayGeometry();

    // Push a debug overlay to the canvas: partition the shot's display
    // triangles by their source face's group (groupOfFace maps a 1-based face
    // index to a group index; faces not present use defaultGroup), one colour
    // per group. `emissive` (optional, per group) flags which groups draw as
    // flat full-intensity highlights instead of shaded; missing entries are
    // treated as not emissive.
    void ApplyFaceGroups(const std::unordered_map<int, int>& groupOfFace,
        const std::vector<glm::vec3>& colors, int defaultGroup,
        const std::vector<bool>& emissive = {});

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

    // Design-check parameter fields (left panel) and the verdict read-outs
    // (right panel). Plain text fields styled like the mould-feature inputs:
    // label + field + separate unit label.
    wxTextCtrl* m_failDraftCtrl = nullptr;   // draft fail threshold (deg)
    wxTextCtrl* m_warnDraftCtrl = nullptr;   // draft warn threshold (deg)
    wxTextCtrl* m_liftCtrl = nullptr;        // separation lift (mm)
    wxStaticText* m_draftStatus = nullptr;   // "Draft Angle Checks" verdict
    wxStaticText* m_demouldStatus = nullptr; // "Separation Test" verdict

    // "Show mould overlay" checkboxes under each simulation's Start button, and
    // whether the separation run has produced an interference solid to show.
    // The checkbox state itself is read from the controls; m_hasSepOverlay
    // gates the separation toggle so checking it before a run does nothing.
    wxCheckBox* m_draftOverlayCheck = nullptr;
    wxCheckBox* m_sepOverlayCheck = nullptr;
    bool        m_hasSepOverlay = false;

    // Right-hand information panel (outer), relaid out by UpdateInfoPanel, and
    // its shot-volume value labels (cm³ primary, in³ secondary).
    wxPanel* m_infoPanel = nullptr;
    wxStaticText* m_volPrimary = nullptr;    // "12.345 cm³"
    wxStaticText* m_volSecondary = nullptr;  // "0.753 in³"

    // Cached result of the last demoldability run, reused by the debug buttons.
    DesignChecks::DemoldabilityResult m_lastResult;
    bool m_hasResult = false;

    // Triggering rays of the last run's undercut faces, for the ray overlay.
    std::vector<DesignChecks::UndercutRay> m_undercutRays;
    bool m_showRays = false;
    bool m_showContacts = false;

    // Which preview part is the shot (index into the canvas's parts), and which
    // debug category is currently shown (-1 = none).
    int m_shotHalfIndex = -1;
    int m_activeDebugCategory = -1;

    // Mould-half meshes are uploaded then dropped; the shot artefacts are
    // RETAINED (CPU-side) because the design checks analyse them on demand:
    // the display mesh (for the overlay), the BREP shape (for face analysis),
    // and the per-display-triangle face-index map (to colour flagged faces).
    std::vector<FileImporter::MeshData> m_pendingHalves;
    FileImporter::MeshData              m_shotMesh;
    TopoDS_Shape                        m_shotShape;
    std::vector<int>                    m_shotFaceIds;
    std::vector<TopoDS_Shape>           m_halfShapes;   // for the separation test
    bool                                m_hasShot = false;
    double                              m_shotVolumeMm3 = 0.0;

    // True when SetData has staged new meshes that have not yet been uploaded
    // to the canvas's GL context (the upload waits until the panel is visible).
    bool                                m_dataDirty = false;
};
