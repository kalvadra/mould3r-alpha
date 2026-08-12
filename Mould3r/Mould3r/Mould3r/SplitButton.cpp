#include "SplitButton.h"
#include <wx/graphics.h>
#include <wx/dcbuffer.h>
#include <algorithm>
#include <memory>

namespace
{
    // Colour helpers — identical maths to RoundedButton so the two controls
    // share a look. (Kept local rather than shared to avoid a header just for
    // three one-liners; if a third owner-drawn control appears, hoist these.)
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
    wxColour Blend(const wxColour& a, const wxColour& b, float t)
    {
        const float u = 1.0f - t;
        return wxColour(
            (unsigned char)(a.Red() * u + b.Red() * t),
            (unsigned char)(a.Green() * u + b.Green() * t),
            (unsigned char)(a.Blue() * u + b.Blue() * t));
    }

    // Build a rectangle path with only its LEFT two corners rounded (top-left
    // and bottom-left); the right edge is left square so it butts flat against
    // the divider. Used to fill the dropdown zone so it shares the pill's outer
    // rounding on the left but meets the action zone with a straight seam.
    void AddLeftRoundedRect(wxGraphicsPath& p,
        double w, double h, double r)
    {
        r = std::min(r, std::min(w, h) * 0.5);
        p.MoveToPoint(w, 0);
        p.AddArcToPoint(0, 0, 0, h, r);   // top-left corner
        p.AddArcToPoint(0, h, w, h, r);   // bottom-left corner
        p.AddLineToPoint(w, h);           // flat bottom to the seam
        p.CloseSubpath();                 // flat right edge back up to start
    }
}

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------
SplitButton::SplitButton(wxWindow* parent, wxWindowID id, const wxString& label,
    const wxPoint& pos, const wxSize& size, long /*style*/)
    : wxWindow(parent, id, pos, size,
        wxBORDER_NONE | wxFULL_REPAINT_ON_RESIZE | wxWANTS_CHARS)
    , m_label(label)
{
    SetBackgroundStyle(wxBG_STYLE_PAINT);
    SetBackgroundColour(parent->GetBackgroundColour());
    SetForegroundColour(*wxBLACK);

    Bind(wxEVT_PAINT, &SplitButton::OnPaint, this);
    Bind(wxEVT_ENTER_WINDOW, &SplitButton::OnMouseEnter, this);
    Bind(wxEVT_LEAVE_WINDOW, &SplitButton::OnMouseLeave, this);
    Bind(wxEVT_MOTION, &SplitButton::OnMouseMove, this);
    Bind(wxEVT_LEFT_DOWN, &SplitButton::OnMouseDown, this);
    Bind(wxEVT_LEFT_UP, &SplitButton::OnMouseUp, this);
    Bind(wxEVT_MOUSE_CAPTURE_LOST, &SplitButton::OnMouseCaptureLost, this);
    Bind(wxEVT_KEY_DOWN, &SplitButton::OnKeyDown, this);
    Bind(wxEVT_SET_FOCUS, &SplitButton::OnFocusChange, this);
    Bind(wxEVT_KILL_FOCUS, &SplitButton::OnFocusChange, this);
}

// ---------------------------------------------------------------------------
// Public accessors
// ---------------------------------------------------------------------------
void SplitButton::SetLabel(const wxString& label)
{
    if (m_label == label) return;
    m_label = label;
    InvalidateBestSize();
    Refresh();
}

void SplitButton::SetMenuItems(const std::vector<wxString>& items)
{
    m_menuItems = items;
    m_selection = items.empty() ? -1 : std::min(m_selection < 0 ? 0 : m_selection,
        (int)items.size() - 1);
    Refresh();
}

void SplitButton::SetSelection(int index)
{
    if (m_menuItems.empty()) { m_selection = -1; return; }
    const int clamped = std::max(0, std::min(index, (int)m_menuItems.size() - 1));
    if (clamped == m_selection) return;
    m_selection = clamped;
    Refresh();
}

void SplitButton::SetCornerRadius(int r)
{
    const int clamped = std::max(0, r);
    if (m_cornerRadius == clamped) return;
    m_cornerRadius = clamped;
    Refresh();
}

wxSize SplitButton::DoGetBestClientSize() const
{
    wxClientDC dc(const_cast<SplitButton*>(this));
    dc.SetFont(GetFont());
    const wxSize textSize = m_label.IsEmpty()
        ? dc.GetTextExtent("M")
        : dc.GetTextExtent(m_label);
    // Action-zone padding (~20) + the dropdown zone on the left.
    return wxSize(textSize.x + 20 + DropZoneWidth(), textSize.y + 16);
}

