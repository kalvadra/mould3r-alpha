// ===========================================================================
// FlowResultsBar.cpp — see FlowResultsBar.h.
// ===========================================================================

#include "FlowResultsBar.h"
#include "RoundedButton.h"
#include "style.h"

#include <wx/dcbuffer.h>
#include <wx/graphics.h>

#include <algorithm>
#include <cmath>
#include <memory>

namespace
{
    wxFont UiFont(int pt, wxFontWeight w = wxFONTWEIGHT_NORMAL)
    {
        return wxFont(pt, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL, w, false, "Segoe UI");
    }

    // Digits after the point for labels spanning `span`.
    int AutoDecimals(float span)
    {
        span = std::fabs(span);
        if (span < 0.05f) return 4;
        if (span < 0.5f)  return 3;
        if (span < 5.0f)  return 2;
        if (span < 50.0f) return 1;
        return 0;
    }

    wxColour Mix(const wxColour& a, const wxColour& b, float t)
    {
        auto m = [t](int x, int y) { return (unsigned char)std::clamp((int)std::lround(x + (y - x) * t), 0, 255); };
        return wxColour(m(a.Red(), b.Red()), m(a.Green(), b.Green()), m(a.Blue(), b.Blue()));
    }
}

// ---------------------------------------------------------------------------
// Timeline — the scrubber: a track over 0..end time with the injection-
// pressure trace, a tick per frame, the played part highlighted and a thumb.
// Click / drag to scrub; Left / Right step a frame, Home / End jump.
// ---------------------------------------------------------------------------
class FlowResultsBar::Timeline : public wxWindow
{
public:
    Timeline(wxWindow* parent, FlowResultsBar* owner)
        : wxWindow(parent, wxID_ANY, wxDefaultPosition, parent->FromDIP(wxSize(200, 40)), wxBORDER_NONE | wxWANTS_CHARS),
          m_owner(owner)
    {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetMinSize(FromDIP(wxSize(160, 40)));
        SetCursor(wxCursor(wxCURSOR_HAND));
        SetToolTip("Drag to scrub through the fill. Left / Right step a frame; Home / End jump.");
        Bind(wxEVT_PAINT, &Timeline::OnPaint, this);
        Bind(wxEVT_SIZE, [this](wxSizeEvent& e) { Refresh(); e.Skip(); });
        Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent& e)
        {
            SetFocus();
            if (!HasCapture()) CaptureMouse();
            m_owner->Stop();
            Scrub(e.GetX());
        });
        Bind(wxEVT_MOTION, [this](wxMouseEvent& e) { if (HasCapture() && e.LeftIsDown()) Scrub(e.GetX()); });
        Bind(wxEVT_LEFT_UP, [this](wxMouseEvent&) { if (HasCapture()) ReleaseMouse(); });
        Bind(wxEVT_MOUSE_CAPTURE_LOST, [](wxMouseCaptureLostEvent&) {});
        Bind(wxEVT_KEY_DOWN, [this](wxKeyEvent& e)
        {
            const int end = m_owner->GetEndPosition();
            int p = m_owner->GetPosition();
            switch (e.GetKeyCode())
            {
            case WXK_LEFT:  p = std::max(0, p - 1); break;
            case WXK_RIGHT: p = std::min(end, p + 1); break;
            case WXK_HOME:  p = 0; break;
            case WXK_END:   p = end; break;
            default: e.Skip(); return;
            }
            m_owner->Stop();
            m_owner->SetPosition(p, true);
        });
    }

