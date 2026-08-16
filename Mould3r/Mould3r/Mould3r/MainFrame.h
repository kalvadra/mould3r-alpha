#pragma once
#include <wx/wx.h>
#include <memory>   // std::unique_ptr — background update checker (U4)
#include <wx/tglbtn.h>
#include "RotateDialog.h"
#include "TranslateDialog.h"
#include "ScaleDialog.h"
#include "PatternDialog.h"
#include "StartupDialog.h"
#include <wx/textctrl.h>
#include "FixtureFile.h"
#include "AppConfig.h"
#include "StartupDialog.h"
#include "ProjectFile.h"
#include "GridSettings.h"    // GridShape / GridSettings — authored via the Grid menu

#include <functional>
#include <unordered_map>

class GLCanvas;
class RoundedButton;   // forward decl — m_btnGenerate pointer below; full def
// is included from MainFrame.cpp where it's used.
class SplitButton;     // forward decl — m_btnExport pointer below; full def
// is included from MainFrame.cpp where it's used.
class InsertEditDialog;  // forward decl — modeless insert-transform editor,
                         // defined entirely in MainFrame.cpp (no header /
                         // .vcxproj entry needed since only the frame uses it)
class PreviewPanel;    // forward decl — m_previewPanel pointer; full def
// is included from MainFrame.cpp where the panel is created.
class wxSimplebook;    // forward decl — m_book pointer; the perspective pager.
class PerspectiveButton; // forward decl — m_btnPrepare/m_btnPreview tabs.
class VentEditToolbar;   // forward decl — m_ventEditToolbar overlay (Part 5).
class SprueEditToolbar;  // forward decl — m_sprueEditToolbar overlay (Edit Sprue).
class CanvasToast;       // forward decl — bottom-centre viewport hint pill,
class UpdateBanner;      // forward decl — bottom-right update notification
                         // card (U4); defined in MainFrame.cpp.
class UpdateChecker;     // forward decl — background startup update check.
                         // defined entirely in MainFrame.cpp (same no-header
                         // arrangement as InsertEditDialog above)

enum class TransformMode { Select, Translate, Rotate, Scale, Pattern, PlaceVent, PlaceRunner, PlaceGate, PlaceEjector, PlaceInsert, RemoveVent, RemoveRunner, RemoveGate, RemoveSprue, RemoveEjector, RemoveInsert, EditVent, EditRunner, EditGate, EditEjector, EditInsert, EditSprue, SelectInjectionPoint, AlignFace, AlignMidplane };

// Sub-tool active while editing a Complex vent path (Part 5). Shared between
// GLCanvas (which owns the authoring logic) and the floating VentEditToolbar
// (which drives it). Lives here, next to TransformMode, so the lightweight
// toolbar header doesn't have to pull in the heavy GLCanvas header.
enum class PathEditTool { Move, AddNode, RemoveNode };

// Sub-tool active while in EditSprue mode, driven by the floating
// SprueEditToolbar. Move drags a radial sprue's endpoint on the parting
// plane; SelectInjectionPoint re-picks which injection point the sprue feeds
// from (the pre-edit-environment behaviour, now one tool among two). Lives
// here next to PathEditTool for the same lightweight-header reason.
enum class SprueEditTool { Move, SelectInjectionPoint };

class MainFrame : public wxFrame
{
public:
    MainFrame(const FixtureDefinition& FixtureDefinition);
    ~MainFrame() override;

    // Called by GLCanvas when Escape is pressed to sync button state
    void SetActiveTool(TransformMode mode);

    // Open the Precision Place dialog for the current selection and apply the
    // resulting absolute XZ move. Shared by the ribbon button handler and the
    // canvas double-click shortcut, so both routes behave identically.
    void PrecisionPlaceSelected();

    // Precision Place for a feature path node (vent/runner nodes that sit on
    // the y=0 plane and aren't perimeter/path-snapped). Called by the canvas
    // when such a node is double-clicked; the canvas has already vetted the
    // index via IsEditNodePrecisePlaceable.
    void PrecisionPlaceEditNode(int nodeIdx);

    // Called by GLCanvas when the user picks an injection point via the
    // SelectInjectionPoint tool. Updates UI fields whose value depends on
    // the injection-point type — currently just the Draft Angle field,
    // which goes to 0° for radial injection points and reverts to the
    // default for axial. Invoked before PlaceSprue so the new value is
    // visible to the sprue build.
    void OnInjectionPointSelected(const InjectionPoint& ip);

