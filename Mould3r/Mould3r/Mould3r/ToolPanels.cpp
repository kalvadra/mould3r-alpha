#include "ToolPanels.h"
#include "RoundedButton.h"
#include "DimensionInput.h"   // fractions / expressions / unit suffixes
#include "style.h"

#include <wx/dcbuffer.h>
#include <wx/graphics.h>

#include <algorithm>
#include <cmath>
#include <memory>

#ifdef __WXMSW__
#include <wx/msw/wrapwin.h>   // windows.h with the wx-safe macro guards
#endif

namespace
{
    // Same metrics as TransformPanel, so the cards read as one family.
    constexpr int    kPad = 12;
    constexpr double kCorner = 8.0;   // card outline AND window clip
    constexpr int    kFieldW = 92;
    constexpr int    kUnitW = 26;
    constexpr double kMmPerInch = 25.4;

    wxFont UiFont(int pt, bool bold = false)
    {
        return wxFont(pt, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
            bold ? wxFONTWEIGHT_BOLD : wxFONTWEIGHT_NORMAL, false, "Segoe UI");
    }

    // Short, readable number: up to `digits` decimals, trailing zeros dropped.
    wxString Fmt(double v, int digits)
    {
        if (std::abs(v) < 0.5 * std::pow(10.0, -digits)) v = 0.0;   // no "-0"
        wxString s = wxString::FromDouble(v, digits);
        if (s.Find('.') != wxNOT_FOUND)
        {
            while (s.EndsWith("0")) s.RemoveLast();
            if (s.EndsWith(".")) s.RemoveLast();
        }
        return s;
    }

    int ReadCount(const wxTextCtrl* c, int fallback)
    {
        double v = 0.0;
        if (!c || !DimInput::ReadExact(c, v) || !std::isfinite(v)) return fallback;
        const long n = std::lround(v);
        return n >= 1 ? int(n) : fallback;
    }
}

// =============================================================================
// ToolCard
// =============================================================================
ToolCard::ToolCard(wxWindow* parent, const wxString& title, int width)
    : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxSize(width, 160),
        wxBORDER_NONE | wxCLIP_CHILDREN | wxTAB_TRAVERSAL),
      m_width(width)
{
    SetBackgroundStyle(wxBG_STYLE_PAINT);   // border painted in OnPaint
    SetBackgroundColour(Style::CardBg);

    m_main = new wxBoxSizer(wxVERTICAL);

    // ---- Header: tool name + selection status --------------------------------
    auto* head = new wxBoxSizer(wxHORIZONTAL);
    m_title = new wxStaticText(this, wxID_ANY, title);
    m_title->SetFont(UiFont(9, true));
    m_title->SetForegroundColour(Style::TextPrimary);
    m_title->SetBackgroundColour(Style::CardBg);
    m_status = new wxStaticText(this, wxID_ANY, "", wxDefaultPosition, wxDefaultSize,
        wxALIGN_RIGHT | wxST_NO_AUTORESIZE);
    m_status->SetFont(UiFont(8));
    m_status->SetForegroundColour(Style::TextMuted);
    m_status->SetBackgroundColour(Style::CardBg);
    head->Add(m_title, 0, wxALIGN_CENTER_VERTICAL);
    head->AddStretchSpacer(1);
    head->Add(m_status, 1, wxALIGN_CENTER_VERTICAL);
    m_main->Add(head, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, kPad);
    m_main->AddSpacer(4);

    m_body = new wxBoxSizer(wxVERTICAL);
    m_main->Add(m_body, 0, wxEXPAND);


    // ---- Buttons -----------------------------------------------------------------
    auto* btns = new wxBoxSizer(wxHORIZONTAL);
    m_btnCancel = new RoundedButton(this, wxID_CANCEL, "Cancel",
        wxDefaultPosition, wxSize(-1, 26), wxBORDER_NONE);
    m_btnCancel->SetBackgroundColour(Style::BtnSmall);
    m_btnCancel->SetForegroundColour(Style::TextPrimary);
    m_btnCancel->SetFont(UiFont(8));
    m_btnOk = new RoundedButton(this, wxID_OK, "OK",
        wxDefaultPosition, wxSize(-1, 26), wxBORDER_NONE);
    m_btnOk->SetBackgroundColour(Style::BtnPlace);
    m_btnOk->SetForegroundColour(Style::TextPrimary);
    m_btnOk->SetFont(UiFont(8, true));
    btns->Add(m_btnCancel, 1, wxRIGHT, 6);
    btns->Add(m_btnOk, 1);
    m_main->Add(btns, 0, wxEXPAND | wxALL, kPad);

    SetSizer(m_main);

    m_btnOk->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { FireOk(); });
    m_btnCancel->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { FireCancel(); });
    Bind(wxEVT_PAINT, &ToolCard::OnPaint, this);
    Bind(wxEVT_SIZE, &ToolCard::OnSize, this);
    Bind(wxEVT_CHAR_HOOK, [this](wxKeyEvent& e)
    {
        if (e.GetKeyCode() == WXK_ESCAPE) { FireCancel(); return; }
        e.Skip();
    });
    Hide();   // the owner shows it while the tool is active
}

