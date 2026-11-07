#include "CanvasToast.h"
#include "style.h"

#include <wx/dcbuffer.h>
#include <wx/dcclient.h>
#include <wx/graphics.h>

#ifdef __WXMSW__
#include <wx/msw/wrapwin.h>   // windows.h with the wx-safe macro guards
#endif

static constexpr int kToastH = 38;
static constexpr int kToastPadX = 20;


CanvasToast::CanvasToast(wxWindow* parent)
    : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxSize(200, kToastH),
        wxBORDER_NONE)
{
    SetBackgroundStyle(wxBG_STYLE_PAINT);   // we paint everything ourselves
    SetBackgroundColour(Style::CardBg);
    Bind(wxEVT_PAINT, &CanvasToast::OnPaint, this);
    Hide();   // nothing to say until a mode raises us
}

// Clip the HWND to a pill so the corners are genuinely cut away and the
// viewport shows through, rather than the card colour squaring off outside the
// drawn outline. wxWindow has no SetShape (that is wxNonOwnedWindow, top-level
// only), so this goes native — same technique as VentEditToolbar. SetWindowRgn
// takes ownership of the region on success, so it must not be deleted then.
void CanvasToast::ApplyPillShape()
{
#ifdef __WXMSW__
    HWND hwnd = (HWND)GetHWND();
    if (!hwnd) return;

    const wxSize sz = GetSize();
    if (sz.x <= 0 || sz.y <= 0) return;

    // +1 because CreateRoundRectRgn treats right/bottom as exclusive; without
    // it the region shaves the last row and column and eats the 1px border.
    HRGN rgn = ::CreateRoundRectRgn(0, 0, sz.x + 1, sz.y + 1, sz.y, sz.y);
    if (!rgn) return;

    if (!::SetWindowRgn(hwnd, rgn, TRUE))
        ::DeleteObject(rgn);
#endif
}

void CanvasToast::ShowMessage(const wxString& msg)
{
    if (IsShown() && msg == m_msg) return;
    m_msg = msg;

    // Size to the text. wxClientDC measures with this window's font, which is
    // the same one OnPaint draws with.
    int tw = 0, th = 0;
    {
        wxClientDC dc(this);
        dc.SetFont(GetFont());
        dc.GetTextExtent(m_msg, &tw, &th);
    }
    SetSize(tw + kToastPadX * 2, kToastH);
    ApplyPillShape();   // re-clip: the width just changed

    if (!IsShown()) Show();
    Raise();            // keep above the GL surface
    Refresh(false);
}

void CanvasToast::HideToast()
{
    if (IsShown()) Hide();
}

void CanvasToast::OnPaint(wxPaintEvent&)
{
    wxAutoBufferedPaintDC dc(this);
    dc.SetBackground(wxBrush(Style::CardBg));
    dc.Clear();

    wxGraphicsContext* gc = wxGraphicsContext::Create(dc);
    if (!gc) return;
    gc->SetAntialiasMode(wxANTIALIAS_DEFAULT);

    const wxSize sz = GetClientSize();
    const double radius = (sz.y - 1.0) / 2.0;   // full pill

    gc->SetBrush(wxBrush(Style::CardBg));
    gc->SetPen(wxPen(Style::Divider, 1));
    gc->DrawRoundedRectangle(0.5, 0.5, sz.x - 1.0, sz.y - 1.0, radius);

    gc->SetFont(GetFont(), Style::TextPrimary);
    double tw, th;
    gc->GetTextExtent(m_msg, &tw, &th);
    gc->DrawText(m_msg, (sz.x - tw) / 2.0, (sz.y - th) / 2.0);

    delete gc;
}
