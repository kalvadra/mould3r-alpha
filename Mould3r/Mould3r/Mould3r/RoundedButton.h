#pragma once
#include <wx/wx.h>

// =============================================================================
// RoundedButton — owner-drawn rounded button matching wxButton's API
//
// Drop-in replacement for wxButton in custom-themed contexts. The native
// Windows BUTTON control can't be made round (no flag, no theme trick),
// so this widget derives from wxWindow and paints itself via
// wxGraphicsContext for anti-aliased rounded corners.
//
// Constructor signature mirrors wxButton's exactly, so migration is a
// near-mechanical `new wxButton(` → `new RoundedButton(` find-and-replace.
// All the wxWindow accessors callers use today work unchanged:
//   * SetBackgroundColour / SetForegroundColour / SetFont — read on every
//     paint, so live colour changes (e.g. the close-X hover handlers in
//     CreateFixtureDialog) keep working.
//   * Bind(wxEVT_BUTTON, ...) — the button emits wxEVT_BUTTON from its
//     own click handler, so existing event bindings stay intact.
//   * Enable / Disable — drawn dimmed; pointer events are dropped.
//
// Hover and pressed states are computed by lightening/darkening the
// current background colour, so callers don't have to declare separate
// "hover" Style:: entries. The focus ring is a 1-px lightened outline,
// shown only when the button has keyboard focus.
//
// Default corner radius is 4px (matches the design tweak we picked). Use
// SetCornerRadius() to override per-instance.
// =============================================================================
class RoundedButton : public wxWindow
{
public:
    RoundedButton(wxWindow* parent, wxWindowID id, const wxString& label,
        const wxPoint& pos = wxDefaultPosition,
        const wxSize& size = wxDefaultSize,
        long           style = 0);

    // wxButton-equivalent label accessors.
    void     SetLabel(const wxString& label) override;
    wxString GetLabel() const override { return m_label; }

    // Per-instance corner radius override. Defaults to 4.
    void SetCornerRadius(int r);
    int  GetCornerRadius() const { return m_cornerRadius; }

    // Visual-disabled state — paints the button in the standard disabled
    // colours (bg + fg blended toward parent bg) WITHOUT blocking mouse
    // or keyboard events. Used by callers that want a "looks disabled"
    // affordance while still receiving clicks — typically so the click
    // handler can show an explanatory dialog instead of running the
    // real action. The MainFrame Export button uses this to display
    // "Mould must be generated before it can be exported" on click.
    //
    // Independent of wxWindow::Enable(). When either is set (real
    // disabled OR visually disabled), the paint goes muted; hover and
    // pressed states stop applying so the button looks consistent
    // regardless of pointer state. Tooltips and clicks still fire as
    // long as the wxWindow side is actually enabled — that's the whole
    // point of this distinction.
    void SetVisuallyDisabled(bool disabled);
    bool IsVisuallyDisabled() const { return m_visuallyDisabled; }

    // wxWindow focus opt-in — required for keyboard-only activation
    // (Space / Enter). AcceptsFocus gates programmatic SetFocus and
    // AcceptsFocusFromKeyboard gates the TAB cycle. Both return false
    // when disabled or hidden so the focus order skips us in that state.
    bool AcceptsFocus() const override { return IsEnabled() && IsShown(); }
    bool AcceptsFocusFromKeyboard() const override { return IsEnabled() && IsShown(); }

protected:
    // Best-size override — lets the widget participate in sizer auto-fit
    // when constructed with wxDefaultSize. Existing call sites mostly
    // pass an explicit wxSize, so this is a fallback path.
    wxSize DoGetBestClientSize() const override;

private:
    void OnPaint(wxPaintEvent&);
    void OnMouseEnter(wxMouseEvent&);
    void OnMouseLeave(wxMouseEvent&);
    void OnMouseDown(wxMouseEvent&);
    void OnMouseUp(wxMouseEvent&);
    void OnMouseCaptureLost(wxMouseCaptureLostEvent&);
    void OnKeyDown(wxKeyEvent&);
    void OnFocusChange(wxFocusEvent&);

    // Synthesise a wxEVT_BUTTON command event addressed to this widget.
    // Mirrors what wxButton does internally on click — keeps the rest of
    // the app's Bind(wxEVT_BUTTON, ...) wiring oblivious to the swap.
    void EmitClick();

    wxString m_label;
    int      m_cornerRadius = 4;
    bool     m_hovered = false;
    bool     m_pressed = false;
    bool     m_visuallyDisabled = false;   // paints muted but still
    // accepts clicks — see
    // SetVisuallyDisabled docs.
};
