#pragma once
#include <wx/wx.h>
#include <wx/frame.h>
#include <functional>
#include <string>
#include <unordered_map>

class FixtureCanvas;

// =============================================================================
// FixtureEditor
//
// Floating top-level window (separate from MainFrame) used to author a new
// fixture — that is, to load the two STEP halves, position them in space,
// and save the resulting .fixture file. Replaces the old "two file dialogs
// + a save dialog" flow that StartupDialog::OnNewFixture used to drive.
//
// What's wired up:
//   * Top ribbon with "Import Mould Half A/B" buttons + path labels;
//     picking a STEP file loads it into the canvas and updates the path.
//   * Lefthand toolbar (Move, Rotate, Scale, Center, Align Face) routed
//     through to the canvas. Move/Rotate/Scale follow MainFrame's
//     dialog-based convention (toggle latches briefly, dialog opens,
//     toggle clears on dialog close); Center is a momentary action;
//     Align Face is a true persistent toggle with ESC-to-cancel.
//   * 3D viewport (FixtureCanvas) with grid, orbit camera, lit rendering
//     of any imported halves, single-half selection (warm-yellow tint
//     indicates selection), and a dark-grey hover overlay during Align
//     Face mode.
//
// What's still pending:
//   * The save flow — we collect both half paths, but nothing writes a
//     .fixture file or notifies StartupDialog of the new fixture yet.
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

    // Top-ribbon Import handlers — open a STEP file picker, then store the
    // chosen path in m_modelA/BPath and update the corresponding path
    // label. Re-importing simply overwrites the previous selection. Once
    // the editor's runtime exists these will also feed the picked file
    // into the canvas, but for now they only update state + label.
    void OnImportModelA(wxCommandEvent&);
    void OnImportModelB(wxCommandEvent&);

    // Shared body for the two Import handlers above. Pops the file dialog
    // with `title`, and on success writes into `outPath`, refreshes
    // `pathLabel` with the chosen path, and returns true. Returns false on
    // cancel (with `outPath` and `pathLabel` left untouched) so the caller
    // can skip downstream work like canvas loading.
    bool PickStepFile(const wxString& title,
        std::string& outPath,
        wxStaticText* pathLabel);

    // Drive the visual state of every registered toggle button to reflect
    // a single active tool. Public; declared above with the rest of the
    // class's public API so the canvas's ESC handler can call it.

    // Per-button visual setters keyed by command ID. Populated by the
    // makeToolBtn helper in BuildToolbar; consumed by SetActiveTool.
    // Same pattern as MainFrame::m_toolBtnSetters.
    std::unordered_map<int, std::function<void(bool)>> m_toolBtnSetters;
    int m_activeToolId = wxID_NONE;

    // Picked-file paths. Empty until the user runs an import. These are
    // the inputs that the (future) save flow will read when writing the
    // .fixture file.
    std::string m_modelAPath;
    std::string m_modelBPath;

    // Path-display labels in the top ribbon. Held as members so the
    // import handlers can update them after a successful pick. Owned by
    // the wxWidgets parent-child hierarchy — no manual delete.
    wxStaticText* m_lblModelAPath = nullptr;
    wxStaticText* m_lblModelBPath = nullptr;

    // 3D viewport. Owned by the parent-child hierarchy. Held as a member
    // so future runtime code (transform handlers, file-load wiring) can
    // push state into the canvas without re-walking the widget tree.
    FixtureCanvas* m_canvas = nullptr;

    enum
    {
        // Range chosen to avoid collisions with MainFrame's tool IDs
        // (which start at wxID_HIGHEST + 1) and StartupDialog's IDs
        // (wxID_HIGHEST + 300).
        ID_FE_ImportModelA = wxID_HIGHEST + 700,
        ID_FE_ImportModelB,
        ID_FE_Move,
        ID_FE_Rotate,
        ID_FE_Scale,
        ID_FE_Center,
        ID_FE_AlignFace
    };
};
