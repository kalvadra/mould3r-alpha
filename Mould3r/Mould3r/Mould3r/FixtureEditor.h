#pragma once
#include <wx/wx.h>
#include <wx/checkbox.h>
#include <wx/choice.h>
#include <wx/frame.h>
#include <wx/textctrl.h>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include "FixtureFile.h"   // InjectionPoint — held by value in m_injectionPoints

class FixtureCanvas;

// =============================================================================
// FixtureEditor
//
// Floating top-level window (separate from MainFrame) used to author a new
// fixture — that is, to load the two STEP halves, position them in space,
// configure default feature parameters, and (eventually) save the resulting
// .fixture file. Replaces the old "two file dialogs + a save dialog" flow
// that StartupDialog::OnNewFixture used to drive.
//
// Layout:
//   [top ribbon: Import A / Import B + paths .... Generate Fixture button]
//   [toolbar | canvas | feature defaults sidebar                          ]
//
// What's wired up:
//   * Top ribbon with "Import Mould Half A/B" buttons + path labels;
//     picking a STEP file loads it into the canvas and updates the path.
//   * "Generate Fixture" primary action button on the right of the ribbon —
//     validates inputs, prompts for a save location (defaults to the
//     fixtures/ folder StartupDialog scans), and writes a .fixture file
//     containing both half paths, per-half pose, and every sidebar
//     default. Closes the editor on success.
//   * Lefthand toolbar (Move, Rotate, Scale, Center, Align Face) routed
//     through to the canvas. Move/Rotate/Scale follow MainFrame's
//     dialog-based convention (toggle latches briefly, dialog opens,
//     toggle clears on dialog close); Center is a momentary action;
//     Align Face is a true persistent toggle with ESC-to-cancel.
//   * 3D viewport (FixtureCanvas) with grid, orbit camera, lit rendering
//     of any imported halves, single-half selection (warm-yellow tint
//     indicates selection), and a dark-grey hover overlay during Align
//     Face mode.
//   * Right-hand "Feature Defaults" sidebar — five per-feature cards
//     (Sprues, Runners, Gates, Vents, Ejectors) exposing the type and
//     dimension defaults that get baked into the saved .fixture file.
//     No Place/Edit/Remove/Clear buttons, no collapsible Settings toggle:
//     the fixture editor isn't a placement context, so the entire card
//     IS the settings.
//
// What's still pending:
//   * Auto-rescan of StartupDialog's fixture list after a successful
//     Generate. Today the user has to click Browse Folder (or close and
//     reopen StartupDialog) to pick up the freshly-written file.
//
// Pattern and Align Midplane are intentionally absent from the toolbar:
// pattern doesn't apply to fixture authoring (a fixture is exactly one A
// and one B half), and midplane alignment is a per-design operation that
// doesn't make sense on the fixture itself.
// =============================================================================
class FixtureEditor : public wxFrame
{
public:
    explicit FixtureEditor(wxWindow* parent);
    ~FixtureEditor() override = default;

    // Pre-populate the editor with values gathered by CreateFixtureDialog
    // before showing it. Sets the displayed paths on both file pickers,
    // immediately loads each half into the canvas (same pipeline the
    // OnSelectModelA/B handlers use), and stashes the fixture name so
    // OnGenerateFixture can use it as the default save filename.
    //
    // Safe to call with empty strings — empty paths just skip the load,
    // matching the picker handlers' guard. Intended to be called exactly
    // once, before Show(); calling it again later would re-trigger
    // LoadHalf and stomp any unsaved pose edits.
    //
    // The optional progress callback is invoked at each step (before/after
    // each LoadHalf). CreateFixtureDialog uses it to drive the dialog's
    // progress bar; pass an empty std::function (the default) to silence
    // it. The percent values are coarse — there's no per-feature hook
    // into FixtureCanvas::LoadHalf yet, so progress steps in chunks.
    using ProgressCallback =
        std::function<void(int percent, const std::string& status)>;
    void SetInitialFixture(const std::string& fixtureName,
        const std::string& modelAPath,
        const std::string& modelBPath,
        ProgressCallback   progress = {});