    // Read current vent dimensions from the left-panel UI fields
    void GetVentDimensions(float& outLength, float& outWidth,
        float& outOverrunStart, float& outOverrunEnd) const;

    // Read current sprue dimensions from the left-panel UI fields
    float GetSprueDiameter() const;
    float GetSprueDraftAngle() const;
    float GetSprueColdSlugDepth() const;
    float GetSprueLength() const;
    float GetSprueOverrun() const;
    float GetRunnerColdPlugDist() const;

    // Unit system
    bool IsImperial() const { return m_imperial; }

    float GetRunnerDiameter() const;

    float GetGateDiameter() const;
    float GetGateDraftAngle() const;
    float GetGateOverrun() const;
    float GetSubRunnerDiameter() const;

    float GetEjectorDiameter() const;
    float GetEjectorLength() const;

    // Insert "Cut scale", as a fraction (the card takes percent; this returns
    // 1.0 for 100%). Unitless, so unlike every other feature getter it does
    // NOT convert for the imperial unit system.
    float GetInsertCutScale() const;

    // Called by GLCanvas when the user picks a parent object in PlaceInsert
    // mode. Runs the import file dialog and hands the result to the canvas,
    // then drops back to Select. Public because the canvas drives it — the
    // parent pick happens on the GL surface, but the file dialog and the
    // side-panel field reads belong to the frame.
    void PlaceInsertOnParent(int parentIdx);

    // Insert Edit dialog lifecycle. OpenInsertEditor is called by the canvas
    // when an insert is picked in EditInsert mode — it creates the modeless
    // dialog or retargets the existing one at `insertId`. ValidateInsertEditor
    // is called by the canvas after a structural insert change: it closes the
    // dialog if its target insert is gone, else refreshes its fields.
    // OnInsertEditorClosed is called by the dialog as it closes so the frame
    // drops its pointer.
    void OpenInsertEditor(int insertId);
    void ValidateInsertEditor();
    void OnInsertEditorClosed();

    // Destroys the modeless editor if open (used by ~MainFrame). Defined after
    // the InsertEditDialog class body in the .cpp — ~MainFrame precedes that
    // class, where the type is still incomplete, so the destroy can't be inlined
    // into the destructor.
    void DestroyInsertEditor();

    // Project save/load support
    const FixtureDefinition& GetFixtureDefinition() const { return m_fixtureDef; }
    GLCanvas* GetCanvas() const { return m_canvas; }

    // Set UI field values (used when restoring a project)
    void SetParameterFields(const ProjectParameters& params);

    // Apply per-feature default overrides from a fixture into the side-panel
    // UI fields. Walks each FixtureDefinition::*Defaults struct and writes
    // only the fields the fixture actually specified (std::optional set);
    // unspecified fields are left at their existing UI value, which on a
    // freshly built panel is the application's hardcoded default. Length
    // fields are converted from the fixture's mm representation to the
    // current display unit; angles pass through unchanged. Type-string
    // overrides that don't match an entry in the corresponding wxChoice are
    // ignored (no-op) to preserve forward compatibility with fixtures that
    // reference types this build doesn't know about yet.
    void ApplyFixtureDefaults(const FixtureDefinition& def);

    // Materialise a selected fixture's geometry into the canvas: for a Library
    // fixture this imports the two STEP/mesh halves; for a Parametric / Dynamic
    // fixture it builds the procedural box halves. ONLY the geometry differs by
    // kind — each call site still handles clearing, injection state, defaults
    // and mould state around this call exactly as it did before.
    void LoadFixtureIntoScene(const FixtureDefinition& fixture);

    // If no fixture has been loaded yet, show the selection dialog and load
    // whatever the user chooses. Intended to be called once, right after the
    // frame is shown on app startup. If the user cancels, the frame stays
    // open but empty — they can pick a fixture later via Fixture -> Change Fixture.
    void PromptForFixtureIfMissing();

private:
    // Menu handlers
    void OnImport(wxCommandEvent& evt);
    void OnCreateFixture(wxCommandEvent&);
    void OnChangeFixture(wxCommandEvent&);

    // Re-open the procedural fixture's dimension/clearance dialog to edit it in
    // place. Only meaningful for Parametric / Dynamic fixtures; the menu item is
    // disabled (via OnUpdateEditFixture) for a library fixture.
    void OnEditFixture(wxCommandEvent&);
    void OnUpdateEditFixture(wxUpdateUIEvent&);
    void OnExit(wxCommandEvent& evt);
    void OnSaveProject(wxCommandEvent&);
    void OnLoadProject(wxCommandEvent&);
    void OnNewProject(wxCommandEvent&);