private:
    int Pad() const { return FromDIP(8); }

    // Positions 0..N (N = the end state) are spaced evenly along the track —
    // frames are much denser in time while filling than while cooling — and
    // times map onto it piecewise-linearly through the frame times.
    int N() const { return (int)m_owner->m_times.size(); }
    double TimeAt(int i) const { return (i >= N()) ? m_owner->m_endTime : m_owner->m_times[(size_t)i]; }
    double IndexOfTime(double tm) const
    {
        const auto& t = m_owner->m_times;
        const int n = N();
        if (n == 0 || tm <= t[0]) return 0.0;
        const int i = (int)(std::upper_bound(t.begin(), t.end(), (float)tm) - t.begin()) - 1;   // t[i] <= tm
        const double a = TimeAt(i), b = TimeAt(i + 1);
        if (i >= n) return n;
        return (b > a) ? i + std::clamp((tm - a) / (b - a), 0.0, 1.0) : (double)(i + 1);
    }

    void Scrub(int x)
    {
        const int n = N();
        if (n == 0) return;
        const int w = GetClientSize().x - 2 * Pad();
        const double f = std::clamp((double)(x - Pad()) / std::max(1, w), 0.0, 1.0);
        m_owner->SetPosition((int)std::lround(f * n), true);
    }

    void OnPaint(wxPaintEvent&)
    {
        wxAutoBufferedPaintDC dc(this);
        dc.SetBackground(wxBrush(Style::CardBg));
        dc.Clear();
        std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::CreateFromUnknownDC(dc));
        if (!gc) return;

        const wxSize sz = GetClientSize();
        const double pad = Pad(), x0 = pad, x1 = sz.x - pad;
        const double yTop = FromDIP(4), yBot = sz.y - FromDIP(6);
        const double w = std::max(1.0, x1 - x0), h = yBot - yTop;
        const int n = std::max(1, N());
        auto XP = [&](double idx) { return x0 + w * std::clamp(idx / n, 0.0, 1.0); };
        auto X = [&](double tm) { return XP(IndexOfTime(tm)); };

        // Track.
        gc->SetPen(*wxTRANSPARENT_PEN);
        gc->SetBrush(wxBrush(Style::InputBg));
        gc->DrawRoundedRectangle(x0 - FromDIP(4), yTop, w + FromDIP(8), h, FromDIP(4));

        // Phase bands (packing / cooling) with their names.
        const auto& ph = m_owner->m_phases;
        if (!ph.empty())
        {
            static const wxColour tint[3] = { wxColour(0, 0, 0, 0), wxColour(0xE0, 0x9B, 0x20, 40), wxColour(0x40, 0xC0, 0xE0, 34) };
            static const char* names[3] = { "fill", "pack", "cool" };
            gc->SetFont(UiFont(7), Style::TextMuted);
            size_t i = 0;
            while (i < ph.size())
            {
                size_t j = i;
                while (j + 1 < ph.size() && ph[j + 1] == ph[i]) ++j;
                const int k = std::clamp(ph[i], 0, 2);
                const double xa = XP((double)i), xb = XP((double)(j + 1));
                if (k > 0)
                {
                    gc->SetPen(*wxTRANSPARENT_PEN);
                    gc->SetBrush(wxBrush(tint[k]));
                    gc->DrawRectangle(xa, yTop, xb - xa, h);
                }
                if (xb - xa > FromDIP(34) && ph.size() > 1 && (ph.front() != ph.back() || k > 0))
                    gc->DrawText(names[k], xa + FromDIP(3), yTop + FromDIP(1));
                i = j + 1;
            }
        }

        // Played part.
        const double xNow = XP((double)m_owner->m_pos);
        gc->SetBrush(wxBrush(wxColour(Style::BtnPlace.Red(), Style::BtnPlace.Green(), Style::BtnPlace.Blue(), 70)));
        gc->DrawRoundedRectangle(x0 - FromDIP(4), yTop, (xNow - x0) + FromDIP(4), h, FromDIP(4));

        // Injection-pressure trace.
        const auto& ht = m_owner->m_histT;
        const auto& hp = m_owner->m_histP;
        float pMax = 0.0f;
        for (float v : hp) pMax = std::max(pMax, v);
        if (ht.size() >= 2 && pMax > 0.0f)
        {
            wxGraphicsPath path = gc->CreatePath();
            bool first = true;
            for (size_t i = 0; i < ht.size() && i < hp.size(); ++i)
            {
                const double px = X(ht[i]);
                const double py = yBot - FromDIP(3) - (h - FromDIP(6)) * (hp[i] / pMax);
                if (first) { path.MoveToPoint(px, py); first = false; }
                else path.AddLineToPoint(px, py);
            }
            gc->SetPen(wxPen(wxColour(0xC3, 0xD1, 0xED, 215), FromDIP(1)));
            gc->SetBrush(*wxTRANSPARENT_BRUSH);
            gc->StrokePath(path);
        }

        // Frame ticks.
        gc->SetPen(wxPen(wxColour(0xB0, 0xB8, 0xC8, 90), 1));
        for (int i = 0; i <= N(); ++i)
        {
            const double px = std::floor(XP((double)i)) + 0.5;
            gc->StrokeLine(px, yBot - FromDIP(3), px, yBot);
        }

        // Thumb.
        const double tx = xNow;
        gc->SetPen(wxPen(*wxWHITE, FromDIP(2)));
        gc->StrokeLine(tx, yTop + 1, tx, yBot - 1);
        gc->SetPen(wxPen(Style::BtnSecondarySelectedBorder, 1));
        gc->SetBrush(wxBrush(*wxWHITE));
        const double r = FromDIP(5);
        gc->DrawEllipse(tx - r, yTop + h * 0.5 - r, 2 * r, 2 * r);

        if (HasFocus())
        {
            gc->SetPen(wxPen(Mix(Style::InputBg, *wxWHITE, 0.35f), 1));
            gc->SetBrush(*wxTRANSPARENT_BRUSH);
            gc->DrawRoundedRectangle(x0 - FromDIP(4) + 0.5, yTop + 0.5, w + FromDIP(8) - 1, h - 1, FromDIP(4));
        }
    }

    FlowResultsBar* m_owner;
};

