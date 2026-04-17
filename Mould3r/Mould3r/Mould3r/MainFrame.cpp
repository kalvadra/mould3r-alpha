#include <wx/filedlg.h>
#include <wx/bmpbndl.h>
#include <wx/stdpaths.h>
#include <wx/filename.h>
#include <wx/file.h>
#include <memory>

#include "MainFrame.h"
#include "GLCanvas.h"
#include "RotateDialog.h"
#include "TranslateDialog.h"
#include "ScaleDialog.h"
#include "AppConfig.h"
#include "style.h"

// ---------------------------------------------------------------------------
// Ribbon colour aliases (shorthand into the Style namespace)
// ---------------------------------------------------------------------------
static const wxColour& kRibbonBg = Style::AppBg;
static const wxColour& kBtnDefault = Style::BtnDefault;
static const wxColour& kBtnActive = Style::BtnActive;
static const wxColour& kBtnHover = Style::BtnHover;
static const wxColour& kTextDefault = Style::TextPrimary;
static const wxColour& kTextActive = Style::TextActive;

// ---------------------------------------------------------------------------
// SVG asset paths (relative to the executable directory)
// ---------------------------------------------------------------------------
static const wxString kAppIconSvg = "res/logos/logo-icon-nobackground.svg";
static const wxString kRibbonLogoSvg = "";

// ---------------------------------------------------------------------------
// Chevron SVG icons (relative to the executable directory)
// ---------------------------------------------------------------------------
static const wxString kChevronDownSvg = "res/icons/chevron-down.svg";
static const wxString kChevronRightSvg = "res/icons/chevron-right.svg";

// ---------------------------------------------------------------------------
// Settings-panel layout constants — keep dimension fields and type dropdowns
// aligned in a consistent right-hand control column.
// ---------------------------------------------------------------------------
static const int kFieldWidth = 90;     // text-entry width (px)
static const int kUnitWidth = 28;     // fixed unit-label column (px)
static const int kFieldGap = 4;      // gap between field and unit label
static const int kCtrlColWidth = kFieldWidth + kFieldGap + kUnitWidth;  // total

