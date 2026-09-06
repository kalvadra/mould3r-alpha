#pragma once

#include <wx/wx.h>
#include <wx/checkbox.h>
#include <wx/choice.h>
#include <string>
#include <vector>

#include <opencascade/TopoDS_Shape.hxx>  // Cast Shot Body BREP (retained)

#include "FileImporter.h"     // FileImporter::MeshData
#include "GridSettings.h"     // GridSettings — forwarded to the casting canvas
#include "FixtureFile.h"      // FixtureKind — gates cast generation
#include "MouldCastDialog.h"  // MouldCastValues / MouldCastPart — the value structs

#include <glm/glm.hpp>        // cached mould-half bounds (cast perimeter)

class GLCanvas;

// Bundle of the artefacts the Casting perspective needs from a Generate Mould
// run. All pointers may be null. The panel copies what it keeps; the caller
// need not preserve the pointees afterwards.
struct CastPreviewInput
{
    // The "Cast Shot Body" (standard shot + vents + scaled inserts + ejector
    // pins) — the intermediate object shown by default in this perspective and
    // the source the cast bases fuse against. `castShape` is the matching BREP
    // solid (null in a mesh scene) so later cast generation can build STEP-
    // exportable solids. When castMesh is null the caller may pass the plain
    // shot mesh as a fallback so the perspective still has something to show.
    const FileImporter::MeshData* castMesh = nullptr;
    const TopoDS_Shape*           castShape = nullptr;

    // The world-space post-cut mould halves — used only for their combined XZ
    // bounding box, which the cast walls / base take as their perimeter.
    const std::vector<FileImporter::MeshData>* halves = nullptr;

    // World-space indexer placement points (each on the y=0 parting plane).
    // Snapshotted here rather than read from the panel's own m_canvas: that
    // canvas is a SEPARATE preview-mode GLCanvas instance from the mould-
    // editing canvas where indexers are actually placed, so its indexer list
    // is always empty. May be null / empty (no indexers placed). The panel
    // copies these; the caller need not preserve the pointee.
    const std::vector<glm::vec3>* indexerPoints = nullptr;

    // True when the last generation was a mesh-toolpath scene (no BREP). Cast
    // bodies are then built as meshes (STL only) rather than OCC solids.
    bool sceneIsMesh = false;

    // Which kind of mould produced this generation. Cast generation is locked
    // to procedural moulds — Parametric / Dynamic. Carried here so the panel
    // can gate its own Generate flow (added in a later step) consistently with
    // MainFrame's tab gating.
    FixtureKind mouldKind = FixtureKind::Library;
};

// ===========================================================================
// CastingPanel
//
// The "Casting" workflow perspective, embedded as a third page inside
// MainFrame (alongside Prepare and Preview). It hosts its OWN GLCanvas running
// in preview mode (grid + orbit/pan/dolly navigation, no editing) and a
// left-hand "Cast Tool Settings" toolbar where the wall / base settings live.
//
// The default viewable object is the Cast Shot Body (the augmented shot used
// as an intermediate); the walls and base are generated on demand and shown as
// they are built, each with its own show/hide toggle.
//
// THIS STEP (data flow): SetData() seeds the Cast Shot Body from a Generate
// Mould run and shows it, default-visible, as preview part 0, with a matching
// "Cast Shot Body" toggle in the "Cast Bodies" card. The wall / base setting
// controls and the actual wall/base generation are added in later steps.
//
// Lifecycle: a single instance lives for the lifetime of MainFrame. The grid
// renders immediately, even with no data. As with PreviewPanel, GL uploads are
// deferred via FlushIfDirty() until the page is actually visible, since a
// canvas on a hidden book page may not yet have a valid drawable.
// ===========================================================================
class CastingPanel : public wxPanel
{
public:
    explicit CastingPanel(wxWindow* parent);

    // (Re)seed the perspective from a Generate Mould run: retain the Cast Shot
    // Body (mesh + BREP), cache the mould-half perimeter bounds, record the
    // mould kind, rebuild the Cast Bodies toggle list, and mark the GL data
    // dirty. The upload happens in FlushIfDirty once the panel is visible.
    void SetData(const CastPreviewInput& in);

