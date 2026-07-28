#include "VentEditToolbar.h"
#include "style.h"

#include <wx/dcbuffer.h>
#include <wx/graphics.h>

#ifdef __WXMSW__
#include <wx/msw/wrapwin.h>   // windows.h with the wx-safe macro guards
#endif

// Fixed overlay size. The canvas pins it top-centre; cells are laid out within.
static constexpr int kBarH = 90;
static constexpr int kPad = 12;

// The bar width is DERIVED from the cell run rather than hand-tuned, so the
// two can no longer drift apart. Left to right:
//   pad | Move 70 +2 | Add 82 +2 | Remove 100 +2 | Place 76 | gap | Smooth | pad
// Dropping the Simple/Complex toggle left Smooth stranded against the right
// edge with a wide gap in the middle; it now sits directly after the node
// tools and the bar shrinks to suit (602 -> 456).
static constexpr int kSmoothW = 84;
static constexpr int kSmoothGap = 14;   // breathing room after Place...
static constexpr int kBarW =
    kPad + 72 + 84 + 102 + 76 + kSmoothGap + kSmoothW + kPad;   // = 456

// Corner radius. Used for BOTH the painted card outline and the window shape,
// so the clip and the border coincide.
static constexpr double kCorner = 8.0;

namespace {

    // Blend two colours (t in [0,1]).
    wxColour Mix(const wxColour& a, const wxColour& b, double t)
    {
        return wxColour(
            (unsigned char)(a.Red() + (b.Red() - a.Red()) * t),
            (unsigned char)(a.Green() + (b.Green() - a.Green()) * t),
            (unsigned char)(a.Blue() + (b.Blue() - a.Blue()) * t));
    }

} // namespace

VentEditToolbar::VentEditToolbar(wxWindow* parent)
    : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxSize(kBarW, kBarH),
        wxBORDER_NONE)
{
    SetBackgroundStyle(wxBG_STYLE_PAINT);   // we paint everything ourselves
    SetBackgroundColour(Style::CardBg);
    SetMinSize(wxSize(kBarW, kBarH));

    Bind(wxEVT_PAINT, &VentEditToolbar::OnPaint, this);
    Bind(wxEVT_LEFT_DOWN, &VentEditToolbar::OnLeftDown, this);
    Bind(wxEVT_MOTION, &VentEditToolbar::OnMotion, this);
    Bind(wxEVT_LEAVE_WINDOW, &VentEditToolbar::OnLeave, this);
    Bind(wxEVT_SIZE, &VentEditToolbar::OnSize, this);

    LayoutCells();
    ApplyRoundedShape();
    Hide();   // owner shows us only while a path is being edited
}

// ---------------------------------------------------------------------------
// ApplyRoundedShape - clip the window itself to the rounded card outline.
//
// Without this the panel is a plain rectangle: OnPaint clears the whole client
// area to CardBg and then strokes a rounded border on top, so the four corners
// keep their square block of card colour outside the arc. Clipping the HWND
// cuts those corners away entirely, letting the viewport show through.
//
// NOTE: wxWindow has no SetShape - that lives on wxNonOwnedWindow (top-level
// windows only), and this bar is a child panel. So we go native: a round-rect
// HRGN handed to SetWindowRgn, which works on child HWNDs. SetWindowRgn takes
// ownership of the region, so it must NOT be deleted here.
//
// The clip is a hard 1-bit region (no antialiasing), so the extreme outer edge
// of each arc is crisp rather than feathered; the stroked border inside it
// still antialiases normally, which is what the eye reads.
//
// Re-applied on size changes; harmless while the bar stays fixed-size.
// ---------------------------------------------------------------------------
void VentEditToolbar::ApplyRoundedShape()
{
#ifdef __WXMSW__
    HWND hwnd = (HWND)GetHWND();
    if (!hwnd) return;

    const wxSize sz = GetSize();
    if (sz.x <= 0 || sz.y <= 0) return;

    // CreateRoundRectRgn treats the right/bottom bounds as exclusive, so pass
    // one past the client extent - otherwise the region shaves the last row and
    // column, eating the 1px card border along the right and bottom edges. Any
    // overshoot beyond the window is clipped by the window itself.
    const int d = (int)(kCorner * 2.0);   // ellipse axes = 2 * corner radius
    HRGN rgn = ::CreateRoundRectRgn(0, 0, sz.x + 1, sz.y + 1, d, d);
    if (!rgn) return;

    // Ownership transfers to the system on success; on failure we must free it.
    if (!::SetWindowRgn(hwnd, rgn, TRUE))
        ::DeleteObject(rgn);
#endif
}

