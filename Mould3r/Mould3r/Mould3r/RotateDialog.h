#pragma once
#include <wx/wx.h>
#include <wx/spinctrl.h>

// Result struct filled in by the dialog
struct RotateValues
{
    float x = 0.0f;   // degrees around X
    float y = 0.0f;   // degrees around Y
    float z = 0.0f;   // degrees around Z
};

class RotateDialog : public wxDialog
{
public:
    RotateDialog(wxWindow* parent);

    // Call after ShowModal() == wxID_OK
    RotateValues GetValues() const;

private:
    wxTextCtrl* m_ctrlX = nullptr;
    wxTextCtrl* m_ctrlY = nullptr;
    wxTextCtrl* m_ctrlZ = nullptr;

    float ParseField(wxTextCtrl* ctrl) const;
};
