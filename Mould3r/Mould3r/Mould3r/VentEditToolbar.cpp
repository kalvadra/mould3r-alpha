#include "VentEditToolbar.h"
#include "style.h"

#include <wx/dcbuffer.h>
#include <wx/graphics.h>

// Fixed overlay size. The canvas pins it top-centre; cells are laid out within.
static constexpr int kBarW = 524;
static constexpr int kBarH = 90;
static constexpr int kPad = 12;

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

    LayoutCells();
    Hide();   // owner shows us only while a vent is being edited
}

void VentEditToolbar::Configure(bool hasSelection, bool complex, bool smooth,
    int nodeCount, PathEditTool tool)
{
    m_hasSelection = hasSelection;
    m_complex = complex;
    m_smooth = smooth;
    m_nodeCount = nodeCount;
    m_tool = tool;
    Refresh(false);
}

void VentEditToolbar::SetLabels(const wxString& title, const wxString& emptyStatus)
{
    m_title = title;
    m_emptyStatus = emptyStatus;
    // No Refresh here — the owner calls Configure() right after, which repaints.
}

void VentEditToolbar::LayoutCells()
{
    const int y = 38;
    const int bh = 38;
    int x = kPad;

    m_rMove = wxRect(x, y, 70, bh);   x += 72;
    m_rAdd = wxRect(x, y, 82, bh);   x += 84;
    m_rRemove = wxRect(x, y, 100, bh);  x += 100;

    // Smooth checkbox cell after a small gap.
    x += 14;
    m_rSmooth = wxRect(x, y, 96, bh);

    // Toggle button right-aligned.
    const int togW = 116;
    m_rToggle = wxRect(kBarW - kPad - togW, y, togW, bh);
}

VentEditToolbar::Cell VentEditToolbar::HitTest(const wxPoint& p) const
{
    if (m_rMove.Contains(p))   return CELL_MOVE;
    if (m_rAdd.Contains(p))    return CELL_ADD;
    if (m_rRemove.Contains(p)) return CELL_REMOVE;
    if (m_rSmooth.Contains(p)) return CELL_SMOOTH;
    if (m_rToggle.Contains(p)) return CELL_TOGGLE;
    return CELL_NONE;
}

bool VentEditToolbar::CellEnabled(Cell c) const
{
    switch (c)
    {
    case CELL_MOVE:   return true;   // select / drag tool — usable with no selection
    case CELL_ADD:    return true;   // snaps onto any existing path
    case CELL_REMOVE: return m_hasSelection && m_complex;
    case CELL_SMOOTH: return m_hasSelection && m_complex;
    case CELL_TOGGLE: return m_hasSelection;
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
    case CELL_SMOOTH: if (m_onSmooth) m_onSmooth(!m_smooth);            break;
    case CELL_TOGGLE: if (m_onToggleComplex) m_onToggleComplex(!m_complex); break;
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
    gc->SetBrush(wxBrush(Style::CardBg));
    gc->SetPen(wxPen(Style::Divider, 1));
    gc->DrawRoundedRectangle(0.5, 0.5, sz.x - 1.0, sz.y - 1.0, 8.0);

    // ---- Header row --------------------------------------------------------
    wxFont titleFont = GetFont().Bold();
    titleFont.SetPointSize(GetFont().GetPointSize());
    gc->SetFont(titleFont, Style::TextSubtle);
    gc->DrawText(m_title, kPad, 11);

    wxString status;
    if (!m_hasSelection)
        status = m_emptyStatus;
    else if (m_complex)
    {
        status = wxString::Format(wxT("Complex \u00B7 %d node"), m_nodeCount);
        if (m_nodeCount != 1) status += wxT("s");
    }
    else
    {
        status = wxT("Simple path");
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

    // ---- Toggle (Make Complex / Make Simple) -------------------------------
    {
        const bool en = CellEnabled(CELL_TOGGLE);
        const wxString label = m_complex ? "Make Simple" : "Make Complex";
        const bool hovered = m_hover == CELL_TOGGLE;
        wxColour bg;
        if (!en)           bg = Mix(Style::CardBg, Style::BtnPlace, 0.30);
        else if (hovered)  bg = Mix(Style::BtnPlace, *wxWHITE, 0.12);
        else               bg = Style::BtnPlace;
        gc->SetBrush(wxBrush(bg));
        gc->SetPen(wxPen(Style::Divider, 1));
        gc->DrawRoundedRectangle(m_rToggle.x, m_rToggle.y,
            m_rToggle.width, m_rToggle.height, 5.0);

        wxColour fg = en ? Style::TextPrimary : Mix(Style::TextMuted, Style::CardBg, 0.3);
        gc->SetFont(GetFont(), fg);
        double lw, lh;
        gc->GetTextExtent(label, &lw, &lh);
        gc->DrawText(label, m_rToggle.x + (m_rToggle.width - lw) / 2.0,
            m_rToggle.y + (m_rToggle.height - lh) / 2.0);
    }

    delete gc;
}