// ---------------------------------------------------------------------------
// LegendView — the colour key: title (unit), the banded ramp exactly as the
// model draws it, five value labels, and swatches for the extra colours.
// ---------------------------------------------------------------------------
class FlowResultsBar::LegendView : public wxWindow
{
public:
    explicit LegendView(wxWindow* parent)
        : wxWindow(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE)
    {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetMinSize(FromDIP(wxSize(280, 44)));
        Bind(wxEVT_PAINT, &LegendView::OnPaint, this);
        Bind(wxEVT_SIZE, [this](wxSizeEvent& e) { Refresh(); e.Skip(); });
    }

    void Set(const Legend& l) { m_l = l; Refresh(); }

private:
    void OnPaint(wxPaintEvent&)
    {
        wxAutoBufferedPaintDC dc(this);
        dc.SetBackground(wxBrush(Style::CardBg));
        dc.Clear();
        if (!m_l.valid) return;

        const wxSize sz = GetClientSize();
        const int pad = FromDIP(4);

        if (!m_l.showRamp)
        {
            // Key only: title, then the swatches in rows left to right.
            dc.SetFont(UiFont(8, wxFONTWEIGHT_BOLD));
            dc.SetTextForeground(Style::TextPrimary);
            dc.DrawText(m_l.title, pad, 0);
            const int rowH = FromDIP(15);
            int x = pad, y = dc.GetTextExtent("Ag").y + FromDIP(3);
            dc.SetFont(UiFont(8));
            for (const Swatch& sw : m_l.swatches)
            {
                const int w = dc.GetTextExtent(sw.label).x + FromDIP(30);
                if (x > pad && x + w > sz.x) { x = pad; y += rowH; }
                dc.SetPen(wxPen(Style::Divider, 1));
                dc.SetBrush(wxBrush(sw.colour));
                dc.DrawRectangle(x, y + FromDIP(1), FromDIP(11), FromDIP(11));
                dc.SetTextForeground(Style::TextMuted);
                dc.DrawText(sw.label, x + FromDIP(16), y);
                x += w;
            }
            return;
        }

        // Swatches take the right-hand end, one row each (up to two rows).
        dc.SetFont(UiFont(8));
        int swW = 0;
        for (const Swatch& s : m_l.swatches)
            swW = std::max(swW, dc.GetTextExtent(s.label).x + FromDIP(18));
        const int swX = sz.x - swW;

        // Title.
        dc.SetFont(UiFont(8, wxFONTWEIGHT_BOLD));
        dc.SetTextForeground(Style::TextPrimary);
        wxString title = m_l.title;
        if (!m_l.unit.IsEmpty()) title << " (" << m_l.unit << ")";
        dc.DrawText(title, pad, 0);
        const int titleH = dc.GetTextExtent("Ag").y;

        // Banded ramp.
        const int barX0 = pad, barX1 = std::max(barX0 + FromDIP(60), swX - FromDIP(14));
        const int barY = titleH + FromDIP(2), barH = FromDIP(10);
        const double bw = (double)(barX1 - barX0) / Heatmap::kBands;
        dc.SetPen(*wxTRANSPARENT_PEN);
        for (int b = 0; b < Heatmap::kBands; ++b)
        {
            float r, g, bl;
            Heatmap::Band(b, r, g, bl);
            dc.SetBrush(wxBrush(Heatmap::ToColour(r, g, bl)));
            const int xa = barX0 + (int)std::lround(b * bw), xb = barX0 + (int)std::lround((b + 1) * bw);
            dc.DrawRectangle(xa, barY, xb - xa, barH);
        }

        // Value labels at 0, 1/4, 1/2, 3/4, 1 of the range, with small ticks.
        dc.SetFont(UiFont(8));
        dc.SetTextForeground(Style::TextMuted);
        dc.SetPen(wxPen(Style::TextMuted, 1));
        const int dec = (m_l.decimals >= 0) ? m_l.decimals : AutoDecimals(m_l.hi - m_l.lo);
        const int ty = barY + barH + FromDIP(1);
        for (int k = 0; k <= 4; ++k)
        {
            const float v = m_l.lo + (m_l.hi - m_l.lo) * (float)k / 4.0f;
            const wxString s = wxString::Format("%.*f", dec, (double)v);
            const int x = barX0 + (int)std::lround((barX1 - barX0) * k / 4.0);
            dc.DrawLine(x, barY + barH, x, ty + FromDIP(2));
            const int tw = dc.GetTextExtent(s).x;
            const int lx = std::clamp(x - tw / 2, 0, std::max(0, barX1 - tw));
            dc.DrawText(s, lx, ty + FromDIP(2));
        }

        // Swatches.
        int sy = barY - FromDIP(1);
        for (const Swatch& s : m_l.swatches)
        {
            dc.SetPen(wxPen(Style::Divider, 1));
            dc.SetBrush(wxBrush(s.colour));
            dc.DrawRectangle(swX, sy + FromDIP(1), FromDIP(11), FromDIP(11));
            dc.SetTextForeground(Style::TextMuted);
            dc.DrawText(s.label, swX + FromDIP(16), sy);
            sy += FromDIP(15);
        }
    }