// ---------------------------------------------------------------------------
// LoadSvgBundle — loads an SVG file and returns a wxBitmapBundle at the
// requested size.  Relative paths are anchored to the executable directory.
// If recolorWhite is true, common fill/stroke colours are replaced with white.
// Returns an invalid bundle if the path is empty or the file can't be read.
// ---------------------------------------------------------------------------
static wxBitmapBundle LoadSvgBundle(const wxString& svgPath,
    const wxSize& size,
    bool recolorWhite = false)
{
    if (svgPath.IsEmpty())
        return wxBitmapBundle();

    wxFileName fn(svgPath);
    if (fn.IsRelative())
    {
        wxFileName exeDir(wxStandardPaths::Get().GetExecutablePath());
        fn.MakeAbsolute(exeDir.GetPath());
    }

    wxFile file(fn.GetFullPath());
    if (!file.IsOpened())
        return wxBitmapBundle();

    wxString svg;
    file.ReadAll(&svg);

    if (recolorWhite)
    {
        svg.Replace("currentColor", "white");
        svg.Replace("\"black\"", "\"white\"");
        svg.Replace("\"#000000\"", "\"white\"");
        svg.Replace("\"#000\"", "\"white\"");
    }

    const wxScopedCharBuffer utf8 = svg.utf8_str();
    return wxBitmapBundle::FromSVG(utf8.data(), size);
}

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
    : wxFrame(nullptr, wxID_ANY, "Mould3r - New Project",
        wxDefaultPosition, wxSize(1200, 800))
    , m_fixtureDef(fixture)
{
    // ---- Window icon from SVG -----------------------------------------------
    {
        wxBitmapBundle iconBundle = LoadSvgBundle(kAppIconSvg, wxSize(32, 32));
        if (iconBundle.IsOk())
        {
            wxIcon icon;
            icon.CopyFromBitmap(iconBundle.GetBitmapFor(this));
            SetIcon(icon);
        }
    }

    // ---- Menu bar ----------------------------------------------------------
    auto* fileMenu = new wxMenu();
    fileMenu->Append(ID_NewProject, "New Project...\tCtrl+N");
    fileMenu->Append(ID_LoadProject, "Open Project...\tCtrl+O");
    fileMenu->Append(ID_SaveProject, "Save Project...\tCtrl+S");
    fileMenu->AppendSeparator();
    fileMenu->Append(ID_Import, "Import...\tCtrl+I");
    fileMenu->Append(ID_ChangeFixture, "Change Fixture...");
    fileMenu->AppendSeparator();
    fileMenu->Append(wxID_EXIT, "Exit\tAlt+F4");

    auto* menuBar = new wxMenuBar();
    menuBar->Append(fileMenu, "&File");
    SetMenuBar(menuBar);

    Bind(wxEVT_MENU, &MainFrame::OnExit, this, wxID_EXIT);
    Bind(wxEVT_MENU, &MainFrame::OnImport, this, ID_Import);
    Bind(wxEVT_MENU, &MainFrame::OnChangeFixture, this, ID_ChangeFixture);
    Bind(wxEVT_MENU, &MainFrame::OnSaveProject, this, ID_SaveProject);
    Bind(wxEVT_MENU, &MainFrame::OnLoadProject, this, ID_LoadProject);
    Bind(wxEVT_MENU, &MainFrame::OnNewProject, this, ID_NewProject);

    // ---- Layout: ribbon on top, canvas below --------------------------------
    auto* root = new wxPanel(this, wxID_ANY);
    root->SetBackgroundColour(kRibbonBg);

    auto* vSizer = new wxBoxSizer(wxVERTICAL);

    wxPanel* ribbon = CreateRibbon(root);

    // 1-px separator line
    auto* sep = new wxPanel(root, wxID_ANY, wxDefaultPosition, wxSize(-1, 1));
    sep->SetBackgroundColour(Style::Accent);

    // In MainFrame constructor, replace the vSizer canvas Add with:
    auto* contentSizer = new wxBoxSizer(wxHORIZONTAL);
    m_canvas = new GLCanvas(root);
    m_leftPanel = CreateLeftPanel(root);

    contentSizer->Add(m_leftPanel, 0, wxEXPAND);
    contentSizer->Add(m_canvas, 1, wxEXPAND);

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

    // ---- App logo (SVG, replaces text title) --------------------------------
    {
        wxBitmapBundle logoBndle = LoadSvgBundle(kRibbonLogoSvg, wxSize(120, 28));
        if (logoBndle.IsOk())
        {
            auto* logo = new wxStaticBitmap(panel, wxID_ANY,
                logoBndle.GetBitmapFor(panel));
            hSizer->Add(logo, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 16);
        }
    }

    // Import button (accent blue, left-aligned)
    auto* btnImport = new wxButton(panel, ID_Import, "Import Model",
        wxDefaultPosition, wxSize(120, 32), wxBORDER_NONE);
    btnImport->SetBackgroundColour(Style::BtnSecondary);
    btnImport->SetForegroundColour(*wxWHITE);
    btnImport->SetFont(wxFont(9, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
        wxFONTWEIGHT_SEMIBOLD, false, "Segoe UI"));
    hSizer->Add(btnImport, 0, wxALIGN_CENTER_VERTICAL);

    hSizer->AddStretchSpacer();

    // ---- Export (right-aligned) ---------------------------------------------
    auto* btnExport = new wxButton(panel, ID_Export, "Export",
        wxDefaultPosition, wxSize(90, 32), wxBORDER_NONE);
    btnExport->SetBackgroundColour(Style::InputBg);
    btnExport->SetForegroundColour(*wxWHITE);
    btnExport->SetFont(wxFont(9, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
        wxFONTWEIGHT_SEMIBOLD, false, "Segoe UI"));
    hSizer->Add(btnExport, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);

    // ---- Generate Mould (green, right-aligned) --------------------------------
    auto* btnGenerate = new wxButton(panel, ID_GenerateMould, "Generate Mould",
        wxDefaultPosition, wxSize(130, 32), wxBORDER_NONE);
    btnGenerate->SetBackgroundColour(Style::BtnGenerate);
    btnGenerate->SetForegroundColour(*wxWHITE);
    btnGenerate->SetFont(wxFont(9, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
        wxFONTWEIGHT_SEMIBOLD, false, "Segoe UI"));
    hSizer->Add(btnGenerate, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 12);

    panel->SetSizer(hSizer);

    // ---- Bind events -------------------------------------------------------
    Bind(wxEVT_BUTTON, &MainFrame::OnImport, this, ID_Import);
    Bind(wxEVT_TOGGLEBUTTON, &MainFrame::OnToolTranslate, this, ID_ToolTranslate);
    Bind(wxEVT_TOGGLEBUTTON, &MainFrame::OnToolRotate, this, ID_ToolRotate);
    Bind(wxEVT_TOGGLEBUTTON, &MainFrame::OnToolScale, this, ID_ToolScale);
    Bind(wxEVT_BUTTON, &MainFrame::OnToolCenter, this, ID_ToolCenter);
    Bind(wxEVT_TOGGLEBUTTON, &MainFrame::OnToolPlaceVent, this, ID_ToolPlaceVent);
    Bind(wxEVT_BUTTON, &MainFrame::OnClearVentPoints, this, ID_ClearVentPoints);
    Bind(wxEVT_BUTTON, &MainFrame::OnPlaceSprue, this, ID_PlaceSprue);
    Bind(wxEVT_BUTTON, &MainFrame::OnClearSprue, this, ID_ClearSprue);
    Bind(wxEVT_TOGGLEBUTTON, &MainFrame::OnPlaceRunner, this, ID_PlaceRunner);
    Bind(wxEVT_BUTTON, &MainFrame::OnClearRunners, this, ID_ClearRunners);
    Bind(wxEVT_TOGGLEBUTTON, &MainFrame::OnPlaceGate, this, ID_PlaceGate);
    Bind(wxEVT_BUTTON, &MainFrame::OnClearGates, this, ID_ClearGates);
    Bind(wxEVT_BUTTON, &MainFrame::OnRemoveVent, this, ID_RemoveVent);
    Bind(wxEVT_BUTTON, &MainFrame::OnRemoveSprue, this, ID_RemoveSprue);
    Bind(wxEVT_BUTTON, &MainFrame::OnRemoveRunner, this, ID_RemoveRunner);
    Bind(wxEVT_BUTTON, &MainFrame::OnRemoveGate, this, ID_RemoveGate);
    Bind(wxEVT_BUTTON, &MainFrame::OnEditVent, this, ID_EditVent);
    Bind(wxEVT_BUTTON, &MainFrame::OnEditRunner, this, ID_EditRunner);
    Bind(wxEVT_BUTTON, &MainFrame::OnEditGate, this, ID_EditGate);
    Bind(wxEVT_BUTTON, &MainFrame::OnGenerateMould, this, ID_GenerateMould);
    Bind(wxEVT_BUTTON, &MainFrame::OnExport, this, ID_Export);

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
            lbl->SetForegroundColour(Style::Accent);
            lbl->SetFont(wxFont(8, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
                wxFONTWEIGHT_BOLD, false, "Segoe UI"));
            sizer->Add(lbl, 0, wxLEFT | wxTOP, 12);

            auto* line = new wxPanel(panel, wxID_ANY, wxDefaultPosition, wxSize(-1, 1));
            line->SetBackgroundColour(Style::Divider);
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
            ctrl->SetBackgroundColour(Style::InputBg);
            ctrl->SetForegroundColour(kTextDefault);

            auto* browse = new wxButton(panel, browseId, "...",
                wxDefaultPosition, wxSize(28, 24));
            browse->SetBackgroundColour(Style::InputBg);
            browse->SetForegroundColour(kTextDefault);

            row->Add(ctrl, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
            row->Add(browse, 0, wxALIGN_CENTER_VERTICAL);
            sizer->Add(row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 12);
        };

    // ---- Export section ----------------------------------------------------
    addSection("EXPORT");
    addPathRow("Output folder:", m_exportPath, ID_BrowseExport);

    sizer->AddStretchSpacer();

    panel->SetSizer(sizer);

    // ---- Binds -------------------------------------------------------------
    Bind(wxEVT_BUTTON, &MainFrame::OnBrowseExport, this, ID_BrowseExport);

    return panel;
}
// ---------------------------------------------------------------------------
// SetActiveTool – mutually exclusive toggle + notify canvas
// ---------------------------------------------------------------------------
void MainFrame::SetActiveTool(TransformMode mode)
{
    // Reset all buttons
    if (m_btnTranslate) { StyleRibbonBtn(m_btnTranslate, false); m_btnTranslate->SetValue(false); }
    if (m_btnRotate) { StyleRibbonBtn(m_btnRotate, false);    m_btnRotate->SetValue(false); }
    if (m_btnScale) { StyleRibbonBtn(m_btnScale, false);     m_btnScale->SetValue(false); }
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
        if (m_btnTranslate) { StyleRibbonBtn(m_btnTranslate, true); m_btnTranslate->SetValue(true); }
        break;
    case TransformMode::Rotate:
        if (m_btnRotate) { StyleRibbonBtn(m_btnRotate, true); m_btnRotate->SetValue(true); }
        break;
    case TransformMode::Scale:
        if (m_btnScale) { StyleRibbonBtn(m_btnScale, true); m_btnScale->SetValue(true); }
        break;
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
    case TransformMode::RemoveVent:
    case TransformMode::RemoveRunner:
    case TransformMode::RemoveGate:
    case TransformMode::RemoveSprue:
    case TransformMode::EditVent:
    case TransformMode::EditRunner:
    case TransformMode::EditGate:
    case TransformMode::Select:
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

void MainFrame::OnRemoveVent(wxCommandEvent&)
{
    // Toggle: if already in RemoveVent mode, return to Select
    if (m_canvas && m_canvas->GetTransformMode() == TransformMode::RemoveVent)
        SetActiveTool(TransformMode::Select);
    else
        SetActiveTool(TransformMode::RemoveVent);
}

void MainFrame::OnRemoveSprue(wxCommandEvent&)
{
    // Toggle: if already in RemoveSprue mode, return to Select
    if (m_canvas && m_canvas->GetTransformMode() == TransformMode::RemoveSprue)
        SetActiveTool(TransformMode::Select);
    else
        SetActiveTool(TransformMode::RemoveSprue);
}

void MainFrame::OnRemoveRunner(wxCommandEvent&)
{
    // Toggle: if already in RemoveRunner mode, return to Select
    if (m_canvas && m_canvas->GetTransformMode() == TransformMode::RemoveRunner)
        SetActiveTool(TransformMode::Select);
    else
        SetActiveTool(TransformMode::RemoveRunner);
}

void MainFrame::OnRemoveGate(wxCommandEvent&)
{
    // Toggle: if already in RemoveGate mode, return to Select
    if (m_canvas && m_canvas->GetTransformMode() == TransformMode::RemoveGate)
        SetActiveTool(TransformMode::Select);
    else
        SetActiveTool(TransformMode::RemoveGate);
}

void MainFrame::OnEditVent(wxCommandEvent&)
{
    if (m_canvas && m_canvas->GetTransformMode() == TransformMode::EditVent)
        SetActiveTool(TransformMode::Select);
    else
        SetActiveTool(TransformMode::EditVent);
}

void MainFrame::OnEditRunner(wxCommandEvent&)
{
    if (m_canvas && m_canvas->GetTransformMode() == TransformMode::EditRunner)
        SetActiveTool(TransformMode::Select);
    else
        SetActiveTool(TransformMode::EditRunner);
}

void MainFrame::OnEditGate(wxCommandEvent&)
{
    if (m_canvas && m_canvas->GetTransformMode() == TransformMode::EditGate)
        SetActiveTool(TransformMode::Select);
    else
        SetActiveTool(TransformMode::EditGate);
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
    m_fixtureDef = fixture;   // keep for project save

    // Clear existing fixtures and reload
    m_canvas->ClearFixtures();

    if (!fixture.modelAPath.empty())
        m_canvas->ImportStepFileAsFixture(fixture.modelAPath);
    if (!fixture.modelBPath.empty())
        m_canvas->ImportStepFileAsFixture(fixture.modelBPath);
}

// ---------------------------------------------------------------------------
// New Project
// ---------------------------------------------------------------------------
void MainFrame::OnNewProject(wxCommandEvent&)
{
    const std::string lastFixture = AppConfig::LoadLastFixture();

    FixtureDefinition fixture;
    std::string error;

    // If a valid default fixture exists, use it directly (same as startup)
    if (!lastFixture.empty() && FixtureFile::Load(lastFixture, fixture, error))
    {
        // Use the default fixture without showing the dialog
    }
    else
    {
        // No valid default — show the selection dialog
        StartupDialog dlg(this);
        dlg.PreSelectFixture(lastFixture);

        if (dlg.ShowModal() != wxID_OK)
            return;

        fixture = dlg.GetFixture();
        AppConfig::SaveLastFixture(fixture.fixturePath);
    }

    // Clear the entire scene
    m_canvas->ClearAll();

    // Load the selected fixture
    m_fixtureDef = fixture;

    if (!fixture.modelAPath.empty())
        m_canvas->ImportStepFileAsFixture(fixture.modelAPath);
    if (!fixture.modelBPath.empty())
        m_canvas->ImportStepFileAsFixture(fixture.modelBPath);

    if (!fixture.injectionPoints.empty())
        m_canvas->SetActiveInjectionPoint(fixture.injectionPoints[0]);

    // Reset project state
    m_projectPath.clear();
    SetTitle("Mould3r - New Project");
}

// ---------------------------------------------------------------------------
// Save Project
// ---------------------------------------------------------------------------
void MainFrame::OnSaveProject(wxCommandEvent&)
{
    wxFileDialog dlg(
        this, "Save Project", "", "",
        "Mould3r Project (*.m3d)|*.m3d|All files (*.*)|*.*",
        wxFD_SAVE | wxFD_OVERWRITE_PROMPT
    );

    if (!m_projectPath.empty())
    {
        wxFileName fn(m_projectPath);
        dlg.SetDirectory(fn.GetPath());
        dlg.SetFilename(fn.GetFullName());
    }

    if (dlg.ShowModal() != wxID_OK)
        return;

    const std::string savePath = dlg.GetPath().ToStdString();

    // Build ProjectData from current state
    ProjectData data;
    data.version = 1;
    data.fixturePath = m_fixtureDef.fixturePath;

    // Objects
    for (const auto& obj : m_canvas->GetObjects())
    {
        ProjectObjectData od;
        od.sourcePath = obj.sourcePath;
        od.pos = obj.pos;
        od.yawDeg = obj.yawDeg;
        od.pitchDeg = obj.pitchDeg;
        od.rollDeg = obj.rollDeg;
        od.scale = obj.scale;
        data.objects.push_back(od);
    }

    // Parameters (read from UI fields)
    {
        auto& p = data.params;
        float ventLength, ventWidth, ventOverrunStart, ventOverrunEnd;
        GetVentDimensions(ventLength, ventWidth, ventOverrunStart, ventOverrunEnd);
        p.ventWidth = ventWidth;
        p.ventLength = ventLength;
        p.ventOverrunStart = ventOverrunStart;
        p.ventOverrunEnd = ventOverrunEnd;
        p.sprueDiameter = GetSprueDiameter();
        p.sprueDraftAngle = GetSprueDraftAngle();
        p.sprueColdSlugDepth = GetSprueColdSlugDepth();
        p.runnerDiameter = GetRunnerDiameter();
        p.runnerColdPlugDist = GetRunnerColdPlugDist();
        p.gateDiameter = GetGateDiameter();
        p.gateDraftAngle = GetGateDraftAngle();
        p.subRunnerDiameter = GetSubRunnerDiameter();
    }

    // Sprue
    {
        const auto& sp = m_canvas->GetSprue();
        auto& sd = data.sprue;
        sd.placed = sp.hasPoint;
        sd.worldPos = sp.worldPos;
        sd.pathStart = sp.pathStart;
        sd.pathEnd = sp.pathEnd;
        sd.partingPos = sp.partingPos;
        sd.hasPartingPoint = sp.hasPartingPoint;
        sd.isDirectInjection = sp.isDirectInjection;
        sd.radius = sp.radius;
        sd.draftAngleDeg = sp.draftAngleDeg;
        sd.coldSlugDepth = sp.coldSlugDepth;

        if (m_canvas->HasActiveInjectionPoint())
            sd.injectionPoint = m_canvas->GetActiveInjectionPoint();
    }

    // Runners
    for (const auto& rf : m_canvas->GetRunners())
        data.runners.push_back(ProjectRunnerData{ rf.point });

    // Gates
    for (const auto& gf : m_canvas->GetGates())
        data.gates.push_back(ProjectGateData{ gf.point.worldPos, gf.point.worldNormal });

    // Vents
    for (const auto& vi : m_canvas->GetVents())
        data.vents.push_back(ProjectVentData{ vi.point.worldPos, vi.point.worldNormal });

    std::string error;
    if (!ProjectFile::Save(savePath, data, error))
    {
        wxMessageBox(error, "Save Failed", wxOK | wxICON_ERROR, this);
        return;
    }

    m_projectPath = savePath;
    SetTitle("Mould3r - " + wxFileName(savePath).GetName());
}

// ---------------------------------------------------------------------------
// Load Project
// ---------------------------------------------------------------------------
void MainFrame::OnLoadProject(wxCommandEvent&)
{
    wxFileDialog dlg(
        this, "Open Project", "", "",
        "Mould3r Project (*.m3d)|*.m3d|All files (*.*)|*.*",
        wxFD_OPEN | wxFD_FILE_MUST_EXIST
    );

    if (dlg.ShowModal() != wxID_OK)
        return;

    const std::string loadPath = dlg.GetPath().ToStdString();

    ProjectData data;
    std::string error;
    if (!ProjectFile::Load(loadPath, data, error))
    {
        wxMessageBox(error, "Load Failed", wxOK | wxICON_ERROR, this);
        return;
    }

    // ---- Clear everything --------------------------------------------------
    m_canvas->ClearAll();

    // ---- Load fixture ------------------------------------------------------
    if (!data.fixturePath.empty())
    {
        FixtureDefinition fixDef;
        std::string fixError;
        if (FixtureFile::Load(data.fixturePath, fixDef, fixError))
        {
            m_fixtureDef = fixDef;
            AppConfig::SaveLastFixture(fixDef.fixturePath);

            if (!fixDef.modelAPath.empty())
                m_canvas->ImportStepFileAsFixture(fixDef.modelAPath);
            if (!fixDef.modelBPath.empty())
                m_canvas->ImportStepFileAsFixture(fixDef.modelBPath);

            // Set injection point (use the one from the sprue data if available,
            // otherwise fall back to the first in the fixture)
            if (data.sprue.placed)
                m_canvas->SetActiveInjectionPoint(data.sprue.injectionPoint);
            else if (!fixDef.injectionPoints.empty())
                m_canvas->SetActiveInjectionPoint(fixDef.injectionPoints[0]);
        }
        else
        {
            wxMessageBox("Could not load fixture:\n" + fixError,
                "Warning", wxOK | wxICON_WARNING, this);
        }
    }

    // ---- Restore UI parameters ---------------------------------------------
    SetParameterFields(data.params);

    // ---- Restore imported objects ------------------------------------------
    for (const auto& obj : data.objects)
    {
        m_canvas->RestoreObject(obj.sourcePath, obj.pos,
            obj.yawDeg, obj.pitchDeg,
            obj.rollDeg, obj.scale);
    }

    // ---- Restore sprue -----------------------------------------------------
    if (data.sprue.placed)
        m_canvas->RestoreSprue(data.sprue);

    // ---- Restore runners ---------------------------------------------------
    for (const auto& rn : data.runners)
        m_canvas->RestoreRunner(rn.point);

    // ---- Restore gates -----------------------------------------------------
    for (const auto& gt : data.gates)
        m_canvas->RestoreGate(gt.pos, gt.normal);

    // ---- Restore vents -----------------------------------------------------
    for (const auto& vn : data.vents)
    {
        m_canvas->RestoreVent(vn.pos, vn.normal,
            data.params.ventWidth,
            data.params.ventLength,
            data.params.ventOverrunStart,
            data.params.ventOverrunEnd);
    }

    // ---- Rebuild all derived GPU geometry -----------------------------------
    m_canvas->RebuildAllFeatures();

    m_projectPath = loadPath;
    SetTitle("Mould3r - " + wxFileName(loadPath).GetName());
}

// ---------------------------------------------------------------------------
// SetParameterFields — populate the left-panel UI fields from saved data.
// ---------------------------------------------------------------------------
void MainFrame::SetParameterFields(const ProjectParameters& p)
{
    auto setField = [](wxTextCtrl* ctrl, float value)
        {
            if (ctrl)
                ctrl->SetValue(wxString::Format("%.4g", value));
        };

    setField(m_ventWidth, p.ventWidth);
    setField(m_ventLength, p.ventLength);
    setField(m_ventOverrunStart, p.ventOverrunStart);
    setField(m_ventOverrunEnd, p.ventOverrunEnd);
    setField(m_sprueDiameter, p.sprueDiameter);
    setField(m_sprueDraftAngle, p.sprueDraftAngle);
    setField(m_sprueColdSlugDepth, p.sprueColdSlugDepth);
    setField(m_runnerDiameter, p.runnerDiameter);
    setField(m_runnerColdSlugDepth, p.runnerColdPlugDist);
    setField(m_gateDiameter, p.gateDiameter);
    setField(m_gateDraftAngle, p.gateDraftAngle);
    setField(m_subRunnerDiameter, p.subRunnerDiameter);
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
    wxDirDialog dlg(this, "Select export folder", "",
        wxDD_DEFAULT_STYLE | wxDD_DIR_MUST_EXIST);
    if (dlg.ShowModal() != wxID_OK)
        return;

    const std::string folder = dlg.GetPath().ToStdString();
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
    auto* headerBtn = new wxToggleButton(parent, wxID_ANY, title,
        wxDefaultPosition, wxSize(-1, 28),
        wxBU_LEFT);
    headerBtn->SetValue(true);
    headerBtn->SetBackgroundColour(Style::SectionHeaderBg);
    headerBtn->SetForegroundColour(Style::Accent);
    headerBtn->SetFont(wxFont(8, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
        wxFONTWEIGHT_BOLD, false, "Segoe UI"));
    headerBtn->SetBitmap(LoadSvgBundle(kChevronDownSvg, wxSize(14, 14), true));
    headerBtn->SetBitmapPosition(wxLEFT);
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
        placeholder->SetForegroundColour(Style::TextDim);
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
            headerBtn->SetBitmap(LoadSvgBundle(
                expanded ? kChevronDownSvg : kChevronRightSvg,
                wxSize(14, 14), true));
            contentRef->Show(expanded);
            parent->Layout();
            parent->GetParent()->Layout();
        });

    return content;
}

