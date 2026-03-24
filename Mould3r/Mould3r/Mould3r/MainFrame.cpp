#include <wx/filedlg.h>

#include "MainFrame.h"
#include "GLCanvas.h"
#include "RotateDialog.h"
#include "TranslateDialog.h"
#include "ScaleDialog.h"
#include "AppConfig.h"

// ---------------------------------------------------------------------------
// Ribbon colours
// ---------------------------------------------------------------------------
static const wxColour kRibbonBg(0x1E, 0x22, 0x2A);   // dark navy
static const wxColour kBtnDefault(0x2A, 0x30, 0x3C);   // muted blue-grey
static const wxColour kBtnActive(0x00, 0x7A, 0xCC);   // accent blue
static const wxColour kBtnHover(0x38, 0x42, 0x52);
static const wxColour kTextDefault(0xC8, 0xD0, 0xDC);
static const wxColour kTextActive(0xFF, 0xFF, 0xFF);

// ---------------------------------------------------------------------------
// Helper: style a single toggle button
// ---------------------------------------------------------------------------
static void StyleRibbonBtn(wxToggleButton* btn, bool active = false)
{
    btn->SetBackgroundColour(active ? kBtnActive : kBtnDefault);
    btn->SetForegroundColour(active ? kTextActive : kTextDefault);
    btn->SetFont(wxFont(9, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
        wxFONTWEIGHT_SEMIBOLD, false, "Segoe UI"));
    btn->Refresh();
}

// ---------------------------------------------------------------------------
// MainFrame
// ---------------------------------------------------------------------------
MainFrame::MainFrame(const FixtureDefinition& fixture)
    : wxFrame(nullptr, wxID_ANY, "Mould3r",
        wxDefaultPosition, wxSize(1200, 800))
{
    // ---- Menu bar ----------------------------------------------------------
    auto* fileMenu = new wxMenu();
    fileMenu->Append(ID_Import, "Import...\tCtrl+I");
    fileMenu->Append(ID_ChangeFixture, "Change Fixture...");  // add this
    fileMenu->AppendSeparator();
    fileMenu->Append(wxID_EXIT, "Exit\tAlt+F4");

    auto* menuBar = new wxMenuBar();
    menuBar->Append(fileMenu, "&File");
    SetMenuBar(menuBar);

    Bind(wxEVT_MENU, &MainFrame::OnExit, this, wxID_EXIT);
    Bind(wxEVT_MENU, &MainFrame::OnImport, this, ID_Import);
    Bind(wxEVT_MENU, &MainFrame::OnChangeFixture, this, ID_ChangeFixture);

    // ---- Layout: ribbon on top, canvas below --------------------------------
    auto* root = new wxPanel(this, wxID_ANY);
    root->SetBackgroundColour(kRibbonBg);

    auto* vSizer = new wxBoxSizer(wxVERTICAL);

    wxPanel* ribbon = CreateRibbon(root);

    // 1-px separator line
    auto* sep = new wxPanel(root, wxID_ANY, wxDefaultPosition, wxSize(-1, 1));
    sep->SetBackgroundColour(wxColour(0x00, 0x7A, 0xCC));

    // In MainFrame constructor, replace the vSizer canvas Add with:
    auto* contentSizer = new wxBoxSizer(wxHORIZONTAL);
    m_canvas = new GLCanvas(root);
    m_leftPanel = CreateLeftPanel(root);
    m_sidePanel = CreateSidePanel(root);

    contentSizer->Add(m_leftPanel, 0, wxEXPAND);
    contentSizer->Add(m_canvas, 1, wxEXPAND);
    contentSizer->Add(m_sidePanel, 0, wxEXPAND);

    vSizer->Add(ribbon, 0, wxEXPAND);
    vSizer->Add(sep, 0, wxEXPAND);
    vSizer->Add(contentSizer, 1, wxEXPAND);

    root->SetSizer(vSizer);

    // Frame sizer
    auto* frameSizer = new wxBoxSizer(wxVERTICAL);
    frameSizer->Add(root, 1, wxEXPAND);
    SetSizer(frameSizer);

    // Start with Select active
    SetActiveTool(TransformMode::Select);

    // Load models from startup config
    if (!fixture.modelAPath.empty())
        m_canvas->ImportStepFileAsFixture(fixture.modelAPath);
    if (!fixture.modelBPath.empty())
        m_canvas->ImportStepFileAsFixture(fixture.modelBPath);

    // Set the active injection point (first in the list for now)
    if (!fixture.injectionPoints.empty())
        m_canvas->SetActiveInjectionPoint(fixture.injectionPoints[0]);
}