    Legend m_l;
};

// ---------------------------------------------------------------------------
// FlowResultsBar
// ---------------------------------------------------------------------------
FlowResultsBar::FlowResultsBar(wxWindow* parent)
    : wxPanel(parent, wxID_ANY), m_timer(this)
{
    SetBackgroundColour(Style::CardBg);

    auto* outer = new wxBoxSizer(wxVERTICAL);
    auto* divider = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxSize(-1, 1));
    divider->SetBackgroundColour(Style::Divider);
    outer->Add(divider, 0, wxEXPAND);

    auto* row = new wxBoxSizer(wxHORIZONTAL);
    m_row = row;

    // Owner's controls (the view selector), always shown at the left.
    m_controls = new wxPanel(this, wxID_ANY);
    m_controls->SetBackgroundColour(Style::CardBg);
    m_controls->SetSizer(new wxBoxSizer(wxHORIZONTAL));
    row->Add(m_controls, 0, wxEXPAND | wxLEFT, FromDIP(12));
    m_sepC = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxSize(1, -1));
    m_sepC->SetBackgroundColour(Style::Divider);
    row->Add(m_sepC, 0, wxEXPAND | wxLEFT, FromDIP(12));

    // Timeline group (bottom row): play / pause, scrubber, readout.
    m_timeGroup = new wxPanel(this, wxID_ANY);
    m_timeGroup->SetBackgroundColour(Style::CardBg);
    auto* tg = new wxBoxSizer(wxHORIZONTAL);

    m_playBtn = new RoundedButton(m_timeGroup, wxID_ANY, "Play", wxDefaultPosition, FromDIP(wxSize(58, 26)), wxBORDER_NONE);
    m_playBtn->SetBackgroundColour(Style::BtnPlace);
    m_playBtn->SetForegroundColour(*wxWHITE);
    m_playBtn->SetFont(UiFont(9, wxFONTWEIGHT_SEMIBOLD));
    m_playBtn->SetToolTip("Play the fill from the start (or from here)");
    m_playBtn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { OnPlay(); });
    tg->Add(m_playBtn, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));

    m_timeline = new Timeline(m_timeGroup, this);
    tg->Add(m_timeline, 1, wxEXPAND | wxTOP | wxBOTTOM, FromDIP(2));

    m_readout = new wxStaticText(m_timeGroup, wxID_ANY, "", wxDefaultPosition, FromDIP(wxSize(190, -1)),
                                 wxST_NO_AUTORESIZE);
    m_readout->SetForegroundColour(Style::TextSubtle);
    m_readout->SetBackgroundColour(Style::CardBg);
    m_readout->SetFont(UiFont(8));
    tg->Add(m_readout, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(10));
    m_timeGroup->SetSizer(tg);

    // Legend, right of the controls.
    m_legend = new LegendView(this);
    row->Add(m_legend, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(12));
    m_spacer = row->AddStretchSpacer(0);

    outer->Add(row, 0, wxEXPAND | wxTOP | wxBOTTOM, FromDIP(6));

    // Timeline on its own full-width row at the very bottom (easier to see and
    // scrub), under a thin divider; shown only for views with an animation.
    m_sep = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxSize(-1, 1));
    m_sep->SetBackgroundColour(Style::Divider);
    outer->Add(m_sep, 0, wxEXPAND);
    outer->Add(m_timeGroup, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP | wxBOTTOM, FromDIP(6));
    SetSizer(outer);
    row->SetMinSize(wxSize(-1, FromDIP(44)));             // the controls row keeps its height

    Bind(wxEVT_TIMER, &FlowResultsBar::OnTimer, this);
    ClearTimeline();
    Relayout();
}

