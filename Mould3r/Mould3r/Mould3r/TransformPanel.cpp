#include "TransformPanel.h"
#include "RoundedButton.h"
#include "style.h"

#include <wx/dcbuffer.h>
#include <wx/graphics.h>

#include <cmath>
#include <memory>

#ifdef __WXMSW__
#include <wx/msw/wrapwin.h>   // windows.h with the wx-safe macro guards
#endif

namespace
{
    constexpr int    kPanelW = 230;
    constexpr int    kPad = 12;
    constexpr double kCorner = 8.0;   // card outline AND window clip
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

    // Empty reads as `emptyValue`; anything else must parse completely.
    bool ParseField(const wxTextCtrl* c, double emptyValue, double& out)
    {
        wxString t = c->GetValue();
        t.Trim(true).Trim(false);
        if (t.IsEmpty()) { out = emptyValue; return true; }
        return t.ToDouble(&out) && std::isfinite(out);
    }
}

TransformPanel::TransformPanel(wxWindow* parent)
    : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxSize(kPanelW, 160),
        wxBORDER_NONE | wxCLIP_CHILDREN | wxTAB_TRAVERSAL)
{
    SetBackgroundStyle(wxBG_STYLE_PAINT);   // border painted in OnPaint
    SetBackgroundColour(Style::CardBg);

    m_main = new wxBoxSizer(wxVERTICAL);

    // ---- Header: tool name + selection status --------------------------------
    auto* head = new wxBoxSizer(wxHORIZONTAL);
    m_title = new wxStaticText(this, wxID_ANY, "MOVE");
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

    // ---- Value rows ------------------------------------------------------------
    m_fx = MakeRow(m_main, "X:", m_ux, m_rowX);
    m_fy = MakeRow(m_main, "Y:", m_uy, m_rowY);
    m_fz = MakeRow(m_main, "Z:", m_uz, m_rowZ);
    m_ff = MakeRow(m_main, "Factor:", m_uf, m_rowF);

    m_note = new wxStaticText(this, wxID_ANY, "");
    m_note->SetFont(UiFont(7));
    m_note->SetForegroundColour(Style::TextMuted);
    m_note->SetBackgroundColour(Style::CardBg);
    m_main->Add(m_note, 0, wxLEFT | wxRIGHT | wxTOP, kPad);

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

    m_btnOk->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { if (m_onOk) m_onOk(); });
    m_btnCancel->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { if (m_onCancel) m_onCancel(); });
    Bind(wxEVT_PAINT, &TransformPanel::OnPaint, this);
    Bind(wxEVT_SIZE, &TransformPanel::OnSize, this);
    Bind(wxEVT_CHAR_HOOK, &TransformPanel::OnCharHook, this);

    Begin(Xform::Tool::Move, false, 0);
    Hide();   // the owner shows it while a tool is active
}

