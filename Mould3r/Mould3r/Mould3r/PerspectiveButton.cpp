#include "PerspectiveButton.h"
#include <wx/graphics.h>
#include <wx/dcbuffer.h>
#include <algorithm>
#include <memory>

namespace
{
    wxColour Lighten(const wxColour& c, int amt)
    {
        return wxColour(
            (unsigned char)std::min(255, c.Red() + amt),
            (unsigned char)std::min(255, c.Green() + amt),
            (unsigned char)std::min(255, c.Blue() + amt),
            c.Alpha());
    }

    // Linear blend of two colours: t = 0 -> a, t = 1 -> b. Used to dim a
    // disabled tab's label toward the surrounding bar so it reads as inert.
    wxColour Blend(const wxColour& a, const wxColour& b, double t)
    {
        const double u = 1.0 - t;
        return wxColour(
            (unsigned char)(a.Red()   * u + b.Red()   * t),
            (unsigned char)(a.Green() * u + b.Green() * t),
            (unsigned char)(a.Blue()  * u + b.Blue()  * t),
            a.Alpha());
    }
}

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------
PerspectiveButton::PerspectiveButton(wxWindow* parent, wxWindowID id,
    const wxString& label, const wxPoint& pos, const wxSize& size)
    : wxWindow(parent, id, pos, size,
        wxBORDER_NONE | wxFULL_REPAINT_ON_RESIZE | wxWANTS_CHARS)
    , m_label(label)
{
    SetBackgroundStyle(wxBG_STYLE_PAINT);
    // Default active fill is the parent's colour until the caller overrides it
    // with SetBackgroundColour (typically a lighter slate, e.g. Style::CardBg).
    SetBackgroundColour(parent->GetBackgroundColour());
    SetForegroundColour(*wxWHITE);

    Bind(wxEVT_PAINT, &PerspectiveButton::OnPaint, this);
    Bind(wxEVT_ENTER_WINDOW, &PerspectiveButton::OnMouseEnter, this);
    Bind(wxEVT_LEAVE_WINDOW, &PerspectiveButton::OnMouseLeave, this);
    Bind(wxEVT_LEFT_DOWN, &PerspectiveButton::OnMouseDown, this);
    Bind(wxEVT_LEFT_UP, &PerspectiveButton::OnMouseUp, this);
    Bind(wxEVT_MOUSE_CAPTURE_LOST, &PerspectiveButton::OnMouseCaptureLost, this);
    Bind(wxEVT_KEY_DOWN, &PerspectiveButton::OnKeyDown, this);
    Bind(wxEVT_SET_FOCUS, &PerspectiveButton::OnFocusChange, this);
    Bind(wxEVT_KILL_FOCUS, &PerspectiveButton::OnFocusChange, this);
}

// ---------------------------------------------------------------------------
// Public accessors
// ---------------------------------------------------------------------------
void PerspectiveButton::SetActive(bool active)
{
    if (m_active == active) return;
    m_active = active;
    Refresh();
}

void PerspectiveButton::SetLabel(const wxString& label)
{
    if (m_label == label) return;
    m_label = label;
    InvalidateBestSize();
    Refresh();
}

wxSize PerspectiveButton::DoGetBestClientSize() const
{
    wxClientDC dc(const_cast<PerspectiveButton*>(this));
    dc.SetFont(GetFont());
    const wxSize textSize = m_label.IsEmpty()
        ? dc.GetTextExtent("M")
        : dc.GetTextExtent(m_label);
    return wxSize(textSize.x + 36, textSize.y + 16);
}

// ---------------------------------------------------------------------------
// Paint — flat tab. Active: fill + bottom accent line. Inactive: blends into
// the parent bar (only the label shows), with a faint hover lift.
// ---------------------------------------------------------------------------
void PerspectiveButton::OnPaint(wxPaintEvent&)
{
    wxAutoBufferedPaintDC dc(this);

    const wxColour parentBg = GetParent() ? GetParent()->GetBackgroundColour()
        : GetBackgroundColour();
    const wxColour activeBg = GetBackgroundColour();

    // Label colour: full-strength when enabled, dimmed toward the bar when the
    // tab is disabled (e.g. Casting for a non-castable mould) so it reads as
    // inert rather than clickable.
    wxColour fg = GetForegroundColour();
    if (!IsEnabled())
        fg = Blend(fg, parentBg, 0.6);

    const wxSize sz = GetClientSize();

    // Fill.
    wxColour bg = m_active ? activeBg : parentBg;
    if (!m_active && IsEnabled() && (m_hovered || m_pressed))
        bg = Lighten(parentBg, 10);

    dc.SetBackground(wxBrush(bg));
    dc.Clear();

    std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::Create(dc));
    if (!gc) return;
    gc->SetAntialiasMode(wxANTIALIAS_DEFAULT);

    // Bottom selection line for the active tab.
    if (m_active && m_accentThickness > 0)
    {
        gc->SetBrush(wxBrush(m_accent));
        gc->SetPen(*wxTRANSPARENT_PEN);
        gc->DrawRectangle(0, sz.y - m_accentThickness, sz.x, m_accentThickness);
    }

    // Label — centred.
    if (!m_label.IsEmpty())
    {
        gc->SetFont(GetFont(), fg);
        wxDouble tw = 0, th = 0, td = 0, te = 0;
        gc->GetTextExtent(m_label, &tw, &th, &td, &te);
        gc->DrawText(m_label, (sz.x - tw) / 2.0, (sz.y - th) / 2.0);
    }
}

// ---------------------------------------------------------------------------
// Mouse / keyboard — same interaction model as RoundedButton.
// ---------------------------------------------------------------------------
void PerspectiveButton::OnMouseEnter(wxMouseEvent& e)
{
    if (!m_hovered) { m_hovered = true; Refresh(); }
    e.Skip();
}

void PerspectiveButton::OnMouseLeave(wxMouseEvent& e)
{
    if (m_hovered) { m_hovered = false; Refresh(); }
    e.Skip();
}

void PerspectiveButton::OnMouseDown(wxMouseEvent& e)
{
    if (!IsEnabled()) { e.Skip(); return; }
    if (!HasCapture()) CaptureMouse();
    m_pressed = true;
    if (CanAcceptFocus()) SetFocus();
    Refresh();
    e.Skip();
}

void PerspectiveButton::OnMouseUp(wxMouseEvent& e)
{
    const bool wasPressed = m_pressed;
    m_pressed = false;
    if (HasCapture()) ReleaseMouse();

    if (wasPressed && IsEnabled())
    {
        const wxSize sz = GetClientSize();
        const wxPoint pt = e.GetPosition();
        if (pt.x >= 0 && pt.x < sz.x && pt.y >= 0 && pt.y < sz.y)
            EmitClick();
    }
    Refresh();
    e.Skip();
}

void PerspectiveButton::OnMouseCaptureLost(wxMouseCaptureLostEvent&)
{
    m_pressed = false;
    Refresh();
}

void PerspectiveButton::OnKeyDown(wxKeyEvent& e)
{
    const int code = e.GetKeyCode();
    if ((code == WXK_SPACE || code == WXK_RETURN || code == WXK_NUMPAD_ENTER)
        && IsEnabled())
    {
        EmitClick();
        return;
    }
    e.Skip();
}

void PerspectiveButton::OnFocusChange(wxFocusEvent& e)
{
    Refresh();
    e.Skip();
}

void PerspectiveButton::EmitClick()
{
    wxCommandEvent evt(wxEVT_BUTTON, GetId());
    evt.SetEventObject(this);
    GetEventHandler()->ProcessEvent(evt);
}