wxTextCtrl* ToolCard::AddValueRow(wxSizer* into, const wxString& label, const wxString& unit,
                                  bool dimension, wxStaticText** unitOut)
{
    auto* row = new wxBoxSizer(wxHORIZONTAL);
    auto* lbl = new wxStaticText(this, wxID_ANY, label);
    lbl->SetFont(UiFont(8));
    lbl->SetForegroundColour(Style::TextMuted);
    lbl->SetBackgroundColour(Style::CardBg);

    auto* ctrl = new wxTextCtrl(this, wxID_ANY, "0", wxDefaultPosition, wxSize(kFieldW, 22),
        wxTE_PROCESS_ENTER);
    ctrl->SetBackgroundColour(Style::InputBg);
    ctrl->SetForegroundColour(Style::TextPrimary);

    auto* u = new wxStaticText(this, wxID_ANY, unit, wxDefaultPosition, wxSize(kUnitW, -1));
    u->SetFont(UiFont(8));
    u->SetForegroundColour(Style::TextSubtle);
    u->SetBackgroundColour(Style::CardBg);
    if (dimension) DimInput::Attach(ctrl, u);
    else           DimInput::Attach(ctrl, DimInput::Unit::None);   // counts: still 2*3 etc.

    row->Add(lbl, 0, wxALIGN_CENTER_VERTICAL);
    row->AddStretchSpacer(1);
    row->Add(ctrl, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
    row->Add(u, 0, wxALIGN_CENTER_VERTICAL);
    auto* outer = new wxBoxSizer(wxVERTICAL);
    outer->Add(row, 0, wxEXPAND | wxTOP, 6);
    into->Add(outer, 0, wxEXPAND | wxLEFT | wxRIGHT, kPad);

    // Enter = OK (DimInput has already turned "1/2" into "0.5" by then).
    ctrl->Bind(wxEVT_TEXT_ENTER, [this](wxCommandEvent&) { FireOk(); });
    if (unitOut) *unitOut = u;
    return ctrl;
}

wxCheckBox* ToolCard::AddCheck(wxSizer* into, const wxString& label)
{
    auto* cb = new wxCheckBox(this, wxID_ANY, label);
    cb->SetFont(UiFont(8));
    cb->SetForegroundColour(Style::TextPrimary);
    cb->SetBackgroundColour(Style::CardBg);
    auto* outer = new wxBoxSizer(wxVERTICAL);
    outer->Add(cb, 0, wxTOP, 8);
    into->Add(outer, 0, wxLEFT | wxRIGHT, kPad);
    return cb;
}

wxChoice* ToolCard::AddChoiceRow(wxSizer* into, const wxString& label, const wxArrayString& items)
{
    auto* row = new wxBoxSizer(wxHORIZONTAL);
    auto* lbl = new wxStaticText(this, wxID_ANY, label);
    lbl->SetFont(UiFont(8));
    lbl->SetForegroundColour(Style::TextMuted);
    lbl->SetBackgroundColour(Style::CardBg);
    auto* ch = new wxChoice(this, wxID_ANY, wxDefaultPosition, wxSize(kFieldW + 6 + kUnitW, -1), items);
    ch->SetBackgroundColour(Style::BtnSmall);
    ch->SetForegroundColour(Style::TextMuted);
    ch->SetSelection(0);
    row->Add(lbl, 0, wxALIGN_CENTER_VERTICAL);
    row->AddStretchSpacer(1);
    row->Add(ch, 0, wxALIGN_CENTER_VERTICAL);
    auto* outer = new wxBoxSizer(wxVERTICAL);
    outer->Add(row, 0, wxEXPAND | wxTOP, 6);
    into->Add(outer, 0, wxEXPAND | wxLEFT | wxRIGHT, kPad);
    return ch;
}

void ToolCard::SetSelectionCount(int selectionCount)
{
    m_selectionCount = selectionCount;
    if (selectionCount <= 0)      m_status->SetLabel("Select an object");
    else if (selectionCount == 1) m_status->SetLabel("1 selected");
    else                          m_status->SetLabel(wxString::Format("%d selected", selectionCount));
    m_btnOk->Enable(selectionCount > 0);
    m_main->Layout();
}

void ToolCard::Refit()
{
    m_main->Layout();
    const wxSize best = m_main->GetMinSize();
    SetSize(wxSize(m_width, best.y));
    ApplyRoundedShape();
    Refresh();
}

void ToolCard::OnPaint(wxPaintEvent&)
{
    wxAutoBufferedPaintDC dc(this);
    dc.SetBackground(wxBrush(Style::CardBg));
    dc.Clear();

    std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::Create(dc));
    if (!gc) return;
    const wxSize sz = GetClientSize();
    gc->SetPen(wxPen(Style::Divider, 1));
    gc->SetBrush(*wxTRANSPARENT_BRUSH);
    gc->DrawRoundedRectangle(0.5, 0.5, sz.x - 1.0, sz.y - 1.0, kCorner);
}