    // Drive the visual state of every registered toggle button to reflect
    // a single active tool. Pass wxID_NONE to clear all toggles. Mirrors
    // the role MainFrame::SetActiveTool plays for the main tool grid.
    //
    // Public so FixtureCanvas can call back here when ESC drops AlignFace
    // mode, the same way GLCanvas calls MainFrame::SetActiveTool from its
    // own Escape handler. Also drives the canvas's transform mode in lock
    // step — see the implementation for the AlignFace-vs-Select mapping.
    void SetActiveTool(int activeId);

private:
    void BuildUI();

    // Builds the top ribbon — two stacked rows, each row is an Import
    // button + a path label that updates when the user picks a file.
    // Returns the panel so the caller can stack it on top of the
    // toolbar+canvas split.
    wxWindow* BuildTopRibbon(wxWindow* parent);

    // Builds the lefthand vertical toolbar (Move / Rotate / Scale / Center
    // / Align Face). Returns the panel so the caller can add it to the
    // frame's outer horizontal sizer.
    wxWindow* BuildToolbar(wxWindow* parent);

    // Builds the 3D viewport area on the right of the toolbar. Owns the
    // FixtureCanvas instance — that pointer is held in m_canvas so future
    // code can drive scene state from the import handlers.
    wxWindow* BuildCanvasArea(wxWindow* parent);

    // Builds the right-hand feature-defaults sidebar — header + scrollable
    // column of feature cards (Sprues, Runners, Gates, Vents, Ejectors).
    // Each card is a self-contained panel with a section title, type
    // dropdown, and dimension fields. There are no Place/Edit/Remove/
    // Clear buttons here (those are MainFrame's placement-mode concerns,
    // not relevant to fixture authoring) and no collapsible Settings
    // toggle — the entire card IS the settings.
    wxWindow* BuildSidePanel(wxWindow* parent);

    // Per-feature card builders. Mirror the structure of MainFrame's
    // CreateXxxContent functions but stripped down: no Place button, no
    // action grid, no collapsible Settings sub-section. Each populates the
    // matching m_xxx* member pointers (type choice, dimension fields) so
    // the future Generate-Fixture handler can read values back.
    wxPanel* CreateUnitToggle(wxWindow* parent);
    wxPanel* CreateInjectionPointsContent(wxWindow* parent);
    wxPanel* CreateSpruesContent(wxWindow* parent);
    wxPanel* CreateRunnersContent(wxWindow* parent);
    wxPanel* CreateGatesContent(wxWindow* parent);
    wxPanel* CreateVentsContent(wxWindow* parent);
    wxPanel* CreateEjectorsContent(wxWindow* parent);

    // Rebuild the list of injection-point entries inside m_injectionListPanel
    // from m_injectionPoints. Called after Add / Edit / Remove. Each entry
    // gets its own Edit and Remove buttons, bound via per-button lambdas
    // that capture the index into m_injectionPoints by value — safe because
    // the panel's children are destroyed and rebuilt on every list mutation,
    // so a bound lambda's captured index is always the current index.
    void RebuildInjectionList();

    // Add / Edit / Remove handlers. Add and Edit open InjectionPointDialog
    // (the Edit variant pre-populates from the existing point); Remove
    // simply drops the point by index. All three end with a call to
    // RebuildInjectionList so the sidebar stays in sync.
    void OnAddInjectionPoint(wxCommandEvent&);
    void EditInjectionPointAt(int index);
    void RemoveInjectionPointAt(int index);

    // Toolbar click handlers. Move/Rotate/Scale open the existing
    // TranslateDialog/RotateDialog/ScaleDialog and forward results to the
    // canvas — same dialog-based UX MainFrame uses for its equivalent
    // tools. Their toggles untoggle immediately on click via
    // SetActiveTool(wxID_NONE) so the buttons don't appear stuck on after
    // the dialog closes. OnToolCenter is a momentary action (no toggle
    // state change). OnToolAlignFace is the only true persistent toggle —
    // it routes through SetActiveTool so the canvas's transform mode
    // stays in sync with the button visual.
    void OnToolMove(wxCommandEvent&);
    void OnToolRotate(wxCommandEvent&);
    void OnToolScale(wxCommandEvent&);
    void OnToolCenter(wxCommandEvent&);
    void OnToolAlignFace(wxCommandEvent&);

