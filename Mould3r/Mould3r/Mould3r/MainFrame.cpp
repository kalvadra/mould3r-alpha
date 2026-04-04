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
    wxPanel* divider = new wxPanel(panel, wxID_ANY, wxDefaultPosition, wxSize(1, 36));
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

    // ---- GATES group -------------------------------------------------------
    auto* gatesSizer = new wxBoxSizer(wxVERTICAL);
    auto* gatesRow = new wxBoxSizer(wxHORIZONTAL);

    m_btnPlaceGate = addTool(ID_PlaceGate, "Place Gate", "Click the part perimeter at the parting plane to place a gate");

    gatesRow->Add(m_btnPlaceGate, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);

    auto* btnClearGates = new wxButton(panel, ID_ClearGates, "Clear",
        wxDefaultPosition, wxSize(50, 32));
    btnClearGates->SetToolTip("Remove all gate placement points");
    btnClearGates->SetBackgroundColour(wxColour(0x2A, 0x30, 0x3C));
    btnClearGates->SetForegroundColour(kTextDefault);
    btnClearGates->SetFont(wxFont(9, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
        wxFONTWEIGHT_SEMIBOLD, false, "Segoe UI"));
    gatesRow->Add(btnClearGates, 0, wxALIGN_CENTER_VERTICAL);

    gatesSizer->Add(gatesRow, 0, wxALIGN_CENTER_HORIZONTAL);
    gatesSizer->Add(addLabel("GATES"), 0, wxALIGN_CENTER_HORIZONTAL | wxTOP, 2);

    hSizer->Add(gatesSizer, 0, wxALIGN_CENTER_VERTICAL | wxTOP | wxBOTTOM, 6);

    // Vertical divider
    hSizer->AddSpacer(16);
    auto* divider5 = new wxPanel(panel, wxID_ANY, wxDefaultPosition, wxSize(1, 36));
    divider5->SetBackgroundColour(wxColour(0x38, 0x44, 0x55));
    hSizer->Add(divider5, 0, wxALIGN_CENTER_VERTICAL);
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
    Bind(wxEVT_BUTTON, &MainFrame::OnPlaceRunner, this, ID_PlaceRunner);
    Bind(wxEVT_BUTTON, &MainFrame::OnClearRunners, this, ID_ClearRunners);
    Bind(wxEVT_TOGGLEBUTTON, &MainFrame::OnPlaceGate, this, ID_PlaceGate);
    Bind(wxEVT_BUTTON, &MainFrame::OnClearGates, this, ID_ClearGates);

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
    if (m_btnPlaceRunner)
    {
        StyleRibbonBtn(m_btnPlaceRunner, false);
        m_btnPlaceRunner->SetValue(false);
    }
    if (m_btnPlaceGate)
    {
        StyleRibbonBtn(m_btnPlaceGate, false);
        m_btnPlaceGate->SetValue(false);
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
    case TransformMode::PlaceRunner:
        if (m_btnPlaceRunner)
        {
            StyleRibbonBtn(m_btnPlaceRunner, true);
            m_btnPlaceRunner->SetValue(true);
        }
        break;
    case TransformMode::PlaceGate:
        if (m_btnPlaceGate)
        {
            StyleRibbonBtn(m_btnPlaceGate, true);
            m_btnPlaceGate->SetValue(true);
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

void MainFrame::OnPlaceRunner(wxCommandEvent&)
{
    // Toggle: if already in PlaceRunner mode, return to Select
    if (m_canvas && m_canvas->GetTransformMode() == TransformMode::PlaceRunner)
        SetActiveTool(TransformMode::Select);
    else
        SetActiveTool(TransformMode::PlaceRunner);
}

void MainFrame::OnClearRunners(wxCommandEvent&)
{
    if (m_canvas)
        m_canvas->ClearRunnerPoints();
}

void MainFrame::OnPlaceGate(wxCommandEvent&)
{
    // Toggle: if already in PlaceGate mode, return to Select
    if (m_canvas && m_canvas->GetTransformMode() == TransformMode::PlaceGate)
        SetActiveTool(TransformMode::Select);
    else
        SetActiveTool(TransformMode::PlaceGate);
}

void MainFrame::OnClearGates(wxCommandEvent&)
{
    if (m_canvas)
        m_canvas->ClearGatePoints();
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

float MainFrame::GetSprueDraftAngle() const
{
    if (!m_sprueDraftAngle) return 1.0f;
    double v = 1.0;
    if (!m_sprueDraftAngle->GetValue().ToDouble(&v)) return 1.0f;
    if (v < 0.0) v = 0.0;
    if (v > 45.0) v = 45.0;
    return static_cast<float>(v);
}

float MainFrame::GetSprueColdSlugDepth() const
{
    if (!m_sprueColdSlugDepth) return 5.0f;
    double v = 5.0;
    if (!m_sprueColdSlugDepth->GetValue().ToDouble(&v)) return 5.0f;
    if (v < 0.0) v = 0.0;
    return static_cast<float>(v);
}

float MainFrame::GetRunnerDiameter() const
{
    if (!m_runnerDiameter) return 5.0f;
    double v = 5.0;
    if (!m_runnerDiameter->GetValue().ToDouble(&v)) return 5.0f;
    return (v > 0.0) ? static_cast<float>(v) : 5.0f;
}

float MainFrame::GetRunnerColdPlugDist() const
{
    if (!m_runnerColdSlugDepth) return 5.0f;
    double v = 5.0;
    if (!m_runnerColdSlugDepth->GetValue().ToDouble(&v)) return 5.0f;
    if (v < 0.0) v = 0.0;
    return static_cast<float>(v);
}

float MainFrame::GetGateDiameter() const
{
    if (!m_gateDiameter) return 3.0f;
    double v = 3.0;
    if (!m_gateDiameter->GetValue().ToDouble(&v)) return 3.0f;
    if (v < 0.0) v = 0.0;
    return static_cast<float>(v);
}

float MainFrame::GetGateDraftAngle() const
{
    if (!m_gateDraftAngle) return 1.0f;
    double v = 1.0;
    if (!m_gateDraftAngle->GetValue().ToDouble(&v)) return 1.0f;
    if (v < 0.0) v = 0.0;
    return static_cast<float>(v);
}

float MainFrame::GetSubRunnerDiameter() const
{
    if (!m_subRunnerDiameter) return 5.0f;
    double v = 5.0;
    if (!m_subRunnerDiameter->GetValue().ToDouble(&v)) return 5.0f;
    if (v <= 0.0) v = 5.0;
    return static_cast<float>(v);
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

    // Draft angle row
    {
        auto* row = new wxBoxSizer(wxHORIZONTAL);

        auto* lbl = new wxStaticText(dimsPanel, wxID_ANY, "Draft angle:",
            wxDefaultPosition, wxSize(60, -1));
        lbl->SetForegroundColour(kTextDefault);
        lbl->SetFont(wxFont(8, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
            wxFONTWEIGHT_NORMAL, false, "Segoe UI"));

        m_sprueDraftAngle = new wxTextCtrl(dimsPanel, wxID_ANY, "1.0",
            wxDefaultPosition, wxSize(70, 22));
        m_sprueDraftAngle->SetBackgroundColour(wxColour(0x2A, 0x30, 0x3C));
        m_sprueDraftAngle->SetForegroundColour(kTextDefault);

        auto* unit = new wxStaticText(dimsPanel, wxID_ANY, "\xC2\xB0");
        unit->SetForegroundColour(wxColour(0x55, 0x6A, 0x85));
        unit->SetFont(wxFont(8, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
            wxFONTWEIGHT_NORMAL, false, "Segoe UI"));

        row->Add(lbl, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
        row->Add(m_sprueDraftAngle, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
        row->Add(unit, 0, wxALIGN_CENTER_VERTICAL);
        dimsSizer->Add(row, 0, wxLEFT | wxTOP, 10);
    }

    // Cold slug depth row
    {
        auto* row = new wxBoxSizer(wxHORIZONTAL);

        auto* lbl = new wxStaticText(dimsPanel, wxID_ANY, "Cold slug:",
            wxDefaultPosition, wxSize(60, -1));
        lbl->SetForegroundColour(kTextDefault);
        lbl->SetFont(wxFont(8, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
            wxFONTWEIGHT_NORMAL, false, "Segoe UI"));

        m_sprueColdSlugDepth = new wxTextCtrl(dimsPanel, wxID_ANY, "5.0",
            wxDefaultPosition, wxSize(70, 22));
        m_sprueColdSlugDepth->SetBackgroundColour(wxColour(0x2A, 0x30, 0x3C));
        m_sprueColdSlugDepth->SetForegroundColour(kTextDefault);

        auto* unit = new wxStaticText(dimsPanel, wxID_ANY, "mm");
        unit->SetForegroundColour(wxColour(0x55, 0x6A, 0x85));
        unit->SetFont(wxFont(8, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
            wxFONTWEIGHT_NORMAL, false, "Segoe UI"));

        row->Add(lbl, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
        row->Add(m_sprueColdSlugDepth, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
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

wxPanel* MainFrame::CreateRunnersContent(wxWindow* parent)
{
    // Local colours sampled from the reference mockup
    static const wxColour kPanelBg(0x2D, 0x31, 0x40);        // card background
    static const wxColour kPlaceBtnBg(0x4C, 0x53, 0x70);     // muted indigo
    static const wxColour kSmallBtnBg(0x3B, 0x40, 0x52);     // subtle raised grey
    static const wxColour kSmallBtnText(0xB0, 0xB8, 0xC8);   // muted label
    static const wxColour kSettingsText(0x9A, 0xA0, 0xB0);   // dimmer sub-header

    auto* panel = new wxPanel(parent, wxID_ANY);
    panel->SetBackgroundColour(kPanelBg);

    auto* sizer = new wxBoxSizer(wxVERTICAL);

    // ---- "Runners" section title (inside the card) --------------------------
    auto* runnersLabel = new wxStaticText(panel, wxID_ANY, "Runners");
    runnersLabel->SetForegroundColour(*wxWHITE);
    runnersLabel->SetFont(wxFont(9, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
        wxFONTWEIGHT_BOLD, false, "Segoe UI"));
    sizer->Add(runnersLabel, 0, wxLEFT | wxTOP, 12);

    sizer->AddSpacer(6);

    // ---- "Place Runner" action button (full-width, muted indigo) ------------
    auto* btnPlace = new wxButton(panel, ID_PlaceRunner, "Place Runner",
        wxDefaultPosition, wxSize(-1, 32), wxBORDER_NONE);
    btnPlace->SetBackgroundColour(kPlaceBtnBg);
    btnPlace->SetForegroundColour(*wxWHITE);
    btnPlace->SetFont(wxFont(9, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
        wxFONTWEIGHT_SEMIBOLD, false, "Segoe UI"));
    sizer->Add(btnPlace, 0, wxEXPAND | wxLEFT | wxRIGHT, 12);

    sizer->AddSpacer(6);

    // ---- Edit / Remove / Clear all — equal-width button row -----------------
    auto* actionGrid = new wxGridSizer(1, 3, 0, 4);   // 1 row, 3 cols, 4px h-gap

    auto makeSmallBtn = [&](const wxString& label) -> wxButton*
        {
            auto* btn = new wxButton(panel, wxID_ANY, label,
                wxDefaultPosition, wxSize(-1, 26), wxBORDER_NONE);
            btn->SetBackgroundColour(kSmallBtnBg);
            btn->SetForegroundColour(kSmallBtnText);
            btn->SetFont(wxFont(8, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
                wxFONTWEIGHT_NORMAL, false, "Segoe UI"));
            return btn;
        };

    auto* btnEdit = makeSmallBtn("Edit");
    auto* btnRemove = makeSmallBtn("Remove");
    auto* btnClearAll = makeSmallBtn("Clear all");
    btnClearAll->SetId(ID_ClearRunners);

    actionGrid->Add(btnEdit, 0, wxEXPAND);
    actionGrid->Add(btnRemove, 0, wxEXPAND);
    actionGrid->Add(btnClearAll, 0, wxEXPAND);
    sizer->Add(actionGrid, 0, wxEXPAND | wxLEFT | wxRIGHT, 12);

    sizer->AddSpacer(8);

    // ---- Collapsible "Settings" sub-section ---------------------------------
    auto* settingsBtn = new wxToggleButton(panel, wxID_ANY,
        wxString::FromUTF8("Settings      \xe2\x96\xbe"),   // ▾ chevron
        wxDefaultPosition, wxSize(-1, 22), wxBU_LEFT | wxBORDER_NONE);
    settingsBtn->SetValue(true);
    settingsBtn->SetBackgroundColour(kPanelBg);
    settingsBtn->SetForegroundColour(kSettingsText);
    settingsBtn->SetFont(wxFont(9, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
        wxFONTWEIGHT_NORMAL, false, "Segoe UI"));
    sizer->Add(settingsBtn, 0, wxEXPAND | wxLEFT | wxRIGHT, 12);

    // Settings content panel (contains the existing type/dimension fields)
    auto* settingsPanel = new wxPanel(panel, wxID_ANY);
    settingsPanel->SetBackgroundColour(kPanelBg);
    auto* settingsSizer = new wxBoxSizer(wxVERTICAL);

    // ---- Runner type dropdown -----------------------------------------------
    auto* typeLabel = new wxStaticText(settingsPanel, wxID_ANY, "Runner type:");
    typeLabel->SetForegroundColour(kSmallBtnText);
    typeLabel->SetFont(wxFont(8, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
        wxFONTWEIGHT_NORMAL, false, "Segoe UI"));
    settingsSizer->Add(typeLabel, 0, wxLEFT | wxTOP, 10);

    m_runnerTypeChoice = new wxChoice(settingsPanel, wxID_ANY);
    m_runnerTypeChoice->SetBackgroundColour(kSmallBtnBg);
    m_runnerTypeChoice->SetForegroundColour(kSmallBtnText);
    m_runnerTypeChoice->Append("Cylindrical");
    m_runnerTypeChoice->SetSelection(0);
    settingsSizer->Add(m_runnerTypeChoice, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 10);

    // ---- Dimensions panel (shown for Cylindrical) ---------------------------
    auto* dimsPanel = new wxPanel(settingsPanel, wxID_ANY);
    dimsPanel->SetBackgroundColour(kPanelBg);

    auto* dimsSizer = new wxBoxSizer(wxVERTICAL);

    // Diameter row
    {
        auto* row = new wxBoxSizer(wxHORIZONTAL);

        auto* lbl = new wxStaticText(dimsPanel, wxID_ANY, "Diameter:",
            wxDefaultPosition, wxSize(60, -1));
        lbl->SetForegroundColour(kSmallBtnText);
        lbl->SetFont(wxFont(8, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
            wxFONTWEIGHT_NORMAL, false, "Segoe UI"));

        m_runnerDiameter = new wxTextCtrl(dimsPanel, wxID_ANY, "4.0",
            wxDefaultPosition, wxSize(70, 22));
        m_runnerDiameter->SetBackgroundColour(kSmallBtnBg);
        m_runnerDiameter->SetForegroundColour(kTextDefault);

        auto* unit = new wxStaticText(dimsPanel, wxID_ANY, "mm");
        unit->SetForegroundColour(kSettingsText);
        unit->SetFont(wxFont(8, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
            wxFONTWEIGHT_NORMAL, false, "Segoe UI"));

        row->Add(lbl, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
        row->Add(m_runnerDiameter, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
        row->Add(unit, 0, wxALIGN_CENTER_VERTICAL);
        dimsSizer->Add(row, 0, wxLEFT | wxTOP, 10);
    }

    // Cold Slug Well Row
    {
        auto* row = new wxBoxSizer(wxHORIZONTAL);

        auto* lbl = new wxStaticText(dimsPanel, wxID_ANY, "Cold slug length:",
            wxDefaultPosition, wxSize(60, -1));
        lbl->SetForegroundColour(kSmallBtnText);
        lbl->SetFont(wxFont(8, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
            wxFONTWEIGHT_NORMAL, false, "Segoe UI"));

        m_runnerColdSlugDepth = new wxTextCtrl(dimsPanel, wxID_ANY, "5.0",
            wxDefaultPosition, wxSize(70, 22));
        m_runnerColdSlugDepth->SetBackgroundColour(kSmallBtnBg);
        m_runnerColdSlugDepth->SetForegroundColour(kTextDefault);

        auto* unit = new wxStaticText(dimsPanel, wxID_ANY, "mm");
        unit->SetForegroundColour(kSettingsText);
        unit->SetFont(wxFont(8, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
            wxFONTWEIGHT_NORMAL, false, "Segoe UI"));

        row->Add(lbl, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
        row->Add(m_runnerColdSlugDepth, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
        row->Add(unit, 0, wxALIGN_CENTER_VERTICAL);
        dimsSizer->Add(row, 0, wxLEFT | wxTOP, 10);
    }

    dimsPanel->SetSizer(dimsSizer);
    settingsSizer->Add(dimsPanel, 0, wxEXPAND | wxBOTTOM, 10);

    settingsPanel->SetSizer(settingsSizer);
    sizer->Add(settingsPanel, 0, wxEXPAND);

    // Show/hide dims based on type selection (future-proofed for more types)
    m_runnerTypeChoice->Bind(wxEVT_CHOICE, [dimsPanel, this](wxCommandEvent&)
        {
            dimsPanel->Show(m_runnerTypeChoice->GetStringSelection() == "Cylindrical");
            dimsPanel->GetParent()->Layout();
            dimsPanel->GetParent()->GetParent()->Layout();
            dimsPanel->GetParent()->GetParent()->GetParent()->Layout();
        });

    // Toggle the Settings sub-section (debounced — 200ms cooldown)
    settingsBtn->Bind(wxEVT_TOGGLEBUTTON, [settingsBtn, settingsPanel, panel](wxCommandEvent&)
        {
            static wxLongLong lastToggleMs = 0;
            wxLongLong now = wxGetLocalTimeMillis();
            if ((now - lastToggleMs).GetValue() < 200)
            {
                // Too fast — revert the toggle state and ignore
                settingsBtn->SetValue(!settingsBtn->GetValue());
                return;
            }
            lastToggleMs = now;

            const bool expanded = settingsBtn->GetValue();
            wxString lbl = expanded
                ? wxString::FromUTF8("Settings      \xe2\x96\xbe")    // ▾
                : wxString::FromUTF8("Settings      \xe2\x96\xb8");   // ▸
            settingsBtn->SetLabel(lbl);
            settingsPanel->Show(expanded);
            panel->Layout();
            panel->GetParent()->Layout();
            panel->GetParent()->GetParent()->Layout();
        });

    panel->SetSizer(sizer);
    return panel;
}

wxPanel* MainFrame::CreateGatesContent(wxWindow* parent)
{
    auto* panel = new wxPanel(parent, wxID_ANY);
    panel->SetBackgroundColour(kRibbonBg);

    auto* sizer = new wxBoxSizer(wxVERTICAL);

    // ---- Gate type dropdown ------------------------------------------------
    auto* typeLabel = new wxStaticText(panel, wxID_ANY, "Gate type:");
    typeLabel->SetForegroundColour(kTextDefault);
    typeLabel->SetFont(wxFont(8, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
        wxFONTWEIGHT_NORMAL, false, "Segoe UI"));
    sizer->Add(typeLabel, 0, wxLEFT | wxTOP, 10);

    m_gateTypeChoice = new wxChoice(panel, wxID_ANY);
    m_gateTypeChoice->SetBackgroundColour(wxColour(0x2A, 0x30, 0x3C));
    m_gateTypeChoice->SetForegroundColour(kTextDefault);
    m_gateTypeChoice->Append("Tapered Cylinder");
    m_gateTypeChoice->SetSelection(0);
    sizer->Add(m_gateTypeChoice, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 10);

    // ---- Dimensions panel (shown for Tapered Cylinder) ---------------------
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

        m_gateDiameter = new wxTextCtrl(dimsPanel, wxID_ANY, "3.0",
            wxDefaultPosition, wxSize(70, 22));
        m_gateDiameter->SetBackgroundColour(wxColour(0x2A, 0x30, 0x3C));
        m_gateDiameter->SetForegroundColour(kTextDefault);

        auto* unit = new wxStaticText(dimsPanel, wxID_ANY, "mm");
        unit->SetForegroundColour(wxColour(0x55, 0x6A, 0x85));
        unit->SetFont(wxFont(8, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
            wxFONTWEIGHT_NORMAL, false, "Segoe UI"));

        row->Add(lbl, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
        row->Add(m_gateDiameter, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
        row->Add(unit, 0, wxALIGN_CENTER_VERTICAL);
        dimsSizer->Add(row, 0, wxLEFT | wxTOP, 10);
    }

    // Draft angle row
    {
        auto* row = new wxBoxSizer(wxHORIZONTAL);

        auto* lbl = new wxStaticText(dimsPanel, wxID_ANY, "Draft angle:",
            wxDefaultPosition, wxSize(60, -1));
        lbl->SetForegroundColour(kTextDefault);
        lbl->SetFont(wxFont(8, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
            wxFONTWEIGHT_NORMAL, false, "Segoe UI"));

        m_gateDraftAngle = new wxTextCtrl(dimsPanel, wxID_ANY, "1.0",
            wxDefaultPosition, wxSize(70, 22));
        m_gateDraftAngle->SetBackgroundColour(wxColour(0x2A, 0x30, 0x3C));
        m_gateDraftAngle->SetForegroundColour(kTextDefault);

        auto* unit = new wxStaticText(dimsPanel, wxID_ANY, "\xC2\xB0");
        unit->SetForegroundColour(wxColour(0x55, 0x6A, 0x85));
        unit->SetFont(wxFont(8, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
            wxFONTWEIGHT_NORMAL, false, "Segoe UI"));

        row->Add(lbl, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
        row->Add(m_gateDraftAngle, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
        row->Add(unit, 0, wxALIGN_CENTER_VERTICAL);
        dimsSizer->Add(row, 0, wxLEFT | wxTOP, 10);
    }

    dimsPanel->SetSizer(dimsSizer);
    sizer->Add(dimsPanel, 0, wxEXPAND | wxBOTTOM, 6);

    // ---- Sub-runner divider ------------------------------------------------
    auto* subSep = new wxPanel(panel, wxID_ANY, wxDefaultPosition, wxSize(-1, 1));
    subSep->SetBackgroundColour(wxColour(0x2A, 0x38, 0x4A));
    sizer->Add(subSep, 0, wxEXPAND | wxLEFT | wxRIGHT, 10);

    // ---- Sub-runner type dropdown ------------------------------------------
    auto* subTypeLabel = new wxStaticText(panel, wxID_ANY, "Sub-runner type:");
    subTypeLabel->SetForegroundColour(kTextDefault);
    subTypeLabel->SetFont(wxFont(8, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
        wxFONTWEIGHT_NORMAL, false, "Segoe UI"));
    sizer->Add(subTypeLabel, 0, wxLEFT | wxTOP, 10);

    m_subRunnerTypeChoice = new wxChoice(panel, wxID_ANY);
    m_subRunnerTypeChoice->SetBackgroundColour(wxColour(0x2A, 0x30, 0x3C));
    m_subRunnerTypeChoice->SetForegroundColour(kTextDefault);
    m_subRunnerTypeChoice->Append("Cylinder");
    m_subRunnerTypeChoice->SetSelection(0);
    sizer->Add(m_subRunnerTypeChoice, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 10);

    // ---- Sub-runner dimensions panel ---------------------------------------
    auto* subDimsPanel = new wxPanel(panel, wxID_ANY);
    subDimsPanel->SetBackgroundColour(kRibbonBg);
    auto* subDimsSizer = new wxBoxSizer(wxVERTICAL);
    {
        auto* row = new wxBoxSizer(wxHORIZONTAL);

        auto* lbl = new wxStaticText(subDimsPanel, wxID_ANY, "Diameter:",
            wxDefaultPosition, wxSize(60, -1));
        lbl->SetForegroundColour(kTextDefault);
        lbl->SetFont(wxFont(8, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
            wxFONTWEIGHT_NORMAL, false, "Segoe UI"));

        m_subRunnerDiameter = new wxTextCtrl(subDimsPanel, wxID_ANY, "5.0",
            wxDefaultPosition, wxSize(70, 22));
        m_subRunnerDiameter->SetBackgroundColour(wxColour(0x2A, 0x30, 0x3C));
        m_subRunnerDiameter->SetForegroundColour(kTextDefault);

        auto* unit = new wxStaticText(subDimsPanel, wxID_ANY, "mm");
        unit->SetForegroundColour(wxColour(0x55, 0x6A, 0x85));
        unit->SetFont(wxFont(8, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
            wxFONTWEIGHT_NORMAL, false, "Segoe UI"));

        row->Add(lbl, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
        row->Add(m_subRunnerDiameter, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
        row->Add(unit, 0, wxALIGN_CENTER_VERTICAL);
        subDimsSizer->Add(row, 0, wxLEFT | wxTOP, 10);
    }
    subDimsPanel->SetSizer(subDimsSizer);
    sizer->Add(subDimsPanel, 0, wxEXPAND | wxBOTTOM, 10);

    panel->SetSizer(sizer);

    // Show/hide gate dims based on type selection
    m_gateTypeChoice->Bind(wxEVT_CHOICE, [dimsPanel, this](wxCommandEvent&)
        {
            dimsPanel->Show(m_gateTypeChoice->GetStringSelection() == "Tapered Cylinder");
            dimsPanel->GetParent()->Layout();
            dimsPanel->GetParent()->GetParent()->Layout();
        });

    return panel;
}

wxPanel* MainFrame::CreateLeftPanel(wxWindow* parent)
{
    auto* panel = new wxPanel(parent, wxID_ANY,
        wxDefaultPosition, wxSize(220, -1));
    panel->SetBackgroundColour(kRibbonBg);

    auto* sizer = new wxBoxSizer(wxVERTICAL);

    // ---- Panel title -------------------------------------------------------
    auto* title = new wxStaticText(panel, wxID_ANY, "MOULD TOOL SETTINGS");
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
    // Runners — always visible (header is inside the content panel)
    wxPanel* runnersContent = CreateRunnersContent(panel);
    sizer->Add(runnersContent, 0, wxEXPAND | wxTOP, 4);
    // Gates — custom content
    wxPanel* gatesContent = CreateGatesContent(panel);
    CreateCollapsibleSection(panel, sizer, "Gates", &gatesContent);
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