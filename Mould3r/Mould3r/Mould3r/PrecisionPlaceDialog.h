#pragma once
#include <wx/wx.h>

// Target absolute world coordinates for the Precision Place tool. Only X and
// Z are authored — the selected object keeps its current Y (height), since the
// tool repositions in the ground (XZ) plane.
struct PrecisionPlaceValues
{
    float x = 0.0f;   // mm, absolute world X
    float z = 0.0f;   // mm, absolute world Z
};

// Modal dialog prompting for an absolute X and Z position. Unlike the
// Translate dialog (which authors a *delta*), the fields here are absolute
// world coordinates, so the constructor pre-fills them with the selection's
// current position to make small nudges and round-trips obvious.
class PrecisionPlaceDialog : public wxDialog
{
public:
    // initX / initZ pre-populate the fields with the selected object's current
    // world position. Pass (0, 0) when there is no selection to read from.
    PrecisionPlaceDialog(wxWindow* parent, float initX = 0.0f, float initZ = 0.0f);

    PrecisionPlaceValues GetValues() const;

private:
    wxTextCtrl* m_ctrlX = nullptr;
    wxTextCtrl* m_ctrlZ = nullptr;

    float ParseField(wxTextCtrl* ctrl) const;
};
