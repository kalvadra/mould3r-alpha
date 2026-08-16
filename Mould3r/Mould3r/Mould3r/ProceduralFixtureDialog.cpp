#include "ProceduralFixtureDialog.h"
#include "WindowEffects.h"   // DWM corner rounding, matching the sibling dialogs

ProceduralFixtureDialog::ProceduralFixtureDialog(wxWindow* parent,
    const FixtureDefinition& def)
    : wxDialog(parent, wxID_ANY,
        def.kind == FixtureKind::Dynamic ? "Dynamic Box Fixture"
                                         : "Parametric Box Fixture",
        wxDefaultPosition, wxSize(360, 240),
        wxDEFAULT_DIALOG_STYLE | wxSTAY_ON_TOP),
    m_kind(def.kind)
{
    const bool dyn = (m_kind == FixtureKind::Dynamic);

    auto* main = new wxBoxSizer(wxVERTICAL);

    // One-line caption explaining what the three fields mean for this kind.
    auto* caption = new wxStaticText(this, wxID_ANY, dyn
        ? "Minimum clearance from the parts and origin to each face.\n"
          "The box auto-fits the scene and updates as objects move."
        : "Total box dimensions, centred on the origin and split into\n"
          "two halves at the parting plane.");
    main->Add(caption, 0, wxLEFT | wxRIGHT | wxTOP, 14);
    main->AddSpacer(4);

    // Seed the fields from the matching param struct.
    const float ix = dyn ? def.dynamic.clearanceX : def.parametric.sizeX;
    const float iy = dyn ? def.dynamic.clearanceY : def.parametric.sizeY;
    const float iz = dyn ? def.dynamic.clearanceZ : def.parametric.sizeZ;

    auto addRow = [&](const wxString& label, wxTextCtrl*& ctrl, float initVal)
    {
        auto* row = new wxBoxSizer(wxHORIZONTAL);
        auto* lbl = new wxStaticText(this, wxID_ANY, label,
            wxDefaultPosition, wxSize(90, -1));
        ctrl = new wxTextCtrl(this, wxID_ANY,
            wxString::Format("%.3f", initVal),
            wxDefaultPosition, wxSize(120, -1));
        auto* unit = new wxStaticText(this, wxID_ANY, "mm");

        row->Add(lbl, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
        row->Add(ctrl, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
        row->Add(unit, 0, wxALIGN_CENTER_VERTICAL);
        main->Add(row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 14);
    };

    addRow(dyn ? "Clearance X:" : "Width (X):", m_ctrlX, ix);
    addRow(dyn ? "Clearance Y:" : "Height (Y):", m_ctrlY, iy);
    addRow(dyn ? "Clearance Z:" : "Depth (Z):", m_ctrlZ, iz);

    main->AddSpacer(10);
    main->Add(CreateButtonSizer(wxOK | wxCANCEL), 0,
        wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 14);

    SetSizer(main);
    Fit();
    CentreOnParent();
    WindowEffects::ApplyRoundedCorners(this);

    // Validate on OK rather than letting the dialog's default handler end it,
    // so a non-positive extent can't reach BRepPrimAPI_MakeBox.
    Bind(wxEVT_BUTTON, &ProceduralFixtureDialog::OnOK, this, wxID_OK);

    m_ctrlX->SetFocus();
    m_ctrlX->SelectAll();
}

float ProceduralFixtureDialog::ParseField(wxTextCtrl* ctrl, float fallback) const
{
    double val = 0.0;
    if (!ctrl->GetValue().ToDouble(&val))
        return fallback;
    return static_cast<float>(val);
}

void ProceduralFixtureDialog::OnOK(wxCommandEvent&)
{
    // Fallback of -1 makes an unparseable field fail the positivity check
    // below rather than silently defaulting to something valid.
    const float x = ParseField(m_ctrlX, -1.0f);
    const float y = ParseField(m_ctrlY, -1.0f);
    const float z = ParseField(m_ctrlZ, -1.0f);

    if (x <= 0.0f || y <= 0.0f || z <= 0.0f)
    {
        const wxString what = (m_kind == FixtureKind::Dynamic)
            ? "Clearances" : "Dimensions";
        wxMessageBox(what + " must be greater than zero.",
            "Invalid value", wxOK | wxICON_WARNING, this);
        return;   // not skipped → the dialog stays open for correction
    }
    EndModal(wxID_OK);
}

ParametricFixtureParams ProceduralFixtureDialog::GetParametric() const
{
    ParametricFixtureParams p;
    p.sizeX = ParseField(m_ctrlX, p.sizeX);
    p.sizeY = ParseField(m_ctrlY, p.sizeY);
    p.sizeZ = ParseField(m_ctrlZ, p.sizeZ);
    return p;
}

DynamicFixtureParams ProceduralFixtureDialog::GetDynamic() const
{
    DynamicFixtureParams d;
    d.clearanceX = ParseField(m_ctrlX, d.clearanceX);
    d.clearanceY = ParseField(m_ctrlY, d.clearanceY);
    d.clearanceZ = ParseField(m_ctrlZ, d.clearanceZ);
    return d;
}
