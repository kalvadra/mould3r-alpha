#pragma once
#include <wx/wx.h>

// Result struct filled in by the dialog.
// The active fields depend on `type`:
//   Circular -> { number, overrideRadius, radius }
//   Grid     -> { numberH, numberV }
struct PatternValues
{
    enum class Type { Circular, Grid };

    Type type = Type::Circular;

    // Circular
    int   number = 4;
    bool  overrideRadius = false;
    float radius = 50.0f;     // mm, only used when overrideRadius is true
    bool  rotateCopies = false;     // gear-tooth: clone yaw matches angular position

    // Grid
    int   numberH = 2;
    int   numberV = 2;
    bool  mirrorH = false;
    bool  mirrorV = false;
    bool  overrideLengthWidth = false;
    float length = 100.0f;     // mm, only used when overrideLengthWidth is true
    float width = 100.0f;     // mm, only used when overrideLengthWidth is true
};

class PatternDialog : public wxDialog
{
public:
    PatternDialog(wxWindow* parent);

    // Call after ShowModal() == wxID_OK
    PatternValues GetValues() const;

private:
    // Type selector
    wxChoice* m_typeChoice = nullptr;

    // Circular pane
    wxPanel* m_circularPanel = nullptr;
    wxTextCtrl* m_circularNumber = nullptr;
    wxCheckBox* m_overrideRadius = nullptr;
    wxPanel* m_radiusRow = nullptr;     // shown only when overrideRadius
    wxTextCtrl* m_radius = nullptr;
    wxCheckBox* m_rotateCopies = nullptr;

    // Grid pane
    wxPanel* m_gridPanel = nullptr;
    wxTextCtrl* m_gridNumH = nullptr;
    wxTextCtrl* m_gridNumV = nullptr;
    wxCheckBox* m_mirrorH = nullptr;
    wxCheckBox* m_mirrorV = nullptr;
    wxCheckBox* m_overrideLW = nullptr;
    wxPanel* m_lwRows = nullptr;     // shown only when overrideLW
    wxTextCtrl* m_length = nullptr;
    wxTextCtrl* m_width = nullptr;

    // Visibility handlers
    void OnTypeChanged(wxCommandEvent&);
    void OnOverrideToggled(wxCommandEvent&);
    void OnLengthWidthOverrideToggled(wxCommandEvent&);

    // Re-runs Layout/Fit on the dialog after show/hide changes
    void RelayoutForVisibility();

    // Parsing helpers (fall back to the supplied default on bad input)
    int   ParseInt(wxTextCtrl* ctrl, int fallback) const;
    float ParseFloat(wxTextCtrl* ctrl, float fallback) const;
};
