#include "InjectionPointDialog.h"

// ---------------------------------------------------------------------------
// Layout mirrors TranslateDialog exactly so the two prompts read as
// siblings: same row gutter (14px), same field width (120), same narrow
// label column (60). The label row is the only addition — placed above
// the numeric rows so the user can name the point first, then drop in the
// coordinates without re-tabbing back up.
// ---------------------------------------------------------------------------
InjectionPointDialog::InjectionPointDialog(wxWindow* parent,
    const wxString& title,
    const InjectionPointValues& initial)
    : wxDialog(parent, wxID_ANY, title,
        wxDefaultPosition, wxSize(320, 260),
        wxDEFAULT_DIALOG_STYLE | wxSTAY_ON_TOP)
{
    auto* main = new wxBoxSizer(wxVERTICAL);

    // Label row — uses the same row geometry as the numeric rows so the
    // four fields align vertically. The unit slot is left blank for the
    // label since "label" has no unit.
    {
        auto* row = new wxBoxSizer(wxHORIZONTAL);
        auto* lbl = new wxStaticText(this, wxID_ANY, "Label:",
            wxDefaultPosition, wxSize(60, -1));
        m_ctrlLabel = new wxTextCtrl(this, wxID_ANY, initial.label,
            wxDefaultPosition, wxSize(180, -1));
        row->Add(lbl, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
        row->Add(m_ctrlLabel, 1, wxALIGN_CENTER_VERTICAL);
        main->Add(row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 14);
    }

    // Numeric rows — identical layout to TranslateDialog so the two prompts
    // feel cousin to each other (this is the closest parallel: a per-axis
    // vec3 with mm units).
    auto addNumRow = [&](const wxString& label, wxTextCtrl*& ctrl,
        float defVal)
        {
            auto* row = new wxBoxSizer(wxHORIZONTAL);
            auto* lbl = new wxStaticText(this, wxID_ANY, label,
                wxDefaultPosition, wxSize(60, -1));
            ctrl = new wxTextCtrl(this, wxID_ANY,
                wxString::Format("%g", defVal),
                wxDefaultPosition, wxSize(120, -1));
            auto* unit = new wxStaticText(this, wxID_ANY, "mm");

            row->Add(lbl, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
            row->Add(ctrl, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
            row->Add(unit, 0, wxALIGN_CENTER_VERTICAL);
            main->Add(row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 14);
        };

    addNumRow("X:", m_ctrlX, initial.x);
    addNumRow("Y:", m_ctrlY, initial.y);
    addNumRow("Z:", m_ctrlZ, initial.z);

    main->AddSpacer(10);
    main->Add(CreateButtonSizer(wxOK | wxCANCEL), 0,
        wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 14);

    SetSizer(main);
    Fit();
    CentreOnParent();

    // Focus the label first — for an Add (empty initial), that's where
    // the user will start; for an Edit (pre-filled), select-all is harmless
    // because the label is the most-likely thing they'd want to retype.
    m_ctrlLabel->SetFocus();
    m_ctrlLabel->SelectAll();
}

float InjectionPointDialog::ParseField(wxTextCtrl* ctrl) const
{
    double val = 0.0;
    if (!ctrl->GetValue().ToDouble(&val))
        val = 0.0;
    return static_cast<float>(val);
}

InjectionPointValues InjectionPointDialog::GetValues() const
{
    InjectionPointValues v;
    v.label = m_ctrlLabel->GetValue().ToStdString();
    v.x = ParseField(m_ctrlX);
    v.y = ParseField(m_ctrlY);
    v.z = ParseField(m_ctrlZ);
    return v;
}