void ToolCard::OnSize(wxSizeEvent& evt)
{
    ApplyRoundedShape();
    Refresh(false);
    evt.Skip();
}

// Same native round-rect clip as TransformPanel::ApplyRoundedShape.
void ToolCard::ApplyRoundedShape()
{
#ifdef __WXMSW__
    HWND hwnd = (HWND)GetHWND();
    if (!hwnd) return;
    const wxSize sz = GetSize();
    if (sz.x <= 0 || sz.y <= 0) return;
    const int d = (int)(kCorner * 2.0);
    HRGN rgn = ::CreateRoundRectRgn(0, 0, sz.x + 1, sz.y + 1, d, d);
    if (!rgn) return;
    ::SetWindowRgn(hwnd, rgn, TRUE);
#endif
}

// =============================================================================
// PlacePanel
// =============================================================================
PlacePanel::PlacePanel(wxWindow* parent)
    : ToolCard(parent, "PLACE", 230)
{
    m_fx = AddValueRow(Body(), "X:", "mm", true, &m_ux);
    m_fz = AddValueRow(Body(), "Z:", "mm", true, &m_uz);

    auto onText = [this](wxCommandEvent& e)
    {
        double x, z;
        if (GetTarget(x, z) && m_onChange) m_onChange(x, z);   // mid-typing: hold
        e.Skip();
    };
    m_fx->Bind(wxEVT_TEXT, onText);
    m_fz->Bind(wxEVT_TEXT, onText);
    Refit();
}

void PlacePanel::Begin(bool imperial, int selectionCount, double xMM, double zMM)
{
    m_imperial = imperial;
    const wxString u = imperial ? "in" : "mm";
    m_ux->SetLabel(u);
    m_uz->SetLabel(u);
    const double k = imperial ? 1.0 / kMmPerInch : 1.0;
    const int digits = imperial ? 4 : 3;
    // ChangeValue: no echo back through OnChange.
    m_fx->ChangeValue(Fmt(xMM * k, digits));
    m_fz->ChangeValue(Fmt(zMM * k, digits));
    DimInput::Revalidate(m_fx);
    DimInput::Revalidate(m_fz);
    SetSelectionCount(selectionCount);
    m_fx->Enable(selectionCount > 0);
    m_fz->Enable(selectionCount > 0);
    Refit();
}

bool PlacePanel::GetTarget(double& xMM, double& zMM) const
{
    double x = 0.0, z = 0.0;
    if (!DimInput::ReadExact(m_fx, x) || !DimInput::ReadExact(m_fz, z)) return false;
    if (!std::isfinite(x) || !std::isfinite(z)) return false;
    const double k = m_imperial ? kMmPerInch : 1.0;
    xMM = x * k;
    zMM = z * k;
    return true;
}

