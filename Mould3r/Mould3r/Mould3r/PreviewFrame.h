#pragma once

#include <wx/wx.h>
#include <wx/tglbtn.h>
#include <vector>
#include <unordered_map>

#include <opencascade/TopoDS_Shape.hxx>  // shot BREP, stored for face checks

#include "FileImporter.h"   // FileImporter::MeshData
#include "DesignChecks.h"   // DesignChecks::DemoldabilityResult

class GLCanvas;
class wxSpinCtrlDouble;

// Bundle of the shot artefacts handed to a preview window. All pointers may be
// null (no shot). The frame copies what it keeps; the caller need not preserve
// the pointees afterwards.
struct ShotPreviewInput
{
    const FileImporter::MeshData* mesh = nullptr;     // display mesh
    const TopoDS_Shape*           shape = nullptr;    // BREP for face checks
    const std::vector<int>*       faceIds = nullptr;  // per display tri -> face
    double                        volumeMm3 = 0.0;
    const std::vector<TopoDS_Shape>* halves = nullptr;  // half solids (separation)
};

// ===========================================================================
// PreviewFrame
//
// A standalone, non-modal top-level window that shows the post-cut mould
// halves on their own. It hosts a GLCanvas running in preview mode (grid +
// halves, full orbit/pan/dolly navigation, no editing) and a small toolbar
// with one show/hide toggle per half.
//
// A fresh PreviewFrame is created every time the mould is generated; the
// caller (MainFrame) destroys any previous instance first so the window always
// reflects the latest generation.
// ===========================================================================
class PreviewFrame : public wxFrame
{
public:
    // `halves` are the world-space post-cut meshes (one per fixture, in
    // fixture order) captured by GLCanvas::GenerateMould. `shot` bundles the
    // shot model (display mesh, BREP, per-triangle face map, volume); when its
    // mesh is present the shot gets its own show/hide toggle, renders in a
    // distinct colour, and is analysable by the design checks. Everything is
    // copied into the preview's own GL context on the first idle pass, once the
    // canvas is realized.
    PreviewFrame(wxWindow* parent,
        const std::vector<FileImporter::MeshData>& halves,
        const ShotPreviewInput& shot = {});

private:
    // Build one show/hide toggle per part: a half toggle for each mould half,
    // then a "Shot" toggle when a shot model is present. Called synchronously
    // from the constructor (count + labels are known up front); the GL upload
    // that gives those toggles something to control is deferred to LoadHalves.
    void BuildToggleBar(int halfCount, bool hasShot);

    // Left panel: a list of runnable simulations, each with its own Start
    // button. Right panel: read-only information about the shot (currently its
    // volume). Both are built once in the constructor.
    wxPanel* BuildSimPanel(wxWindow* parent);
    wxPanel* BuildInfoPanel(wxWindow* parent);

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
    // per group.
    void ApplyFaceGroups(const std::unordered_map<int, int>& groupOfFace,
        const std::vector<glm::vec3>& colors, int defaultGroup);

    // Upload the captured meshes into the canvas's context (halves first, then
    // the shot) and enable the toggles. Run via CallAfter so the canvas window
    // is fully realized (its GL context valid) before any GL call is issued.
    void LoadHalves();

    // Apply the app palette to a toggle based on its current value, so a
    // "shown" part reads as active and a "hidden" one as inactive.
    void StyleToggle(wxToggleButton* btn, bool visible) const;

    GLCanvas* m_canvas = nullptr;
    wxPanel* m_toolbar = nullptr;
    std::vector<wxToggleButton*> m_halfToggles;

    // Design-check controls (left panel) and the verdict read-out (right panel).
    wxSpinCtrlDouble* m_failDraftCtrl = nullptr;  // draft fail threshold (deg)
    wxSpinCtrlDouble* m_warnDraftCtrl = nullptr;  // draft warn threshold (deg)
    wxSpinCtrlDouble* m_liftCtrl = nullptr;       // separation lift (mm)
    wxStaticText* m_checkStatus = nullptr;        // "Demoldability: ..." line

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
};