void VentEditToolbar::OnSize(wxSizeEvent& evt)
{
    ApplyRoundedShape();
    Refresh(false);
    evt.Skip();
}

void VentEditToolbar::Configure(bool hasSelection, bool complex, bool smooth,
    int nodeCount, PathEditTool tool, bool canPlaceNode)
{
    m_hasSelection = hasSelection;
    m_complex = complex;
    m_smooth = smooth;
    m_nodeCount = nodeCount;
    m_tool = tool;
    m_canPlaceNode = canPlaceNode;
    Refresh(false);
}

void VentEditToolbar::SetLabels(const wxString& title, const wxString& emptyStatus)
{
    m_title = title;
    m_emptyStatus = emptyStatus;
    // No Refresh here - the owner calls Configure() right after, which repaints.
}

void VentEditToolbar::LayoutCells()
{
    const int y = 38;
    const int bh = 38;
    int x = kPad;

    m_rMove = wxRect(x, y, 70, bh);   x += 72;
    m_rAdd = wxRect(x, y, 82, bh);   x += 84;
    m_rRemove = wxRect(x, y, 100, bh);  x += 102;
    // Place... groups with the node tools - it acts on the selected node.
    m_rPlace = wxRect(x, y, 76, bh);   x += 76;

    // Smooth follows the node tools directly. kBarW is derived from this
    // same run, so the cell lands flush against the right-hand padding with
    // no leftover space.
    x += kSmoothGap;
    m_rSmooth = wxRect(x, y, kSmoothW, bh);
}

VentEditToolbar::Cell VentEditToolbar::HitTest(const wxPoint& p) const
{
    if (m_rMove.Contains(p))   return CELL_MOVE;
    if (m_rAdd.Contains(p))    return CELL_ADD;
    if (m_rRemove.Contains(p)) return CELL_REMOVE;
    if (m_rPlace.Contains(p))  return CELL_PLACE;
    if (m_rSmooth.Contains(p)) return CELL_SMOOTH;
    return CELL_NONE;
}

bool VentEditToolbar::CellEnabled(Cell c) const
{
    switch (c)
    {
    case CELL_MOVE:   return true;   // select / drag tool - usable with no selection
    case CELL_ADD:    return true;   // snaps onto any existing path
    // Only interior nodes can be removed: node[0] (origin / feed point) and
    // the terminus are protected in every feature's Remove handler. So there
    // is nothing to remove until the path carries at least three nodes.
    case CELL_REMOVE: return m_hasSelection && m_complex && m_nodeCount >= 3;
    case CELL_PLACE:  return m_hasSelection && m_complex && m_canPlaceNode;
    case CELL_SMOOTH: return m_hasSelection && m_complex;
    default:          return false;
    }
}

void VentEditToolbar::OnLeftDown(wxMouseEvent& evt)
{
    const Cell c = HitTest(evt.GetPosition());
    if (c == CELL_NONE || !CellEnabled(c)) return;

    switch (c)
    {
    case CELL_MOVE:   if (m_onTool) m_onTool(PathEditTool::Move);       break;
    case CELL_ADD:    if (m_onTool) m_onTool(PathEditTool::AddNode);    break;
    case CELL_REMOVE: if (m_onTool) m_onTool(PathEditTool::RemoveNode); break;
    case CELL_PLACE:  if (m_onPlaceNode) m_onPlaceNode();               break;
    case CELL_SMOOTH: if (m_onSmooth) m_onSmooth(!m_smooth);            break;
    default: break;
    }
    // The owner pushes fresh state back via Configure(); no local mutation here.
}

void VentEditToolbar::OnMotion(wxMouseEvent& evt)
{
    const Cell c = HitTest(evt.GetPosition());
    if (c != m_hover) { m_hover = c; Refresh(false); }
}