// ---------------------------------------------------------------------------
// CreateRibbon  – horizontal panel with labelled tool groups
// ---------------------------------------------------------------------------
wxPanel* MainFrame::CreateRibbon(wxWindow* parent)
{
    auto* panel = new wxPanel(parent, wxID_ANY, wxDefaultPosition, wxSize(-1, 48));
    panel->SetBackgroundColour(kRibbonBg);

    auto* hSizer = new wxBoxSizer(wxHORIZONTAL);

    hSizer->AddSpacer(12);

    // Import button
    auto* btnImport = new wxButton(panel, ID_Import, "Import Model",
        wxDefaultPosition, wxSize(110, 32));
    btnImport->SetBackgroundColour(wxColour(0x2A, 0x30, 0x3C));
    btnImport->SetForegroundColour(kTextDefault);
    btnImport->SetFont(wxFont(9, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
        wxFONTWEIGHT_SEMIBOLD, false, "Segoe UI"));
    hSizer->Add(btnImport, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 12);

    // Vertical divider
    auto* importDivider = new wxPanel(panel, wxID_ANY, wxDefaultPosition, wxSize(1, 36));
    importDivider->SetBackgroundColour(wxColour(0x38, 0x44, 0x55));
    hSizer->Add(importDivider, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 16);

    // ----- Group label helper -----------------------------------------------
    auto addLabel = [&](const wxString& text)
        {
            auto* lbl = new wxStaticText(panel, wxID_ANY, text);
            lbl->SetForegroundColour(wxColour(0x55, 0x6A, 0x85));
            lbl->SetFont(wxFont(7, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
                wxFONTWEIGHT_NORMAL, false, "Segoe UI"));
            return lbl;
        };

    // ----- Toggle button helper ---------------------------------------------
    auto addTool = [&](int id, const wxString& label, const wxString& tooltip)
        -> wxToggleButton*
        {
            auto* btn = new wxToggleButton(panel, id, label,
                wxDefaultPosition, wxSize(90, 32));
            btn->SetToolTip(tooltip);
            StyleRibbonBtn(btn, false);
            return btn;
        };

    // Change addTool lambda return type and add a regular button helper
    auto addAction = [&](int id, const wxString& label, const wxString& tooltip)
        -> wxButton*
        {
            auto* btn = new wxButton(panel, id, label,
                wxDefaultPosition, wxSize(90, 32));
            btn->SetToolTip(tooltip);
            btn->SetBackgroundColour(wxColour(0x2A, 0x30, 0x3C));
            btn->SetForegroundColour(kTextDefault);
            btn->SetFont(wxFont(9, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
                wxFONTWEIGHT_SEMIBOLD, false, "Segoe UI"));
            return btn;
        };

    // ---- TRANSFORM group ---------------------------------------------------
    auto* transformSizer = new wxBoxSizer(wxVERTICAL);

    auto* toolRow = new wxBoxSizer(wxHORIZONTAL);

    m_btnTranslate = addTool(ID_ToolTranslate, "+    Translate", "Drag to translate  (LMB)");
    m_btnRotate = addTool(ID_ToolRotate, "O    Rotate", "Drag to rotate  (LMB)");
    m_btnScale = addTool(ID_ToolScale, "<>   Scale", "Drag up/down to scale  (LMB)");
    m_btnCenter = addAction(ID_ToolCenter, "[ ] Center", "Move selected object to origin");

    toolRow->Add(m_btnTranslate, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
    toolRow->Add(m_btnRotate, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
    toolRow->Add(m_btnScale, 0, wxALIGN_CENTER_VERTICAL);
    toolRow->Add(m_btnCenter, 0, wxALIGN_CENTER_VERTICAL);

    transformSizer->Add(toolRow, 0, wxALIGN_CENTER_HORIZONTAL);
    transformSizer->Add(addLabel("TRANSFORM"), 0, wxALIGN_CENTER_HORIZONTAL | wxTOP, 2);

    hSizer->Add(transformSizer, 0, wxALIGN_CENTER_VERTICAL | wxTOP | wxBOTTOM, 6);

    // Vertical divider
    hSizer->AddSpacer(16);
    auto* divider = new wxPanel(panel, wxID_ANY, wxDefaultPosition, wxSize(1, 36));
    divider->SetBackgroundColour(wxColour(0x38, 0x44, 0x55));
    hSizer->Add(divider, 0, wxALIGN_CENTER_VERTICAL);
    hSizer->AddSpacer(16);

    // ---- VENTS group -------------------------------------------------------
    auto* ventsSizer = new wxBoxSizer(wxVERTICAL);
    auto* ventsRow = new wxBoxSizer(wxHORIZONTAL);

    m_btnPlaceVent = addTool(ID_ToolPlaceVent, "Place Vent", "Click a surface to place a vent point");

    ventsRow->Add(m_btnPlaceVent, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);

    auto* btnClearVents = new wxButton(panel, ID_ClearVentPoints, "Clear",
        wxDefaultPosition, wxSize(50, 32));
    btnClearVents->SetToolTip("Remove all vent placement points");
    btnClearVents->SetBackgroundColour(wxColour(0x2A, 0x30, 0x3C));
    btnClearVents->SetForegroundColour(kTextDefault);
    btnClearVents->SetFont(wxFont(9, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
        wxFONTWEIGHT_SEMIBOLD, false, "Segoe UI"));
    ventsRow->Add(btnClearVents, 0, wxALIGN_CENTER_VERTICAL);

    ventsSizer->Add(ventsRow, 0, wxALIGN_CENTER_HORIZONTAL);
    ventsSizer->Add(addLabel("VENTS"), 0, wxALIGN_CENTER_HORIZONTAL | wxTOP, 2);

    hSizer->Add(ventsSizer, 0, wxALIGN_CENTER_VERTICAL | wxTOP | wxBOTTOM, 6);

    // Vertical divider
    hSizer->AddSpacer(16);
    auto* divider2 = new wxPanel(panel, wxID_ANY, wxDefaultPosition, wxSize(1, 36));
    divider2->SetBackgroundColour(wxColour(0x38, 0x44, 0x55));
    hSizer->Add(divider2, 0, wxALIGN_CENTER_VERTICAL);
    hSizer->AddSpacer(16);

    // ---- SPRUES group ------------------------------------------------------
    auto* spruesSizer = new wxBoxSizer(wxVERTICAL);
    auto* spruesRow = new wxBoxSizer(wxHORIZONTAL);

    m_btnPlaceSprue = new wxButton(panel, ID_PlaceSprue, "Place Sprue",
        wxDefaultPosition, wxSize(90, 32));
    m_btnPlaceSprue->SetToolTip("Place a sprue sphere at the active injection point");
    m_btnPlaceSprue->SetBackgroundColour(wxColour(0x2A, 0x30, 0x3C));
    m_btnPlaceSprue->SetForegroundColour(kTextDefault);
    m_btnPlaceSprue->SetFont(wxFont(9, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
        wxFONTWEIGHT_SEMIBOLD, false, "Segoe UI"));
    spruesRow->Add(m_btnPlaceSprue, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);

    auto* btnClearSprue = new wxButton(panel, ID_ClearSprue, "Clear",
        wxDefaultPosition, wxSize(50, 32));
    btnClearSprue->SetToolTip("Remove the placed sprue sphere");
    btnClearSprue->SetBackgroundColour(wxColour(0x2A, 0x30, 0x3C));
    btnClearSprue->SetForegroundColour(kTextDefault);
    btnClearSprue->SetFont(wxFont(9, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
        wxFONTWEIGHT_SEMIBOLD, false, "Segoe UI"));
    spruesRow->Add(btnClearSprue, 0, wxALIGN_CENTER_VERTICAL);

    spruesSizer->Add(spruesRow, 0, wxALIGN_CENTER_HORIZONTAL);
    spruesSizer->Add(addLabel("SPRUES"), 0, wxALIGN_CENTER_HORIZONTAL | wxTOP, 2);

    hSizer->Add(spruesSizer, 0, wxALIGN_CENTER_VERTICAL | wxTOP | wxBOTTOM, 6);

    // Vertical divider
    hSizer->AddSpacer(16);
    auto* divider3 = new wxPanel(panel, wxID_ANY, wxDefaultPosition, wxSize(1, 36));
    divider3->SetBackgroundColour(wxColour(0x38, 0x44, 0x55));
    hSizer->Add(divider3, 0, wxALIGN_CENTER_VERTICAL);
    hSizer->AddSpacer(16);

    // ---- Hint label --------------------------------------------------------
    auto* hint = new wxStaticText(panel, wxID_ANY,
        "LMB: orbit / transform    MMB: pan    Scroll: zoom    Shift+LMB: pan");
    hint->SetForegroundColour(wxColour(0x44, 0x55, 0x66));
    hint->SetFont(wxFont(8, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_ITALIC,
        wxFONTWEIGHT_NORMAL, false, "Segoe UI"));
    hSizer->Add(hint, 0, wxALIGN_CENTER_VERTICAL);

    hSizer->AddStretchSpacer();
    panel->SetSizer(hSizer);

    // ---- Bind toggle events ------------------------------------------------
    Bind(wxEVT_BUTTON, &MainFrame::OnImport, this, ID_Import);
    Bind(wxEVT_TOGGLEBUTTON, &MainFrame::OnToolTranslate, this, ID_ToolTranslate);
    Bind(wxEVT_TOGGLEBUTTON, &MainFrame::OnToolRotate, this, ID_ToolRotate);
    Bind(wxEVT_TOGGLEBUTTON, &MainFrame::OnToolScale, this, ID_ToolScale);
    Bind(wxEVT_BUTTON, &MainFrame::OnToolCenter, this, ID_ToolCenter);
    Bind(wxEVT_TOGGLEBUTTON, &MainFrame::OnToolPlaceVent, this, ID_ToolPlaceVent);
    Bind(wxEVT_BUTTON, &MainFrame::OnClearVentPoints, this, ID_ClearVentPoints);
    Bind(wxEVT_BUTTON, &MainFrame::OnPlaceSprue, this, ID_PlaceSprue);
    Bind(wxEVT_BUTTON, &MainFrame::OnClearSprue, this, ID_ClearSprue);

    return panel;
}

wxPanel* MainFrame::CreateSidePanel(wxWindow* parent)
{
    auto* panel = new wxPanel(parent, wxID_ANY, wxDefaultPosition, wxSize(220, -1));
    panel->SetBackgroundColour(kRibbonBg);

    auto* sizer = new wxBoxSizer(wxVERTICAL);

    // ---- Section label helper ----------------------------------------------
    auto addSection = [&](const wxString& text)
        {
            auto* lbl = new wxStaticText(panel, wxID_ANY, text);
            lbl->SetForegroundColour(wxColour(0x00, 0x7A, 0xCC));
            lbl->SetFont(wxFont(8, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
                wxFONTWEIGHT_BOLD, false, "Segoe UI"));
            sizer->Add(lbl, 0, wxLEFT | wxTOP, 12);

            auto* line = new wxPanel(panel, wxID_ANY, wxDefaultPosition, wxSize(-1, 1));
            line->SetBackgroundColour(wxColour(0x2A, 0x38, 0x4A));
            sizer->Add(line, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 12);
        };

    // ---- Path row helper ---------------------------------------------------
    auto addPathRow = [&](const wxString& label, wxTextCtrl*& ctrl, int browseId)
        {
            auto* lbl = new wxStaticText(panel, wxID_ANY, label);
            lbl->SetForegroundColour(kTextDefault);
            lbl->SetFont(wxFont(8, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
                wxFONTWEIGHT_NORMAL, false, "Segoe UI"));
            sizer->Add(lbl, 0, wxLEFT | wxTOP, 12);

            auto* row = new wxBoxSizer(wxHORIZONTAL);
            ctrl = new wxTextCtrl(panel, wxID_ANY, "",
                wxDefaultPosition, wxSize(140, 24), wxTE_READONLY);
            ctrl->SetBackgroundColour(wxColour(0x2A, 0x30, 0x3C));
            ctrl->SetForegroundColour(kTextDefault);

            auto* browse = new wxButton(panel, browseId, "...",
                wxDefaultPosition, wxSize(28, 24));
            browse->SetBackgroundColour(wxColour(0x2A, 0x30, 0x3C));
            browse->SetForegroundColour(kTextDefault);

            row->Add(ctrl, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
            row->Add(browse, 0, wxALIGN_CENTER_VERTICAL);
            sizer->Add(row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 12);
        };

    // ---- Generate section ----------------------------------------------------
    addSection("MOULD");

    auto* btnGenerate = new wxButton(panel, ID_GenerateMould, "Generate Mould",
        wxDefaultPosition, wxSize(-1, 36));
    btnGenerate->SetBackgroundColour(wxColour(0x1A, 0x6B, 0x3A));   // dark green
    btnGenerate->SetForegroundColour(*wxWHITE);
    btnGenerate->SetFont(wxFont(10, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
        wxFONTWEIGHT_BOLD, false, "Segoe UI"));
    sizer->Add(btnGenerate, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 12);

    Bind(wxEVT_BUTTON, &MainFrame::OnGenerateMould, this, ID_GenerateMould);

    // ---- Export section ----------------------------------------------------
    addSection("EXPORT");
    addPathRow("Output folder:", m_exportPath, ID_BrowseExport);

    // ---- Spacer pushes export button to bottom -----------------------------
    sizer->AddStretchSpacer();

    // 1px separator above button
    auto* bottomLine = new wxPanel(panel, wxID_ANY, wxDefaultPosition, wxSize(-1, 1));
    bottomLine->SetBackgroundColour(wxColour(0x00, 0x7A, 0xCC));
    sizer->Add(bottomLine, 0, wxEXPAND);

    auto* btnExport = new wxButton(panel, ID_Export, "Export",
        wxDefaultPosition, wxSize(-1, 36));
    btnExport->SetBackgroundColour(wxColour(0x00, 0x7A, 0xCC));
    btnExport->SetForegroundColour(*wxWHITE);
    btnExport->SetFont(wxFont(10, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
        wxFONTWEIGHT_BOLD, false, "Segoe UI"));
    sizer->Add(btnExport, 0, wxEXPAND);

    panel->SetSizer(sizer);

    // ---- Binds -------------------------------------------------------------
    Bind(wxEVT_BUTTON, &MainFrame::OnBrowseExport, this, ID_BrowseExport);
    Bind(wxEVT_BUTTON, &MainFrame::OnExport, this, ID_Export);

    return panel;
}
// ---------------------------------------------------------------------------
// SetActiveTool – mutually exclusive toggle + notify canvas
// ---------------------------------------------------------------------------
void MainFrame::SetActiveTool(TransformMode mode)
{
    // Reset all buttons
    StyleRibbonBtn(m_btnTranslate, false); m_btnTranslate->SetValue(false);
    StyleRibbonBtn(m_btnRotate, false);    m_btnRotate->SetValue(false);
    StyleRibbonBtn(m_btnScale, false);     m_btnScale->SetValue(false);
    if (m_btnPlaceVent)
    {
        StyleRibbonBtn(m_btnPlaceVent, false);
        m_btnPlaceVent->SetValue(false);
    }

    // Activate selected
    switch (mode)
    {
    case TransformMode::Translate:
        StyleRibbonBtn(m_btnTranslate, true);  m_btnTranslate->SetValue(true);  break;
    case TransformMode::Rotate:
        StyleRibbonBtn(m_btnRotate, true);     m_btnRotate->SetValue(true);     break;
    case TransformMode::Scale:
        StyleRibbonBtn(m_btnScale, true);      m_btnScale->SetValue(true);      break;
    case TransformMode::PlaceVent:
        if (m_btnPlaceVent)
        {
            StyleRibbonBtn(m_btnPlaceVent, true);
            m_btnPlaceVent->SetValue(true);
        }
        break;
    }

    if (m_canvas)
        m_canvas->SetTransformMode(mode);
}

// ---------------------------------------------------------------------------
// Ribbon button handlers
// ---------------------------------------------------------------------------

void MainFrame::OnToolSelect(wxCommandEvent&) { SetActiveTool(TransformMode::Select); }

void MainFrame::OnToolTranslate(wxCommandEvent&)
{
    SetActiveTool(TransformMode::Select);

    if (!m_canvas) return;

    TranslateDialog dlg(this);
    if (dlg.ShowModal() != wxID_OK)
        return;

    if (!m_canvas->HasSelection())
    {
        return;
    }

    const TranslateValues v = dlg.GetValues();
    m_canvas->ApplyTranslation(v.x, v.y, v.z);
}

void MainFrame::OnToolRotate(wxCommandEvent&)
{
    // Keep the button in its previous visual state — it's a dialog, not a mode toggle
    SetActiveTool(TransformMode::Select);

    if (!m_canvas) return;

    RotateDialog dlg(this);
    if (dlg.ShowModal() != wxID_OK)
        return;

    if (!m_canvas->HasSelection())
    {
        return;
    }

    const RotateValues v = dlg.GetValues();
    m_canvas->ApplyRotation(v.x, v.y, v.z);
}

void MainFrame::OnToolScale(wxCommandEvent&)
{
    SetActiveTool(TransformMode::Select);

    if (!m_canvas) return;

    ScaleDialog dlg(this);
    if (dlg.ShowModal() != wxID_OK)
        return;

    if (!m_canvas->HasSelection())
    {
        return;
    }

    const ScaleValues v = dlg.GetValues();
    m_canvas->ApplyScale(v.uniform);
}

void MainFrame::OnToolCenter(wxCommandEvent&)
{
    if (!m_canvas) return;

    if (!m_canvas->HasSelection())
    {
        return;
    }

    m_canvas->CenterSelectedObject();
}

void MainFrame::OnToolPlaceVent(wxCommandEvent&)
{
    // Toggle: if already in PlaceVent mode, return to Select
    if (m_canvas && m_canvas->GetTransformMode() == TransformMode::PlaceVent)
        SetActiveTool(TransformMode::Select);
    else
        SetActiveTool(TransformMode::PlaceVent);
}

void MainFrame::OnClearVentPoints(wxCommandEvent&)
{
    if (m_canvas)
        m_canvas->ClearVentPoints();
}

void MainFrame::OnPlaceSprue(wxCommandEvent&)
{
    if (m_canvas)
        m_canvas->PlaceSprue();
}

void MainFrame::OnClearSprue(wxCommandEvent&)
{
    if (m_canvas)
        m_canvas->ClearSprue();
}

void MainFrame::GetVentDimensions(float& outLength, float& outWidth,
    float& outOverrunStart, float& outOverrunEnd) const
{
    // Safe parse helper — returns defaultVal if text is empty or non-numeric
    auto parseField = [](wxTextCtrl* ctrl, float defaultVal) -> float
        {
            if (!ctrl) return defaultVal;
            double v = defaultVal;
            if (!ctrl->GetValue().ToDouble(&v)) return defaultVal;
            return (v > 0.0) ? static_cast<float>(v) : defaultVal;
        };

    outLength = parseField(m_ventLength, 5.0f);
    outWidth = parseField(m_ventWidth, 2.0f);
    outOverrunStart = parseField(m_ventOverrunStart, 0.5f);
    outOverrunEnd = parseField(m_ventOverrunEnd, 0.5f);
}

float MainFrame::GetSprueDiameter() const
{
    if (!m_sprueDiameter) return 5.0f;
    double v = 5.0;
    if (!m_sprueDiameter->GetValue().ToDouble(&v)) return 5.0f;
    return (v > 0.0) ? static_cast<float>(v) : 5.0f;
}

// ---------------------------------------------------------------------------
// Menu handlers
// ---------------------------------------------------------------------------
void MainFrame::OnExit(wxCommandEvent&)
{
    Close(true);
}

void MainFrame::OnImport(wxCommandEvent&)
{
    wxFileDialog dlg(
        this, "Import STEP", "", "",
        "STEP files (*.step;*.stp)|*.step;*.stp|All files (*.*)|*.*",
        wxFD_OPEN | wxFD_FILE_MUST_EXIST
    );

    if (dlg.ShowModal() != wxID_OK)
        return;

    m_canvas->ImportStepFile(dlg.GetPath().ToStdString());
}

void MainFrame::OnChangeFixture(wxCommandEvent&)
{
    const std::string lastFixture = AppConfig::LoadLastFixture();

    StartupDialog dlg(this);
    dlg.PreSelectFixture(lastFixture);

    if (dlg.ShowModal() != wxID_OK)
        return;

    FixtureDefinition fixture = dlg.GetFixture();
    AppConfig::SaveLastFixture(fixture.fixturePath);

    // Clear existing fixtures and reload
    m_canvas->ClearFixtures();

    if (!fixture.modelAPath.empty())
        m_canvas->ImportStepFileAsFixture(fixture.modelAPath);
    if (!fixture.modelBPath.empty())
        m_canvas->ImportStepFileAsFixture(fixture.modelBPath);
}

void MainFrame::OnBrowseExport(wxCommandEvent&)
{
    wxDirDialog dlg(this, "Select export folder", "",
        wxDD_DEFAULT_STYLE | wxDD_DIR_MUST_EXIST);
    if (dlg.ShowModal() == wxID_OK)
        m_exportPath->SetValue(dlg.GetPath());
}

void MainFrame::OnExport(wxCommandEvent&)
{
    if (m_exportPath->GetValue().IsEmpty())
    {
        wxMessageBox("Please set an export folder before exporting.",
            "Missing Path", wxOK | wxICON_WARNING, this);
        return;
    }

    const std::string folder = m_exportPath->GetValue().ToStdString();
    m_canvas->ExportFixtures(folder + "/model_a.step",
        folder + "/model_b.step");
}

void MainFrame::OnGenerateMould(wxCommandEvent&)
{
    if (!m_canvas) return;
    m_canvas->GenerateMould();
}

wxPanel* MainFrame::CreateCollapsibleSection(wxWindow* parent,
    wxSizer* parentSizer,
    const wxString& title,
    wxPanel** contentOut)
{
    auto* headerBtn = new wxToggleButton(parent, wxID_ANY, "v  " + title,
        wxDefaultPosition, wxSize(-1, 28),
        wxBU_LEFT);
    headerBtn->SetValue(true);
    headerBtn->SetBackgroundColour(wxColour(0x25, 0x2B, 0x36));
    headerBtn->SetForegroundColour(wxColour(0x00, 0x7A, 0xCC));
    headerBtn->SetFont(wxFont(8, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
        wxFONTWEIGHT_BOLD, false, "Segoe UI"));
    parentSizer->Add(headerBtn, 0, wxEXPAND | wxTOP, 4);

    // Use custom content if provided, otherwise build placeholder
    wxPanel* content = nullptr;
    if (contentOut && *contentOut)
    {
        content = *contentOut;
        parentSizer->Add(content, 0, wxEXPAND);
    }
    else
    {
        content = new wxPanel(parent, wxID_ANY);
        content->SetBackgroundColour(kRibbonBg);

        auto* cs = new wxBoxSizer(wxVERTICAL);
        auto* placeholder = new wxStaticText(content, wxID_ANY,
            "Add options later");
        placeholder->SetForegroundColour(wxColour(0x44, 0x55, 0x66));
        placeholder->SetFont(wxFont(8, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_ITALIC,
            wxFONTWEIGHT_NORMAL, false, "Segoe UI"));
        cs->Add(placeholder, 0, wxALL, 10);
        content->SetSizer(cs);
        parentSizer->Add(content, 0, wxEXPAND);

        if (contentOut) *contentOut = content;
    }

    wxPanel* contentRef = content;
    headerBtn->Bind(wxEVT_TOGGLEBUTTON, [headerBtn, contentRef,
        parent, title](wxCommandEvent&)
        {
            const bool expanded = headerBtn->GetValue();
            headerBtn->SetLabel((expanded ? "v  " : ">  ") + title);
            contentRef->Show(expanded);
            parent->Layout();
            parent->GetParent()->Layout();
        });

    return content;
}

wxPanel* MainFrame::CreateVentsContent(wxWindow* parent)
{
    auto* panel = new wxPanel(parent, wxID_ANY);
    panel->SetBackgroundColour(kRibbonBg);

    auto* sizer = new wxBoxSizer(wxVERTICAL);

    // ---- Vent type dropdown ------------------------------------------------
    auto* typeLabel = new wxStaticText(panel, wxID_ANY, "Vent type:");
    typeLabel->SetForegroundColour(kTextDefault);
    typeLabel->SetFont(wxFont(8, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
        wxFONTWEIGHT_NORMAL, false, "Segoe UI"));
    sizer->Add(typeLabel, 0, wxLEFT | wxTOP, 10);

    m_ventTypeChoice = new wxChoice(panel, wxID_ANY);
    m_ventTypeChoice->SetBackgroundColour(wxColour(0x2A, 0x30, 0x3C));
    m_ventTypeChoice->SetForegroundColour(kTextDefault);
    m_ventTypeChoice->Append("Rectangular");
    m_ventTypeChoice->SetSelection(0);
    sizer->Add(m_ventTypeChoice, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 10);

    // ---- Dimensions panel (shown when Rectangular selected) ----------------
    m_ventDimsPanel = new wxPanel(panel, wxID_ANY);
    m_ventDimsPanel->SetBackgroundColour(kRibbonBg);

    auto* dimsSizer = new wxBoxSizer(wxVERTICAL);

    // Helper: labelled mm field
    auto addDimRow = [&](const wxString& label, wxTextCtrl*& ctrl,
        const wxString& defaultVal)
        {
            auto* row = new wxBoxSizer(wxHORIZONTAL);

            auto* lbl = new wxStaticText(m_ventDimsPanel, wxID_ANY, label,
                wxDefaultPosition, wxSize(90, -1));
            lbl->SetForegroundColour(kTextDefault);
            lbl->SetFont(wxFont(8, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
                wxFONTWEIGHT_NORMAL, false, "Segoe UI"));

            ctrl = new wxTextCtrl(m_ventDimsPanel, wxID_ANY, defaultVal,
                wxDefaultPosition, wxSize(70, 22));
            ctrl->SetBackgroundColour(wxColour(0x2A, 0x30, 0x3C));
            ctrl->SetForegroundColour(kTextDefault);

            auto* unit = new wxStaticText(m_ventDimsPanel, wxID_ANY, "mm");
            unit->SetForegroundColour(wxColour(0x55, 0x6A, 0x85));
            unit->SetFont(wxFont(8, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
                wxFONTWEIGHT_NORMAL, false, "Segoe UI"));

            row->Add(lbl, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
            row->Add(ctrl, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
            row->Add(unit, 0, wxALIGN_CENTER_VERTICAL);

            dimsSizer->Add(row, 0, wxLEFT | wxTOP, 10);
        };

    addDimRow("Length:", m_ventLength, "5.0");
    addDimRow("Width:", m_ventWidth, "2.0");
    addDimRow("Overrun (start):", m_ventOverrunStart, "0.5");
    addDimRow("Overrun (end):", m_ventOverrunEnd, "0.5");

    m_ventDimsPanel->SetSizer(dimsSizer);
    sizer->Add(m_ventDimsPanel, 0, wxEXPAND | wxBOTTOM, 10);

    panel->SetSizer(sizer);

    // Show/hide dims based on type selection
    m_ventTypeChoice->Bind(wxEVT_CHOICE, [this](wxCommandEvent&)
        {
            m_ventDimsPanel->Show(m_ventTypeChoice->GetStringSelection() == "Rectangular");
            m_ventDimsPanel->GetParent()->Layout();
            m_ventDimsPanel->GetParent()->GetParent()->Layout();
        });

    return panel;
}

wxPanel* MainFrame::CreateSpruesContent(wxWindow* parent)
{
    auto* panel = new wxPanel(parent, wxID_ANY);
    panel->SetBackgroundColour(kRibbonBg);

    auto* sizer = new wxBoxSizer(wxVERTICAL);

    // ---- Sprue type dropdown -----------------------------------------------
    auto* typeLabel = new wxStaticText(panel, wxID_ANY, "Sprue type:");
    typeLabel->SetForegroundColour(kTextDefault);
    typeLabel->SetFont(wxFont(8, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
        wxFONTWEIGHT_NORMAL, false, "Segoe UI"));
    sizer->Add(typeLabel, 0, wxLEFT | wxTOP, 10);

    m_sprueTypeChoice = new wxChoice(panel, wxID_ANY);
    m_sprueTypeChoice->SetBackgroundColour(wxColour(0x2A, 0x30, 0x3C));
    m_sprueTypeChoice->SetForegroundColour(kTextDefault);
    m_sprueTypeChoice->Append("Cylinder");
    m_sprueTypeChoice->SetSelection(0);
    sizer->Add(m_sprueTypeChoice, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 10);

    // ---- Dimensions panel (shown for Cylinder) -----------------------------
    auto* dimsPanel = new wxPanel(panel, wxID_ANY);
    dimsPanel->SetBackgroundColour(kRibbonBg);

    auto* dimsSizer = new wxBoxSizer(wxVERTICAL);

    // Diameter row
    {
        auto* row = new wxBoxSizer(wxHORIZONTAL);

        auto* lbl = new wxStaticText(dimsPanel, wxID_ANY, "Diameter:",
            wxDefaultPosition, wxSize(60, -1));
        lbl->SetForegroundColour(kTextDefault);
        lbl->SetFont(wxFont(8, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
            wxFONTWEIGHT_NORMAL, false, "Segoe UI"));

        m_sprueDiameter = new wxTextCtrl(dimsPanel, wxID_ANY, "5.0",
            wxDefaultPosition, wxSize(70, 22));
        m_sprueDiameter->SetBackgroundColour(wxColour(0x2A, 0x30, 0x3C));
        m_sprueDiameter->SetForegroundColour(kTextDefault);

        auto* unit = new wxStaticText(dimsPanel, wxID_ANY, "mm");
        unit->SetForegroundColour(wxColour(0x55, 0x6A, 0x85));
        unit->SetFont(wxFont(8, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
            wxFONTWEIGHT_NORMAL, false, "Segoe UI"));

        row->Add(lbl, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
        row->Add(m_sprueDiameter, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
        row->Add(unit, 0, wxALIGN_CENTER_VERTICAL);
        dimsSizer->Add(row, 0, wxLEFT | wxTOP, 10);
    }

    dimsPanel->SetSizer(dimsSizer);
    sizer->Add(dimsPanel, 0, wxEXPAND | wxBOTTOM, 10);

    panel->SetSizer(sizer);

    // Show/hide dims based on type selection (future-proofed for more types)
    m_sprueTypeChoice->Bind(wxEVT_CHOICE, [dimsPanel, this](wxCommandEvent&)
        {
            dimsPanel->Show(m_sprueTypeChoice->GetStringSelection() == "Cylinder");
            dimsPanel->GetParent()->Layout();
            dimsPanel->GetParent()->GetParent()->Layout();
        });

    return panel;
}

wxPanel* MainFrame::CreateLeftPanel(wxWindow* parent)
{
    auto* panel = new wxPanel(parent, wxID_ANY,
        wxDefaultPosition, wxSize(180, -1));
    panel->SetBackgroundColour(kRibbonBg);

    auto* sizer = new wxBoxSizer(wxVERTICAL);

    // ---- Panel title -------------------------------------------------------
    auto* title = new wxStaticText(panel, wxID_ANY, "MOULD TOOLS");
    title->SetForegroundColour(wxColour(0x44, 0x55, 0x66));
    title->SetFont(wxFont(7, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
        wxFONTWEIGHT_BOLD, false, "Segoe UI"));
    sizer->Add(title, 0, wxLEFT | wxTOP, 12);

    auto* titleLine = new wxPanel(panel, wxID_ANY,
        wxDefaultPosition, wxSize(-1, 1));
    titleLine->SetBackgroundColour(wxColour(0x2A, 0x38, 0x4A));
    sizer->Add(titleLine, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 10);

    sizer->AddSpacer(4);

    // ---- Collapsible sections ----------------------------------------------
    // Sprues — custom content
    wxPanel* spruesContent = CreateSpruesContent(panel);
    CreateCollapsibleSection(panel, sizer, "Sprues", &spruesContent);
    CreateCollapsibleSection(panel, sizer, "Runners");
    CreateCollapsibleSection(panel, sizer, "Gates");
    // Vents — custom content
    wxPanel* ventsContent = CreateVentsContent(panel);
    CreateCollapsibleSection(panel, sizer, "Vents", &ventsContent);

    sizer->AddStretchSpacer();

    // Right border line
    auto* borderLine = new wxPanel(panel, wxID_ANY,
        wxDefaultPosition, wxSize(1, -1));
    borderLine->SetBackgroundColour(wxColour(0x2A, 0x38, 0x4A));
    sizer->Add(borderLine, 0, wxEXPAND | wxRIGHT, 0);

    panel->SetSizer(sizer);
    return panel;
}