    // Drop all loaded parts and reset to the empty (grid-only) state.
    void ClearData();

    // If data is staged and the panel is now visible, push it into the canvas's
    // GL context. Called by MainFrame right after it switches the active
    // perspective to Casting (and harmlessly a no-op when nothing is pending or
    // the panel is still hidden). Idempotent.
    void FlushIfDirty();

    // Push the ground-plane grid configuration to the casting canvas so it
    // matches the other perspectives. MainFrame calls this on entry to the
    // Casting perspective.
    void SetGridSettings(const GridSettings& s);

    // Prompt for the wall + base characteristics and build them around the Cast
    // Shot Body: two bases straddling the y = 0 parting plane (each fused with
    // the matching y-split half of the shot) and four perimeter walls, split at
    // y = 0 into Top Cast / Bottom Cast groups. Invoked by MainFrame's top-right
    // "Generate Mould Casts" button. Locked to procedural (Parametric / Dynamic)
    // moulds — the same gate as the Casting tab itself.
    void GenerateCasts();

    // True once a Cast Shot Body has been seeded (used by later cast-generation
    // steps; harmless to query earlier).
    bool HasCastShotBody() const { return m_hasCastShot; }

    // One exportable cast body: a filename-safe suffix, the display mesh (always
    // present, used for STL export), and — in a BREP scene — the exact OCC solid
    // (used for STEP export). `hasShape` gates the STEP path. Consumed by
    // MainFrame's "Export Casts" (added in a later step).
    struct CastExportBody
    {
        std::string            suffix;
        FileImporter::MeshData mesh;
        TopoDS_Shape           shape;
        bool                   hasShape = false;
    };

    // Cast bodies retained for export (populated by GenerateCasts): the Top Base
    // and Bottom Base, plus ONE half of each wall (the two halves are symmetric
    // about y = 0, so only one need be saved). Empty until casts have been
    // generated.
    bool HasCastBodies() const { return !m_castExports.empty(); }
    const std::vector<CastExportBody>& GetCastExportBodies() const
    {
        return m_castExports;
    }

private:
    // Left toolbar — "CAST TOOL SETTINGS" (the Base / Walls setting cards) over
    // a "CAST BODIES" visibility card. Built once in the constructor.
    wxPanel* BuildToolPanel(wxWindow* parent);

    // The controls for one part section (Base / Walls), inlined into the
    // toolbar. Mirrors MouldCastDialog::PartControls one-for-one so the same
    // value logic (ReadPartUI) reproduces MouldCastDialog::ReadPart exactly.
    struct PartUI
    {
        wxCheckBox* enable = nullptr;
        wxChoice*   type = nullptr;
        wxTextCtrl* thickness = nullptr;
        wxChoice*   unit = nullptr;
        wxTextCtrl* extra = nullptr;
        wxChoice*   extraUnit = nullptr;
        wxTextCtrl* tongueW = nullptr;    wxChoice* tongueWUnit = nullptr;
        wxTextCtrl* tongueT = nullptr;    wxChoice* tongueTUnit = nullptr;
        wxTextCtrl* grooveTol = nullptr;  wxChoice* grooveTolUnit = nullptr;
    };

    // Build one collapsible "Base" / "Walls" card into `parent`, populating
    // `ui`. `typeChoices` seeds the type dropdown; `defThickness` pre-fills the
    // thickness; `extraLabel` labels the extra-distance row ("Extra Flange:" /
    // "Extra Wall:"); `withJoint` adds the tongue-and-groove rows (walls only).
    wxPanel* BuildPartCard(wxWindow* parent, const wxString& title,
        const wxArrayString& typeChoices, double defThickness,
        const wxString& extraLabel, bool withJoint, PartUI& ui);

    // Grey a card's rows to match its Enable checkbox, live.
    void SyncPartEnabled(const PartUI& ui);

    // Read the toolbar controls back into a MouldCastValues (replacing the old
    // MouldCastDialog::GetValues). ReadPartUI mirrors MouldCastDialog::ReadPart.
    MouldCastValues ReadToolbarValues() const;
    MouldCastPart   ReadPartUI(const PartUI& ui) const;