void VentEditToolbar::OnLeave(wxMouseEvent&)
{
    if (m_hover != CELL_NONE) { m_hover = CELL_NONE; Refresh(false); }
}

void VentEditToolbar::OnPaint(wxPaintEvent&)
{
    wxAutoBufferedPaintDC dc(this);
    dc.SetBackground(wxBrush(Style::CardBg));
    dc.Clear();

    wxGraphicsContext* gc = wxGraphicsContext::Create(dc);
    if (!gc) return;
    gc->SetAntialiasMode(wxANTIALIAS_DEFAULT);

    const wxSize sz = GetClientSize();

    // ---- Card background ---------------------------------------------------
    // Matches the window shape set in ApplyRoundedShape, so the corners the
    // clip cuts away are exactly the corners this outline curves around.
    gc->SetBrush(wxBrush(Style::CardBg));
    gc->SetPen(wxPen(Style::Divider, 1));
    gc->DrawRoundedRectangle(0.5, 0.5, sz.x - 1.0, sz.y - 1.0, kCorner);

    // ---- Header row --------------------------------------------------------
    wxFont titleFont = GetFont().Bold();
    titleFont.SetPointSize(GetFont().GetPointSize());
    gc->SetFont(titleFont, Style::TextSubtle);
    gc->DrawText(m_title, kPad, 11);

    // Status now reports node count only - Simple vs Complex is an internal
    // distinction the user drives by adding nodes, not something to label. A
    // Simple path reads as its implicit two nodes.
    wxString status;
    if (!m_hasSelection)
    {
        status = m_emptyStatus;
    }
    else
    {
        const int n = DisplayNodeCount();
        status = wxString::Format(wxT("%d node"), n);
        if (n != 1) status += wxT("s");
    }
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

    const bool moveOn = m_tool == PathEditTool::Move;
    const bool addOn = m_tool == PathEditTool::AddNode;
    const bool remOn = m_tool == PathEditTool::RemoveNode && CellEnabled(CELL_REMOVE);

    drawButton(m_rMove, "Move", CellEnabled(CELL_MOVE), moveOn,
        m_hover == CELL_MOVE, Style::BtnActive);
    drawButton(m_rAdd, "Add Node", CellEnabled(CELL_ADD), addOn,
        m_hover == CELL_ADD, Style::BtnActive);
    drawButton(m_rRemove, "Remove Node", CellEnabled(CELL_REMOVE), remOn,
        m_hover == CELL_REMOVE, Style::BtnActive);
    // Place... is a momentary action (opens a dialog), so it never reads
    // "active" the way the tool cells do.
    drawButton(m_rPlace, "Place...", CellEnabled(CELL_PLACE), false,
        m_hover == CELL_PLACE, Style::BtnActive);

    // ---- Smooth checkbox cell ---------------------------------------------
    {
        const bool en = CellEnabled(CELL_SMOOTH);
        const wxRect& r = m_rSmooth;
        const int box = 16;
        const int by = r.y + (r.height - box) / 2;

        wxColour boxBg = m_smooth && en ? Style::BtnActive
            : Mix(Style::InputBg, Style::CardBg, en ? 0.0 : 0.4);
        gc->SetBrush(wxBrush(boxBg));
        gc->SetPen(wxPen(en ? Style::Accent : Style::Divider, 1));
        gc->DrawRoundedRectangle(r.x, by, box, box, 3.0);

        if (m_smooth)
        {
            // simple check mark
            wxGraphicsPath path = gc->CreatePath();
            path.MoveToPoint(r.x + 3.5, by + 8.5);
            path.AddLineToPoint(r.x + 7.0, by + 12.0);
            path.AddLineToPoint(r.x + 12.5, by + 4.5);
            gc->SetPen(wxPen(en ? *wxWHITE : Style::TextMuted, 2));
            gc->StrokePath(path);
        }

        wxColour fg = en ? Style::TextPrimary : Mix(Style::TextMuted, Style::CardBg, 0.3);
        gc->SetFont(GetFont(), fg);
        double lw, lh;
        gc->GetTextExtent("Smooth", &lw, &lh);
        gc->DrawText("Smooth", r.x + box + 6, r.y + (r.height - lh) / 2.0);
    }

    delete gc;
}
