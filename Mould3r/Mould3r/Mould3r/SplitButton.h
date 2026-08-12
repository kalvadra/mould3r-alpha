#pragma once
#include <wx/wx.h>
#include <vector>

// =============================================================================
// SplitButton — owner-drawn split button: a primary action area plus a
// dropdown chevron zone that pops a mode menu.
//
// Visually a single rounded pill (same look as RoundedButton) divided into
// two clickable zones:
//   * Dropdown zone (left)  — a chevron; clicking it pops a checkable menu of
//                             the items set via SetMenuItems. Picking an item
//                             fires wxEVT_CHOICE (GetInt() = new index) and
//                             updates the checked mark. The button does NOT
//                             change its own label — the owner decides what
//                             the label should read for the chosen mode and
//                             calls SetLabel from the wxEVT_CHOICE handler.
//   * Action zone (right)   — the label; clicking it (or Space/Enter with
//                             keyboard focus) fires wxEVT_BUTTON, exactly like
//                             RoundedButton, so existing Bind(wxEVT_BUTTON,...)
//                             wiring works unchanged.
//
// The two events keep the "what mode am I in" state in the owner (MainFrame),
// mirroring how the app already threads small bits of UI state through command
// handlers rather than baking policy into the widgets. Colour handling (hover
// / press lightening + darkening, disabled muting) matches RoundedButton so
// the two read as one family; the dropdown zone carries a permanent subtle
// darken so the split reads at rest, not only on hover.
// =============================================================================
class SplitButton : public wxWindow
{
public:
    SplitButton(wxWindow* parent, wxWindowID id, const wxString& label,
        const wxPoint& pos = wxDefaultPosition,
        const wxSize& size = wxDefaultSize,
        long           style = 0);

    // Action-zone label accessors (wxButton-equivalent).
    void     SetLabel(const wxString& label) override;
    wxString GetLabel() const override { return m_label; }

    // Replace the dropdown menu items. Resets the checked selection to 0 (or
    // to none if empty). Does not fire wxEVT_CHOICE — this is owner-driven
    // setup, not a user pick.
    void SetMenuItems(const std::vector<wxString>& items);

    // Current / programmatic checked item. Out-of-range is clamped-ignored.
    // SetSelection does not fire wxEVT_CHOICE (it's a programmatic set); only
    // a user pick from the popped menu does.
    int  GetSelection() const { return m_selection; }
    void SetSelection(int index);

    // Per-instance corner radius override. Defaults to 4 (matches RoundedButton).
    void SetCornerRadius(int r);
    int  GetCornerRadius() const { return m_cornerRadius; }

    // Focus opt-in, same contract as RoundedButton — participates in the TAB
    // cycle and programmatic SetFocus only while enabled and shown.
    bool AcceptsFocus() const override { return IsEnabled() && IsShown(); }
    bool AcceptsFocusFromKeyboard() const override { return IsEnabled() && IsShown(); }

protected:
    wxSize DoGetBestClientSize() const override;

private:
    void OnPaint(wxPaintEvent&);
    void OnMouseEnter(wxMouseEvent&);
    void OnMouseLeave(wxMouseEvent&);
    void OnMouseMove(wxMouseEvent&);
    void OnMouseDown(wxMouseEvent&);
    void OnMouseUp(wxMouseEvent&);
    void OnMouseCaptureLost(wxMouseCaptureLostEvent&);
    void OnKeyDown(wxKeyEvent&);
    void OnFocusChange(wxFocusEvent&);

    // Width of the left dropdown zone in client pixels.
    int  DropZoneWidth() const { return 30; }
    // True if client-x falls in the dropdown zone (vs the action zone).
    bool InDropZone(int x) const { return x < DropZoneWidth(); }

    // Pop the mode menu anchored under the dropdown zone; wires each item to
    // update the selection, fire wxEVT_CHOICE, and repaint.
    void ShowMenu();

    // Fire wxEVT_BUTTON for the action zone (mirrors RoundedButton::EmitClick).
    void EmitClick();

    wxString              m_label;
    std::vector<wxString> m_menuItems;
    int                   m_selection = 0;      // checked item, or -1 if none

    int  m_cornerRadius = 4;

    bool m_hovered      = false;   // pointer anywhere over the control
    bool m_hoverDrop    = false;   // pointer specifically over the drop zone
    bool m_pressed      = false;   // left button currently held
    bool m_pressedDrop  = false;   // …and the press landed in the drop zone

    // Menu-open latch: while the popup is up we hold the drop zone in its
    // pressed look so the affordance reads as active, then clear on dismiss.
    bool m_menuOpen     = false;
};