wxTextCtrl* TransformPanel::MakeRow(wxSizer* parentSz, const wxString& label,
                                    wxStaticText*& unit, wxSizer*& rowOut)
{
    auto* row = new wxBoxSizer(wxHORIZONTAL);
    auto* lbl = new wxStaticText(this, wxID_ANY, label, wxDefaultPosition, wxSize(52, -1));
    lbl->SetFont(UiFont(8));
    lbl->SetForegroundColour(Style::TextMuted);
    lbl->SetBackgroundColour(Style::CardBg);

    auto* ctrl = new wxTextCtrl(this, wxID_ANY, "0", wxDefaultPosition, wxSize(110, 22),
        wxTE_PROCESS_ENTER);
    ctrl->SetBackgroundColour(Style::InputBg);
    ctrl->SetForegroundColour(Style::TextPrimary);

    unit = new wxStaticText(this, wxID_ANY, "mm", wxDefaultPosition, wxSize(26, -1));
    unit->SetFont(UiFont(8));
    unit->SetForegroundColour(Style::TextSubtle);
    unit->SetBackgroundColour(Style::CardBg);

    row->Add(lbl, 0, wxALIGN_CENTER_VERTICAL);
    row->AddStretchSpacer(1);
    row->Add(ctrl, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
    row->Add(unit, 0, wxALIGN_CENTER_VERTICAL);
    // Wrapped so the row carries its own 6 px top gap (hidden with it) while
    // the sides line up with the card padding.
    auto* outer = new wxBoxSizer(wxVERTICAL);
    outer->Add(row, 0, wxEXPAND | wxTOP, 6);
    parentSz->Add(outer, 0, wxEXPAND | wxLEFT | wxRIGHT, kPad);

    ctrl->Bind(wxEVT_TEXT, &TransformPanel::OnText, this);
    ctrl->Bind(wxEVT_TEXT_ENTER, [this](wxCommandEvent&) { if (m_onOk) m_onOk(); });

    rowOut = outer;
    return ctrl;
}

void TransformPanel::Begin(Xform::Tool tool, bool imperial, int selectionCount)
{
    m_tool = tool;
    m_imperial = imperial;
    m_delta = Xform::Delta();

    const bool xyz = tool != Xform::Tool::Scale;
    m_main->Show(m_rowX, xyz);
    m_main->Show(m_rowY, xyz);
    m_main->Show(m_rowZ, xyz);
    m_main->Show(m_rowF, !xyz);

    switch (tool)
    {
    case Xform::Tool::Rotate:
        m_title->SetLabel("ROTATE");
        for (wxStaticText* u : { m_ux, m_uy, m_uz }) u->SetLabel(wxString::FromUTF8("\xC2\xB0"));
        m_note->SetLabel(wxString::FromUTF8("About the selection centre, world axes (Z \xE2\x86\x92 X \xE2\x86\x92 Y)"));
        break;
    case Xform::Tool::Scale:
        m_title->SetLabel("SCALE");
        m_uf->SetLabel(wxString::FromUTF8("\xC3\x97"));
        m_note->SetLabel("Uniform, about the selection centre");
        break;
    case Xform::Tool::Move:
    default:
        m_title->SetLabel("MOVE");
        for (wxStaticText* u : { m_ux, m_uy, m_uz }) u->SetLabel(imperial ? "in" : "mm");
        m_note->SetLabel("Offset along the world axes");
        break;
    }

    SetSelectionCount(selectionCount);   // writes the fields + status, relayouts
}

void TransformPanel::SetSelectionCount(int selectionCount)
{
    m_selectionCount = selectionCount;
    m_delta = Xform::Delta();
    WriteFields();
    UpdateStatus();

    const bool live = selectionCount > 0;
    for (wxTextCtrl* c : { m_fx, m_fy, m_fz, m_ff }) c->Enable(live);
    m_btnOk->Enable(live);

    m_main->Layout();
    const wxSize best = m_main->GetMinSize();
    SetSize(wxSize(kPanelW, best.y));
    ApplyRoundedShape();
    Refresh();
}

void TransformPanel::SetDelta(const Xform::Delta& d)
{
    m_delta = d;
    WriteFields();
}

void TransformPanel::WriteFields()
{
    // ChangeValue, not SetValue: programmatic updates must not echo back
    // through OnText as if the user had typed them.
    if (m_tool == Xform::Tool::Scale)
    {
        m_ff->ChangeValue(Fmt(m_delta.scale, 4));
        return;
    }
    const glm::vec3 v = (m_tool == Xform::Tool::Rotate)
        ? m_delta.rotDeg
        : m_delta.move / float(m_imperial ? kMmPerInch : 1.0);
    const int digits = (m_tool == Xform::Tool::Rotate) ? 2 : (m_imperial ? 4 : 3);
    m_fx->ChangeValue(Fmt(v.x, digits));
    m_fy->ChangeValue(Fmt(v.y, digits));
    m_fz->ChangeValue(Fmt(v.z, digits));
}

bool TransformPanel::ReadFields(Xform::Delta& out) const
{
    out = Xform::Delta();
    if (m_tool == Xform::Tool::Scale)
    {
        double f = 1.0;
        if (!ParseField(m_ff, 1.0, f) || !(f > 0.0)) return false;   // a factor must be > 0
        out.scale = (float)f;
        return true;
    }
    double x = 0.0, y = 0.0, z = 0.0;
    if (!ParseField(m_fx, 0.0, x) || !ParseField(m_fy, 0.0, y) || !ParseField(m_fz, 0.0, z))
        return false;
    if (m_tool == Xform::Tool::Rotate)
        out.rotDeg = glm::vec3((float)x, (float)y, (float)z);
    else
    {
        const double k = m_imperial ? kMmPerInch : 1.0;
        out.move = glm::vec3((float)(x * k), (float)(y * k), (float)(z * k));
    }
    return true;
}

void TransformPanel::OnText(wxCommandEvent&)
{
    Xform::Delta d;
    if (!ReadFields(d)) return;   // mid-typing — hold the last preview
    m_delta = d;
    if (m_onChange) m_onChange(m_delta);
}

void TransformPanel::OnCharHook(wxKeyEvent& evt)
{
    if (evt.GetKeyCode() == WXK_ESCAPE)
    {
        if (m_onCancel) m_onCancel();
        return;
    }
    evt.Skip();
}

void TransformPanel::UpdateStatus()
{
    if (m_selectionCount <= 0)
        m_status->SetLabel("Select an object");
    else if (m_selectionCount == 1)
        m_status->SetLabel("1 selected");
    else
        m_status->SetLabel(wxString::Format("%d selected", m_selectionCount));
}

void TransformPanel::OnPaint(wxPaintEvent&)
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

void TransformPanel::OnSize(wxSizeEvent& evt)
{
    ApplyRoundedShape();
    Refresh(false);
    evt.Skip();
}

// Same native round-rect clip as VentEditToolbar::ApplyRoundedShape: wxWindow
// has no SetShape for child windows, so hand an HRGN to SetWindowRgn (which
// takes ownership of it).
void TransformPanel::ApplyRoundedShape()
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