void FlowResultsBar::AddControl(wxWindow* w, int flags, int border)
{
    m_controls->GetSizer()->Add(w, 0, flags, border);
    m_controls->Layout();
    Layout();
}

// Show / hide the timeline and legend (and the separators between them) and
// give the spare width to the timeline, else the legend, else a spacer.
void FlowResultsBar::Relayout()
{
    const bool tl = !m_times.empty(), lg = m_legendValid;
    bool changed = false;
    auto show = [&](wxWindow* w, bool on) { if (w->IsShown() != on) { w->Show(on); changed = true; } };
    show(m_timeGroup, tl);
    show(m_sep, tl);
    show(m_legend, lg);
    show(m_sepC, lg);
    const int legendProp = lg ? 1 : 0, spacerProp = lg ? 0 : 1;
    if (m_row->GetItem(m_legend)->GetProportion() != legendProp) { m_row->GetItem(m_legend)->SetProportion(legendProp); changed = true; }
    if (m_spacer->GetProportion() != spacerProp) { m_spacer->SetProportion(spacerProp); changed = true; }
    if (changed)
    {
        Layout();
        if (GetParent()) GetParent()->Layout();                  // the bar's height changes with the timeline row
    }
}

FlowResultsBar::~FlowResultsBar()
{
    m_timer.Stop();
}

