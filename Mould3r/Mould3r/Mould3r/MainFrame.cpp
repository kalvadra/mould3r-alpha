#include <wx/filedlg.h>

#include "MainFrame.h"
#include "GLCanvas.h"
#include "RotateDialog.h"
#include "TranslateDialog.h"
#include "ScaleDialog.h"

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
MainFrame::MainFrame(const StartupConfig& config)
    : wxFrame(nullptr, wxID_ANY, "Mould3r",
        wxDefaultPosition, wxSize(1200, 800))
{
    // ---- Menu bar ----------------------------------------------------------
    auto* fileMenu = new wxMenu();
    fileMenu->Append(ID_Import, "Import...\tCtrl+I");
    fileMenu->AppendSeparator();
    fileMenu->Append(wxID_EXIT, "Exit\tAlt+F4");

    auto* menuBar = new wxMenuBar();
    menuBar->Append(fileMenu, "&File");
    SetMenuBar(menuBar);

    Bind(wxEVT_MENU, &MainFrame::OnExit, this, wxID_EXIT);
    Bind(wxEVT_MENU, &MainFrame::OnImport, this, ID_Import);

    // ---- Layout: ribbon on top, canvas below --------------------------------
    auto* root = new wxPanel(this, wxID_ANY);
    root->SetBackgroundColour(kRibbonBg);

    auto* vSizer = new wxBoxSizer(wxVERTICAL);

    wxPanel* ribbon = CreateRibbon(root);
    vSizer->Add(ribbon, 0, wxEXPAND);

    // 1-px separator line
    auto* sep = new wxPanel(root, wxID_ANY, wxDefaultPosition, wxSize(-1, 1));
    sep->SetBackgroundColour(wxColour(0x00, 0x7A, 0xCC));
    vSizer->Add(sep, 0, wxEXPAND);

    m_canvas = new GLCanvas(root);
    vSizer->Add(m_canvas, 1, wxEXPAND);

    root->SetSizer(vSizer);

    // Frame sizer
    auto* frameSizer = new wxBoxSizer(wxVERTICAL);
    frameSizer->Add(root, 1, wxEXPAND);
    SetSizer(frameSizer);

    // Start with Select active
    SetActiveTool(TransformMode::Select);

    // Load models from startup config
    if (!config.modelAPath.empty())
        m_canvas->ImportStepFile(config.modelAPath);
    if (!config.modelBPath.empty())
        m_canvas->ImportStepFile(config.modelBPath);
}

// ---------------------------------------------------------------------------
// CreateRibbon  – horizontal panel with labelled tool groups
// ---------------------------------------------------------------------------
wxPanel* MainFrame::CreateRibbon(wxWindow* parent)
{
    auto* panel = new wxPanel(parent, wxID_ANY, wxDefaultPosition, wxSize(-1, 48));
    panel->SetBackgroundColour(kRibbonBg);

    auto* hSizer = new wxBoxSizer(wxHORIZONTAL);

    // Left padding
    hSizer->AddSpacer(12);

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

    // ---- TRANSFORM group ---------------------------------------------------
    auto* transformSizer = new wxBoxSizer(wxVERTICAL);

    auto* toolRow = new wxBoxSizer(wxHORIZONTAL);

    m_btnTranslate = addTool(ID_ToolTranslate, "+    Translate", "Drag to translate  (LMB)");
    m_btnRotate = addTool(ID_ToolRotate, "O    Rotate", "Drag to rotate  (LMB)");
    m_btnScale = addTool(ID_ToolScale, "<>   Scale", "Drag up/down to scale  (LMB)");

    toolRow->Add(m_btnTranslate, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
    toolRow->Add(m_btnRotate, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
    toolRow->Add(m_btnScale, 0, wxALIGN_CENTER_VERTICAL);

    transformSizer->Add(toolRow, 0, wxALIGN_CENTER_HORIZONTAL);
    transformSizer->Add(addLabel("TRANSFORM"), 0, wxALIGN_CENTER_HORIZONTAL | wxTOP, 2);

    hSizer->Add(transformSizer, 0, wxALIGN_CENTER_VERTICAL | wxTOP | wxBOTTOM, 6);

    // Vertical divider
    hSizer->AddSpacer(16);
    auto* divider = new wxPanel(panel, wxID_ANY, wxDefaultPosition, wxSize(1, 36));
    divider->SetBackgroundColour(wxColour(0x38, 0x44, 0x55));
    hSizer->Add(divider, 0, wxALIGN_CENTER_VERTICAL);
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
    Bind(wxEVT_TOGGLEBUTTON, &MainFrame::OnToolTranslate, this, ID_ToolTranslate);
    Bind(wxEVT_TOGGLEBUTTON, &MainFrame::OnToolRotate, this, ID_ToolRotate);
    Bind(wxEVT_TOGGLEBUTTON, &MainFrame::OnToolScale, this, ID_ToolScale);

    return panel;
}

// ---------------------------------------------------------------------------
// SetActiveTool – mutually exclusive toggle + notify canvas
// ---------------------------------------------------------------------------
void MainFrame::SetActiveTool(TransformMode mode)
{
    // Reset all buttons
    StyleRibbonBtn(m_btnTranslate, false); m_btnTranslate->SetValue(false);
    StyleRibbonBtn(m_btnRotate, false); m_btnRotate->SetValue(false);
    StyleRibbonBtn(m_btnScale, false); m_btnScale->SetValue(false);

    // Activate selected
    switch (mode)
    {
    case TransformMode::Translate:
        StyleRibbonBtn(m_btnTranslate, true); m_btnTranslate->SetValue(true); break;
    case TransformMode::Rotate:
        StyleRibbonBtn(m_btnRotate, true);    m_btnRotate->SetValue(true);    break;
    case TransformMode::Scale:
        StyleRibbonBtn(m_btnScale, true);     m_btnScale->SetValue(true);     break;
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