    // Ribbon tool handlers
    void OnToolSelect(wxCommandEvent& evt);
    void OnToolTranslate(wxCommandEvent& evt);
    void OnToolRotate(wxCommandEvent& evt);
    void OnToolScale(wxCommandEvent& evt);
    void OnToolPattern(wxCommandEvent& evt);
    void OnToolPrecisionPlace(wxCommandEvent& evt);
    void OnToolCenter(wxCommandEvent& evt);
    void OnToolAlignFace(wxCommandEvent& evt);
    void OnToolAlignMidplane(wxCommandEvent& evt);
    void OnToolPlaceVent(wxCommandEvent& evt);

    void OnPlaceSprue(wxCommandEvent&);
    void OnClearSprue(wxCommandEvent&);

    void OnPlaceRunner(wxCommandEvent& evt);
    void OnClearRunners(wxCommandEvent&);

    void OnPlaceGate(wxCommandEvent& evt);
    void OnClearGates(wxCommandEvent&);

    // Ejectors. Place toggles between Select and PlaceEjector; Clear wipes
    // every ejector via the canvas helper. Remove / Edit toggle into their
    // respective transient picking modes the same way the other features do.
    // The canvas-side TransformMode handlers and ClearEjectors() are placeholder
    // hooks for now — see comments in MainFrame.cpp / GLCanvas.cpp.
    void OnPlaceEjector(wxCommandEvent& evt);
    void OnClearEjectors(wxCommandEvent&);

    // Inserts. Place uses the current single selection as the parent if there
    // is one, otherwise it toggles into PlaceInsert so the next canvas click
    // picks the parent. Remove toggles into RemoveInsert (click a body to
    // delete it); Clear wipes every insert. Edit is a deliberate no-op stub —
    // the button exists so the card matches its neighbours, and the offset /
    // rotation authoring lands behind it later.
    void OnPlaceInsert(wxCommandEvent& evt);
    void OnClearInserts(wxCommandEvent&);

    void OnRemoveVent(wxCommandEvent&);
    void OnRemoveSprue(wxCommandEvent&);
    void OnRemoveRunner(wxCommandEvent&);
    void OnRemoveGate(wxCommandEvent&);
    void OnRemoveEjector(wxCommandEvent&);
    void OnRemoveInsert(wxCommandEvent&);

    void OnEditVent(wxCommandEvent&);
    void OnEditRunner(wxCommandEvent&);
    void OnEditGate(wxCommandEvent&);
    void OnEditSprue(wxCommandEvent&);
    void OnEditEjector(wxCommandEvent&);
    void OnEditInsert(wxCommandEvent&);

    void OnSetMetric(wxCommandEvent&);
    void OnSetImperial(wxCommandEvent&);

    // ---- Grid menu ---------------------------------------------------------
    // Opens the consolidated Grid Settings dialog (shape / size / spacing /
    // major divisions), stores the result in m_gridSettings, and pushes it to
    // the canvas so the rendered grid updates.
    void OnGridSettings(wxCommandEvent&);
    void OnAbout(wxCommandEvent&);
    void OnCheckForUpdates(wxCommandEvent&);
    void OnToggleAutoUpdateCheck(wxCommandEvent&);

    // ---- Workflow perspectives ---------------------------------------------
    // The window hosts two stacked perspectives in a wxSimplebook: "Prepare"
    // (the editing view — left panel + main canvas) and "Preview" (the post-cut
    // mould review — PreviewPanel). The ribbon's Prepare / Preview buttons (and
    // any future callers) switch between them via SetPerspective; each
    // perspective swaps in its own menu bar, while the ribbon stays shared.
    enum class Perspective { Prepare, Preview };
    void SetPerspective(Perspective which);
    void OnPerspectivePrepare(wxCommandEvent&);
    void OnPerspectivePreview(wxCommandEvent&);

    // Builds the two menu bars once at construction. Prepare carries the full
    // File / Fixture / Units / Import set; Preview is minimal (File -> Exit)
    // for now and grows as preview-specific actions are added.
    wxMenuBar* BuildPrepareMenuBar();
    wxMenuBar* BuildPreviewMenuBar();

    // Builds the Grid menu (Shape submenu + Change Size / Change Spacing),
    // reflecting the current m_gridSettings in the shape radio state.
    wxMenu* BuildGridMenu();
    wxMenu* BuildHelpMenu();