    // Top-ribbon "Generate Fixture" handler. Validates that both halves
    // are imported and loaded, prompts for a save location (defaulting to
    // the fixtures/ folder next to the executable), gathers paths +
    // per-half pose + every sidebar default into a FixtureDefinition,
    // writes the .fixture file via FixtureFile::Save, and closes the
    // editor on success.
    void OnGenerateFixture(wxCommandEvent&);

    // Top-ribbon "Select" button handlers — one per half. Each opens a
    // wxFileDialog (STEP/IGES filter, same as CreateFixtureDialog), writes
    // the chosen path into the matching text field, stores it in
    // m_modelA/BPath, and pushes it through to the canvas LoadHalf
    // pipeline. The text fields themselves are editable but don't have
    // change handlers — typing a path doesn't load it until the user
    // presses Select (intentional: avoids reload-on-every-keystroke).
    void OnSelectModelA(wxCommandEvent&);
    void OnSelectModelB(wxCommandEvent&);

    // Top-ribbon "Hide" checkbox handlers — one per half. Forward the new
    // check state to FixtureCanvas::SetHalfVisible (checked = hide, so
    // visibility = !checked). The canvas owns the actual show/hide
    // behaviour (render + pick skip); these handlers are pure forwarders.
    void OnHideHalfA(wxCommandEvent&);
    void OnHideHalfB(wxCommandEvent&);

    // Per-button visual setters keyed by command ID. Populated by the
    // makeToolBtn helper in BuildToolbar; consumed by SetActiveTool.
    // Same pattern as MainFrame::m_toolBtnSetters.
    std::unordered_map<int, std::function<void(bool)>> m_toolBtnSetters;
    int m_activeToolId = wxID_NONE;

    // Picked-file paths. Empty until the user selects a file. These are
    // the inputs the save flow reads when writing the .fixture file.
    std::string m_modelAPath;
    std::string m_modelBPath;

    // Human-readable fixture name supplied by CreateFixtureDialog (empty
    // when the editor is opened without going through that dialog — e.g.
    // future direct-launch flows). When non-empty, OnGenerateFixture uses
    // it as the default save filename instead of the hardcoded
    // "NewFixture.fixture". The name is not currently persisted inside
    // the .fixture file itself — if that becomes useful, the FixtureFile
    // schema will need a new field; deferred for now.
    std::string m_fixtureName;

    // Path text fields in the top ribbon (one per half). Editable
    // wxTextCtrls paired with adjacent "Select" buttons that open a
    // wxFileDialog — same idiom CreateFixtureDialog uses, replacing the
    // earlier wxFilePickerCtrl system-themed widgets so the two surfaces
    // visually match. Held as members so the picker handlers and save
    // flow can read/set the displayed path. Owned by the wxWidgets
    // parent-child hierarchy — no manual delete.
    wxTextCtrl* m_pathACtrl = nullptr;
    wxTextCtrl* m_pathBCtrl = nullptr;

    // Per-half "Hide" checkboxes in the top ribbon. Held as members so
    // (a) the OnHideHalf handlers can read the checked state, and (b) the
    // load paths (OnSelectModelA/B, SetInitialFixture) can force them
    // back to unchecked when a new file is loaded — matching the canvas's
    // "Destroy resets visible=true" semantics so the UI and canvas stay
    // in sync after a re-import.
    wxCheckBox* m_hideACheck = nullptr;
    wxCheckBox* m_hideBCheck = nullptr;

    // 3D viewport. Owned by the parent-child hierarchy. Held as a member
    // so future runtime code (transform handlers, file-load wiring) can
    // push state into the canvas without re-walking the widget tree.
    FixtureCanvas* m_canvas = nullptr;

    // ---- Feature-defaults sidebar fields ----------------------------------
    // Same naming convention as MainFrame's matching members — keeps the
    // generate-fixture handler symmetric with MainFrame's existing
    // FixtureDefinition-population code in ApplyFixtureDefaults. All field
    // pointers are owned by the wxWidgets parent-child hierarchy (sidebar
    // → card panel → control); no manual delete.
    //
    // Distance fields are mm-only — the fixture file format stores
    // distances in mm, and the fixture editor doesn't carry the
    // metric/imperial toggle MainFrame does. Angle fields are degrees.

