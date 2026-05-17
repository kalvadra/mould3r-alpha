#include "RoundedButton.h"
#include <wx/graphics.h>
#include <wx/dcbuffer.h>
#include <algorithm>
#include <memory>

namespace
{
    // ---- Colour math helpers ----------------------------------------------
    // wxWidgets doesn't ship lighten/darken helpers, and pulling in HSL
    // conversion is overkill for what's really just "shove every channel
    // by N". Clamp at the byte boundaries.
    wxColour Lighten(const wxColour& c, int amt)
    {
        return wxColour(
            (unsigned char)std::min(255, c.Red() + amt),
            (unsigned char)std::min(255, c.Green() + amt),
            (unsigned char)std::min(255, c.Blue() + amt),
            c.Alpha());
    }
    wxColour Darken(const wxColour& c, int amt)
    {
        return wxColour(
            (unsigned char)std::max(0, c.Red() - amt),
            (unsigned char)std::max(0, c.Green() - amt),
            (unsigned char)std::max(0, c.Blue() - amt),
            c.Alpha());
    }
    // Linear blend between two colours by t in [0, 1]. Used for the
    // disabled state — blend toward the parent's bg to mute the button.
    wxColour Blend(const wxColour& a, const wxColour& b, float t)
    {
        const float u = 1.0f - t;
        return wxColour(
            (unsigned char)(a.Red() * u + b.Red() * t),
            (unsigned char)(a.Green() * u + b.Green() * t),
            (unsigned char)(a.Blue() * u + b.Blue() * t));
    }
}

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------
RoundedButton::RoundedButton(wxWindow* parent, wxWindowID id, const wxString& label,
    const wxPoint& pos, const wxSize& size, long /*style*/)
    : wxWindow(parent, id, pos, size,
        wxBORDER_NONE | wxFULL_REPAINT_ON_RESIZE | wxWANTS_CHARS)
    , m_label(label)
{
    // wxBG_STYLE_PAINT: we promise to fill the whole client area in
    // OnPaint, so wxWidgets skips the default erase-background pass.
    // Required when using wxAutoBufferedPaintDC — without it the buffer
    // contents and the erase pass fight and the result flickers.
    SetBackgroundStyle(wxBG_STYLE_PAINT);

    // Start with the parent's bg — call sites typically immediately
    // override with SetBackgroundColour, but having a sensible default
    // means a freshly-constructed RoundedButton with no styling at
    // least disappears into its container instead of drawing a default
    // grey rectangle.
    SetBackgroundColour(parent->GetBackgroundColour());
    SetForegroundColour(*wxBLACK);

    Bind(wxEVT_PAINT, &RoundedButton::OnPaint, this);
    Bind(wxEVT_ENTER_WINDOW, &RoundedButton::OnMouseEnter, this);
    Bind(wxEVT_LEAVE_WINDOW, &RoundedButton::OnMouseLeave, this);
    Bind(wxEVT_LEFT_DOWN, &RoundedButton::OnMouseDown, this);
    Bind(wxEVT_LEFT_UP, &RoundedButton::OnMouseUp, this);
    Bind(wxEVT_MOUSE_CAPTURE_LOST, &RoundedButton::OnMouseCaptureLost, this);
    Bind(wxEVT_KEY_DOWN, &RoundedButton::OnKeyDown, this);
    Bind(wxEVT_SET_FOCUS, &RoundedButton::OnFocusChange, this);
    Bind(wxEVT_KILL_FOCUS, &RoundedButton::OnFocusChange, this);
}

// ---------------------------------------------------------------------------
// Public accessors
// ---------------------------------------------------------------------------
void RoundedButton::SetLabel(const wxString& label)
{
    if (m_label == label) return;
    m_label = label;
    InvalidateBestSize();
    Refresh();
}

void RoundedButton::SetCornerRadius(int r)
{
    const int clamped = std::max(0, r);
    if (m_cornerRadius == clamped) return;
    m_cornerRadius = clamped;
    Refresh();
}

wxSize RoundedButton::DoGetBestClientSize() const
{
    // Same horizontal/vertical padding as a typical wxButton: ~10px each
    // side, ~8px top/bottom. Callers that pass an explicit wxSize bypass
    // this entirely (the explicit size wins).
    wxClientDC dc(const_cast<RoundedButton*>(this));
    dc.SetFont(GetFont());
    const wxSize textSize = m_label.IsEmpty()
        ? dc.GetTextExtent("M")   // arbitrary fallback to avoid 0-height
        : dc.GetTextExtent(m_label);
    return wxSize(textSize.x + 20, textSize.y + 16);
}