    // Drive the two ribbon perspective tabs so the active one reads as
    // selected. Safe to call before either tab exists (no-ops).
    void UpdatePerspectiveButtons();

    // Activates a tool button and deactivates the others (also called by GLCanvas on Escape)

    //Creates the left panel
    wxPanel* m_leftPanel = nullptr;
    wxPanel* CreateLeftPanel(wxWindow* parent);

    // Collapsible section helper
    wxPanel* CreateCollapsibleSection(wxWindow* parent, wxSizer* parentSizer,
        const wxString& title, wxPanel** contentOut = nullptr);
    wxPanel* CreateVentsContent(wxWindow* parent);
    wxPanel* CreateSpruesContent(wxWindow* parent);
    wxPanel* CreateRunnersContent(wxWindow* parent);
    wxPanel* CreateGatesContent(wxWindow* parent);
    wxPanel* CreateEjectorsContent(wxWindow* parent);
    wxPanel* CreateInsertsContent(wxWindow* parent);

    // Builds a "Place …" button with the standard side-panel styling (rounded
    // corners, BtnPlace fill — matching the Sprue feature button) and registers
    // a setter into m_toolBtnSetters so SetActiveTool can drive its visual
    // state externally (button click, Escape, canvas-internal mode transitions).
    RoundedButton* MakePlaceButton(wxWindow* parent, int id,
        const wxString& label);

    // Vent field members
    wxChoice* m_ventTypeChoice = nullptr;
    wxPanel* m_ventDimsPanel = nullptr;
    wxTextCtrl* m_ventLength = nullptr;
    wxTextCtrl* m_ventWidth = nullptr;
    wxTextCtrl* m_ventOverrunStart = nullptr;
    wxTextCtrl* m_ventOverrunEnd = nullptr;

    // Sprue field members
    wxChoice* m_sprueTypeChoice = nullptr;
    wxTextCtrl* m_sprueDiameter = nullptr;
    wxTextCtrl* m_sprueDraftAngle = nullptr;
    wxTextCtrl* m_sprueColdSlugDepth = nullptr;
    wxTextCtrl* m_sprueLength = nullptr;
    wxTextCtrl* m_sprueOverrun = nullptr;

    // Runner field members
    wxChoice* m_runnerTypeChoice = nullptr;
    wxTextCtrl* m_runnerDiameter = nullptr;
    wxTextCtrl* m_runnerColdSlugDepth = nullptr;

    // Gate field members
    wxChoice* m_gateTypeChoice = nullptr;
    wxTextCtrl* m_gateDiameter = nullptr;
    wxTextCtrl* m_gateDraftAngle = nullptr;
    wxTextCtrl* m_gateOverrun = nullptr;     // mm extension into the model

    // Sub-runner field members (within the Gates section)
    wxChoice* m_subRunnerTypeChoice = nullptr;
    wxTextCtrl* m_subRunnerDiameter = nullptr;

    // Ejector field members. Type dropdown drives which dimension panel is
    // visible — same pattern as the Gate "Tapered Cylinder" branch. Currently
    // only "Cylindrical" is offered; new geometries plug in by appending to
    // the wxChoice and adding a Show() branch in CreateEjectorsContent.
    wxChoice* m_ejectorTypeChoice = nullptr;
    wxTextCtrl* m_ejectorDiameter = nullptr;
    wxTextCtrl* m_ejectorLength = nullptr;

    // Insert field members. An insert has no authored dimensions — its
    // geometry is whatever was imported — so the Settings panel carries the
    // single "Cut scale" percentage rather than a type dropdown + dimension
    // rows. Read at placement time and captured onto the InsertFeature, the
    // same convention the dimension fields use for every other feature.
    wxTextCtrl* m_insertCutScale = nullptr;

    // The modeless insert-transform editor, or null when closed. Owned by
    // wxWidgets (parented to this frame); ~MainFrame destroys it explicitly so
    // it can't outlive the frame or fire a close-notify into a half-destroyed
    // frame.
    InsertEditDialog* m_insertEditDialog = nullptr;

    // Creates the top ribbon panel
    wxPanel* CreateRibbon(wxWindow* parent);

    GLCanvas* m_canvas = nullptr;

    // Part 5: floating overlay toolbar for authoring complex vent paths. Lives
    // as a child of m_canvas, shown only while a vent is being edited.
    VentEditToolbar* m_ventEditToolbar = nullptr;
    void UpdateVentEditToolbar();   // canvas path-edit-changed hook target