    // Unit toggle (metric / imperial) -- above injection points card.
    // m_isMetric drives display and save-time conversion.
    // m_mmFields / m_mmUnitLabels are parallel vectors populated by the
    // CreateXxxContent helpers for every dimension field whose unit is mm
    // (draft-angle degree fields are excluded). The toggle handler iterates
    // them to update labels and convert displayed values in one pass.
    bool          m_isMetric = true;
    std::vector<wxTextCtrl*>   m_mmFields;
    std::vector<wxStaticText*> m_mmUnitLabels;
    wxPanel* m_metricSeg = nullptr;   // left segment (active by default)
    wxStaticText* m_metricLbl = nullptr;
    wxPanel* m_imperialSeg = nullptr;   // right segment
    wxStaticText* m_imperialLbl = nullptr;

    // Sprue
    wxChoice* m_sprueTypeChoice = nullptr;
    wxPanel* m_sprueDimsPanel = nullptr;   // shown/hidden by type choice
    wxTextCtrl* m_sprueDiameter = nullptr;
    wxTextCtrl* m_sprueDraftAngle = nullptr;
    wxTextCtrl* m_sprueColdSlugDepth = nullptr;
    wxTextCtrl* m_sprueLength = nullptr;

    // Runner
    wxChoice* m_runnerTypeChoice = nullptr;
    wxPanel* m_runnerDimsPanel = nullptr;   // shown/hidden by type choice
    wxTextCtrl* m_runnerDiameter = nullptr;
    wxTextCtrl* m_runnerColdSlugDepth = nullptr;

    // Gate (+ sub-runner — same card per MainFrame's convention)
    wxChoice* m_gateTypeChoice = nullptr;
    wxPanel* m_gateDimsPanel = nullptr;      // shown/hidden by type choice
    wxTextCtrl* m_gateDiameter = nullptr;
    wxTextCtrl* m_gateDraftAngle = nullptr;
    wxChoice* m_subRunnerTypeChoice = nullptr;
    wxPanel* m_subRunnerDimsPanel = nullptr; // shown/hidden by type choice
    wxTextCtrl* m_subRunnerDiameter = nullptr;

    // Vent
    wxChoice* m_ventTypeChoice = nullptr;
    wxPanel* m_ventDimsPanel = nullptr;     // shown/hidden by type choice
    wxTextCtrl* m_ventLength = nullptr;
    wxTextCtrl* m_ventWidth = nullptr;
    wxTextCtrl* m_ventOverrunStart = nullptr;
    wxTextCtrl* m_ventOverrunEnd = nullptr;

    // Ejector
    wxChoice* m_ejectorTypeChoice = nullptr;
    wxPanel* m_ejectorDimsPanel = nullptr;  // shown/hidden by type choice
    wxTextCtrl* m_ejectorDiameter = nullptr;
    wxTextCtrl* m_ejectorLength = nullptr;

    // ---- Injection points ------------------------------------------------
    // Live list of points the user has added through the side-panel card.
    // Survives Add / Edit / Remove operations as a plain vector — order
    // mirrors the visible list, and OnGenerateFixture copies it straight
    // into FixtureDefinition::injectionPoints. Defaults each new point to
    // InjectionType::Radial (the file format's default for unspecified
    // types); the dialog doesn't surface a type picker today.
    std::vector<InjectionPoint> m_injectionPoints;

    // Container panel for the per-entry rows. Held as a member so
    // RebuildInjectionList can walk back to it after an Add / Edit /
    // Remove without re-finding it through the widget tree. Owned by the
    // wxWidgets parent-child hierarchy.
    wxPanel* m_injectionListPanel = nullptr;

    enum
    {
        // Range chosen to avoid collisions with MainFrame's tool IDs
        // (which start at wxID_HIGHEST + 1), StartupDialog's IDs
        // (wxID_HIGHEST + 300), and CreateFixtureDialog (HIGHEST + 400).
        ID_FE_SelectModelA = wxID_HIGHEST + 700,
        ID_FE_SelectModelB,
        ID_FE_HideA,
        ID_FE_HideB,
        ID_FE_GenerateFixture,
        ID_FE_Move,
        ID_FE_Rotate,
        ID_FE_Scale,
        ID_FE_Center,
        ID_FE_AlignFace,
        ID_FE_AddInjectionPoint
        // Edit / Remove buttons inside the injection list don't get static
        // IDs — they're rebuilt on every list mutation, so each one binds
        // its own lambda directly via wxButton::Bind in RebuildInjectionList.
    };
};