// ---------------------------------------------------------------------------
// Paint
// ---------------------------------------------------------------------------
void RoundedButton::OnPaint(wxPaintEvent&)
{
    wxAutoBufferedPaintDC dc(this);

    // Flood the client area with the PARENT's bg first. The rounded rect
    // we draw next covers most of this; the leftover triangles in the
    // four corners (outside the rounded shape, inside the client rect)
    // pick up the parent's colour, which is what we want for the corners
    // to "blend out" cleanly. Assumes the parent panel is a solid colour
    // — true everywhere we use this in the app.
    const wxColour parentBg = GetParent() ? GetParent()->GetBackgroundColour()
        : GetBackgroundColour();
    dc.SetBackground(wxBrush(parentBg));
    dc.Clear();

    std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::Create(dc));
    if (!gc) return;
    gc->SetAntialiasMode(wxANTIALIAS_DEFAULT);

    const wxSize sz = GetClientSize();

    // Resolve bg / fg based on interaction state. The colour deltas are
    // small (12 / 16) — Windows 11 button hover feedback is similarly
    // subtle, and a louder change would fight the rest of the dark
    // theme. Disabled state desaturates toward the parent bg.
    wxColour bg = GetBackgroundColour();
    wxColour fg = GetForegroundColour();

    if (!IsEnabled())
    {
        bg = Blend(bg, parentBg, 0.55f);
        fg = Blend(fg, parentBg, 0.55f);
    }
    else if (m_pressed && m_hovered)
    {
        bg = Darken(bg, 16);
    }
    else if (m_hovered)
    {
        bg = Lighten(bg, 12);
    }

    // Body — rounded rect, no stroke. Pixel-aligned coordinates so the
    // edge stays crisp; the AA mode only matters at the corners.
    gc->SetBrush(wxBrush(bg));
    gc->SetPen(*wxTRANSPARENT_PEN);
    gc->DrawRoundedRectangle(0, 0, sz.x, sz.y, m_cornerRadius);

    // Focus ring — 1-px outline inset by half a pixel so it sits on the
    // pixel boundary rather than spanning two pixels (which would render
    // softer). Only drawn when keyboard-focused; mouse focus alone
    // (e.g. just clicking) doesn't trip it.
    if (HasFocus() && IsEnabled())
    {
        gc->SetPen(wxPen(Lighten(bg, 35), 1));
        gc->SetBrush(*wxTRANSPARENT_BRUSH);
        gc->DrawRoundedRectangle(0.5, 0.5, sz.x - 1.0, sz.y - 1.0,
            std::max(0, m_cornerRadius - 1));
    }

    // Label — centred. GetTextExtent on wxGraphicsContext returns the
    // four metrics; we only need width and height for centring.
    if (!m_label.IsEmpty())
    {
        gc->SetFont(GetFont(), fg);
        wxDouble tw = 0, th = 0, td = 0, te = 0;
        gc->GetTextExtent(m_label, &tw, &th, &td, &te);
        gc->DrawText(m_label,
            (sz.x - tw) / 2.0,
            (sz.y - th) / 2.0);
    }
}

// ---------------------------------------------------------------------------
// Mouse handling
// ---------------------------------------------------------------------------
void RoundedButton::OnMouseEnter(wxMouseEvent& e)
{
    if (m_hovered) { e.Skip(); return; }
    m_hovered = true;
    Refresh();
    e.Skip();
}

void RoundedButton::OnMouseLeave(wxMouseEvent& e)
{
    if (!m_hovered) { e.Skip(); return; }
    m_hovered = false;
    Refresh();
    e.Skip();
}

void RoundedButton::OnMouseDown(wxMouseEvent& e)
{
    if (!IsEnabled()) { e.Skip(); return; }
    // Capture so we keep getting move/up events even if the cursor
    // strays outside the button. Required so the "release outside =
    // no click" rule works — wxEVT_LEFT_UP wouldn't fire otherwise.
    if (!HasCapture()) CaptureMouse();
    m_pressed = true;
    if (CanAcceptFocus()) SetFocus();
    Refresh();
    e.Skip();
}

void RoundedButton::OnMouseUp(wxMouseEvent& e)
{
    const bool wasPressed = m_pressed;
    m_pressed = false;
    if (HasCapture()) ReleaseMouse();

    // Click is a press + release-inside-bounds pair. Releasing outside
    // discards the click, matching the standard button drag-cancel
    // gesture (press, drag away, release = no action).
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

void RoundedButton::OnMouseCaptureLost(wxMouseCaptureLostEvent&)
{
    // Capture can be ripped away by Alt+Tab, system modals, etc.
    // Clear pressed state silently — no click should fire from this
    // path. wxWidgets asserts in debug if we don't handle this event.
    m_pressed = false;
    Refresh();
}

// ---------------------------------------------------------------------------
// Keyboard handling
// ---------------------------------------------------------------------------
void RoundedButton::OnKeyDown(wxKeyEvent& e)
{
    const int code = e.GetKeyCode();
    // Space and Enter both fire the button — same behaviour as the
    // native wxButton on Windows. Other keys propagate (so e.g. Tab
    // continues to navigate focus).
    if ((code == WXK_SPACE || code == WXK_RETURN || code == WXK_NUMPAD_ENTER)
        && IsEnabled())
    {
        EmitClick();
        return;
    }
    e.Skip();
}

void RoundedButton::OnFocusChange(wxFocusEvent& e)
{
    Refresh();
    e.Skip();
}

// ---------------------------------------------------------------------------
// Click emission
// ---------------------------------------------------------------------------
void RoundedButton::EmitClick()
{
    // Hand-rolled event dispatch so the existing Bind(wxEVT_BUTTON, ...)
    // wiring across the app keeps working unchanged. ProcessEvent (not
    // QueueEvent) so the handler runs synchronously — wxButton's
    // native behaviour is also synchronous, and several call sites in
    // FixtureEditor / dialogs assume that ordering.
    wxCommandEvent evt(wxEVT_BUTTON, GetId());
    evt.SetEventObject(this);
    GetEventHandler()->ProcessEvent(evt);
}
