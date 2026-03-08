#pragma once
#include <wx/wx.h>

class GLCanvas;

class MainFrame : public wxFrame
{
public:
    MainFrame();

private:
    void OnImport(wxCommandEvent& evt);
    void OnExit(wxCommandEvent& evt);

    GLCanvas* m_canvas = nullptr;

    enum {
        ID_Import = wxID_HIGHEST + 100
    };
};