wxPanel* MainFrame::CreateVentsContent(wxWindow* parent)
{

    auto* panel = new wxPanel(parent, wxID_ANY);
    panel->SetBackgroundColour(Style::CardBg);

    auto* sizer = new wxBoxSizer(wxVERTICAL);

    // ---- Section title ------------------------------------------------------
    auto* titleLabel = new wxStaticText(panel, wxID_ANY, "Vents");
    titleLabel->SetForegroundColour(*wxWHITE);
    titleLabel->SetFont(wxFont(9, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
        wxFONTWEIGHT_BOLD, false, "Segoe UI"));
    sizer->Add(titleLabel, 0, wxLEFT | wxTOP, 12);
    sizer->AddSpacer(6);

    // ---- "Place Vent" toggle button -----------------------------------------
    auto* btnPlace = new wxToggleButton(panel, ID_ToolPlaceVent, "Place Vent",
        wxDefaultPosition, wxSize(-1, 32), wxBORDER_NONE);
    btnPlace->SetBackgroundColour(Style::BtnPlace);
    btnPlace->SetForegroundColour(*wxWHITE);
    btnPlace->SetFont(wxFont(9, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
        wxFONTWEIGHT_SEMIBOLD, false, "Segoe UI"));
    sizer->Add(btnPlace, 0, wxEXPAND | wxLEFT | wxRIGHT, 12);
    sizer->AddSpacer(6);

    // ---- Edit / Remove / Clear all ------------------------------------------
    auto* actionGrid = new wxGridSizer(1, 3, 0, 4);
    auto makeSmallBtn = [&](const wxString& label) -> wxButton*
        {
            auto* btn = new wxButton(panel, wxID_ANY, label,
                wxDefaultPosition, wxSize(-1, 26), wxBORDER_NONE);
            btn->SetBackgroundColour(Style::BtnSmall);
            btn->SetForegroundColour(Style::TextPrimary);
            btn->SetFont(wxFont(8, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
                wxFONTWEIGHT_NORMAL, false, "Segoe UI"));
            return btn;
        };
    auto* btnEdit = makeSmallBtn("Edit");
    btnEdit->SetId(ID_EditVent);
    auto* btnRemove = makeSmallBtn("Remove");
    btnRemove->SetId(ID_RemoveVent);
    auto* btnClearAll = makeSmallBtn("Clear all");
    btnClearAll->SetId(ID_ClearVentPoints);
    actionGrid->Add(btnEdit, 0, wxEXPAND);
    actionGrid->Add(btnRemove, 0, wxEXPAND);
    actionGrid->Add(btnClearAll, 0, wxEXPAND);
    sizer->Add(actionGrid, 0, wxEXPAND | wxLEFT | wxRIGHT, 12);
    sizer->AddSpacer(8);

    // ---- Collapsible "Settings" sub-section ---------------------------------
    auto* settingsBtn = new wxToggleButton(panel, wxID_ANY,
        "Settings",
        wxDefaultPosition, wxSize(-1, 22), wxBU_LEFT | wxBORDER_NONE);
    settingsBtn->SetValue(false);
    settingsBtn->SetBackgroundColour(Style::CardBg);
    settingsBtn->SetForegroundColour(Style::TextSubtle);
    settingsBtn->SetFont(wxFont(9, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
        wxFONTWEIGHT_NORMAL, false, "Segoe UI"));
    settingsBtn->SetBitmap(LoadSvgBundle(kChevronRightSvg, wxSize(12, 12), true));
    settingsBtn->SetBitmapPosition(wxRIGHT);
    sizer->Add(settingsBtn, 0, wxEXPAND | wxLEFT | wxRIGHT, 12);

    // Settings content
    auto* settingsPanel = new wxPanel(panel, wxID_ANY);
    settingsPanel->SetBackgroundColour(Style::CardBg);
    auto* settingsSizer = new wxBoxSizer(wxVERTICAL);

    // ---- Vent type dropdown (inline with label) -----------------------------
    auto* typeRow = new wxBoxSizer(wxHORIZONTAL);
    auto* typeLabel = new wxStaticText(settingsPanel, wxID_ANY, "Vent type:");
    typeLabel->SetForegroundColour(Style::TextMuted);
    typeLabel->SetFont(wxFont(8, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
        wxFONTWEIGHT_NORMAL, false, "Segoe UI"));

    m_ventTypeChoice = new wxChoice(settingsPanel, wxID_ANY,
        wxDefaultPosition, wxSize(kCtrlColWidth, -1));
    m_ventTypeChoice->SetBackgroundColour(Style::BtnSmall);
    m_ventTypeChoice->SetForegroundColour(Style::TextMuted);
    m_ventTypeChoice->Append("Rectangular");
    m_ventTypeChoice->SetSelection(0);
    typeRow->Add(typeLabel, 0, wxALIGN_CENTER_VERTICAL);
    typeRow->AddStretchSpacer(1);
    typeRow->Add(m_ventTypeChoice, 0, wxALIGN_CENTER_VERTICAL);
    settingsSizer->Add(typeRow, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 10);

    // ---- Dimensions panel --------------------------------------------------
    m_ventDimsPanel = new wxPanel(settingsPanel, wxID_ANY);
    m_ventDimsPanel->SetBackgroundColour(Style::CardBg);
    auto* dimsSizer = new wxBoxSizer(wxVERTICAL);

    auto addDimRow = [&](const wxString& label, wxTextCtrl*& ctrl,
        const wxString& defaultVal)
        {
            auto* row = new wxBoxSizer(wxHORIZONTAL);
            auto* lbl = new wxStaticText(m_ventDimsPanel, wxID_ANY, label);
            lbl->SetForegroundColour(Style::TextMuted);
            lbl->SetFont(wxFont(8, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
                wxFONTWEIGHT_NORMAL, false, "Segoe UI"));
            ctrl = new wxTextCtrl(m_ventDimsPanel, wxID_ANY, defaultVal,
                wxDefaultPosition, wxSize(kFieldWidth, 22));
            ctrl->SetBackgroundColour(Style::BtnSmall);
            ctrl->SetForegroundColour(kTextDefault);
            auto* unit = new wxStaticText(m_ventDimsPanel, wxID_ANY, "mm");
            unit->SetForegroundColour(Style::TextSubtle);
            unit->SetFont(wxFont(8, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
                wxFONTWEIGHT_NORMAL, false, "Segoe UI"));
            unit->SetMinSize(wxSize(kUnitWidth, -1));
            row->Add(lbl, 0, wxALIGN_CENTER_VERTICAL);
            row->AddStretchSpacer(1);
            row->Add(ctrl, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, kFieldGap);
            row->Add(unit, 0, wxALIGN_CENTER_VERTICAL);
            dimsSizer->Add(row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 10);
        };

    addDimRow("Length:", m_ventLength, "1.0");
    addDimRow("Width:", m_ventWidth, "2.0");
    addDimRow("Overrun (start):", m_ventOverrunStart, "0.5");
    addDimRow("Overrun (end):", m_ventOverrunEnd, "0.5");

    m_ventDimsPanel->SetSizer(dimsSizer);
    settingsSizer->Add(m_ventDimsPanel, 0, wxEXPAND | wxBOTTOM, 10);

    settingsPanel->SetSizer(settingsSizer);
    settingsPanel->Show(false);
    sizer->Add(settingsPanel, 0, wxEXPAND);

    m_ventTypeChoice->Bind(wxEVT_CHOICE, [this](wxCommandEvent&)
        {
            m_ventDimsPanel->Show(m_ventTypeChoice->GetStringSelection() == "Rectangular");
            m_ventDimsPanel->GetParent()->Layout();
            m_ventDimsPanel->GetParent()->GetParent()->Layout();
            m_ventDimsPanel->GetParent()->GetParent()->GetParent()->Layout();
        });

    settingsBtn->Bind(wxEVT_TOGGLEBUTTON, [settingsBtn, settingsPanel, panel](wxCommandEvent&)
        {
            static wxLongLong lastToggleMs = 0;
            wxLongLong now = wxGetLocalTimeMillis();
            if ((now - lastToggleMs).GetValue() < 200) { settingsBtn->SetValue(!settingsBtn->GetValue()); return; }
            lastToggleMs = now;
            const bool expanded = settingsBtn->GetValue();
            settingsBtn->SetBitmap(LoadSvgBundle(
                expanded ? kChevronDownSvg : kChevronRightSvg,
                wxSize(12, 12), true));
            settingsBtn->SetBitmapPosition(wxRIGHT);
            settingsPanel->Show(expanded);
            panel->Layout(); panel->GetParent()->Layout(); panel->GetParent()->GetParent()->Layout();
        });

    panel->SetSizer(sizer);
    return panel;
}

