#include "MouldCastDialog.h"
#include "WindowEffects.h"   // DWM corner rounding for this dialog frame

#include <wx/statbox.h>

// ---------------------------------------------------------------------------
// Layout follows the house dialog style (PatternDialog / InjectionPointDialog):
// a narrow label column, aligned field widths, 14px outer gutter, WindowEffects
// rounded corners, and STAY_ON_TOP. The two parts (Base / Walls) live in
// wxStaticBox groups so they read as clearly separate sections; the enable
// checkbox at the top of each greys out its own type + thickness rows.
// ---------------------------------------------------------------------------

namespace
{
    constexpr int kLabelW = 90;    // label column width
    constexpr int kFieldW = 120;   // text field width

    // The "Generate Casts" affirmative button reuses wxID_OK so the dialog's
    // built-in OK/Cancel semantics (Enter / Esc, EndModal) still apply.
    // We just relabel it.
}

MouldCastDialog::MouldCastDialog(wxWindow* parent,
    const MouldCastValues& initial)
    : wxDialog(parent, wxID_ANY, "Generate Mould Casts",
        wxDefaultPosition, wxDefaultSize,
        wxDEFAULT_DIALOG_STYLE | wxSTAY_ON_TOP)
{
    auto* main = new wxBoxSizer(wxVERTICAL);

    // ---- Base section — the only type for now is "Flange" -----------------
    {
        wxArrayString baseTypes;
        baseTypes.Add("Flange");
        m_base = BuildPartSection(this, main, "Base", baseTypes,
            initial.base, /*defThickness*/ 5.0,
            /*withExtra*/ true, /*defExtra*/ 0.0, "Extra Flange:");
    }

    // ---- Walls section — the only type for now is "Clover" ----------------
    {
        wxArrayString wallTypes;
        wallTypes.Add("Clover");
        m_walls = BuildPartSection(this, main, "Walls", wallTypes,
            initial.walls, /*defThickness*/ 5.0,
            /*withExtra*/ true, /*defExtra*/ 0.0, "Extra Wall:",
            /*withJoint*/ true);
    }

    // ---- Generate Casts / Cancel ------------------------------------------
    // Standard OK/Cancel sizer, with the OK button relabelled to the action.
    auto* btnSizer = CreateButtonSizer(wxOK | wxCANCEL);
    if (auto* ok = FindWindow(wxID_OK))
        static_cast<wxButton*>(ok)->SetLabel("Generate Casts");
    main->AddSpacer(6);
    main->Add(btnSizer, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 14);

    SetSizer(main);
    Fit();
    CentreOnParent();
    WindowEffects::ApplyRoundedCorners(this);

    // Reflect the initial enabled state (greys out rows on disabled parts).
    SyncEnabled(m_base);
    SyncEnabled(m_walls);
}