    // Floating overlay toolbar for the Edit Sprue environment (Move / Select
    // Injection Point). Separate window from the vent path toolbar — the two
    // never show at once (their modes are mutually exclusive), both pinned
    // top-centre. Same canvas path-edit-changed hook drives both updaters.
    SprueEditToolbar* m_sprueEditToolbar = nullptr;
    void UpdateSprueEditToolbar();

    // Bottom-centre hint overlay. Sibling of the canvas within m_preparePage,
    // pinned by GLCanvas::RepositionCanvasToast. Driven from SetActiveTool so
    // every route into a mode (button, Escape, canvas-internal transition)
    // updates it through one place.
    CanvasToast* m_canvasToast = nullptr;
    void UpdateCanvasToast(TransformMode mode);

    // ---- Background startup update check (U4) ----------------------------
    // The checker is owned here so an in-flight request survives the timer
    // handler returning, and is torn down (cancelling the request) with the
    // frame. The banner is a child of m_preparePage, sibling of the canvas,
    // pinned bottom-right; created lazily on the first notification.
    std::unique_ptr<UpdateChecker> m_startupChecker;
    UpdateBanner* m_updateBanner = nullptr;
    wxTimer m_startupUpdateTimer;
    void StartStartupUpdateCheck();
    void ShowUpdateBanner(const wxString& latestVersion, const wxString& targetUrl);
    void PositionUpdateBanner();

    // The two stacked workflow perspectives and the pager that switches between
    // them. m_preparePage wraps the left panel + main canvas; m_previewPanel is
    // the embedded preview perspective (replaces the old breakout PreviewFrame).
    wxSimplebook* m_book = nullptr;
    wxPanel* m_preparePage = nullptr;
    PreviewPanel* m_previewPanel = nullptr;
    Perspective m_perspective = Perspective::Prepare;

    // Per-perspective menu bars, both built once in the constructor and swapped
    // via SetMenuBar on perspective change. The frame only auto-destroys the
    // currently-attached bar at teardown, so ~MainFrame deletes the detached one.
    wxMenuBar* m_prepareMenuBar = nullptr;
    wxMenuBar* m_previewMenuBar = nullptr;

    // Ribbon perspective-switch tabs (shared top bar). Held so SetPerspective
    // can drive their selected styling.
    PerspectiveButton* m_btnPrepare = nullptr;
    PerspectiveButton* m_btnPreview = nullptr;

    // Stored fixture definition (for project save/load)
    FixtureDefinition m_fixtureDef;

    // Current project file path (empty if unsaved)
    std::string m_projectPath;

    // Unit system (false = metric/mm, true = imperial/in)
    bool m_imperial = false;
    std::vector<wxStaticText*> m_mmUnitLabels;  // labels that switch "mm"↔"in"

    // Grid configuration (shape / size / spacing), authored via the Grid menu.
    // Stored in mm; not yet applied to the live GridRenderer.
    GridSettings m_gridSettings;

    // Toggle-button setter registry. Each entry is (command-id → set-active(bool)).
    // makeToolBtn registers a setter for each toggle-style ribbon button so that
    // SetActiveTool can drive the visuals from the canonical TransformMode,
    // regardless of how the mode was changed (button click, Escape key,
    // mode-completion in the canvas, programmatic, ...).
    //
    // Replaces the previous wxToggleButton* member pointers, which were never
    // assigned because the actual ribbon buttons are custom wxPanel-based
    // controls built inside makeToolBtn rather than native wxToggleButton.
    std::unordered_map<int, std::function<void(bool)>> m_toolBtnSetters;

    wxPanel* m_sidePanel = nullptr;
    wxTextCtrl* m_exportPath = nullptr;

    // Top-ribbon primary-action buttons. Generate Mould shows in the Prepare
    // perspective and Export in Preview; SetPerspective toggles their
    // visibility so exactly one occupies the top-right slot at a time.
    RoundedButton* m_btnGenerate = nullptr;

    // Export is a split button: its action zone runs the current export mode
    // and its dropdown zone picks the mode ("Mould" / "Shot body"). The mode
    // lives here rather than in the widget so the export handlers can branch
    // on it; the widget just reports picks via wxEVT_CHOICE.
    SplitButton* m_btnExport = nullptr;

    enum class ExportMode { Mould, ShotBody };
    ExportMode m_exportMode = ExportMode::Mould;

