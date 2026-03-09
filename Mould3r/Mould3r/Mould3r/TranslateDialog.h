#pragma once
#include <wx/wx.h>

struct TranslateValues
{
    float x = 0.0f;   // mm
    float y = 0.0f;
    float z = 0.0f;
};

class TranslateDialog : public wxDialog
{
public:
    TranslateDialog(wxWindow* parent);
    TranslateValues GetValues() const;

private:
    wxTextCtrl* m_ctrlX = nullptr;
    wxTextCtrl* m_ctrlY = nullptr;
    wxTextCtrl* m_ctrlZ = nullptr;

    float ParseField(wxTextCtrl* ctrl) const;
};