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
#include "ProjectFile.h"

class GLCanvas;

enum class TransformMode { Select, Translate, Rotate, Scale, PlaceVent, PlaceRunner, PlaceGate, RemoveVent, RemoveRunner, RemoveGate, RemoveSprue, EditVent, EditRunner, EditGate };

class MainFrame : public wxFrame
{
public:
    MainFrame(const FixtureDefinition& FixtureDefinition);

    // Called by GLCanvas when Escape is pressed to sync button state
    void SetActiveTool(TransformMode mode);

    // Read current vent dimensions from the left-panel UI fields
    void GetVentDimensions(float& outLength, float& outWidth,
        float& outOverrunStart, float& outOverrunEnd) const;

    // Read current sprue dimensions from the left-panel UI fields
    float GetSprueDiameter() const;
    float GetSprueDraftAngle() const;
    float GetSprueColdSlugDepth() const;
    float GetRunnerColdPlugDist() const;

    float GetRunnerDiameter() const;

    float GetGateDiameter() const;
    float GetGateDraftAngle() const;
    float GetSubRunnerDiameter() const;

    // Project save/load support
    const FixtureDefinition& GetFixtureDefinition() const { return m_fixtureDef; }
    GLCanvas* GetCanvas() const { return m_canvas; }

    // Set UI field values (used when restoring a project)
    void SetParameterFields(const ProjectParameters& params);

private:
    // Menu handlers
    void OnImport(wxCommandEvent& evt);
    void OnChangeFixture(wxCommandEvent&);
    void OnExit(wxCommandEvent& evt);
    void OnSaveProject(wxCommandEvent&);
    void OnLoadProject(wxCommandEvent&);
    void OnNewProject(wxCommandEvent&);

    // Ribbon tool handlers
    void OnToolSelect(wxCommandEvent& evt);
    void OnToolTranslate(wxCommandEvent& evt);
    void OnToolRotate(wxCommandEvent& evt);
    void OnToolScale(wxCommandEvent& evt);
    void OnToolCenter(wxCommandEvent& evt);
    void OnToolPlaceVent(wxCommandEvent& evt);

    void OnPlaceSprue(wxCommandEvent&);
    void OnClearSprue(wxCommandEvent&);

    void OnPlaceRunner(wxCommandEvent& evt);
    void OnClearRunners(wxCommandEvent&);

    void OnPlaceGate(wxCommandEvent& evt);
    void OnClearGates(wxCommandEvent&);

    void OnRemoveVent(wxCommandEvent&);
    void OnRemoveSprue(wxCommandEvent&);
    void OnRemoveRunner(wxCommandEvent&);
    void OnRemoveGate(wxCommandEvent&);

    void OnEditVent(wxCommandEvent&);
    void OnEditRunner(wxCommandEvent&);
    void OnEditGate(wxCommandEvent&);

    // Activates a tool button and deactivates the others (also called by GLCanvas on Escape)

    //Creates the left panel
    wxPanel* m_leftPanel = nullptr;
    wxPanel* CreateLeftPanel(wxWindow* parent);

    // Collapsible section helper
    wxPanel* CreateCollapsibleSection(wxWindow* parent, wxSizer* parentSizer,
        const wxString& title, wxPanel** contentOut = nullptr);
    wxPanel* CreateVentsContent(wxWindow* parent);
    wxPanel* CreateSpruesContent(wxWindow* parent);
    wxPanel* CreateRunnersContent(wxWindow* parent);
    wxPanel* CreateGatesContent(wxWindow* parent);

    // Vent field members
    wxChoice* m_ventTypeChoice = nullptr;
    wxPanel* m_ventDimsPanel = nullptr;
    wxTextCtrl* m_ventLength = nullptr;
    wxTextCtrl* m_ventWidth = nullptr;
    wxTextCtrl* m_ventOverrunStart = nullptr;
    wxTextCtrl* m_ventOverrunEnd = nullptr;

    // Sprue field members
    wxChoice* m_sprueTypeChoice = nullptr;
    wxTextCtrl* m_sprueDiameter = nullptr;
    wxTextCtrl* m_sprueDraftAngle = nullptr;
    wxTextCtrl* m_sprueColdSlugDepth = nullptr;

    // Runner field members
    wxChoice* m_runnerTypeChoice = nullptr;
    wxTextCtrl* m_runnerDiameter = nullptr;
    wxTextCtrl* m_runnerColdSlugDepth = nullptr;

    // Gate field members
    wxChoice* m_gateTypeChoice = nullptr;
    wxTextCtrl* m_gateDiameter = nullptr;
    wxTextCtrl* m_gateDraftAngle = nullptr;

    // Sub-runner field members (within the Gates section)
    wxChoice* m_subRunnerTypeChoice = nullptr;
    wxTextCtrl* m_subRunnerDiameter = nullptr;

    // Creates the top ribbon panel
    wxPanel* CreateRibbon(wxWindow* parent);

    GLCanvas* m_canvas = nullptr;

    // Stored fixture definition (for project save/load)
    FixtureDefinition m_fixtureDef;

    // Current project file path (empty if unsaved)
    std::string m_projectPath;

    // Transform tool buttons
    wxToggleButton* m_btnTranslate = nullptr;
    wxToggleButton* m_btnRotate = nullptr;
    wxToggleButton* m_btnScale = nullptr;
    wxButton* m_btnCenter = nullptr;

    // Vent tool button (ribbon — Vents group)
    wxToggleButton* m_btnPlaceVent = nullptr;

    // Sprue tool buttons (ribbon — Sprues group)
    wxButton* m_btnPlaceSprue = nullptr;

    // Runner tool button (ribbon — Runners group)
    wxToggleButton* m_btnPlaceRunner = nullptr;

    // Gate tool button (ribbon — Gates group)
    wxToggleButton* m_btnPlaceGate = nullptr;

    wxPanel* m_sidePanel = nullptr;
    wxTextCtrl* m_exportPath = nullptr;

    void OnBrowseExport(wxCommandEvent&);
    void OnExport(wxCommandEvent&);
    void OnGenerateMould(wxCommandEvent&);
    void OnClearVentPoints(wxCommandEvent&);

    wxPanel* CreateSidePanel(wxWindow* parent);

    enum {
        ID_Import = wxID_HIGHEST + 100,
        ID_ToolSelect,
        ID_ToolTranslate,
        ID_ToolRotate,
        ID_ToolScale,
        ID_ToolCenter,
        ID_ToolPlaceVent,
        ID_BrowseExport,
        ID_Export,
        ID_GenerateMould,
        ID_ChangeFixture,
        ID_ClearVentPoints,
        ID_PlaceSprue,
        ID_ClearSprue,
        ID_PlaceRunner,
        ID_ClearRunners,
        ID_PlaceGate,
        ID_ClearGates,
        ID_RemoveVent,
        ID_RemoveSprue,
        ID_RemoveRunner,
        ID_RemoveGate,
        ID_EditVent,
        ID_EditRunner,
        ID_EditGate,
        ID_SaveProject,
        ID_LoadProject,
        ID_NewProject
    };
};
