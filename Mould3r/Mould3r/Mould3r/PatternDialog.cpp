#include "PatternDialog.h"

PatternDialog::PatternDialog(wxWindow* parent)
    : wxDialog(parent, wxID_ANY, "Pattern",
        wxDefaultPosition, wxDefaultSize,
        wxDEFAULT_DIALOG_STYLE | wxSTAY_ON_TOP)
{
    auto* main = new wxBoxSizer(wxVERTICAL);

    // ---- Pattern type selector ---------------------------------------------
    wxString typeChoices[] = { "Circular", "Grid" };
    m_typeRadio = new wxRadioBox(this, wxID_ANY, "Pattern type",
        wxDefaultPosition, wxDefaultSize,
        2, typeChoices,
        2, wxRA_SPECIFY_COLS);
    m_typeRadio->SetSelection(0);   // Circular by default
    main->Add(m_typeRadio, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 14);

    // Helper: append a "Label  [field]  unit" row to the given sizer, taking
    // the parent window so the controls are correctly attached.
    auto addRow = [](wxWindow* rowParent, wxSizer* parentSizer,
        const wxString& label, wxTextCtrl*& outCtrl,
        const wxString& initial,
        const wxString& unit = wxEmptyString)
        {
            auto* row = new wxBoxSizer(wxHORIZONTAL);
            auto* lbl = new wxStaticText(rowParent, wxID_ANY, label,
                wxDefaultPosition, wxSize(120, -1));
            outCtrl = new wxTextCtrl(rowParent, wxID_ANY, initial,
                wxDefaultPosition, wxSize(80, -1));

            row->Add(lbl, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
            row->Add(outCtrl, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);

            if (!unit.IsEmpty())
            {
                auto* unitLbl = new wxStaticText(rowParent, wxID_ANY, unit);
                row->Add(unitLbl, 0, wxALIGN_CENTER_VERTICAL);
            }

            parentSizer->Add(row, 0, wxEXPAND | wxTOP, 8);
        };

    // ---- Circular sub-panel ------------------------------------------------
    m_circularPanel = new wxPanel(this, wxID_ANY);
    auto* circSizer = new wxBoxSizer(wxVERTICAL);

    addRow(m_circularPanel, circSizer, "Number:", m_ctrlCount, "4");

    m_chkRadiusOverride = new wxCheckBox(m_circularPanel, wxID_ANY,
        "Radius override");
    circSizer->Add(m_chkRadiusOverride, 0, wxTOP, 8);

    // Wrap the pattern-radius row in its own panel so Show/Hide cleanly
    // collapses or restores the row when the override checkbox toggles.
    m_radiusRow = new wxPanel(m_circularPanel, wxID_ANY);
    auto* radiusRowSizer = new wxBoxSizer(wxHORIZONTAL);
    auto* lblRadius = new wxStaticText(m_radiusRow, wxID_ANY, "Pattern radius:",
        wxDefaultPosition, wxSize(120, -1));
    m_ctrlRadius = new wxTextCtrl(m_radiusRow, wxID_ANY, "0.0",
        wxDefaultPosition, wxSize(80, -1));
    auto* lblRadiusUnit = new wxStaticText(m_radiusRow, wxID_ANY, "mm");
    radiusRowSizer->Add(lblRadius, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
    radiusRowSizer->Add(m_ctrlRadius, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
    radiusRowSizer->Add(lblRadiusUnit, 0, wxALIGN_CENTER_VERTICAL);
    m_radiusRow->SetSizer(radiusRowSizer);
    m_radiusRow->Hide();   // hidden until the override checkbox is ticked
    circSizer->Add(m_radiusRow, 0, wxEXPAND | wxTOP, 8);

    m_circularPanel->SetSizer(circSizer);
    main->Add(m_circularPanel, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 14);

    // ---- Grid sub-panel ----------------------------------------------------
    m_gridPanel = new wxPanel(this, wxID_ANY);
    auto* gridSizer = new wxBoxSizer(wxVERTICAL);

    addRow(m_gridPanel, gridSizer, "Number horizontal:", m_ctrlCountX, "2");
    addRow(m_gridPanel, gridSizer, "Number vertical:", m_ctrlCountY, "2");

    m_gridPanel->SetSizer(gridSizer);
    m_gridPanel->Hide();   // Circular is the default selection
    main->Add(m_gridPanel, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 14);

    // ---- OK / Cancel -------------------------------------------------------
    main->AddSpacer(10);
    main->Add(CreateButtonSizer(wxOK | wxCANCEL), 0,
        wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 14);

    SetSizer(main);
    Fit();
    CentreOnParent();

    // ---- Dynamic event bindings --------------------------------------------
    m_typeRadio->Bind(wxEVT_RADIOBOX,
        &PatternDialog::OnTypeChanged, this);
    m_chkRadiusOverride->Bind(wxEVT_CHECKBOX,
        &PatternDialog::OnRadiusOverrideToggled, this);

    m_ctrlCount->SetFocus();
    m_ctrlCount->SelectAll();
}

void PatternDialog::OnTypeChanged(wxCommandEvent&)
{
    const bool circular = (m_typeRadio->GetSelection() == 0);
    m_circularPanel->Show(circular);
    m_gridPanel->Show(!circular);
    Layout();
    Fit();
}

void PatternDialog::OnRadiusOverrideToggled(wxCommandEvent&)
{
    m_radiusRow->Show(m_chkRadiusOverride->IsChecked());
    m_circularPanel->Layout();
    Layout();
    Fit();
}

int PatternDialog::ParseInt(wxTextCtrl* ctrl, int fallback) const
{
    long val = fallback;
    if (!ctrl->GetValue().ToLong(&val))
        val = fallback;
    return static_cast<int>(val);
}

float PatternDialog::ParseFloat(wxTextCtrl* ctrl, float fallback) const
{
    double val = fallback;
    if (!ctrl->GetValue().ToDouble(&val))
        val = fallback;
    return static_cast<float>(val);
}

PatternValues PatternDialog::GetValues() const
{
    PatternValues v;

    v.type = (m_typeRadio->GetSelection() == 0)
        ? PatternValues::Type::Circular
        : PatternValues::Type::Grid;

    // Circular fields — clamp count to a sensible minimum
    v.count = ParseInt(m_ctrlCount, 1);
    if (v.count < 1) v.count = 1;

    v.radiusOverride = m_chkRadiusOverride->IsChecked();
    v.radius = ParseFloat(m_ctrlRadius, 0.0f);

    // Grid fields — clamp counts to a sensible minimum
    v.countX = ParseInt(m_ctrlCountX, 1);
    if (v.countX < 1) v.countX = 1;
    v.countY = ParseInt(m_ctrlCountY, 1);
    if (v.countY < 1) v.countY = 1;

    return v;
}
