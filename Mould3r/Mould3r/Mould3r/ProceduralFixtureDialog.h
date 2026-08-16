#pragma once
#include <wx/wx.h>
#include "FixtureFile.h"

// Modal dialog that collects the parameters for a procedural fixture chosen
// from the fixture-select menu. Three millimetre fields whose meaning depends
// on the fixture kind:
//   Parametric — total X / Y / Z box dimensions. The box is centred on the
//                origin and split into two equal halves at the y = 0 parting
//                plane.
//   Dynamic    — per-axis MINIMUM clearance from the scene bodies (and the
//                origin) to each face; the box auto-fits the scene.
//
// Construct from a FixtureDefinition: the constructor reads `kind` to pick the
// title/labels and seeds the fields from the matching param struct, so the
// same dialog serves both first-time creation (from the picker) and later
// editing (re-opened with the current values). After a wxID_OK result, read
// the edited values back with GetParametric() or GetDynamic() — whichever
// matches the kind. The OK button validates that all three values are positive;
// a zero or negative extent would make a null OCC box.
class ProceduralFixtureDialog : public wxDialog
{
public:
    ProceduralFixtureDialog(wxWindow* parent, const FixtureDefinition& def);

    ParametricFixtureParams GetParametric() const;
    DynamicFixtureParams    GetDynamic() const;

private:
    void  OnOK(wxCommandEvent& evt);
    float ParseField(wxTextCtrl* ctrl, float fallback) const;

    FixtureKind m_kind;
    wxTextCtrl* m_ctrlX = nullptr;
    wxTextCtrl* m_ctrlY = nullptr;
    wxTextCtrl* m_ctrlZ = nullptr;
};
