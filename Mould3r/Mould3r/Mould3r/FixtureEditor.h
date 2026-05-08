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
//   * Lefthand toolbar (Move, Rotate, Scale, Center, Align Face) with
//     mutually-exclusive toggle visuals — handlers themselves are still
//     stubs and don't yet mutate geometry.
//   * 3D viewport (FixtureCanvas) with grid, orbit camera, and lit
//     rendering of any imported halves.
//
// What's still pending:
//   * The toolbar handlers — currently only flip toggle state; they need
//     to drive transform modes on the canvas the way MainFrame's MODEL
//     TOOLS panel drives the main canvas.
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

    // Toolbar click handlers — stubs for now. Each persistent-toggle
    // handler routes through SetActiveTool so the four toggles stay
    // mutually exclusive (matching MainFrame's MODEL TOOLS behavior).
    // OnToolCenter is the only momentary action; it does not affect the
    // active-tool state.
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
    // a single active tool. Pass wxID_NONE to clear all toggles. Mirrors
    // the role MainFrame::SetActiveTool plays for the main tool grid.
    void SetActiveTool(int activeId);

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