// ---------------------------------------------------------------------------
// Build one titled part section: an "Enable Generation" checkbox, then a Type
// dropdown and a Thickness field with a mm / in unit selector.
// ---------------------------------------------------------------------------
MouldCastDialog::PartControls MouldCastDialog::BuildPartSection(wxWindow* parent,
    wxSizer* into, const wxString& title, const wxArrayString& typeChoices,
    const MouldCastPart& initial, double defThickness,
    bool withExtra, double defExtra, const wxString& extraLabel,
    bool withJoint)
{
    PartControls pc;

    auto* box = new wxStaticBox(parent, wxID_ANY, title);
    auto* boxSizer = new wxStaticBoxSizer(box, wxVERTICAL);
    wxWindow* host = box;   // controls parent onto the static box

    // Enable Generation checkbox.
    pc.enable = new wxCheckBox(host, wxID_ANY, "Enable Generation");
    pc.enable->SetValue(initial.enabled);
    boxSizer->Add(pc.enable, 0, wxLEFT | wxRIGHT | wxTOP, 10);

    // Type row.
    {
        auto* row = new wxBoxSizer(wxHORIZONTAL);
        auto* lbl = new wxStaticText(host, wxID_ANY, "Type:",
            wxDefaultPosition, wxSize(kLabelW, -1));
        pc.type = new wxChoice(host, wxID_ANY,
            wxDefaultPosition, wxSize(kFieldW, -1), typeChoices);

        // Pre-select the initial type if it matches an option; else the first.
        int sel = initial.type.empty()
            ? wxNOT_FOUND
            : pc.type->FindString(wxString::FromUTF8(initial.type.c_str()));
        pc.type->SetSelection(sel != wxNOT_FOUND ? sel : 0);

        row->Add(lbl, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
        row->Add(pc.type, 0, wxALIGN_CENTER_VERTICAL);
        boxSizer->Add(row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 10);
    }

    // Thickness row — value field + a mm / in unit selector.
    {
        auto* row = new wxBoxSizer(wxHORIZONTAL);
        auto* lbl = new wxStaticText(host, wxID_ANY, "Thickness:",
            wxDefaultPosition, wxSize(kLabelW, -1));

        const double initThick =
            (initial.thickness > 0.0) ? initial.thickness : defThickness;
        pc.thickness = new wxTextCtrl(host, wxID_ANY,
            wxString::Format("%g", initThick),
            wxDefaultPosition, wxSize(kFieldW, -1));

        wxArrayString units;
        units.Add("mm");
        units.Add("in");
        pc.unit = new wxChoice(host, wxID_ANY,
            wxDefaultPosition, wxSize(56, -1), units);
        pc.unit->SetSelection(initial.inches ? 1 : 0);

        row->Add(lbl, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
        row->Add(pc.thickness, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
        row->Add(pc.unit, 0, wxALIGN_CENTER_VERTICAL);
        boxSizer->Add(row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 10);
    }

    // Extra-distance row. Value field + mm / in selector, matching the
    // thickness row. For the base it's the flange overhang added to every side;
    // for the walls it's the extra Clover overhang past the perimeter.
    if (withExtra)
    {
        auto* row = new wxBoxSizer(wxHORIZONTAL);
        auto* lbl = new wxStaticText(host, wxID_ANY, extraLabel,
            wxDefaultPosition, wxSize(kLabelW, -1));

        const double initExtra =
            (initial.extraDistance > 0.0) ? initial.extraDistance : defExtra;
        pc.extra = new wxTextCtrl(host, wxID_ANY,
            wxString::Format("%g", initExtra),
            wxDefaultPosition, wxSize(kFieldW, -1));

        wxArrayString units;
        units.Add("mm");
        units.Add("in");
        pc.extraUnit = new wxChoice(host, wxID_ANY,
            wxDefaultPosition, wxSize(56, -1), units);
        pc.extraUnit->SetSelection(initial.extraDistanceInches ? 1 : 0);

        row->Add(lbl, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
        row->Add(pc.extra, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
        row->Add(pc.extraUnit, 0, wxALIGN_CENTER_VERTICAL);
        boxSizer->Add(row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 10);
    }

    // Tongue-and-groove joint rows (walls only). A value field + mm / in unit,
    // one per parameter.
    if (withJoint)
    {
        auto addUnitRow = [&](const wxString& label, wxTextCtrl*& ctrl,
            wxChoice*& unitSel, double defVal, bool defInches)
        {
            auto* row = new wxBoxSizer(wxHORIZONTAL);
            auto* l = new wxStaticText(host, wxID_ANY, label,
                wxDefaultPosition, wxSize(kLabelW, -1));
            ctrl = new wxTextCtrl(host, wxID_ANY, wxString::Format("%g", defVal),
                wxDefaultPosition, wxSize(kFieldW, -1));
            wxArrayString units; units.Add("mm"); units.Add("in");
            unitSel = new wxChoice(host, wxID_ANY,
                wxDefaultPosition, wxSize(56, -1), units);
            unitSel->SetSelection(defInches ? 1 : 0);
            row->Add(l, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
            row->Add(ctrl, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
            row->Add(unitSel, 0, wxALIGN_CENTER_VERTICAL);
            boxSizer->Add(row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 10);
        };

        const double initTW = (initial.tongueWidth > 0.0) ? initial.tongueWidth : 2.0;
        const double initTT = (initial.tongueThickness > 0.0) ? initial.tongueThickness : 2.0;
        const double initGT = initial.grooveTolerance;   // default 0
        addUnitRow("Tongue Width:", pc.tongueW, pc.tongueWUnit, initTW,
            initial.tongueWidthInches);
        addUnitRow("Tongue Thick:", pc.tongueT, pc.tongueTUnit, initTT,
            initial.tongueThicknessInches);
        addUnitRow("Groove Tol:", pc.grooveTol, pc.grooveTolUnit, initGT,
            initial.grooveToleranceInches);
    }

    boxSizer->AddSpacer(4);
    into->Add(boxSizer, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 14);

    // Grey out the rows when the part is disabled, live.
    pc.enable->Bind(wxEVT_CHECKBOX,
        [this, pc](wxCommandEvent&) { SyncEnabled(pc); });

    return pc;
}

// ---------------------------------------------------------------------------
// Enable / disable a section's type + thickness controls to match its toggle.
// ---------------------------------------------------------------------------
void MouldCastDialog::SyncEnabled(const PartControls& pc)
{
    const bool on = pc.enable && pc.enable->GetValue();
    if (pc.type)      pc.type->Enable(on);
    if (pc.thickness) pc.thickness->Enable(on);
    if (pc.unit)      pc.unit->Enable(on);
    if (pc.extra)     pc.extra->Enable(on);
    if (pc.extraUnit) pc.extraUnit->Enable(on);
    if (pc.tongueW)      pc.tongueW->Enable(on);
    if (pc.tongueWUnit)  pc.tongueWUnit->Enable(on);
    if (pc.tongueT)      pc.tongueT->Enable(on);
    if (pc.tongueTUnit)  pc.tongueTUnit->Enable(on);
    if (pc.grooveTol)    pc.grooveTol->Enable(on);
    if (pc.grooveTolUnit)pc.grooveTolUnit->Enable(on);
}

// ---------------------------------------------------------------------------
// Read one section's controls back into a MouldCastPart.
// ---------------------------------------------------------------------------
MouldCastPart MouldCastDialog::ReadPart(const PartControls& pc) const
{
    MouldCastPart p;
    p.enabled = pc.enable && pc.enable->GetValue();

    if (pc.type && pc.type->GetSelection() != wxNOT_FOUND)
        p.type = pc.type->GetStringSelection().ToStdString();

    double t = 0.0;
    if (pc.thickness && !pc.thickness->GetValue().ToDouble(&t))
        t = 0.0;
    p.thickness = t;

    p.inches = pc.unit && pc.unit->GetSelection() == 1;

    double ex = 0.0;
    if (pc.extra && !pc.extra->GetValue().ToDouble(&ex))
        ex = 0.0;
    p.extraDistance = (ex > 0.0) ? ex : 0.0;
    p.extraDistanceInches = pc.extraUnit && pc.extraUnit->GetSelection() == 1;

    auto readVal = [](wxTextCtrl* c) -> double {
        double v = 0.0;
        if (c && c->GetValue().ToDouble(&v) && v > 0.0) return v;
        return 0.0;
    };
    p.tongueWidth = readVal(pc.tongueW);
    p.tongueWidthInches = pc.tongueWUnit && pc.tongueWUnit->GetSelection() == 1;
    p.tongueThickness = readVal(pc.tongueT);
    p.tongueThicknessInches = pc.tongueTUnit && pc.tongueTUnit->GetSelection() == 1;
    // Groove tolerance may legitimately be 0, so don't clamp it away.
    double gt = 0.0;
    if (pc.grooveTol && !pc.grooveTol->GetValue().ToDouble(&gt)) gt = 0.0;
    p.grooveTolerance = (gt > 0.0) ? gt : 0.0;
    p.grooveToleranceInches = pc.grooveTolUnit && pc.grooveTolUnit->GetSelection() == 1;
    return p;
}

MouldCastValues MouldCastDialog::GetValues() const
{
    MouldCastValues v;
    v.base = ReadPart(m_base);
    v.walls = ReadPart(m_walls);
    return v;
}
