#include <wx/filedlg.h>

#include "MainFrame.h"
#include "GLCanvas.h"

MainFrame::MainFrame()
    : wxFrame(nullptr, wxID_ANY, "wxWidgets Modern OpenGL (wxGLCanvas)",
        wxDefaultPosition, wxSize(1000, 700))
{
    auto* fileMenu = new wxMenu();
    fileMenu->Append(ID_Import, "Import...\tCtrl+I");
    fileMenu->AppendSeparator();
    fileMenu->Append(wxID_EXIT, "Exit\tAlt+F4");

    auto* menuBar = new wxMenuBar();
    menuBar->Append(fileMenu, "&File");
    SetMenuBar(menuBar);

    Bind(wxEVT_MENU, &MainFrame::OnExit, this, wxID_EXIT);
    Bind(wxEVT_MENU, &MainFrame::OnImport, this, ID_Import);

    m_canvas = new GLCanvas(this);
}

void MainFrame::OnExit(wxCommandEvent&)
{
    Close(true);
}

void MainFrame::OnImport(wxCommandEvent&)
{
    wxFileDialog dlg(
        this,
        "Import STEP",
        "", "",
        "STEP files (*.step;*.stp)|*.step;*.stp|All files (*.*)|*.*",
        wxFD_OPEN | wxFD_FILE_MUST_EXIST
    );

    if (dlg.ShowModal() != wxID_OK)
        return;

    const wxString path = dlg.GetPath();

    // Call into your canvas/import pipeline
    // (Implement this on GLCanvas to load + upload + Refresh)
    m_canvas->ImportStepFile(path.ToStdString());
}