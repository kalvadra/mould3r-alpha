#include "SprueEditToolbar.h"
#include "style.h"

#include <wx/dcbuffer.h>
#include <wx/graphics.h>

#ifdef __WXMSW__
#include <wx/msw/wrapwin.h>   // windows.h with the wx-safe macro guards
#endif

// Fixed overlay size. The canvas pins it top-centre; cells are laid out within.
static constexpr int kBarH = 90;
static constexpr int kPad = 12;

// Two cells laid out left to right, bar width derived from the run so the two
// can't drift apart:
//   pad | Move 70 +8 | Select Injection Point 168 | pad
static constexpr int kMoveW = 70;
static constexpr int kMoveGap = 8;
static constexpr int kSelectW = 168;
static constexpr int kBarW = kPad + kMoveW + kMoveGap + kSelectW + kPad;

// Corner radius shared by the painted card outline and the window shape, so the
// clip and the border coincide.
static constexpr double kCorner = 8.0;

namespace {
    wxColour Mix(const wxColour& a, const wxColour& b, double t)
    {
        return wxColour(
            (unsigned char)(a.Red() + (b.Red() - a.Red()) * t),
            (unsigned char)(a.Green() + (b.Green() - a.Green()) * t),
            (unsigned char)(a.Blue() + (b.Blue() - a.Blue()) * t));
    }
} // namespace

SprueEditToolbar::SprueEditToolbar(wxWindow* parent)
    : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxSize(kBarW, kBarH),
        wxBORDER_NONE)
{
    SetBackgroundStyle(wxBG_STYLE_PAINT);   // we paint everything ourselves
    SetBackgroundColour(Style::CardBg);
    SetMinSize(wxSize(kBarW, kBarH));

    Bind(wxEVT_PAINT, &SprueEditToolbar::OnPaint, this);
    Bind(wxEVT_LEFT_DOWN, &SprueEditToolbar::OnLeftDown, this);
    Bind(wxEVT_MOTION, &SprueEditToolbar::OnMotion, this);
    Bind(wxEVT_LEAVE_WINDOW, &SprueEditToolbar::OnLeave, this);
    Bind(wxEVT_SIZE, &SprueEditToolbar::OnSize, this);

    LayoutCells();
    ApplyRoundedShape();
    Hide();   // owner shows us only while the sprue is being edited
}

// ---------------------------------------------------------------------------
// ApplyRoundedShape - clip the window itself to the rounded card outline. See
// VentEditToolbar::ApplyRoundedShape for the full rationale; this is the same
// native round-rect HRGN technique, needed because a child panel can't use
// wxWindow::SetShape.
// ---------------------------------------------------------------------------
void SprueEditToolbar::ApplyRoundedShape()
{
#ifdef __WXMSW__
    HWND hwnd = (HWND)GetHWND();
    if (!hwnd) return;

    const wxSize sz = GetSize();
    if (sz.x <= 0 || sz.y <= 0) return;

    const int d = (int)(kCorner * 2.0);   // ellipse axes = 2 * corner radius
    HRGN rgn = ::CreateRoundRectRgn(0, 0, sz.x + 1, sz.y + 1, d, d);
    if (!rgn) return;

    if (!::SetWindowRgn(hwnd, rgn, TRUE))
        ::DeleteObject(rgn);
#endif
}

void SprueEditToolbar::OnSize(wxSizeEvent& evt)
{
    ApplyRoundedShape();
    Refresh(false);
    evt.Skip();
}

void SprueEditToolbar::Configure(bool hasRadialSprue, bool canSelectIp,
    SprueEditTool tool)
{
    m_hasRadialSprue = hasRadialSprue;
    m_canSelectIp = canSelectIp;
    m_tool = tool;
    Refresh(false);
}

void SprueEditToolbar::LayoutCells()
{
    const int y = 38;
    const int bh = 38;
    int x = kPad;

    m_rMove = wxRect(x, y, kMoveW, bh);   x += kMoveW + kMoveGap;
    m_rSelectIp = wxRect(x, y, kSelectW, bh);
}

SprueEditToolbar::Cell SprueEditToolbar::HitTest(const wxPoint& p) const
{
    if (m_rMove.Contains(p))     return CELL_MOVE;
    if (m_rSelectIp.Contains(p)) return CELL_SELECT_IP;
    return CELL_NONE;
}