// ---------------------------------------------------------------------------
// Paint
// ---------------------------------------------------------------------------
void SplitButton::OnPaint(wxPaintEvent&)
{
    wxAutoBufferedPaintDC dc(this);

    const wxColour parentBg = GetParent() ? GetParent()->GetBackgroundColour()
        : GetBackgroundColour();
    dc.SetBackground(wxBrush(parentBg));
    dc.Clear();

    std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::Create(dc));
    if (!gc) return;
    gc->SetAntialiasMode(wxANTIALIAS_DEFAULT);

    const wxSize sz = GetClientSize();
    const double dropW = std::min((double)DropZoneWidth(), sz.x * 0.5);

    const bool disabled = !IsEnabled();

    // Base colours, then per-state adjustment applied PER ZONE so hovering the
    // chevron doesn't light the whole pill and vice-versa. The dropdown zone
    // gets a standing darken so the split is legible even at rest.
    wxColour baseBg = GetBackgroundColour();
    wxColour fg     = GetForegroundColour();
    if (disabled)
    {
        baseBg = Blend(baseBg, parentBg, 0.55f);
        fg     = Blend(fg, parentBg, 0.55f);
    }

    auto zoneColour = [&](bool isDrop) -> wxColour
    {
        wxColour c = baseBg;
        if (isDrop) c = Darken(c, 16);   // standing split shade
        if (disabled) return c;

        const bool pressedHere = isDrop
            ? (m_pressedDrop && m_hoverDrop) || m_menuOpen
            : (m_pressed && !m_pressedDrop && m_hovered && !m_hoverDrop);
        const bool hoverHere = isDrop ? m_hoverDrop
            : (m_hovered && !m_hoverDrop);

        if (pressedHere) c = Darken(c, 16);
        else if (hoverHere) c = Lighten(c, 12);
        return c;
    };

    // Whole pill in the action-zone colour first (this paints the right side
    // and the rounded right corners) …
    gc->SetPen(*wxTRANSPARENT_PEN);
    gc->SetBrush(wxBrush(zoneColour(false)));
    gc->DrawRoundedRectangle(0, 0, sz.x, sz.y, m_cornerRadius);

    // … then the left dropdown zone on top, left-rounded so it keeps the pill's
    // outer corners but meets the action zone flat at the seam.
    {
        wxGraphicsPath p = gc->CreatePath();
        AddLeftRoundedRect(p, dropW, sz.y, m_cornerRadius);
        gc->SetBrush(wxBrush(zoneColour(true)));
        gc->DrawPath(p);
    }

    // Seam divider — a faint line a shade darker than the pill.
    gc->SetPen(wxPen(Darken(baseBg, 28), 1));
    gc->StrokeLine(dropW, 4, dropW, sz.y - 4);

    // Focus ring (keyboard focus only), around the whole pill.
    if (HasFocus() && IsEnabled())
    {
        gc->SetPen(wxPen(Lighten(baseBg, 35), 1));
        gc->SetBrush(*wxTRANSPARENT_BRUSH);
        gc->DrawRoundedRectangle(0.5, 0.5, sz.x - 1.0, sz.y - 1.0,
            std::max(0, m_cornerRadius - 1));
    }

    // Chevron — a downward "v" centred in the dropdown zone.
    {
        const double cx = dropW * 0.5;
        const double cy = sz.y * 0.5;
        const double half = 4.0;     // half-width of the chevron
        const double drop = 3.0;     // vertical drop from the arms to the tip
        wxGraphicsPath ch = gc->CreatePath();
        ch.MoveToPoint(cx - half, cy - drop * 0.5);
        ch.AddLineToPoint(cx, cy + drop * 0.5);
        ch.AddLineToPoint(cx + half, cy - drop * 0.5);
        gc->SetPen(wxPen(fg, 1.6));
        gc->SetBrush(*wxTRANSPARENT_BRUSH);
        gc->StrokePath(ch);
    }

    // Action label — centred in the action zone (right of the seam).
    if (!m_label.IsEmpty())
    {
        gc->SetFont(GetFont(), fg);
        wxDouble tw = 0, th = 0, td = 0, te = 0;
        gc->GetTextExtent(m_label, &tw, &th, &td, &te);
        const double zoneX = dropW;
        const double zoneW = sz.x - dropW;
        gc->DrawText(m_label,
            zoneX + (zoneW - tw) / 2.0,
            (sz.y - th) / 2.0);
    }
}

// ---------------------------------------------------------------------------
// Mouse handling
// ---------------------------------------------------------------------------
void SplitButton::OnMouseEnter(wxMouseEvent& e)
{
    m_hovered = true;
    m_hoverDrop = InDropZone(e.GetPosition().x);
    Refresh();
    e.Skip();
}

