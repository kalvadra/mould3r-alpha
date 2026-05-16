#pragma once
#include <wx/wx.h>
#include <wx/gauge.h>
#include <functional>
#include <string>

// =============================================================================
// CreateFixtureDialog
//
// Small modal dialog that gathers the three pieces of metadata needed to
// open a new fixture in FixtureEditor: a human-readable fixture name and the
// two STEP file paths (Mould Half A / Mould Half B). Replaces the previous
// "click New Fixture → editor opens empty" flow; the editor now opens with
// both halves already loaded and a default save filename already chosen.
//
// Reached from two places, both of which previously instantiated
// FixtureEditor directly:
//   * StartupDialog "New Fixture..." button
//   * MainFrame "Fixture → Create Fixture..." menu item
//
// Styling mirrors StartupDialog 1-for-1 — same hairline sky-blue border,
// same frameless title row with drag-to-move + close X, same Style::
// button colours. The intent is for both dialogs to read as part of the
// same family.
//
// On wxID_OK, callers consume GetFixtureName() / GetModelAPath() /
// GetModelBPath() and pass the trio into FixtureEditor::SetInitialFixture.
// On wxID_CANCEL the caller does nothing — same idiom as StartupDialog.
//
// Loading: callers can register a load handler via SetLoadHandler. When
// Create is pressed and validates, the dialog swaps its content panel for
// a progress view (status text + wxGauge) and invokes the handler,
// passing it a ProgressFn the handler can call to update the bar. When
// the handler returns, the dialog EndModal(wxID_OK). With no handler set,
// Create just closes immediately — same behaviour as before progress was
// added.
// =============================================================================
class CreateFixtureDialog : public wxDialog
{
public:
    explicit CreateFixtureDialog(wxWindow* parent);

    std::string GetFixtureName() const { return m_fixtureName; }
    std::string GetModelAPath()  const { return m_modelAPath; }
    std::string GetModelBPath()  const { return m_modelBPath; }

    // Callback signature used to report progress from a load handler back
    // into the dialog's progress UI. percent is 0–100 (values outside the
    // range are clamped). status is the human-readable line shown above
    // the bar; pass empty to leave it unchanged. The dialog wxYields()
    // after each call so the UI repaints between steps — handlers don't
    // need to yield themselves.
    using ProgressFn = std::function<void(int percent, const std::string& status)>;

    // Register a handler that runs the slow work between Create being
    // pressed and the dialog closing. Typical use: the caller creates a
    // hidden FixtureEditor, then in the handler calls
    // editor->SetInitialFixture(..., progress) to drive the editor's
    // canvas loads. When the handler returns the dialog EndModal(wxID_OK);
    // if it throws, the exception propagates out of ShowModal().
    //
    // If no handler is registered, OnCreate just closes the dialog
    // immediately without showing the progress panel — useful for unit
    // testing or for any caller that wants to do the loading themselves
    // after ShowModal returns.
    void SetLoadHandler(std::function<void(ProgressFn)> handler)
    {
        m_loadHandler = std::move(handler);
    }

private:
    // Button handlers. OnSelectModelA/B open a wxFileDialog (Open mode,
    // STEP filter) and write the chosen path into the matching text field.
    // OnCreate validates non-empty + on-disk existence; on success it
    // swaps to the progress panel, invokes m_loadHandler, then EndModal.
    void OnSelectModelA(wxCommandEvent&);
    void OnSelectModelB(wxCommandEvent&);
    void OnCreate(wxCommandEvent&);

    // Swap the content area from the form view to the progress view.
    // Called once, from OnCreate after validation succeeds. The form
    // panel is left around in memory (not destroyed) — there's no path
    // back from the progress view today, so we don't need to swap back,
    // but keeping it allocated avoids a destroy/rebuild dance if a
    // "retry on error" branch ever gets added.
    void ShowLoadingState();

    // Update the progress widgets and yield so the change paints. Bound
    // into the ProgressFn passed to m_loadHandler; safe to call before
    // ShowLoadingState (it just no-ops since m_loadingPanel is hidden).
    void UpdateProgress(int percent, const std::string& status);

    // Frameless drag handling — identical pattern to StartupDialog. The
    // custom title row stands in for the missing system title bar so the
    // user can still move the window. wxMouseCaptureLost is required:
    // wxWidgets asserts in debug builds if a capture is silently abandoned
    // (e.g. via Alt+Tab).
    void OnTitleMouseDown(wxMouseEvent&);
    void OnTitleMouseUp(wxMouseEvent&);
    void OnTitleMouseMove(wxMouseEvent&);
    void OnTitleCaptureLost(wxMouseCaptureLostEvent&);

    // Helper: open a STEP/IGES file picker rooted at the parent of the
    // current text-field value (if any), and write the chosen path back
    // into the field. Shared by the two Select handlers.
    void PickModelPath(wxTextCtrl* target);

    wxPanel* m_titleRow = nullptr;

    // Form view (input fields + button row). Wrapped in its own panel so
    // it can be hidden in one call when loading starts.
    wxPanel* m_formPanel = nullptr;
    wxTextCtrl* m_nameCtrl = nullptr;
    wxTextCtrl* m_pathACtrl = nullptr;
    wxTextCtrl* m_pathBCtrl = nullptr;

    // Loading view — shown after Create validates and the load handler
    // starts. m_loadingPanel is parked in the same sizer slot as
    // m_formPanel; only one is visible at a time.
    wxPanel* m_loadingPanel = nullptr;
    wxStaticText* m_loadingStatus = nullptr;
    wxGauge* m_loadingGauge = nullptr;

    // Caller-registered load handler — see SetLoadHandler comment.
    std::function<void(ProgressFn)> m_loadHandler;

    // Captured at OnCreate-time from the controls above so callers don't
    // have to reach back into the wxTextCtrls after EndModal.
    std::string m_fixtureName;
    std::string m_modelAPath;
    std::string m_modelBPath;

    // Drag-to-move state for the frameless title row. m_dragOffset is the
    // screen-space offset from the dialog's top-left to where the user
    // clicked, captured at MouseDown and held constant for the drag.
    wxPoint m_dragOffset;
    bool    m_dragging = false;

    enum
    {
        // Range chosen to avoid collisions with StartupDialog (HIGHEST+300)
        // and FixtureEditor (HIGHEST+700).
        ID_SelectA = wxID_HIGHEST + 400,
        ID_SelectB,
        ID_Create,
        ID_Cancel
    };
};
