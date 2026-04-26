#pragma once
#include <wx/wx.h>

// ---------------------------------------------------------------------------
// PatternValues — captures the user's choices from a PatternDialog.
//
// Two pattern types are supported. The fields relevant to the chosen type are
// guaranteed to be populated; the other fields hold safe defaults.
// ---------------------------------------------------------------------------
struct PatternValues
{
    enum class Type { Circular, Grid };

    Type type = Type::Circular;

    // Circular ---------------------------------------------------------------
    int   count = 4;        // number of objects around the circle
    bool  radiusOverride = false;    // if false, caller infers radius from scene
    float radius = 0.0f;     // mm; meaningful only when radiusOverride

    // Grid -------------------------------------------------------------------
    int   countX = 2;                // number of objects horizontally
    int   countY = 2;                // number of objects vertically
};

// ---------------------------------------------------------------------------
// PatternDialog — modal dialog for the Pattern model tool.
//
// Layout swaps based on the radio selection: choosing "Circular" shows the
// count + optional radius-override row; choosing "Grid" shows two count
// fields. The dialog re-fits itself when the layout changes.
// ---------------------------------------------------------------------------
class PatternDialog : public wxDialog
{
public:
    PatternDialog(wxWindow* parent);
    PatternValues GetValues() const;

private:
    // Type selector
    wxRadioBox* m_typeRadio = nullptr;

    // Circular sub-panel
    wxPanel* m_circularPanel = nullptr;
    wxTextCtrl* m_ctrlCount = nullptr;
    wxCheckBox* m_chkRadiusOverride = nullptr;
    wxPanel* m_radiusRow = nullptr;   // hidden until override on
    wxTextCtrl* m_ctrlRadius = nullptr;

    // Grid sub-panel
    wxPanel* m_gridPanel = nullptr;
    wxTextCtrl* m_ctrlCountX = nullptr;
    wxTextCtrl* m_ctrlCountY = nullptr;

    void OnTypeChanged(wxCommandEvent&);
    void OnRadiusOverrideToggled(wxCommandEvent&);

    int   ParseInt(wxTextCtrl* ctrl, int fallback) const;
    float ParseFloat(wxTextCtrl* ctrl, float fallback) const;
};