void SplitButton::OnMouseLeave(wxMouseEvent& e)
{
    m_hovered = false;
    m_hoverDrop = false;
    Refresh();
    e.Skip();
}

void SplitButton::OnMouseMove(wxMouseEvent& e)
{
    const bool nowDrop = InDropZone(e.GetPosition().x);
    if (nowDrop != m_hoverDrop || !m_hovered)
    {
        m_hovered = true;
        m_hoverDrop = nowDrop;
        Refresh();
    }
    e.Skip();
}

void SplitButton::OnMouseDown(wxMouseEvent& e)
{
    if (!IsEnabled()) { e.Skip(); return; }
    if (!HasCapture()) CaptureMouse();
    m_pressed = true;
    m_pressedDrop = InDropZone(e.GetPosition().x);
    if (CanAcceptFocus()) SetFocus();
    Refresh();
    e.Skip();
}

void SplitButton::OnMouseUp(wxMouseEvent& e)
{
    const bool wasPressed = m_pressed;
    const bool wasDrop = m_pressedDrop;
    m_pressed = false;
    m_pressedDrop = false;
    if (HasCapture()) ReleaseMouse();

    if (wasPressed && IsEnabled())
    {
        const wxSize sz = GetClientSize();
        const wxPoint pt = e.GetPosition();
        const bool inside =
            pt.x >= 0 && pt.x < sz.x && pt.y >= 0 && pt.y < sz.y;
        // A click counts only if release lands in the SAME zone it started in
        // — press chevron, drift onto the label, release = no action, matching
        // the standard drag-cancel gesture but per-zone.
        if (inside && InDropZone(pt.x) == wasDrop)
        {
            if (wasDrop) ShowMenu();
            else         EmitClick();
        }
    }
    Refresh();
    e.Skip();
}

void SplitButton::OnMouseCaptureLost(wxMouseCaptureLostEvent&)
{
    m_pressed = false;
    m_pressedDrop = false;
    Refresh();
}

// ---------------------------------------------------------------------------
// Keyboard handling
// ---------------------------------------------------------------------------
void SplitButton::OnKeyDown(wxKeyEvent& e)
{
    const int code = e.GetKeyCode();
    if ((code == WXK_SPACE || code == WXK_RETURN || code == WXK_NUMPAD_ENTER)
        && IsEnabled())
    {
        EmitClick();          // Space / Enter = primary action
        return;
    }
    if ((code == WXK_DOWN || code == WXK_NUMPAD_DOWN) && IsEnabled())
    {
        ShowMenu();           // Down = open the mode menu (native-combo idiom)
        return;
    }
    e.Skip();
}

void SplitButton::OnFocusChange(wxFocusEvent& e)
{
    Refresh();
    e.Skip();
}

// ---------------------------------------------------------------------------
// Menu
// ---------------------------------------------------------------------------
void SplitButton::ShowMenu()
{
    if (m_menuItems.empty()) return;

    wxMenu menu;
    // Id base well clear of the app's own command ids; the returned id is
    // decoded back to an index below. Check items give the current mode a
    // visible tick in the popped menu.
    const int kBase = wxID_HIGHEST + 5000;
    for (size_t i = 0; i < m_menuItems.size(); ++i)
    {
        wxMenuItem* it = menu.AppendCheckItem(kBase + (int)i, m_menuItems[i]);
        if ((int)i == m_selection) it->Check(true);
    }

    // Hold the drop zone in its active look while the menu is up.
    m_menuOpen = true;
    Refresh();
    Update();

    // Synchronous popup: returns the chosen id, or wxID_NONE if dismissed.
    // Anchor under the dropdown zone's bottom-left. Avoids popup event-routing
    // subtleties — the result comes straight back.
    const int chosenId = GetPopupMenuSelectionFromUser(
        menu, wxPoint(0, GetClientSize().y));

    m_menuOpen = false;
    Refresh();

    if (chosenId == wxID_NONE) return;
    const int picked = chosenId - kBase;
    if (picked >= 0 && picked < (int)m_menuItems.size() && picked != m_selection)
    {
        m_selection = picked;
        Refresh();
        // Notify the owner of the mode change. wxEVT_CHOICE with the new index
        // in GetInt() — parallels a wxChoice selection so the handler side
        // reads naturally.
        wxCommandEvent evt(wxEVT_CHOICE, GetId());
        evt.SetEventObject(this);
        evt.SetInt(m_selection);
        GetEventHandler()->ProcessEvent(evt);
    }
}

// ---------------------------------------------------------------------------
// Click emission (action zone)
// ---------------------------------------------------------------------------
void SplitButton::EmitClick()
{
    wxCommandEvent evt(wxEVT_BUTTON, GetId());
    evt.SetEventObject(this);
    GetEventHandler()->ProcessEvent(evt);
}
