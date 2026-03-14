#pragma once
#include <wx/wx.h>

struct StartupConfig
{
    std::string modelAPath;
    std::string modelBPath;
};

class StartupDialog : public wxDialog
{
public:
    StartupDialog(wxWindow* parent);
    StartupConfig GetConfig() const;

private:
    void OnBrowseA(wxCommandEvent&);
    void OnBrowseB(wxCommandEvent&);
    void OnOK(wxCommandEvent&);

    wxTextCtrl* m_pathA = nullptr;
    wxTextCtrl* m_pathB = nullptr;

    enum { ID_BrowseA = wxID_HIGHEST + 200, ID_BrowseB };
};