#include "PrecisionPlaceDialog.h"
#include "WindowEffects.h"   // DWM corner rounding for this dialog frame

PrecisionPlaceDialog::PrecisionPlaceDialog(wxWindow* parent, float initX, float initZ)
    : wxDialog(parent, wxID_ANY, "Precision Place",
        wxDefaultPosition, wxSize(280, 180),
        wxDEFAULT_DIALOG_STYLE | wxSTAY_ON_TOP)
{
    auto* main = new wxBoxSizer(wxVERTICAL);

    auto addRow = [&](const wxString& label, wxTextCtrl*& ctrl, float initVal)
        {
            auto* row = new wxBoxSizer(wxHORIZONTAL);
            auto* lbl = new wxStaticText(this, wxID_ANY, label,
                wxDefaultPosition, wxSize(60, -1));
            ctrl = new wxTextCtrl(this, wxID_ANY,
                wxString::Format("%.3f", initVal),
                wxDefaultPosition, wxSize(120, -1));
            auto* unit = new wxStaticText(this, wxID_ANY, "mm");

            row->Add(lbl, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
            row->Add(ctrl, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
            row->Add(unit, 0, wxALIGN_CENTER_VERTICAL);
            main->Add(row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 14);
        };

    addRow("X:", m_ctrlX, initX);
    addRow("Z:", m_ctrlZ, initZ);

    main->AddSpacer(10);
    main->Add(CreateButtonSizer(wxOK | wxCANCEL), 0,
        wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 14);

    SetSizer(main);
    Fit();
    CentreOnParent();
    WindowEffects::ApplyRoundedCorners(this);
    m_ctrlX->SetFocus();
    m_ctrlX->SelectAll();
}

float PrecisionPlaceDialog::ParseField(wxTextCtrl* ctrl) const
{
    double val = 0.0;
    if (!ctrl->GetValue().ToDouble(&val))
        val = 0.0;
    return static_cast<float>(val);
}

PrecisionPlaceValues PrecisionPlaceDialog::GetValues() const
{
    return { ParseField(m_ctrlX), ParseField(m_ctrlZ) };
}