    // Order MUST match the SplitButton's menu-item order (see CreateRibbon):
    // index 0 = Mould, index 1 = Shot body. UpdateExportButtonLabel keeps the
    // action-zone label in sync with m_exportMode.
    void UpdateExportButtonLabel();

    // Tri-state model for "is the Export button about to do something
    // reasonable?". Replaces an earlier bool that conflated the
    // "never-generated" and "dirty-since-generation" cases — they look
    // identical to the user but warrant different messages on click.
    //
    //   NeverGenerated  - hard block: no mould geometry exists yet, so
    //                     Export pops "Mould must be generated before it
    //                     can be exported" and returns.
    //   Clean           - happy path: export runs without prompting.
    //   Dirty           - mould exists but an edit happened since the
    //                     last successful generation; Export pops a
    //                     Yes/No warning that the output may not reflect
    //                     the current scene. Yes proceeds, No bails. The
    //                     state remains Dirty after a Yes — the geometry
    //                     hasn't actually been refreshed, so a second
    //                     click warrants a second warning.
    //
    // Transitions are direct field assignments at the call sites — no
    // wrapper methods, since each transition is a one-liner and the
    // state graph is small enough to scan at a glance:
    //   *               -> NeverGenerated   on project reset (new / load /
    //                                       import / fixture swap)
    //   NeverGenerated  -> Clean            on successful Generate Mould
    //   Dirty           -> Clean            on successful Generate Mould
    //   Clean           -> Dirty            on any scene mutation
    //                                       (canvas callback or an
    //                                       explicit transition at the
    //                                       relevant project handler)
    //   NeverGenerated  -> NeverGenerated   on a scene mutation when no
    //                                       mould has ever been built —
    //                                       handled by the if-guard in
    //                                       the canvas callback below.
    enum class MouldState { NeverGenerated, Clean, Dirty };
    MouldState m_mouldState = MouldState::NeverGenerated;

    void OnBrowseExport(wxCommandEvent&);
    // Action-zone click on the export split button: dispatches to the current
    // mode's exporter (DoExportMould / DoExportShotBody).
    void OnExport(wxCommandEvent&);
    // Dropdown pick on the export split button (wxEVT_CHOICE): updates
    // m_exportMode and the action-zone label. Does not export.
    void OnExportModeChanged(wxCommandEvent&);
    void OnGenerateMould(wxCommandEvent&);
    void OnClearVentPoints(wxCommandEvent&);

    // The actual export routines behind the split button's action zone. Split
    // out from OnExport so the dispatcher stays a thin mode switch; each keeps
    // the tri-state MouldState gate + file-dialog flow for its own output.
    void DoExportMould();
    void DoExportShotBody();

    wxPanel* CreateSidePanel(wxWindow* parent);

    enum {
        ID_Import = wxID_HIGHEST + 100,
        ID_PerspectivePrepare,
        ID_PerspectivePreview,
        ID_ToolSelect,
        ID_ToolTranslate,
        ID_ToolRotate,
        ID_ToolScale,
        ID_ToolPattern,
        ID_ToolPrecisionPlace,
        ID_ToolCenter,
        ID_ToolAlignFace,
        ID_ToolAlignMidplane,
        ID_ToolPlaceVent,
        ID_BrowseExport,
        ID_Export,
        ID_GenerateMould,
        ID_CreateFixture,
        ID_ChangeFixture,
        ID_EditFixture,
        ID_ClearVentPoints,
        ID_PlaceSprue,
        ID_ClearSprue,
        ID_PlaceRunner,
        ID_ClearRunners,
        ID_PlaceGate,
        ID_ClearGates,
        ID_PlaceEjector,
        ID_ClearEjectors,
        ID_PlaceInsert,
        ID_ClearInserts,
        ID_RemoveVent,
        ID_RemoveSprue,
        ID_RemoveRunner,
        ID_RemoveGate,
        ID_RemoveEjector,
        ID_RemoveInsert,
        ID_EditVent,
        ID_EditRunner,
        ID_EditGate,
        ID_EditSprue,
        ID_EditEjector,
        ID_EditInsert,
        ID_SaveProject,
        ID_LoadProject,
        ID_NewProject,
        ID_UnitMetric,
        ID_UnitImperial,
        ID_GridSettings,
        ID_MeshQualityOff,
        ID_MeshQualityDraft,
        ID_MeshQualityNormal,
        ID_MeshQualityHigh,
        ID_CheckForUpdates,
        ID_AutoUpdateCheck,
        ID_StartupUpdateTimer
    };
};