void FlowResultsBar::SetLegend(const Legend& legend)
{
    m_legendValid = legend.valid;
    m_legend->Set(legend);
    Relayout();
}

void FlowResultsBar::SetTimeline(const TimelineData& d)
{
    if (d.times.empty()) { ClearTimeline(); return; }
    const bool same = (d.key == m_key && d.times.size() == m_times.size());
    m_endTitle = d.endTitle;
    m_endNote = d.endNote;
    if (!same)
    {
        Stop();
        m_key = d.key;
        m_times = d.times; m_filled = d.filledPct; m_inlet = d.inletMPa; m_phases = d.phases;
        m_histT = d.historyT; m_histP = d.historyMPa;
        m_endTime = std::max(d.endTime, d.times.back());
        m_pos = (int)m_times.size();                 // start at the end state
    }
    Relayout();
    UpdateReadout();
    m_timeline->Refresh();
}

void FlowResultsBar::ClearTimeline()
{
    Stop();
    m_key = -1;
    m_times.clear(); m_filled.clear(); m_inlet.clear(); m_histT.clear(); m_histP.clear(); m_phases.clear();
    m_endTime = 0.0f;
    m_pos = 0;
    Relayout();
}

void FlowResultsBar::SetPosition(int pos, bool notify)
{
    pos = std::clamp(pos, 0, (int)m_times.size());
    if (pos == m_pos) return;
    m_pos = pos;
    UpdateReadout();
    m_timeline->Refresh();
    m_timeline->Update();
    if (notify && onFrameChanged) onFrameChanged(m_pos);
}

void FlowResultsBar::Stop()
{
    if (m_timer.IsRunning()) m_timer.Stop();
    if (m_playBtn) m_playBtn->SetLabel("Play");
}

void FlowResultsBar::OnPlay()
{
    if (m_timer.IsRunning()) { Stop(); return; }
    if (m_times.empty()) return;
    if (AtEnd()) SetPosition(0, true);
    m_playBtn->SetLabel("Pause");
    m_timer.Start(70);                                // ~14 frames per second
}

void FlowResultsBar::OnTimer(wxTimerEvent&)
{
    if (AtEnd()) { Stop(); return; }
    SetPosition(m_pos + 1, true);
    if (AtEnd()) Stop();
}

void FlowResultsBar::UpdateReadout()
{
    if (!m_readout) return;
    wxString s;
    if (m_times.empty()) s = "";
    else if (AtEnd())
    {
        s = m_endTitle + wxString::Format(m_endTime < 5.0f ? "  %.3f s" : "  %.2f s", m_endTime);
        if (!m_endNote.IsEmpty()) s << "\n" << m_endNote;
    }
    else
    {
        const size_t i = (size_t)m_pos;
        const int phase = (i < m_phases.size()) ? m_phases[i] : 0;
        if (phase == 1)
            s = wxString::Format("t %.2f s  |  packing\nholding %.1f MPa", m_times[i], i < m_inlet.size() ? m_inlet[i] : 0.0f);
        else if (phase == 2)
            s = wxString::Format("t %.2f s  |  cooling\nnozzle released", m_times[i]);
        else
        {
            s = wxString::Format("t %.3f s  |  %.0f%% filled", m_times[i], i < m_filled.size() ? m_filled[i] : 0.0f);
            if (i < m_inlet.size()) s << wxString::Format("\ninjection %.1f MPa", m_inlet[i]);
        }
    }
    m_readout->SetLabel(s);
}
