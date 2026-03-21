#pragma once
#include <wx/wx.h>
#include <wx/tglbtn.h>
#include "RotateDialog.h"
#include "TranslateDialog.h"
#include "ScaleDialog.h"
#include "StartupDialog.h"
#include <wx/textctrl.h>
#include "FixtureFile.h"
#include "AppConfig.h"
#include "StartupDialog.h"

class GLCanvas;

enum class TransformMode { Select, Translate, Rotate, Scale };

class MainFrame : public wxFrame
{
public:
    MainFrame(const FixtureDefinition& FixtureDefinition);

private:
    // Menu handlers
    void OnImport(wxCommandEvent& evt);
    void OnChangeFixture(wxCommandEvent&);
    void OnExit(wxCommandEvent& evt);

    // Ribbon tool handlers
    void OnToolSelect(wxCommandEvent& evt);
    void OnToolTranslate(wxCommandEvent& evt);
    void OnToolRotate(wxCommandEvent& evt);
    void OnToolScale(wxCommandEvent& evt);
    void OnToolCenter(wxCommandEvent& evt);

    // Activates a tool button and deactivates the others
    void SetActiveTool(TransformMode mode);

    //Creates the left panel
    wxPanel* m_leftPanel = nullptr;
    wxPanel* CreateLeftPanel(wxWindow* parent);

    // Collapsible section helper
    wxPanel* CreateCollapsibleSection(wxWindow* parent, wxSizer* parentSizer,
        const wxString& title, wxPanel** contentOut = nullptr);
    wxPanel* CreateVentsContent(wxWindow* parent);

    // Vent field members
    wxChoice* m_ventTypeChoice = nullptr;
    wxPanel* m_ventDimsPanel = nullptr;
    wxTextCtrl* m_ventLength = nullptr;
    wxTextCtrl* m_ventWidth = nullptr;

    // Creates the top ribbon panel
    wxPanel* CreateRibbon(wxWindow* parent);

    GLCanvas* m_canvas = nullptr;

    wxToggleButton* m_btnTranslate = nullptr;
    wxToggleButton* m_btnRotate = nullptr;
    wxToggleButton* m_btnScale = nullptr;
    wxButton* m_btnCenter = nullptr;  // regular button, not toggle

    wxPanel* m_sidePanel = nullptr;
    wxTextCtrl* m_exportPath = nullptr;

    void OnBrowseExport(wxCommandEvent&);
    void OnExport(wxCommandEvent&);
    void OnGenerateMould(wxCommandEvent&);

    wxPanel* CreateSidePanel(wxWindow* parent);

    enum {
        ID_Import = wxID_HIGHEST + 100,
        ID_ToolSelect,
        ID_ToolTranslate,
        ID_ToolRotate,
        ID_ToolScale,
        ID_ToolCenter,
        ID_BrowseExport,
        ID_Export,
        ID_GenerateMould,
        ID_ChangeFixture
    };
};