wxPanel* MainFrame::CreateSpruesContent(wxWindow* parent)
{

    auto* panel = new wxPanel(parent, wxID_ANY);
    panel->SetBackgroundColour(Style::CardBg);
    auto* sizer = new wxBoxSizer(wxVERTICAL);

    auto* titleLabel = new wxStaticText(panel, wxID_ANY, "Sprues");
    titleLabel->SetForegroundColour(*wxWHITE);
    titleLabel->SetFont(wxFont(9, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
        wxFONTWEIGHT_BOLD, false, "Segoe UI"));
    sizer->Add(titleLabel, 0, wxLEFT | wxTOP, 12);
    sizer->AddSpacer(6);

    // Place Sprue — regular button (one-shot action, not a toggle mode)
    auto* btnPlace = new wxButton(panel, ID_PlaceSprue, "Place Sprue",
        wxDefaultPosition, wxSize(-1, 32), wxBORDER_NONE);
    btnPlace->SetBackgroundColour(Style::BtnPlace);
    btnPlace->SetForegroundColour(*wxWHITE);
    btnPlace->SetFont(wxFont(9, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
        wxFONTWEIGHT_SEMIBOLD, false, "Segoe UI"));
    sizer->Add(btnPlace, 0, wxEXPAND | wxLEFT | wxRIGHT, 12);
    sizer->AddSpacer(6);

    auto* actionGrid = new wxGridSizer(1, 3, 0, 4);
    auto makeSmallBtn = [&](const wxString& label) -> wxButton* {
        auto* btn = new wxButton(panel, wxID_ANY, label,
            wxDefaultPosition, wxSize(-1, 26), wxBORDER_NONE);
        btn->SetBackgroundColour(Style::BtnSmall);
        btn->SetForegroundColour(Style::TextPrimary);
        btn->SetFont(wxFont(8, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
            wxFONTWEIGHT_NORMAL, false, "Segoe UI"));
        return btn;
        };
    auto* btnEdit = makeSmallBtn("Edit");
    auto* btnRemove = makeSmallBtn("Remove");
    btnRemove->SetId(ID_RemoveSprue);
    auto* btnClearAll = makeSmallBtn("Clear all");
    btnClearAll->SetId(ID_ClearSprue);
    actionGrid->Add(btnEdit, 0, wxEXPAND);
    actionGrid->Add(btnRemove, 0, wxEXPAND);
    actionGrid->Add(btnClearAll, 0, wxEXPAND);
    sizer->Add(actionGrid, 0, wxEXPAND | wxLEFT | wxRIGHT, 12);
    sizer->AddSpacer(8);

    // Collapsible Settings
    auto* settingsBtn = new wxToggleButton(panel, wxID_ANY,
        "Settings",
        wxDefaultPosition, wxSize(-1, 22), wxBU_LEFT | wxBORDER_NONE);
    settingsBtn->SetValue(false);
    settingsBtn->SetBackgroundColour(Style::CardBg);
    settingsBtn->SetForegroundColour(Style::TextSubtle);
    settingsBtn->SetFont(wxFont(9, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
        wxFONTWEIGHT_NORMAL, false, "Segoe UI"));
    settingsBtn->SetBitmap(LoadSvgBundle(kChevronRightSvg, wxSize(12, 12), true));
    settingsBtn->SetBitmapPosition(wxRIGHT);
    sizer->Add(settingsBtn, 0, wxEXPAND | wxLEFT | wxRIGHT, 12);

    auto* settingsPanel = new wxPanel(panel, wxID_ANY);
    settingsPanel->SetBackgroundColour(Style::CardBg);
    auto* settingsSizer = new wxBoxSizer(wxVERTICAL);

    auto* typeRow = new wxBoxSizer(wxHORIZONTAL);
    auto* typeLabel = new wxStaticText(settingsPanel, wxID_ANY, "Sprue type:");
    typeLabel->SetForegroundColour(Style::TextMuted);
    typeLabel->SetFont(wxFont(8, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
        wxFONTWEIGHT_NORMAL, false, "Segoe UI"));

    m_sprueTypeChoice = new wxChoice(settingsPanel, wxID_ANY,
        wxDefaultPosition, wxSize(kCtrlColWidth, -1));
    m_sprueTypeChoice->SetBackgroundColour(Style::BtnSmall);
    m_sprueTypeChoice->SetForegroundColour(Style::TextMuted);
    m_sprueTypeChoice->Append("Cylinder");
    m_sprueTypeChoice->SetSelection(0);
    typeRow->Add(typeLabel, 0, wxALIGN_CENTER_VERTICAL);
    typeRow->AddStretchSpacer(1);
    typeRow->Add(m_sprueTypeChoice, 0, wxALIGN_CENTER_VERTICAL);
    settingsSizer->Add(typeRow, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 10);

    auto* dimsPanel = new wxPanel(settingsPanel, wxID_ANY);
    dimsPanel->SetBackgroundColour(Style::CardBg);
    auto* dimsSizer = new wxBoxSizer(wxVERTICAL);

    auto addRow = [&](const wxString& label, wxTextCtrl*& ctrl, const wxString& defVal, const wxString& unitStr) {
        auto* row = new wxBoxSizer(wxHORIZONTAL);
        auto* lbl = new wxStaticText(dimsPanel, wxID_ANY, label);
        lbl->SetForegroundColour(Style::TextMuted);
        lbl->SetFont(wxFont(8, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL, wxFONTWEIGHT_NORMAL, false, "Segoe UI"));
        ctrl = new wxTextCtrl(dimsPanel, wxID_ANY, defVal, wxDefaultPosition, wxSize(kFieldWidth, 22));
        ctrl->SetBackgroundColour(Style::BtnSmall); ctrl->SetForegroundColour(kTextDefault);
        auto* u = new wxStaticText(dimsPanel, wxID_ANY, unitStr);
        u->SetForegroundColour(Style::TextSubtle);
        u->SetFont(wxFont(8, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL, wxFONTWEIGHT_NORMAL, false, "Segoe UI"));
        u->SetMinSize(wxSize(kUnitWidth, -1));
        row->Add(lbl, 0, wxALIGN_CENTER_VERTICAL);
        row->AddStretchSpacer(1);
        row->Add(ctrl, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, kFieldGap);
        row->Add(u, 0, wxALIGN_CENTER_VERTICAL);
        dimsSizer->Add(row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 10);
        };

    addRow("Diameter:", m_sprueDiameter, "5.0", "mm");
    addRow("Draft angle:", m_sprueDraftAngle, "1.0", wxString::FromUTF8("\xC2\xB0"));
    addRow("Cold slug:", m_sprueColdSlugDepth, "5.0", "mm");

    dimsPanel->SetSizer(dimsSizer);
    settingsSizer->Add(dimsPanel, 0, wxEXPAND | wxBOTTOM, 10);
    settingsPanel->SetSizer(settingsSizer);
    settingsPanel->Show(false);
    sizer->Add(settingsPanel, 0, wxEXPAND);

    m_sprueTypeChoice->Bind(wxEVT_CHOICE, [dimsPanel, this](wxCommandEvent&) {
        dimsPanel->Show(m_sprueTypeChoice->GetStringSelection() == "Cylinder");
        dimsPanel->GetParent()->Layout(); dimsPanel->GetParent()->GetParent()->Layout();
        dimsPanel->GetParent()->GetParent()->GetParent()->Layout();
        });

    settingsBtn->Bind(wxEVT_TOGGLEBUTTON, [settingsBtn, settingsPanel, panel](wxCommandEvent&) {
        static wxLongLong lastToggleMs = 0;
        wxLongLong now = wxGetLocalTimeMillis();
        if ((now - lastToggleMs).GetValue() < 200) { settingsBtn->SetValue(!settingsBtn->GetValue()); return; }
        lastToggleMs = now;
        const bool expanded = settingsBtn->GetValue();
        settingsBtn->SetBitmap(LoadSvgBundle(
            expanded ? kChevronDownSvg : kChevronRightSvg,
            wxSize(12, 12), true));
        settingsBtn->SetBitmapPosition(wxRIGHT);
        settingsPanel->Show(expanded);
        panel->Layout(); panel->GetParent()->Layout(); panel->GetParent()->GetParent()->Layout();
        });

    panel->SetSizer(sizer);
    return panel;
}

