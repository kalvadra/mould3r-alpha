#pragma once
#include <wx/wx.h>

// =============================================================================
// PerspectiveButton — flat tab-style toggle for the workflow perspective switch
//
// Owner-drawn (like RoundedButton) but rendered as a square-cornered tab rather
// than a pill: the ACTIVE tab fills with its background colour (set via
// SetBackgroundColour — a lighter slate) and draws a thin accent line along its
// bottom edge; the INACTIVE tab paints in the parent's colour so it blends into
// the surrounding bar, showing only its label. Two of these placed adjacent
// read as a connected tab strip (Prepare | Preview).
//
// Emits wxEVT_BUTTON on click/space/enter, so existing Bind(wxEVT_BUTTON, ...)
// wiring works unchanged. SetActive() drives the selected state externally
// (the owner flips both tabs on a perspective change).
// =============================================================================
class PerspectiveButton : public wxWindow
{
public:
    PerspectiveButton(wxWindow* parent, wxWindowID id, const wxString& label,
        const wxPoint& pos = wxDefaultPosition,
        const wxSize& size = wxDefaultSize);

    // Selected state. The active tab paints its background + accent underline;
    // the inactive one blends into the parent bar.
    void SetActive(bool active);
    bool IsActive() const { return m_active; }

    // Colour of the bottom selection line drawn when active. Defaults to a
    // light accent blue; override to theme it.
    void SetAccentColour(const wxColour& c) { m_accent = c; Refresh(); }

    // Thickness (px) of the bottom selection line. Default 3.
    void SetAccentThickness(int px) { m_accentThickness = px; Refresh(); }

    void     SetLabel(const wxString& label) override;
    wxString GetLabel() const override { return m_label; }

    bool AcceptsFocus() const override { return IsEnabled() && IsShown(); }
    bool AcceptsFocusFromKeyboard() const override { return IsEnabled() && IsShown(); }

protected:
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
    void EmitClick();

    wxString m_label;
    bool     m_active = false;
    bool     m_hovered = false;
    bool     m_pressed = false;
    int      m_accentThickness = 3;
    wxColour m_accent{ 0x4F, 0x7C, 0xD0 };   // light selection blue
};
