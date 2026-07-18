// GridSettingsDialog.cpp
#include "GridSettingsDialog.h"
#include "WindowEffects.h"   // DWM corner rounding for this dialog frame

static constexpr double kMMPerInch = 25.4;

// Major-divisions radio option order; index 3 is "Custom".
static const int kMajorPresets[] = { 2, 5, 10 };
static constexpr int kMajorCustomIndex = 3;

GridSettingsDialog::GridSettingsDialog(wxWindow* parent,
                                       const GridSettings& current,
                                       bool startImperial)
    : wxDialog(parent, wxID_ANY, "Grid Settings",
        wxDefaultPosition, wxDefaultSize,
        wxDEFAULT_DIALOG_STYLE | wxSTAY_ON_TOP)
    , m_current(current)
    , m_displayImperial(startImperial)
{
    const wxString unit = startImperial ? "in" : "mm";
    const double   toDisplay = startImperial ? (1.0 / kMMPerInch) : 1.0;

    auto* main = new wxBoxSizer(wxVERTICAL);

    // Builds a "label: [field] unit" row on the given parent/sizer. The unit
    // suffix label is registered so it follows the mm/in toggle.
    auto addRow = [&](wxWindow* parent, wxSizer* into,
                      const wxString& label, wxTextCtrl*& ctrl, float initMM)
        {
            auto* row = new wxBoxSizer(wxHORIZONTAL);
            auto* lbl = new wxStaticText(parent, wxID_ANY, label,
                wxDefaultPosition, wxSize(60, -1));
            ctrl = new wxTextCtrl(parent, wxID_ANY,
                wxString::Format("%.4g", initMM * toDisplay),
                wxDefaultPosition, wxSize(110, -1));
            auto* unitLbl = new wxStaticText(parent, wxID_ANY, unit);
            m_unitLabels.push_back(unitLbl);

            row->Add(lbl, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
            row->Add(ctrl, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
            row->Add(unitLbl, 0, wxALIGN_CENTER_VERTICAL);
            into->Add(row, 0, wxTOP, 6);
        };

    // ---- Shape -------------------------------------------------------------
    {
        wxArrayString choices;
        choices.Add("Rectangular");
        choices.Add("Circular");
        m_shapeBox = new wxRadioBox(this, wxID_ANY, "Shape",
            wxDefaultPosition, wxDefaultSize, choices, 2, wxRA_SPECIFY_COLS);
        m_shapeBox->SetSelection(
            current.shape == GridShape::Circular ? 1 : 0);
        m_shapeBox->Bind(wxEVT_RADIOBOX, &GridSettingsDialog::OnShapeChanged, this);
        main->Add(m_shapeBox, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 14);
    }

    // ---- Size (shape-dependent) -------------------------------------------
    {
        auto* sizeBox = new wxStaticBoxSizer(wxVERTICAL, this, "Size");

        m_rectPanel = new wxPanel(this, wxID_ANY);
        auto* rectSizer = new wxBoxSizer(wxVERTICAL);
        addRow(m_rectPanel, rectSizer, "X:", m_ctrlX, current.sizeX);
        addRow(m_rectPanel, rectSizer, "Y:", m_ctrlY, current.sizeY);
        m_rectPanel->SetSizer(rectSizer);

        m_circPanel = new wxPanel(this, wxID_ANY);
        auto* circSizer = new wxBoxSizer(wxVERTICAL);
        addRow(m_circPanel, circSizer, "Radius:", m_ctrlRadius, current.radius);
        // Spokes: radial divisions. Unitless, so it skips the unit suffix and
        // the mm/in conversion (unlike the length rows above).
        {
            auto* row = new wxBoxSizer(wxHORIZONTAL);
            auto* lbl = new wxStaticText(m_circPanel, wxID_ANY, "Spokes:",
                wxDefaultPosition, wxSize(60, -1));
            m_ctrlSpokes = new wxTextCtrl(m_circPanel, wxID_ANY,
                wxString::Format("%d", current.spokes),
                wxDefaultPosition, wxSize(110, -1));
            row->Add(lbl, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
            row->Add(m_ctrlSpokes, 0, wxALIGN_CENTER_VERTICAL);
            circSizer->Add(row, 0, wxTOP, 6);
        }
        m_circPanel->SetSizer(circSizer);

        sizeBox->Add(m_rectPanel, 0, wxEXPAND);
        sizeBox->Add(m_circPanel, 0, wxEXPAND);
        main->Add(sizeBox, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 14);

        const bool circ = (current.shape == GridShape::Circular);
        m_rectPanel->Show(!circ);
        m_circPanel->Show(circ);
    }

    // ---- Spacing -----------------------------------------------------------
    {
        auto* spacingBox = new wxStaticBoxSizer(wxVERTICAL, this, "Spacing");
        addRow(spacingBox->GetStaticBox(), spacingBox,
               "Spacing:", m_ctrlSpacing, current.spacing);
        main->Add(spacingBox, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 14);
    }

    // ---- Major divisions ---------------------------------------------------
    {
        auto* majorOuter = new wxStaticBoxSizer(wxHORIZONTAL, this,
            "Major line every");

        wxArrayString choices;
        choices.Add("2");
        choices.Add("5");
        choices.Add("10");
        choices.Add("Custom");
        m_majorBox = new wxRadioBox(majorOuter->GetStaticBox(), wxID_ANY,
            wxEmptyString, wxDefaultPosition, wxDefaultSize,
            choices, 4, wxRA_SPECIFY_COLS);
        m_majorBox->Bind(wxEVT_RADIOBOX, &GridSettingsDialog::OnMajorChanged, this);

        m_customEntry = new wxTextCtrl(majorOuter->GetStaticBox(), wxID_ANY,
            wxEmptyString, wxDefaultPosition, wxSize(50, -1));

        // Pre-select the preset matching the incoming value, else Custom.
        int sel = kMajorCustomIndex;
        for (int i = 0; i < 3; ++i)
            if (current.majorEvery == kMajorPresets[i]) { sel = i; break; }
        m_majorBox->SetSelection(sel);
        m_customEntry->SetValue(wxString::Format("%d", current.majorEvery));
        m_customEntry->Enable(sel == kMajorCustomIndex);

        majorOuter->Add(m_majorBox, 0, wxALIGN_CENTER_VERTICAL);
        majorOuter->Add(m_customEntry, 0,
            wxALIGN_CENTER_VERTICAL | wxLEFT, 8);
        main->Add(majorOuter, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 14);
    }

    // ---- Units -------------------------------------------------------------
    {
        wxArrayString choices;
        choices.Add("mm");
        choices.Add("in");
        m_units = new wxRadioBox(this, wxID_ANY, "Units",
            wxDefaultPosition, wxDefaultSize, choices, 2, wxRA_SPECIFY_COLS);
        m_units->SetSelection(startImperial ? 1 : 0);
        m_units->Bind(wxEVT_RADIOBOX, &GridSettingsDialog::OnUnitChanged, this);
        main->Add(m_units, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 14);
    }

    main->AddSpacer(12);
    main->Add(CreateButtonSizer(wxOK | wxCANCEL), 0,
        wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 14);

    SetSizerAndFit(main);
    CentreOnParent();
    WindowEffects::ApplyRoundedCorners(this);
}

// When the shape changes, swap the visible size fields and relayout.
void GridSettingsDialog::OnShapeChanged(wxCommandEvent&)
{
    const bool circ = (m_shapeBox->GetSelection() == 1);
    m_rectPanel->Show(!circ);
    m_circPanel->Show(circ);
    Layout();
    Fit();
}

// Enable the custom-count field only when "Custom" is selected.
void GridSettingsDialog::OnMajorChanged(wxCommandEvent&)
{
    m_customEntry->Enable(m_majorBox->GetSelection() == kMajorCustomIndex);
}

// Live-convert every length field between mm and inches when the unit toggles.
void GridSettingsDialog::OnUnitChanged(wxCommandEvent&)
{
    const bool nowImperial = (m_units->GetSelection() == 1);
    if (nowImperial == m_displayImperial)
        return;

    wxTextCtrl* fields[] = { m_ctrlX, m_ctrlY, m_ctrlRadius, m_ctrlSpacing };
    for (auto* c : fields)
    {
        if (!c) continue;
        double v = 0.0;
        if (!c->GetValue().ToDouble(&v)) continue;
        if (nowImperial) v /= kMMPerInch;   // mm -> in
        else             v *= kMMPerInch;   // in -> mm
        c->SetValue(wxString::Format("%.4g", v));
    }

    for (auto* lbl : m_unitLabels)
        lbl->SetLabel(nowImperial ? "in" : "mm");

    m_displayImperial = nowImperial;
    Layout();
}

float GridSettingsDialog::ParseLenMM(wxTextCtrl* ctrl, float fallbackMM) const
{
    if (!ctrl) return fallbackMM;
    double v = 0.0;
    if (!ctrl->GetValue().ToDouble(&v))
        return fallbackMM;
    if (m_units->GetSelection() == 1)   // inches selected
        v *= kMMPerInch;
    return static_cast<float>(v);
}

GridSettings GridSettingsDialog::GetSettings() const
{
    GridSettings s = m_current;

    s.shape = (m_shapeBox->GetSelection() == 1) ? GridShape::Circular
                                                : GridShape::Rectangular;

    // Read all size fields regardless of shape so nothing is lost when the
    // other shape is reselected later; the renderer uses only what it needs.
    s.sizeX   = ParseLenMM(m_ctrlX,       m_current.sizeX);
    s.sizeY   = ParseLenMM(m_ctrlY,       m_current.sizeY);
    s.radius  = ParseLenMM(m_ctrlRadius,  m_current.radius);
    s.spacing = ParseLenMM(m_ctrlSpacing, m_current.spacing);

    // Spokes: unitless integer, clamp to a sane minimum.
    if (m_ctrlSpokes)
    {
        long v = 0;
        if (m_ctrlSpokes->GetValue().ToLong(&v) && v >= 1)
            s.spokes = static_cast<int>(v);
        else
            s.spokes = m_current.spokes;
    }

    const int sel = m_majorBox->GetSelection();
    if (sel >= 0 && sel < 3)
    {
        s.majorEvery = kMajorPresets[sel];
    }
    else
    {
        long v = 0;
        if (m_customEntry->GetValue().ToLong(&v) && v >= 1)
            s.majorEvery = static_cast<int>(v);
        else
            s.majorEvery = m_current.majorEvery;
    }

    return s;
}