wxPanel* MainFrame::CreateRunnersContent(wxWindow* parent)
{
    // Local colours sampled from the reference mockup

    auto* panel = new wxPanel(parent, wxID_ANY);
    panel->SetBackgroundColour(Style::CardBg);

    auto* sizer = new wxBoxSizer(wxVERTICAL);

    // ---- "Runners" section title (inside the card) --------------------------
    auto* runnersLabel = new wxStaticText(panel, wxID_ANY, "Runners");
    runnersLabel->SetForegroundColour(*wxWHITE);
    runnersLabel->SetFont(wxFont(9, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
        wxFONTWEIGHT_BOLD, false, "Segoe UI"));
    sizer->Add(runnersLabel, 0, wxLEFT | wxTOP, 12);

    sizer->AddSpacer(6);

    // ---- "Place Runner" toggle button (full-width, muted indigo) -------------
    auto* btnPlace = new wxToggleButton(panel, ID_PlaceRunner, "Place Runner",
        wxDefaultPosition, wxSize(-1, 32), wxBORDER_NONE);
    btnPlace->SetBackgroundColour(Style::BtnPlace);
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
            btn->SetBackgroundColour(Style::BtnSmall);
            btn->SetForegroundColour(Style::TextPrimary);
            btn->SetFont(wxFont(8, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
                wxFONTWEIGHT_NORMAL, false, "Segoe UI"));
            return btn;
        };

    auto* btnEdit = makeSmallBtn("Edit");
    btnEdit->SetId(ID_EditRunner);
    auto* btnRemove = makeSmallBtn("Remove");
    btnRemove->SetId(ID_RemoveRunner);
    auto* btnClearAll = makeSmallBtn("Clear all");
    btnClearAll->SetId(ID_ClearRunners);

    actionGrid->Add(btnEdit, 0, wxEXPAND);
    actionGrid->Add(btnRemove, 0, wxEXPAND);
    actionGrid->Add(btnClearAll, 0, wxEXPAND);
    sizer->Add(actionGrid, 0, wxEXPAND | wxLEFT | wxRIGHT, 12);

    sizer->AddSpacer(8);

    // ---- Collapsible "Settings" sub-section ---------------------------------
    auto* settingsBtn = new wxToggleButton(panel, wxID_ANY,
        "Settings",
        wxDefaultPosition, wxSize(-1, 22), wxBU_LEFT | wxBORDER_NONE);
    settingsBtn->SetValue(false);
    settingsBtn->SetBackgroundColour(Style::CardBg);
    settingsBtn->SetForegroundColour(Style::TextSubtle);
    settingsBtn->SetFont(wxFont(9, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
        wxFONTWEIGHT_NORMAL, false, "Segoe UI"));
    settingsBtn->SetBitmap(LoadSvgBundle(kChevronRightSvg, wxSize(12, 12), true));
    settingsBtn->SetBitmapPosition(wxRIGHT);
    sizer->Add(settingsBtn, 0, wxEXPAND | wxLEFT | wxRIGHT, 12);

    // Settings content panel (contains the existing type/dimension fields)
    auto* settingsPanel = new wxPanel(panel, wxID_ANY);
    settingsPanel->SetBackgroundColour(Style::CardBg);
    auto* settingsSizer = new wxBoxSizer(wxVERTICAL);

    // ---- Runner type dropdown (inline with label) ----------------------------
    auto* typeRow = new wxBoxSizer(wxHORIZONTAL);
    auto* typeLabel = new wxStaticText(settingsPanel, wxID_ANY, "Runner type:");
    typeLabel->SetForegroundColour(Style::TextMuted);
    typeLabel->SetFont(wxFont(8, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
        wxFONTWEIGHT_NORMAL, false, "Segoe UI"));

    m_runnerTypeChoice = new wxChoice(settingsPanel, wxID_ANY,
        wxDefaultPosition, wxSize(kCtrlColWidth, -1));
    m_runnerTypeChoice->SetBackgroundColour(Style::BtnSmall);
    m_runnerTypeChoice->SetForegroundColour(Style::TextMuted);
    m_runnerTypeChoice->Append("Cylindrical");
    m_runnerTypeChoice->SetSelection(0);
    typeRow->Add(typeLabel, 0, wxALIGN_CENTER_VERTICAL);
    typeRow->AddStretchSpacer(1);
    typeRow->Add(m_runnerTypeChoice, 0, wxALIGN_CENTER_VERTICAL);
    settingsSizer->Add(typeRow, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 10);

    // ---- Dimensions panel (shown for Cylindrical) ---------------------------
    auto* dimsPanel = new wxPanel(settingsPanel, wxID_ANY);
    dimsPanel->SetBackgroundColour(Style::CardBg);

    auto* dimsSizer = new wxBoxSizer(wxVERTICAL);

    // Diameter row
    {
        auto* row = new wxBoxSizer(wxHORIZONTAL);

        auto* lbl = new wxStaticText(dimsPanel, wxID_ANY, "Diameter:");
        lbl->SetForegroundColour(Style::TextMuted);
        lbl->SetFont(wxFont(8, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
            wxFONTWEIGHT_NORMAL, false, "Segoe UI"));

        m_runnerDiameter = new wxTextCtrl(dimsPanel, wxID_ANY, "4.0",
            wxDefaultPosition, wxSize(kFieldWidth, 22));
        m_runnerDiameter->SetBackgroundColour(Style::BtnSmall);
        m_runnerDiameter->SetForegroundColour(kTextDefault);

        auto* unit = new wxStaticText(dimsPanel, wxID_ANY, "mm");
        unit->SetForegroundColour(Style::TextSubtle);
        unit->SetFont(wxFont(8, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
            wxFONTWEIGHT_NORMAL, false, "Segoe UI"));
        unit->SetMinSize(wxSize(kUnitWidth, -1));

        row->Add(lbl, 0, wxALIGN_CENTER_VERTICAL);
        row->AddStretchSpacer(1);
        row->Add(m_runnerDiameter, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, kFieldGap);
        row->Add(unit, 0, wxALIGN_CENTER_VERTICAL);
        dimsSizer->Add(row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 10);
    }

    // Cold Slug Well Row
    {
        auto* row = new wxBoxSizer(wxHORIZONTAL);

        auto* lbl = new wxStaticText(dimsPanel, wxID_ANY, "Cold slug length:");
        lbl->SetForegroundColour(Style::TextMuted);
        lbl->SetFont(wxFont(8, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
            wxFONTWEIGHT_NORMAL, false, "Segoe UI"));

        m_runnerColdSlugDepth = new wxTextCtrl(dimsPanel, wxID_ANY, "5.0",
            wxDefaultPosition, wxSize(kFieldWidth, 22));
        m_runnerColdSlugDepth->SetBackgroundColour(Style::BtnSmall);
        m_runnerColdSlugDepth->SetForegroundColour(kTextDefault);

        auto* unit = new wxStaticText(dimsPanel, wxID_ANY, "mm");
        unit->SetForegroundColour(Style::TextSubtle);
        unit->SetFont(wxFont(8, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
            wxFONTWEIGHT_NORMAL, false, "Segoe UI"));
        unit->SetMinSize(wxSize(kUnitWidth, -1));

        row->Add(lbl, 0, wxALIGN_CENTER_VERTICAL);
        row->AddStretchSpacer(1);
        row->Add(m_runnerColdSlugDepth, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, kFieldGap);
        row->Add(unit, 0, wxALIGN_CENTER_VERTICAL);
        dimsSizer->Add(row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 10);
    }

    dimsPanel->SetSizer(dimsSizer);
    settingsSizer->Add(dimsPanel, 0, wxEXPAND | wxBOTTOM, 10);

    settingsPanel->SetSizer(settingsSizer);
    settingsPanel->Show(false);
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
            settingsBtn->SetBitmap(LoadSvgBundle(
                expanded ? kChevronDownSvg : kChevronRightSvg,
                wxSize(12, 12), true));
            settingsBtn->SetBitmapPosition(wxRIGHT);
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
    panel->SetBackgroundColour(Style::CardBg);
    auto* sizer = new wxBoxSizer(wxVERTICAL);

    auto* titleLabel = new wxStaticText(panel, wxID_ANY, "Gates");
    titleLabel->SetForegroundColour(*wxWHITE);
    titleLabel->SetFont(wxFont(9, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
        wxFONTWEIGHT_BOLD, false, "Segoe UI"));
    sizer->Add(titleLabel, 0, wxLEFT | wxTOP, 12);
    sizer->AddSpacer(6);

    auto* btnPlace = new wxToggleButton(panel, ID_PlaceGate, "Place Gate",
        wxDefaultPosition, wxSize(-1, 32), wxBORDER_NONE);
    btnPlace->SetBackgroundColour(Style::BtnPlace);
    btnPlace->SetForegroundColour(*wxWHITE);
    btnPlace->SetFont(wxFont(9, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
        wxFONTWEIGHT_SEMIBOLD, false, "Segoe UI"));
    sizer->Add(btnPlace, 0, wxEXPAND | wxLEFT | wxRIGHT, 12);
    sizer->AddSpacer(6);

    auto* actionGrid = new wxGridSizer(1, 3, 0, 4);
    auto makeSmallBtn = [&](const wxString& label) -> wxButton* {
        auto* btn = new wxButton(panel, wxID_ANY, label,
            wxDefaultPosition, wxSize(-1, 26), wxBORDER_NONE);
        btn->SetBackgroundColour(Style::BtnSmall);
        btn->SetForegroundColour(Style::TextPrimary);
        btn->SetFont(wxFont(8, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
            wxFONTWEIGHT_NORMAL, false, "Segoe UI"));
        return btn;
        };
    auto* btnEdit = makeSmallBtn("Edit");
    btnEdit->SetId(ID_EditGate);
    auto* btnRemove = makeSmallBtn("Remove");
    btnRemove->SetId(ID_RemoveGate);
    auto* btnClearAll = makeSmallBtn("Clear all");
    btnClearAll->SetId(ID_ClearGates);
    actionGrid->Add(btnEdit, 0, wxEXPAND);
    actionGrid->Add(btnRemove, 0, wxEXPAND);
    actionGrid->Add(btnClearAll, 0, wxEXPAND);
    sizer->Add(actionGrid, 0, wxEXPAND | wxLEFT | wxRIGHT, 12);
    sizer->AddSpacer(8);

    // Collapsible Settings
    auto* settingsBtn = new wxToggleButton(panel, wxID_ANY,
        "Settings",
        wxDefaultPosition, wxSize(-1, 22), wxBU_LEFT | wxBORDER_NONE);
    settingsBtn->SetValue(false);
    settingsBtn->SetBackgroundColour(Style::CardBg);
    settingsBtn->SetForegroundColour(Style::TextSubtle);
    settingsBtn->SetFont(wxFont(9, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
        wxFONTWEIGHT_NORMAL, false, "Segoe UI"));
    settingsBtn->SetBitmap(LoadSvgBundle(kChevronRightSvg, wxSize(12, 12), true));
    settingsBtn->SetBitmapPosition(wxRIGHT);
    sizer->Add(settingsBtn, 0, wxEXPAND | wxLEFT | wxRIGHT, 12);

    auto* settingsPanel = new wxPanel(panel, wxID_ANY);
    settingsPanel->SetBackgroundColour(Style::CardBg);
    auto* settingsSizer = new wxBoxSizer(wxVERTICAL);

    // Helper for dimension rows
    auto addRow = [&](wxWindow* parent_, wxSizer* parentSz,
        const wxString& label, wxTextCtrl*& ctrl,
        const wxString& defVal, const wxString& unitStr, int lblW = 60)
        {
            auto* row = new wxBoxSizer(wxHORIZONTAL);
            auto* lbl = new wxStaticText(parent_, wxID_ANY, label);
            lbl->SetForegroundColour(Style::TextMuted);
            lbl->SetFont(wxFont(8, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL, wxFONTWEIGHT_NORMAL, false, "Segoe UI"));
            ctrl = new wxTextCtrl(parent_, wxID_ANY, defVal, wxDefaultPosition, wxSize(kFieldWidth, 22));
            ctrl->SetBackgroundColour(Style::BtnSmall); ctrl->SetForegroundColour(kTextDefault);
            auto* u = new wxStaticText(parent_, wxID_ANY, unitStr);
            u->SetForegroundColour(Style::TextSubtle);
            u->SetFont(wxFont(8, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL, wxFONTWEIGHT_NORMAL, false, "Segoe UI"));
            u->SetMinSize(wxSize(kUnitWidth, -1));
            row->Add(lbl, 0, wxALIGN_CENTER_VERTICAL);
            row->AddStretchSpacer(1);
            row->Add(ctrl, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, kFieldGap);
            row->Add(u, 0, wxALIGN_CENTER_VERTICAL);
            parentSz->Add(row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 10);
        };

    // ---- Gate type dropdown (inline with label) ------------------------------
    auto* typeRow = new wxBoxSizer(wxHORIZONTAL);
    auto* typeLabel = new wxStaticText(settingsPanel, wxID_ANY, "Gate type:");
    typeLabel->SetForegroundColour(Style::TextMuted);
    typeLabel->SetFont(wxFont(8, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
        wxFONTWEIGHT_NORMAL, false, "Segoe UI"));

    m_gateTypeChoice = new wxChoice(settingsPanel, wxID_ANY,
        wxDefaultPosition, wxSize(kCtrlColWidth, -1));
    m_gateTypeChoice->SetBackgroundColour(Style::BtnSmall);
    m_gateTypeChoice->SetForegroundColour(Style::TextMuted);
    m_gateTypeChoice->Append("Tapered Cylinder");
    m_gateTypeChoice->SetSelection(0);
    typeRow->Add(typeLabel, 0, wxALIGN_CENTER_VERTICAL);
    typeRow->AddStretchSpacer(1);
    typeRow->Add(m_gateTypeChoice, 0, wxALIGN_CENTER_VERTICAL);
    settingsSizer->Add(typeRow, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 10);

    // Gate dimensions
    auto* dimsPanel = new wxPanel(settingsPanel, wxID_ANY);
    dimsPanel->SetBackgroundColour(Style::CardBg);
    auto* dimsSizer = new wxBoxSizer(wxVERTICAL);
    addRow(dimsPanel, dimsSizer, "Diameter:", m_gateDiameter, "3.0", "mm");
    addRow(dimsPanel, dimsSizer, "Draft angle:", m_gateDraftAngle, "1.0", wxString::FromUTF8("\xC2\xB0"));
    dimsPanel->SetSizer(dimsSizer);
    settingsSizer->Add(dimsPanel, 0, wxEXPAND | wxBOTTOM, 6);

    // ---- Sub-runner divider ------------------------------------------------
    auto* subSep = new wxPanel(settingsPanel, wxID_ANY, wxDefaultPosition, wxSize(-1, 1));
    subSep->SetBackgroundColour(Style::Divider);
    settingsSizer->Add(subSep, 0, wxEXPAND | wxLEFT | wxRIGHT, 10);

    // ---- Sub-runner type dropdown (inline with label) ------------------------
    auto* subTypeRow = new wxBoxSizer(wxHORIZONTAL);
    auto* subTypeLabel = new wxStaticText(settingsPanel, wxID_ANY, "Sub-runner type:");
    subTypeLabel->SetForegroundColour(Style::TextMuted);
    subTypeLabel->SetFont(wxFont(8, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
        wxFONTWEIGHT_NORMAL, false, "Segoe UI"));

    m_subRunnerTypeChoice = new wxChoice(settingsPanel, wxID_ANY,
        wxDefaultPosition, wxSize(kCtrlColWidth, -1));
    m_subRunnerTypeChoice->SetBackgroundColour(Style::BtnSmall);
    m_subRunnerTypeChoice->SetForegroundColour(Style::TextMuted);
    m_subRunnerTypeChoice->Append("Cylinder");
    m_subRunnerTypeChoice->SetSelection(0);
    subTypeRow->Add(subTypeLabel, 0, wxALIGN_CENTER_VERTICAL);
    subTypeRow->AddStretchSpacer(1);
    subTypeRow->Add(m_subRunnerTypeChoice, 0, wxALIGN_CENTER_VERTICAL);
    settingsSizer->Add(subTypeRow, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 10);

    // Sub-runner dimensions
    auto* subDimsPanel = new wxPanel(settingsPanel, wxID_ANY);
    subDimsPanel->SetBackgroundColour(Style::CardBg);
    auto* subDimsSizer = new wxBoxSizer(wxVERTICAL);
    addRow(subDimsPanel, subDimsSizer, "Diameter:", m_subRunnerDiameter, "5.0", "mm");
    subDimsPanel->SetSizer(subDimsSizer);
    settingsSizer->Add(subDimsPanel, 0, wxEXPAND | wxBOTTOM, 10);

    settingsPanel->SetSizer(settingsSizer);
    settingsPanel->Show(false);
    sizer->Add(settingsPanel, 0, wxEXPAND);

    m_gateTypeChoice->Bind(wxEVT_CHOICE, [dimsPanel, this](wxCommandEvent&) {
        dimsPanel->Show(m_gateTypeChoice->GetStringSelection() == "Tapered Cylinder");
        dimsPanel->GetParent()->Layout(); dimsPanel->GetParent()->GetParent()->Layout();
        dimsPanel->GetParent()->GetParent()->GetParent()->Layout();
        });

    settingsBtn->Bind(wxEVT_TOGGLEBUTTON, [settingsBtn, settingsPanel, panel](wxCommandEvent&) {
        static wxLongLong lastToggleMs = 0;
        wxLongLong now = wxGetLocalTimeMillis();
        if ((now - lastToggleMs).GetValue() < 200) { settingsBtn->SetValue(!settingsBtn->GetValue()); return; }
        lastToggleMs = now;
        const bool expanded = settingsBtn->GetValue();
        settingsBtn->SetBitmap(LoadSvgBundle(
            expanded ? kChevronDownSvg : kChevronRightSvg,
            wxSize(12, 12), true));
        settingsBtn->SetBitmapPosition(wxRIGHT);
        settingsPanel->Show(expanded);
        panel->Layout(); panel->GetParent()->Layout(); panel->GetParent()->GetParent()->Layout();
        });

    panel->SetSizer(sizer);
    return panel;
}