// =============================================================================
// PatternPanel
// =============================================================================
PatternPanel::PatternPanel(wxWindow* parent)
    : ToolCard(parent, "PATTERN", 272)
{
    wxArrayString types;
    types.Add("Circular");
    types.Add("Grid");
    m_type = AddChoiceRow(Body(), "Type:", types);

    // Circular
    m_circular = new wxBoxSizer(wxVERTICAL);
    Body()->Add(m_circular, 0, wxEXPAND);
    m_number = AddValueRow(m_circular, "Number:", "", false);
    m_number->ChangeValue("4");
    m_overrideRadius = AddCheck(m_circular, "Radius override");
    {
        auto* holder = new wxBoxSizer(wxVERTICAL);
        m_radius = AddValueRow(holder, "Pattern radius:", "mm", true, &m_radiusUnit);
        m_radius->ChangeValue("50");
        m_circular->Add(holder, 0, wxEXPAND);
        m_radiusRow = holder;
    }
    m_rotateCopies = AddCheck(m_circular, "Rotate copies");
    m_rotateCopies->SetValue(true);

    // Grid
    m_grid = new wxBoxSizer(wxVERTICAL);
    Body()->Add(m_grid, 0, wxEXPAND);
    m_numH = AddValueRow(m_grid, "Number horizontal:", "", false);
    m_numH->ChangeValue("2");
    m_numV = AddValueRow(m_grid, "Number vertical:", "", false);
    m_numV->ChangeValue("2");
    m_mirrorH = AddCheck(m_grid, "Mirror horizontal");
    m_mirrorV = AddCheck(m_grid, "Mirror vertical");
    m_overrideLW = AddCheck(m_grid, "Length / width override");
    {
        auto* holder = new wxBoxSizer(wxVERTICAL);
        m_length = AddValueRow(holder, "Length:", "mm", true, &m_lengthUnit);
        m_length->ChangeValue("100");
        m_width = AddValueRow(holder, "Width:", "mm", true, &m_widthUnit);
        m_width->ChangeValue("100");
        m_grid->Add(holder, 0, wxEXPAND);
        m_lwRows = holder;
    }
    for (wxTextCtrl* f : { m_number, m_radius, m_numH, m_numV, m_length, m_width })
        DimInput::Revalidate(f);

    m_type->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) { SyncVisibility(); });
    m_overrideRadius->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent&) { SyncVisibility(); });
    m_overrideLW->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent&) { SyncVisibility(); });
    SyncVisibility();
}

void PatternPanel::SyncVisibility()
{
    const bool circular = m_type->GetSelection() == 0;
    Body()->Show(m_circular, circular, true);
    Body()->Show(m_grid, !circular, true);
    if (circular) m_circular->Show(m_radiusRow, m_overrideRadius->GetValue(), true);
    else          m_grid->Show(m_lwRows, m_overrideLW->GetValue(), true);
    Refit();   // the card stays anchored by its top-right corner, so no re-pin
}

void PatternPanel::Begin(bool imperial, int selectionCount)
{
    if (imperial != m_imperial)
    {
        // The unit system changed since the panel was last open: keep the
        // lengths, converted. (Just these three fields -- not ConvertLengths,
        // which would sweep every mm field on the main window.)
        const double f = imperial ? 1.0 / kMmPerInch : kMmPerInch;
        for (wxTextCtrl* c : { m_radius, m_length, m_width })
        {
            double v = 0.0;
            if (DimInput::Read(c, v))
                c->ChangeValue(Fmt(v * f, imperial ? 4 : 3));
        }
        m_imperial = imperial;
        const wxString u = imperial ? "in" : "mm";
        for (wxStaticText* l : { m_radiusUnit, m_lengthUnit, m_widthUnit }) l->SetLabel(u);
        for (wxTextCtrl* c : { m_radius, m_length, m_width }) DimInput::Revalidate(c);
    }
    SetSelectionCount(selectionCount);
    SyncVisibility();
}

PatternValues PatternPanel::GetValues() const
{
    PatternValues v;
    const double k = m_imperial ? kMmPerInch : 1.0;
    auto len = [&](const wxTextCtrl* c, float fallbackMM)
    {
        double d = 0.0;
        return DimInput::Read(c, d) && d > 0.0 ? float(d * k) : fallbackMM;
    };
    v.type = (m_type->GetSelection() == 0) ? PatternValues::Type::Circular
                                           : PatternValues::Type::Grid;
    v.number = ReadCount(m_number, 4);
    v.overrideRadius = m_overrideRadius->GetValue();
    v.radius = len(m_radius, 50.0f);
    v.rotateCopies = m_rotateCopies->GetValue();
    v.numberH = ReadCount(m_numH, 2);
    v.numberV = ReadCount(m_numV, 2);
    v.mirrorH = m_mirrorH->GetValue();
    v.mirrorV = m_mirrorV->GetValue();
    v.overrideLengthWidth = m_overrideLW->GetValue();
    v.length = len(m_length, 100.0f);
    v.width = len(m_width, 100.0f);
    return v;
}
