#include "ScaleDialog.h"
#include "WindowEffects.h"   // DWM corner rounding for this dialog frame

ScaleDialog::ScaleDialog(wxWindow* parent)
    : wxDialog(parent, wxID_ANY, "Scale Object",
        wxDefaultPosition, wxSize(280, 160),
        wxDEFAULT_DIALOG_STYLE | wxSTAY_ON_TOP)
{
    auto* main = new wxBoxSizer(wxVERTICAL);

    auto* row = new wxBoxSizer(wxHORIZONTAL);
    auto* lbl = new wxStaticText(this, wxID_ANY, "Scale:",
        wxDefaultPosition, wxSize(60, -1));
    m_ctrlUniform = new wxTextCtrl(this, wxID_ANY, "1.0",
        wxDefaultPosition, wxSize(120, -1));
    auto* unit = new wxStaticText(this, wxID_ANY, "x");

    row->Add(lbl, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
    row->Add(m_ctrlUniform, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
    row->Add(unit, 0, wxALIGN_CENTER_VERTICAL);
    main->Add(row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 14);

    auto* hint = new wxStaticText(this, wxID_ANY,
        "e.g. 2.0 = double size,  0.5 = half size");
    hint->SetForegroundColour(wxColour(0x88, 0x88, 0x88));
    main->Add(hint, 0, wxLEFT | wxTOP, 14);

    main->AddSpacer(10);
    main->Add(CreateButtonSizer(wxOK | wxCANCEL), 0,
        wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 14);

    SetSizer(main);
    Fit();
    CentreOnParent();
    WindowEffects::ApplyRoundedCorners(this);
    m_ctrlUniform->SetFocus();
    m_ctrlUniform->SelectAll();
}

float ScaleDialog::ParseField(wxTextCtrl* ctrl) const
{
    double val = 1.0;
    if (!ctrl->GetValue().ToDouble(&val))
        val = 1.0;
    return static_cast<float>(val);
}

ScaleValues ScaleDialog::GetValues() const
{
    return { ParseField(m_ctrlUniform) };
}