bool SprueEditToolbar::CellEnabled(Cell c) const
{
    switch (c)
    {
    case CELL_MOVE:      return m_hasRadialSprue;  // radial only (axial locked)
    case CELL_SELECT_IP: return m_canSelectIp;
    default:             return false;
    }
}

void SprueEditToolbar::OnLeftDown(wxMouseEvent& evt)
{
    const Cell c = HitTest(evt.GetPosition());
    if (c == CELL_NONE || !CellEnabled(c)) return;

    switch (c)
    {
    case CELL_MOVE:      if (m_onTool) m_onTool(SprueEditTool::Move); break;
    case CELL_SELECT_IP: if (m_onTool) m_onTool(SprueEditTool::SelectInjectionPoint); break;
    default: break;
    }
    // The owner pushes fresh state back via Configure(); no local mutation here.
}

void SprueEditToolbar::OnMotion(wxMouseEvent& evt)
{
    const Cell c = HitTest(evt.GetPosition());
    if (c != m_hover) { m_hover = c; Refresh(false); }
}

void SprueEditToolbar::OnLeave(wxMouseEvent&)
{
    if (m_hover != CELL_NONE) { m_hover = CELL_NONE; Refresh(false); }
}

void SprueEditToolbar::OnPaint(wxPaintEvent&)
{
    wxAutoBufferedPaintDC dc(this);
    dc.SetBackground(wxBrush(Style::CardBg));
    dc.Clear();

    wxGraphicsContext* gc = wxGraphicsContext::Create(dc);
    if (!gc) return;
    gc->SetAntialiasMode(wxANTIALIAS_DEFAULT);

    const wxSize sz = GetClientSize();

    // ---- Card background (matches the clipped window shape) ----------------
    gc->SetBrush(wxBrush(Style::CardBg));
    gc->SetPen(wxPen(Style::Divider, 1));
    gc->DrawRoundedRectangle(0.5, 0.5, sz.x - 1.0, sz.y - 1.0, kCorner);

    // ---- Header row --------------------------------------------------------
    wxFont titleFont = GetFont().Bold();
    titleFont.SetPointSize(GetFont().GetPointSize());
    gc->SetFont(titleFont, Style::TextSubtle);
    gc->DrawText("EDIT SPRUE", kPad, 11);

    // Right-hand hint: what's editable given the current state.
    wxString status;
    if (m_hasRadialSprue)      status = "Radial sprue";
    else if (m_canSelectIp)    status = "Axial sprue";
    else                       status = "No sprue";
    gc->SetFont(GetFont(), Style::TextMuted);
    double tw, th;
    gc->GetTextExtent(status, &tw, &th);
    gc->DrawText(status, sz.x - kPad - tw, 12);

    // ---- Helper: draw a labelled button cell -------------------------------
    auto drawButton = [&](const wxRect& r, const wxString& label,
        bool enabled, bool active, bool hovered, const wxColour& accent)
        {
            wxColour bg;
            if (!enabled)        bg = Mix(Style::CardBg, Style::BtnDefault, 0.35);
            else if (active)     bg = accent;
            else if (hovered)    bg = Style::BtnHover;
            else                 bg = Style::BtnDefault;

            gc->SetBrush(wxBrush(bg));
            gc->SetPen(active && enabled ? wxPen(Mix(accent, *wxWHITE, 0.25), 1)
                : wxPen(Style::Divider, 1));
            gc->DrawRoundedRectangle(r.x, r.y, r.width, r.height, 5.0);

            wxColour fg = enabled ? Style::TextPrimary
                : Mix(Style::TextMuted, Style::CardBg, 0.3);
            gc->SetFont(GetFont(), fg);
            double lw, lh;
            gc->GetTextExtent(label, &lw, &lh);
            gc->DrawText(label, r.x + (r.width - lw) / 2.0,
                r.y + (r.height - lh) / 2.0);
        };

    const bool moveOn = m_tool == SprueEditTool::Move && CellEnabled(CELL_MOVE);
    const bool selOn = m_tool == SprueEditTool::SelectInjectionPoint
        && CellEnabled(CELL_SELECT_IP);

    drawButton(m_rMove, "Move", CellEnabled(CELL_MOVE), moveOn,
        m_hover == CELL_MOVE, Style::BtnActive);
    drawButton(m_rSelectIp, "Select Injection Point", CellEnabled(CELL_SELECT_IP),
        selOn, m_hover == CELL_SELECT_IP, Style::BtnActive);

    delete gc;
}