wxPanel* MainFrame::CreateLeftPanel(wxWindow* parent)
{
    // Outer container: content column + right border
    auto* outer = new wxPanel(parent, wxID_ANY,
        wxDefaultPosition, wxSize(300, -1));
    outer->SetBackgroundColour(kRibbonBg);
    auto* outerSizer = new wxBoxSizer(wxHORIZONTAL);

    // Content column: fixed Model Tools on top, scrollable mould settings below
    auto* column = new wxPanel(outer, wxID_ANY);
    column->SetBackgroundColour(kRibbonBg);
    auto* colSizer = new wxBoxSizer(wxVERTICAL);

    // ---- Model Tools section (fixed, non-scrolling) -------------------------
    {

        auto* toolsPanel = new wxPanel(column, wxID_ANY);
        toolsPanel->SetBackgroundColour(kRibbonBg);
        auto* toolsSizer = new wxBoxSizer(wxVERTICAL);

        // Header matching the MOULD TOOL SETTINGS style
        auto* toolsLabel = new wxStaticText(toolsPanel, wxID_ANY, "MODEL TOOLS");
        toolsLabel->SetForegroundColour(Style::TextPrimary);
        toolsLabel->SetFont(wxFont(7, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
            wxFONTWEIGHT_BOLD, false, "Segoe UI"));
        toolsSizer->Add(toolsLabel, 0, wxLEFT | wxTOP, 12);

        //auto* toolsLine = new wxPanel(toolsPanel, wxID_ANY,
        //    wxDefaultPosition, wxSize(-1, 1));
        //toolsLine->SetBackgroundColour(Style::TextPrimary);
        //toolsSizer->Add(toolsLine, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 10);

        toolsSizer->AddSpacer(8);

        auto* grid = new wxGridSizer(2, 2, 4, 4);

        // ---- SVG icon paths for model tool buttons --------------------------------
        // Fill in the path to each SVG file (relative to the executable, or absolute).
        // Leave a string empty ("") to show the text label only.
        static const wxString kIconMove = "res/icons/arrows-move.svg";
        static const wxString kIconRotate = "res/icons/rotate-2.svg";
        static const wxString kIconScale = "res/icons/resize.svg";
        static const wxString kIconCenter = "res/icons/focus-centered.svg";

        // Helper: load an SVG, recolor all strokes and fills to white, and return a
        // wxBitmapBundle.  Relative paths are anchored to the executable directory.
        // Returns an invalid bundle (IsOk() == false) if the path is empty or missing.
        auto LoadToolIcon = [](const wxString& svgPath) -> wxBitmapBundle
            {
                if (svgPath.IsEmpty())
                    return wxBitmapBundle();

                wxFileName fn(svgPath);
                if (fn.IsRelative())
                {
                    wxFileName exeDir(wxStandardPaths::Get().GetExecutablePath());
                    fn.MakeAbsolute(exeDir.GetPath());
                }

                // Read raw SVG text so we can override its colors before rendering.
                wxFile file(fn.GetFullPath());
                if (!file.IsOpened())
                    return wxBitmapBundle();
                wxString svg;
                file.ReadAll(&svg);

                // Replace the most common color tokens used by icon sets (e.g. Lucide)
                // with plain white so the icon matches the button text color.
                svg.Replace("currentColor", "white");
                svg.Replace("\"black\"", "\"white\"");
                svg.Replace("\"#000000\"", "\"white\"");
                svg.Replace("\"#000\"", "\"white\"");

                const wxScopedCharBuffer utf8 = svg.utf8_str();
                return wxBitmapBundle::FromSVG(utf8.data(), wxSize(18, 18));
            };

        static const wxFont kToolBtnFont(9, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
            wxFONTWEIGHT_BOLD, false, "Segoe UI");

        auto makeToolBtn = [&](int id, const wxString& label, bool toggle,
            const wxString& svgPath = "") -> wxWindow*
            {
                // Use a plain wxPanel so we can freely position the icon+text
                // with a sizer, giving true centred layout that native buttons
                // won't provide once a bitmap is attached.
                auto* panel = new wxPanel(toolsPanel, wxID_ANY,
                    wxDefaultPosition, wxSize(-1, 34), wxBORDER_NONE);
                panel->SetBackgroundColour(Style::BtnSecondary);

                // Inner horizontal sizer: [icon] [gap] [label]
                auto* hSizer = new wxBoxSizer(wxHORIZONTAL);

                wxStaticBitmap* bmpCtrl = nullptr;
                wxBitmapBundle icon = LoadToolIcon(svgPath);
                if (icon.IsOk())
                {
                    bmpCtrl = new wxStaticBitmap(panel, wxID_ANY,
                        icon.GetBitmapFor(panel));
                    hSizer->Add(bmpCtrl, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 5);
                }

                auto* txt = new wxStaticText(panel, wxID_ANY, label);
                txt->SetForegroundColour(Style::TextPrimary);
                txt->SetBackgroundColour(Style::BtnSecondary);
                txt->SetFont(kToolBtnFont);
                hSizer->Add(txt, 0, wxALIGN_CENTER_VERTICAL);

                // Wrap in a centering sizer using stretch spacers
                auto* outer = new wxBoxSizer(wxHORIZONTAL);
                outer->AddStretchSpacer(1);
                outer->Add(hSizer, 0, wxALIGN_CENTER_VERTICAL);
                outer->AddStretchSpacer(1);
                panel->SetSizer(outer);

                // Shared toggle state (avoids raw-pointer lifetime issues)
                auto toggled = std::make_shared<bool>(false);

                // Helpers to apply normal / hover / active colours
                auto applyColours = [=](const wxColour& bg, const wxColour& fg) {
                    panel->SetBackgroundColour(bg);
                    txt->SetBackgroundColour(bg);
                    txt->SetForegroundColour(fg);
                    panel->Refresh();
                    txt->Refresh();
                    };

                // Left-click: fire the appropriate command event and update visuals
                auto onClick = [=](wxMouseEvent& e) {
                    if (toggle)
                    {
                        *toggled = !*toggled;
                        applyColours(*toggled ? Style::BtnSecondarySelected : Style::BtnSecondary,
                            *toggled ? kTextActive : Style::TextPrimary);
                        wxCommandEvent evt(wxEVT_TOGGLEBUTTON, id);
                        evt.SetEventObject(panel);
                        evt.SetInt(*toggled ? 1 : 0);
                        panel->GetEventHandler()->ProcessEvent(evt);
                    }
                    else
                    {
                        wxCommandEvent evt(wxEVT_BUTTON, id);
                        evt.SetEventObject(panel);
                        panel->GetEventHandler()->ProcessEvent(evt);
                    }
                    e.Skip();
                    };

                // Hover colours (only when not toggled-on)
                auto onEnter = [=](wxMouseEvent& e) {
                    if (!*toggled)
                        applyColours(Style::BtnSecondaryHover, Style::TextPrimary);
                    e.Skip();
                    };
                auto onLeave = [=](wxMouseEvent& e) {
                    if (!*toggled)
                        applyColours(Style::BtnSecondary, Style::TextPrimary);
                    e.Skip();
                    };

                // Bind events to the panel and every child so the full hit-area works
                for (wxWindow* w : { (wxWindow*)panel, (wxWindow*)txt,
                                     (wxWindow*)bmpCtrl })
                {
                    if (!w) continue;
                    w->Bind(wxEVT_LEFT_UP, onClick);
                    w->Bind(wxEVT_ENTER_WINDOW, onEnter);
                    w->Bind(wxEVT_LEAVE_WINDOW, onLeave);
                }

                return panel;
            };

        grid->Add(makeToolBtn(ID_ToolTranslate, "Move", true, kIconMove), 0, wxEXPAND);
        grid->Add(makeToolBtn(ID_ToolRotate, "Rotate", true, kIconRotate), 0, wxEXPAND);
        grid->Add(makeToolBtn(ID_ToolScale, "Scale", true, kIconScale), 0, wxEXPAND);
        grid->Add(makeToolBtn(ID_ToolCenter, "Center", false, kIconCenter), 0, wxEXPAND);

        toolsSizer->Add(grid, 0, wxEXPAND | wxLEFT | wxRIGHT, 12);
        toolsSizer->AddSpacer(10);

        toolsPanel->SetSizer(toolsSizer);
        colSizer->Add(toolsPanel, 0, wxEXPAND);
    }

    //Create dividing line
    auto* titleLine = new wxPanel(column, wxID_ANY,
        wxDefaultPosition, wxSize(-1, 1));
    titleLine->SetBackgroundColour(Style::TextPrimary);
    colSizer->Add(titleLine, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 10);


    // ---- Mould settings title (fixed, non-scrolling) ------------------------
    auto* title = new wxStaticText(column, wxID_ANY, "MOULD TOOL SETTINGS");
    title->SetForegroundColour(Style::TextPrimary);
    title->SetFont(wxFont(7, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
        wxFONTWEIGHT_BOLD, false, "Segoe UI"));
    colSizer->Add(title, 0, wxLEFT | wxTOP, 12);
    colSizer->AddSpacer(8);


    // ---- Scrollable mould tool settings -------------------------------------
    auto* scrollWin = new wxScrolledWindow(column, wxID_ANY,
        wxDefaultPosition, wxDefaultSize,
        wxVSCROLL | wxBORDER_NONE);
    scrollWin->SetScrollRate(0, 8);
    scrollWin->SetBackgroundColour(kRibbonBg);

    auto* sizer = new wxBoxSizer(wxVERTICAL);

    sizer->AddSpacer(4);

    // ---- Feature sections (headers built into each content panel) -----------
    wxPanel* spruesContent = CreateSpruesContent(scrollWin);
    sizer->Add(spruesContent, 0, wxEXPAND | wxTOP, 8);

    wxPanel* runnersContent = CreateRunnersContent(scrollWin);
    sizer->Add(runnersContent, 0, wxEXPAND | wxTOP, 8);

    wxPanel* gatesContent = CreateGatesContent(scrollWin);
    sizer->Add(gatesContent, 0, wxEXPAND | wxTOP, 8);

    wxPanel* ventsContent = CreateVentsContent(scrollWin);
    sizer->Add(ventsContent, 0, wxEXPAND | wxTOP, 8);

    sizer->AddSpacer(12);

    scrollWin->SetSizer(sizer);
    colSizer->Add(scrollWin, 1, wxEXPAND);   // scroll area fills remaining space

    column->SetSizer(colSizer);
    outerSizer->Add(column, 1, wxEXPAND);

    // Right border line
    auto* borderLine = new wxPanel(outer, wxID_ANY,
        wxDefaultPosition, wxSize(1, -1));
    borderLine->SetBackgroundColour(Style::Divider);
    outerSizer->Add(borderLine, 0, wxEXPAND);

    outer->SetSizer(outerSizer);
    return outer;
}