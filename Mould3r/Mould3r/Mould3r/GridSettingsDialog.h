// GridSettingsDialog.h
#pragma once
#include <wx/wx.h>
#include <wx/radiobox.h>
#include <vector>
#include "GridSettings.h"

// ---------------------------------------------------------------------------
// Consolidated "Grid Settings" dialog.
//
// One dialog for the whole grid configuration, since the fields are related
// and some predicate others: the Shape selection decides whether the size
// area shows X/Y (rectangular) or Radius (circular). Also authors spacing and
// the "major line every N divisions" choice.
//
// A single mm/in radio governs every length field (X, Y, Radius, Spacing) and
// live-converts them when toggled; the major-divisions count is unitless.
// Values are returned in millimeters via GetSettings().
// ---------------------------------------------------------------------------
class GridSettingsDialog : public wxDialog
{
public:
    GridSettingsDialog(wxWindow* parent,
                       const GridSettings& current,
                       bool startImperial);

    GridSettings GetSettings() const;

private:
    void OnShapeChanged(wxCommandEvent&);
    void OnMajorChanged(wxCommandEvent&);
    void OnUnitChanged(wxCommandEvent&);

    // Parse a length field (in the currently selected unit) to mm.
    float ParseLenMM(wxTextCtrl* ctrl, float fallbackMM) const;

    // Fallback (incoming) values, in mm, for any field left unpar. These also
    // carry through the field the current shape doesn't display.
    GridSettings m_current;

    wxRadioBox* m_shapeBox = nullptr;
    wxRadioBox* m_majorBox = nullptr;
    wxRadioBox* m_units = nullptr;

    // Size area: rect panel (X, Y) and circular panel (Radius); one shown.
    wxPanel*    m_rectPanel = nullptr;
    wxPanel*    m_circPanel = nullptr;
    wxTextCtrl* m_ctrlX = nullptr;
    wxTextCtrl* m_ctrlY = nullptr;
    wxTextCtrl* m_ctrlRadius = nullptr;
    wxTextCtrl* m_ctrlSpokes = nullptr;   // circular radial divisions (unitless)

    wxTextCtrl* m_ctrlSpacing = nullptr;
    wxTextCtrl* m_customEntry = nullptr;   // major-every custom count

    // "mm"/"in" suffix labels that follow the unit toggle.
    std::vector<wxStaticText*> m_unitLabels;

    // Unit the length fields are currently displayed in.
    bool m_displayImperial = false;
};