    // Build the show/hide checkboxes in the Cast Bodies card: one "Cast Shot
    // Body" checkbox when a shot body is present. The generated walls / base
    // are added as collapsible groups by AddCastGroup. Re-runnable: call
    // ClearVisibilityChecks first to drop the previous set.
    void BuildVisibilityChecks(bool hasCastShot);
    void ClearVisibilityChecks();

    // Upload the retained Cast Shot Body into the canvas's context as preview
    // part 0 and honour its checkbox state. Run via CallAfter so the canvas
    // window is fully realized (its GL context valid) before any GL call.
    void LoadCastBodies();

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

    // Add a collapsible cast GROUP to the Cast Bodies card: a "master" checkbox
    // that shows / hides the whole group, plus a chevron that expands to
    // per-child checkboxes. Uploads each child's mesh to the canvas. The group
    // panels are tracked (m_castGroupPanels) so a cast re-generation can drop
    // them via ClearCastChecks without disturbing the Cast Shot Body toggle.
    void AddCastGroup(const wxString& groupLabel, std::vector<CastChild>& children);
    void ClearCastChecks();

    GLCanvas* m_canvas = nullptr;

    // Cast Tool Settings controls (Base / Walls cards), read by GenerateCasts.
    PartUI m_baseUI;
    PartUI m_wallsUI;

    // "Cast Bodies" visibility card: m_visPanel is the card body,
    // m_visEmptyLabel shows "No bodies generated" when nothing is loaded, and
    // m_castShotCheck is the Cast Shot Body toggle (null until one is seeded).
    wxPanel*      m_visPanel = nullptr;
    wxStaticText* m_visEmptyLabel = nullptr;
    wxCheckBox*   m_castShotCheck = nullptr;

    // Generated cast-body widgets, kept apart from m_castShotCheck so a cast
    // re-generation can drop and rebuild just these. Cast bodies are organised
    // into collapsible "Top Cast" / "Bottom Cast" groups: m_castGroupPanels
    // holds each group's container panel (destroying it frees the child
    // checkboxes too), and m_castChecks references every child checkbox.
    std::vector<wxCheckBox*> m_castChecks;
    std::vector<wxPanel*>    m_castGroupPanels;

    // Exportable cast bodies (survive the GPU upload). Rebuilt on each
    // GenerateCasts; consumed by MainFrame's "Export Casts".
    std::vector<CastExportBody> m_castExports;

    // Retained Cast Shot Body (survives the GPU upload — later cast generation
    // fuses the split halves of this body into the bases).
    FileImporter::MeshData m_castShotMesh;
    TopoDS_Shape           m_castShotShape;   // BREP (BREP scenes)
    bool                   m_hasCastShot = false;
    bool                   m_hasCastShotShape = false;

    // Combined mould-half bounding box (the cast walls / base perimeter),
    // cached from the halves passed to SetData.
    glm::vec3 m_halvesMin{ 0.0f };
    glm::vec3 m_halvesMax{ 0.0f };
    bool      m_hasHalvesBounds = false;

    // Snapshot of the indexer placement points from the Generate Mould run,
    // copied in SetData. Read by GenerateCasts instead of m_canvas->GetIndexers()
    // — see CastPreviewInput::indexerPoints for why the panel's own canvas
    // can't be queried for these.
    std::vector<glm::vec3> m_indexerPoints;

    // Which mould kind produced this generation, and whether it was a mesh
    // scene (no BREP). Consumed by the cast generation added in a later step.
    FixtureKind m_mouldKind = FixtureKind::Library;
    bool        m_sceneIsMesh = false;

    // Number of non-cast preview parts (the Cast Shot Body). Generated cast
    // bodies append at and above this index; a re-generation truncates the
    // canvas back to it.
    int m_castAnchorCount = 0;

    // True when SetData has staged a Cast Shot Body that has not yet been
    // uploaded to the canvas's GL context (the upload waits until visible).
    bool m_dataDirty = false;
};
