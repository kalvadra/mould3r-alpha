#pragma once
#include <wx/wx.h>
#include <functional>

#include "TransformSession.h"

class RoundedButton;

// =============================================================================
// TransformPanel — the live Move / Rotate / Scale panel, pinned to the right
// edge of a viewport (replaces the old modal TranslateDialog / RotateDialog /
// ScaleDialog). Used by both the Prepare canvas (MainFrame) and the Fixture
// Editor.
//
// Like VentEditToolbar it is a SIBLING of the GL canvas raised above it (a true
// child of a wxGLCanvas can be overdrawn by SwapBuffers); the owning canvas
// pins it top-right. Unlike that toolbar it hosts native text fields, which
// draw within this panel's own window.
//
// It is dumb about the scene. The owner starts it with Begin(), and receives
// intent through callbacks:
//   * OnChange(delta) — fired as the user types, whenever every field parses,
//     so the model previews live. A half-typed field ("-", "1e") just holds
//     the last preview.
//   * OnOk / OnCancel — the OK / Cancel buttons, Enter in a field, and Esc.
// SetDelta() pushes values in WITHOUT firing OnChange (for the on-canvas
// handles, next step).
//
// Values are the CHANGE being applied (offset, angles, factor), as the old
// dialogs were. Lengths display in the owner's unit (mm / in) and are always
// handed out in mm.
// =============================================================================
class TransformPanel : public wxPanel
{
public:
    explicit TransformPanel(wxWindow* parent);

    // Reset to the identity change for `tool`, retitle and show the matching
    // rows. `selectionCount` drives the status text and greys the fields when
    // nothing is selected. The panel's size can change; re-pin it afterwards.
    void Begin(Xform::Tool tool, bool imperial, int selectionCount);

    // New selection mid-tool: keep the tool, reset to identity.
    void SetSelectionCount(int selectionCount);

    void         SetDelta(const Xform::Delta& d);   // no OnChange
    Xform::Delta GetDelta() const { return m_delta; }
    Xform::Tool  GetTool() const { return m_tool; }

    void SetOnChange(std::function<void(const Xform::Delta&)> cb) { m_onChange = std::move(cb); }
    void SetOnOk(std::function<void()> cb) { m_onOk = std::move(cb); }
    void SetOnCancel(std::function<void()> cb) { m_onCancel = std::move(cb); }

private:
    void OnPaint(wxPaintEvent&);
    void OnSize(wxSizeEvent&);
    void OnText(wxCommandEvent&);
    void OnCharHook(wxKeyEvent&);
    void ApplyRoundedShape();

    wxTextCtrl* MakeRow(wxSizer* parentSz, const wxString& label, wxStaticText*& unit,
                        wxSizer*& rowOut);
    void        WriteFields();                 // m_delta -> fields (no events)
    bool        ReadFields(Xform::Delta& out) const;
    void        UpdateStatus();

    Xform::Tool  m_tool = Xform::Tool::Move;
    Xform::Delta m_delta;
    bool         m_imperial = false;
    int          m_selectionCount = 0;

    wxStaticText* m_title = nullptr;
    wxStaticText* m_status = nullptr;
    wxStaticText* m_note = nullptr;

    // X / Y / Z rows (Move, Rotate) and the factor row (Scale).
    wxTextCtrl*   m_fx = nullptr;
    wxTextCtrl*   m_fy = nullptr;
    wxTextCtrl*   m_fz = nullptr;
    wxTextCtrl*   m_ff = nullptr;
    wxStaticText* m_ux = nullptr;
    wxStaticText* m_uy = nullptr;
    wxStaticText* m_uz = nullptr;
    wxStaticText* m_uf = nullptr;
    wxSizer*      m_rowX = nullptr;
    wxSizer*      m_rowY = nullptr;
    wxSizer*      m_rowZ = nullptr;
    wxSizer*      m_rowF = nullptr;
    wxBoxSizer*   m_main = nullptr;

    RoundedButton* m_btnOk = nullptr;
    RoundedButton* m_btnCancel = nullptr;

    std::function<void(const Xform::Delta&)> m_onChange;
    std::function<void()>                    m_onOk;
    std::function<void()>                    m_onCancel;
};
