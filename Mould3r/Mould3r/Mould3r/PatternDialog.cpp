#include "PatternDialog.h"

PatternDialog::PatternDialog(wxWindow* parent)
    : wxDialog(parent, wxID_ANY, "Pattern",
        wxDefaultPosition, wxDefaultSize,
        wxDEFAULT_DIALOG_STYLE | wxSTAY_ON_TOP)
{
    auto* main = new wxBoxSizer(wxVERTICAL);

    // ---- Type selector -----------------------------------------------------
    {
        auto* row = new wxBoxSizer(wxHORIZONTAL);
        auto* lbl = new wxStaticText(this, wxID_ANY, "Type:",
            wxDefaultPosition, wxSize(120, -1));

        wxArrayString types;
        types.Add("Circular");
        types.Add("Grid");
        m_typeChoice = new wxChoice(this, wxID_ANY,
            wxDefaultPosition, wxSize(140, -1), types);
        m_typeChoice->SetSelection(0);

        row->Add(lbl, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
        row->Add(m_typeChoice, 0, wxALIGN_CENTER_VERTICAL);
        main->Add(row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 14);
    }

    // ---- Circular pane -----------------------------------------------------
    m_circularPanel = new wxPanel(this, wxID_ANY);
    {
        auto* s = new wxBoxSizer(wxVERTICAL);

        // Number row
        {
            auto* row = new wxBoxSizer(wxHORIZONTAL);
            auto* lbl = new wxStaticText(m_circularPanel, wxID_ANY, "Number:",
                wxDefaultPosition, wxSize(120, -1));
            m_circularNumber = new wxTextCtrl(m_circularPanel, wxID_ANY, "4",
                wxDefaultPosition, wxSize(120, -1));
            row->Add(lbl, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
            row->Add(m_circularNumber, 0, wxALIGN_CENTER_VERTICAL);
            s->Add(row, 0, wxEXPAND | wxTOP, 10);
        }

        // Radius over-ride checkbox
        m_overrideRadius = new wxCheckBox(m_circularPanel, wxID_ANY,
            "Radius over-ride");
        s->Add(m_overrideRadius, 0, wxTOP, 10);

        // Pattern radius row (initially hidden — only shown when override is checked)
        m_radiusRow = new wxPanel(m_circularPanel, wxID_ANY);
        {
            auto* row = new wxBoxSizer(wxHORIZONTAL);
            auto* lbl = new wxStaticText(m_radiusRow, wxID_ANY, "Pattern radius:",
                wxDefaultPosition, wxSize(120, -1));
            m_radius = new wxTextCtrl(m_radiusRow, wxID_ANY, "50.0",
                wxDefaultPosition, wxSize(120, -1));
            auto* unit = new wxStaticText(m_radiusRow, wxID_ANY, "mm");
            row->Add(lbl, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
            row->Add(m_radius, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
            row->Add(unit, 0, wxALIGN_CENTER_VERTICAL);
            m_radiusRow->SetSizer(row);
        }
        s->Add(m_radiusRow, 0, wxEXPAND | wxTOP, 8);
        m_radiusRow->Show(false);     // hidden until checkbox toggles

        // Rotate copies checkbox — when checked, each clone's local yaw is
        // adjusted so it faces outward like the original (gear-tooth style).
        m_rotateCopies = new wxCheckBox(m_circularPanel, wxID_ANY,
            "Rotate copies");
        m_rotateCopies->SetValue(true);
        s->Add(m_rotateCopies, 0, wxTOP, 10);

        m_circularPanel->SetSizer(s);
    }
    main->Add(m_circularPanel, 0, wxEXPAND | wxLEFT | wxRIGHT, 14);

    // ---- Grid pane ---------------------------------------------------------
    m_gridPanel = new wxPanel(this, wxID_ANY);
    {
        auto* s = new wxBoxSizer(wxVERTICAL);

        auto addNumRow = [&](const wxString& label, wxTextCtrl*& ctrl,
            const wxString& dflt)
            {
                auto* row = new wxBoxSizer(wxHORIZONTAL);
                auto* lbl = new wxStaticText(m_gridPanel, wxID_ANY, label,
                    wxDefaultPosition, wxSize(120, -1));
                ctrl = new wxTextCtrl(m_gridPanel, wxID_ANY, dflt,
                    wxDefaultPosition, wxSize(120, -1));
                row->Add(lbl, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
                row->Add(ctrl, 0, wxALIGN_CENTER_VERTICAL);
                s->Add(row, 0, wxEXPAND | wxTOP, 10);
            };

        addNumRow("Number horizontal:", m_gridNumH, "2");
        addNumRow("Number vertical:", m_gridNumV, "2");

        // Mirror checkboxes
        m_mirrorH = new wxCheckBox(m_gridPanel, wxID_ANY, "Mirror horizontal");
        s->Add(m_mirrorH, 0, wxTOP, 10);
        m_mirrorV = new wxCheckBox(m_gridPanel, wxID_ANY, "Mirror vertical");
        s->Add(m_mirrorV, 0, wxTOP, 6);

        // Length/width over-ride section — same pattern as circular's radius
        // over-ride: a checkbox that reveals dimension fields when checked.
        m_overrideLW = new wxCheckBox(m_gridPanel, wxID_ANY,
            "Length/width over-ride");
        s->Add(m_overrideLW, 0, wxTOP, 10);

        // Sub-panel that holds both dimension rows together so they show/hide
        // as a single unit.
        m_lwRows = new wxPanel(m_gridPanel, wxID_ANY);
        {
            auto* lwSizer = new wxBoxSizer(wxVERTICAL);

            auto addDimRow = [&](const wxString& label, wxTextCtrl*& ctrl,
                const wxString& dflt)
                {
                    auto* row = new wxBoxSizer(wxHORIZONTAL);
                    auto* lbl = new wxStaticText(m_lwRows, wxID_ANY, label,
                        wxDefaultPosition, wxSize(120, -1));
                    ctrl = new wxTextCtrl(m_lwRows, wxID_ANY, dflt,
                        wxDefaultPosition, wxSize(120, -1));
                    auto* unit = new wxStaticText(m_lwRows, wxID_ANY, "mm");
                    row->Add(lbl, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
                    row->Add(ctrl, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
                    row->Add(unit, 0, wxALIGN_CENTER_VERTICAL);
                    lwSizer->Add(row, 0, wxEXPAND | wxTOP, 8);
                };

            addDimRow("Length over-ride:", m_length, "100.0");
            addDimRow("Width over-ride:", m_width, "100.0");

            m_lwRows->SetSizer(lwSizer);
        }
        s->Add(m_lwRows, 0, wxEXPAND);
        m_lwRows->Show(false);     // hidden until checkbox toggles

        m_gridPanel->SetSizer(s);
    }
    main->Add(m_gridPanel, 0, wxEXPAND | wxLEFT | wxRIGHT, 14);
    m_gridPanel->Show(false);     // Circular is the default

    // ---- OK / Cancel -------------------------------------------------------
    main->AddSpacer(14);
    main->Add(CreateButtonSizer(wxOK | wxCANCEL), 0,
        wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 14);

    SetSizer(main);
    Fit();
    CentreOnParent();

    // Bind dynamic-show handlers
    m_typeChoice->Bind(wxEVT_CHOICE,
        &PatternDialog::OnTypeChanged, this);
    m_overrideRadius->Bind(wxEVT_CHECKBOX,
        &PatternDialog::OnOverrideToggled, this);
    m_overrideLW->Bind(wxEVT_CHECKBOX,
        &PatternDialog::OnLengthWidthOverrideToggled, this);

    m_circularNumber->SetFocus();
    m_circularNumber->SelectAll();
}

void PatternDialog::OnTypeChanged(wxCommandEvent&)
{
    const bool circular = (m_typeChoice->GetSelection() == 0);
    m_circularPanel->Show(circular);
    m_gridPanel->Show(!circular);
    RelayoutForVisibility();
}

void PatternDialog::OnOverrideToggled(wxCommandEvent&)
{
    m_radiusRow->Show(m_overrideRadius->IsChecked());
    RelayoutForVisibility();
}

void PatternDialog::OnLengthWidthOverrideToggled(wxCommandEvent&)
{
    m_lwRows->Show(m_overrideLW->IsChecked());
    RelayoutForVisibility();
}

void PatternDialog::RelayoutForVisibility()
{
    // Recompute panel sizes, then resize the dialog to its new best fit.
    Layout();
    Fit();
}

int PatternDialog::ParseInt(wxTextCtrl* ctrl, int fallback) const
{
    long v = 0;
    if (!ctrl->GetValue().ToLong(&v) || v < 1)
        return fallback;
    return static_cast<int>(v);
}

float PatternDialog::ParseFloat(wxTextCtrl* ctrl, float fallback) const
{
    double v = 0.0;
    if (!ctrl->GetValue().ToDouble(&v))
        return fallback;
    return static_cast<float>(v);
}

PatternValues PatternDialog::GetValues() const
{
    PatternValues v;
    v.type = (m_typeChoice->GetSelection() == 0)
        ? PatternValues::Type::Circular
        : PatternValues::Type::Grid;

    v.number = ParseInt(m_circularNumber, 4);
    v.overrideRadius = m_overrideRadius->IsChecked();
    v.radius = ParseFloat(m_radius, 50.0f);
    v.rotateCopies = m_rotateCopies->IsChecked();

    v.numberH = ParseInt(m_gridNumH, 2);
    v.numberV = ParseInt(m_gridNumV, 2);
    v.mirrorH = m_mirrorH->IsChecked();
    v.mirrorV = m_mirrorV->IsChecked();
    v.overrideLengthWidth = m_overrideLW->IsChecked();
    v.length = ParseFloat(m_length, 100.0f);
    v.width = ParseFloat(m_width, 100.0f);

    return v;
}
