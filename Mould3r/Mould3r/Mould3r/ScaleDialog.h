#pragma once
#include <wx/wx.h>

struct ScaleValues
{
    float uniform = 1.0f;   // multiplier, e.g. 2.0 = double size
};

class ScaleDialog : public wxDialog
{
public:
    ScaleDialog(wxWindow* parent);
    ScaleValues GetValues() const;

private:
    wxTextCtrl* m_ctrlUniform = nullptr;

    float ParseField(wxTextCtrl* ctrl) const;
};