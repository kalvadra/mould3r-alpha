#include "PreviewPanel.h"
#include "GLCanvas.h"
#include "style.h"
#include "DesignChecks.h"
#include "RoundedButton.h"

#include <wx/spinctrl.h>
#include <wx/scrolwin.h>
#include <wx/tglbtn.h>
#include <wx/bmpbndl.h>
#include <wx/file.h>
#include <wx/filename.h>
#include <wx/stdpaths.h>

#include <algorithm>
#include <functional>
#include <string>
#include <utility>
#include <vector>

// ---------------------------------------------------------------------------
// Chevron icons + SVG loader, kept file-local so the preview's collapsible
// cards match the Prepare-side collapsible sections. (MainFrame has its own
// internal-linkage copy; duplicated here to avoid coupling the two TUs.)
// ---------------------------------------------------------------------------
static const wxString kChevronDownSvg = "res/icons/chevron-down.svg";
static const wxString kChevronRightSvg = "res/icons/chevron-right.svg";

static wxBitmapBundle LoadSvgBundle(const wxString& svgPath,
    const wxSize& size, bool recolorWhite = false)
{
    if (svgPath.IsEmpty()) return wxBitmapBundle();

    wxFileName fn(svgPath);
    if (fn.IsRelative())
    {
        wxFileName exeDir(wxStandardPaths::Get().GetExecutablePath());
        fn.MakeAbsolute(exeDir.GetPath());
    }

    wxFile file(fn.GetFullPath());
    if (!file.IsOpened()) return wxBitmapBundle();

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
// Id base for the visibility checkboxes — one sequential id per part so each
// checkbox has a unique, stable id (the part index it controls is captured in
// the handler lambda).
// ---------------------------------------------------------------------------
static const int kHalfToggleIdBase = wxID_HIGHEST + 5000;

// Letter label for a half index: 0 -> "A", 1 -> "B", ... wrapping is not a
// concern (moulds have two halves), but stay defined past 'Z' just in case.
static std::string HalfLetter(int index)
{
    if (index < 0) return std::string();
    if (index < 26) return std::string(1, char('A' + index));
    return std::to_string(index + 1);
}

// Feature-input field metrics, mirroring the Prepare-side mould-feature rows.
static const int kFieldWidth = 90;   // text-entry width (px)
static const int kUnitWidth = 28;    // fixed unit-label column (px)
static const int kFieldGap = 4;      // gap between field and unit label

// A feature-style parameter row: label (left) + text field + unit label
// (right), matching the mould-feature inputs. Returns the field for reads.
static wxTextCtrl* AddFieldRow(wxWindow* parent, wxBoxSizer* into,
    const wxString& label, const wxString& defaultVal, const wxString& unitStr)
{
    auto* row = new wxBoxSizer(wxHORIZONTAL);

    auto* lbl = new wxStaticText(parent, wxID_ANY, label);
    lbl->SetForegroundColour(Style::TextMuted);
    lbl->SetFont(wxFont(8, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
        wxFONTWEIGHT_NORMAL, false, "Segoe UI"));

    auto* ctrl = new wxTextCtrl(parent, wxID_ANY, defaultVal,
        wxDefaultPosition, wxSize(kFieldWidth, 22));
    ctrl->SetBackgroundColour(Style::BtnSmall);
    ctrl->SetForegroundColour(Style::TextPrimary);

    auto* unit = new wxStaticText(parent, wxID_ANY, unitStr);
    unit->SetForegroundColour(Style::TextSubtle);
    unit->SetFont(wxFont(8, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
        wxFONTWEIGHT_NORMAL, false, "Segoe UI"));
    unit->SetMinSize(wxSize(kUnitWidth, -1));

    row->Add(lbl, 0, wxALIGN_CENTER_VERTICAL);
    row->AddStretchSpacer(1);
    row->Add(ctrl, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, kFieldGap);
    row->Add(unit, 0, wxALIGN_CENTER_VERTICAL);
    into->Add(row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 10);
    return ctrl;
}

// Parse a numeric field, falling back to a default on empty/garbage input.
static double ParseField(wxTextCtrl* ctrl, double defaultVal)
{
    if (!ctrl) return defaultVal;
    double v = 0.0;
    if (ctrl->GetValue().ToDouble(&v)) return v;
    return defaultVal;
}

// ---------------------------------------------------------------------------
// Construction — builds the static perspective UI (left simulations panel with
// the show/hide checkboxes, preview canvas, info panel) with no data loaded.
// The canvas renders its grid immediately; parts arrive later via SetData.
// ---------------------------------------------------------------------------
PreviewPanel::PreviewPanel(wxWindow* parent)
    : wxPanel(parent, wxID_ANY)
{
    SetBackgroundColour(Style::AppBg);

    auto* root = new wxBoxSizer(wxVERTICAL);

    // ---- Main row: simulations | canvas | information ---------------------
    auto* middle = new wxBoxSizer(wxHORIZONTAL);

    wxPanel* simPanel = BuildSimPanel(this);
    middle->Add(simPanel, 0, wxEXPAND | wxRIGHT, 1);

    m_canvas = new GLCanvas(this);
    m_canvas->SetPreviewMode(true);
    middle->Add(m_canvas, 1, wxEXPAND);

    wxPanel* infoPanel = BuildInfoPanel(this);
    middle->Add(infoPanel, 0, wxEXPAND | wxLEFT, 1);

    root->Add(middle, 1, wxEXPAND);

    SetSizer(root);

    UpdateInfoPanel();  // populate the "no shot yet" state
}

// ---------------------------------------------------------------------------
// (Re)seed the preview with a fresh set of post-cut halves and an optional shot.
// ---------------------------------------------------------------------------
void PreviewPanel::SetData(const std::vector<FileImporter::MeshData>& halves,
    const ShotPreviewInput& shot)
{
    // Stash the new data, replacing whatever the previous generation left.
    m_pendingHalves = halves;

    m_shotMesh = FileImporter::MeshData();
    m_shotShape = TopoDS_Shape();
    m_shotFaceIds.clear();
    m_halfShapes.clear();
    m_hasShot = false;
    m_shotVolumeMm3 = 0.0;

    if (shot.mesh)
    {
        m_shotMesh = *shot.mesh;
        if (shot.shape)   m_shotShape = *shot.shape;
        if (shot.faceIds) m_shotFaceIds = *shot.faceIds;
        if (shot.halves)  m_halfShapes = *shot.halves;
        m_shotVolumeMm3 = shot.volumeMm3;
        m_hasShot = true;
    }
    // The shot is appended after the mould halves in LoadHalves, so its
    // preview-part index is the half count.
    m_shotHalfIndex = m_hasShot ? (int)halves.size() : -1;

    // Reset any debug overlay state carried over from the previous generation.
    m_hasResult = false;
    m_activeDebugCategory = -1;
    m_showRays = false;
    m_showContacts = false;
    m_undercutRays.clear();

    // Drop the previous generation's GPU parts now (clears its own context).
    if (m_canvas)
    {
        m_canvas->ClearShotDebugColoring();
        m_canvas->ShowShotDebugRays(false);
        m_canvas->ShowShotDebugContacts(false);
        m_canvas->ShowShotDebugSolid(false);
        m_canvas->ClearPreviewHalves();
    }

    // Rebuild the dynamic UI for the new part set.
    ClearVisibilityChecks();
    BuildVisibilityChecks((int)halves.size(), m_hasShot);
    UpdateInfoPanel();

    // The mesh upload waits until we're actually visible — a canvas on a hidden
    // book page may not have a valid drawable yet.
    m_dataDirty = true;
    FlushIfDirty();
}

// ---------------------------------------------------------------------------
// Reset to the empty (grid-only) state.
// ---------------------------------------------------------------------------
void PreviewPanel::ClearData()
{
    m_pendingHalves.clear();
    m_shotMesh = FileImporter::MeshData();
    m_shotShape = TopoDS_Shape();
    m_shotFaceIds.clear();
    m_halfShapes.clear();
    m_hasShot = false;
    m_shotVolumeMm3 = 0.0;
    m_shotHalfIndex = -1;

    m_hasResult = false;
    m_activeDebugCategory = -1;
    m_showRays = false;
    m_showContacts = false;
    m_undercutRays.clear();

    if (m_canvas)
    {
        m_canvas->ClearShotDebugColoring();
        m_canvas->ShowShotDebugRays(false);
        m_canvas->ShowShotDebugContacts(false);
        m_canvas->ShowShotDebugSolid(false);
        m_canvas->ClearPreviewHalves();
        m_canvas->Refresh(false);
    }

    ClearVisibilityChecks();
    UpdateInfoPanel();
    m_dataDirty = false;
}

// ---------------------------------------------------------------------------
// Push staged meshes into the canvas once we're visible. No-op when nothing is
// pending or the panel is still hidden (MainFrame calls this again on show).
// ---------------------------------------------------------------------------
void PreviewPanel::FlushIfDirty()
{
    if (!m_dataDirty || !m_canvas) return;
    if (!IsShownOnScreen()) return;   // wait until the book page is visible

    // CallAfter so the page is laid out and the canvas realized (its GL context
    // valid) before AddPreviewHalf issues any GL call.
    CallAfter([this]()
        {
            if (!m_dataDirty) return;
            LoadHalves();
            m_dataDirty = false;
        });
}

// ---------------------------------------------------------------------------
// Left panel — runnable simulations. Styled to match the Prepare-side left
// panel: a fixed-width column (AppBg) with a section title and a scrollable
// stack of collapsible cards (CardBg, chevron headers — the mould-feature
// look). Each simulation is its own collapsible card.
// ---------------------------------------------------------------------------
wxPanel* PreviewPanel::BuildSimPanel(wxWindow* parent)
{
    auto* outer = new wxPanel(parent, wxID_ANY,
        wxDefaultPosition, wxSize(280, -1));
    outer->SetBackgroundColour(Style::AppBg);
    auto* outerSizer = new wxBoxSizer(wxHORIZONTAL);

    auto* column = new wxPanel(outer, wxID_ANY);
    column->SetBackgroundColour(Style::AppBg);
    auto* colSizer = new wxBoxSizer(wxVERTICAL);

    // ---- Preview Output Bodies (visibility), above Simulations ------------
    // Section label + a card holding one checkbox per loaded part, or a
    // "No bodies generated" message before anything is generated.
    auto* visTitle = new wxStaticText(column, wxID_ANY, "PREVIEW OUTPUT BODIES");
    visTitle->SetForegroundColour(Style::TextPrimary);
    visTitle->SetFont(wxFont(7, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
        wxFONTWEIGHT_BOLD, false, "Segoe UI"));
    colSizer->Add(visTitle, 0, wxLEFT | wxTOP, 12);
    colSizer->AddSpacer(6);

    m_visPanel = new wxPanel(column, wxID_ANY);
    m_visPanel->SetBackgroundColour(Style::CardBg);
    auto* visSizer = new wxBoxSizer(wxVERTICAL);

    m_visEmptyLabel = new wxStaticText(m_visPanel, wxID_ANY, "No bodies generated");
    m_visEmptyLabel->SetForegroundColour(Style::TextMuted);
    m_visEmptyLabel->SetFont(wxFont(9, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_ITALIC,
        wxFONTWEIGHT_NORMAL, false, "Segoe UI"));
    visSizer->Add(m_visEmptyLabel, 0, wxALL, 10);

    m_visPanel->SetSizer(visSizer);
    colSizer->Add(m_visPanel, 0, wxEXPAND | wxLEFT | wxRIGHT, 12);

    // Divider between the bodies card and the simulations (matches the
    // Prepare panel's section divider).
    auto* divider = new wxPanel(column, wxID_ANY, wxDefaultPosition, wxSize(-1, 1));
    divider->SetBackgroundColour(Style::Divider);
    colSizer->Add(divider, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 10);

    // Section title (matches the Prepare panel's "MOULD TOOL SETTINGS").
    auto* title = new wxStaticText(column, wxID_ANY, "SIMULATIONS");
    title->SetForegroundColour(Style::TextPrimary);
    title->SetFont(wxFont(7, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
        wxFONTWEIGHT_BOLD, false, "Segoe UI"));
    colSizer->Add(title, 0, wxLEFT | wxTOP, 12);
    colSizer->AddSpacer(8);

    auto* scrollWin = new wxScrolledWindow(column, wxID_ANY,
        wxDefaultPosition, wxDefaultSize, wxVSCROLL | wxBORDER_NONE);
    scrollWin->SetScrollRate(0, 8);
    scrollWin->SetBackgroundColour(Style::AppBg);
    auto* sizer = new wxBoxSizer(wxVERTICAL);
    sizer->AddSpacer(4);

    // Collapsible card factory: a CardBg card with a chevron header that
    // expands/collapses its body. `fill` populates the body (parent + sizer
    // supplied) so each simulation drops its own controls in.
    auto makeCard = [&](const wxString& simName,
        const std::function<void(wxWindow*, wxBoxSizer*)>& fill)
    {
        auto* card = new wxPanel(scrollWin, wxID_ANY);
        card->SetBackgroundColour(Style::CardBg);
        auto* cardSizer = new wxBoxSizer(wxVERTICAL);

        auto* header = new wxToggleButton(card, wxID_ANY, simName,
            wxDefaultPosition, wxSize(-1, 28), wxBU_LEFT | wxBORDER_NONE);
        header->SetValue(true);
        header->SetBackgroundColour(Style::CardBg);
        header->SetForegroundColour(*wxWHITE);
        header->SetFont(wxFont(9, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
            wxFONTWEIGHT_BOLD, false, "Segoe UI"));
        header->SetBitmap(LoadSvgBundle(kChevronDownSvg, wxSize(12, 12), true));
        header->SetBitmapPosition(wxRIGHT);
        cardSizer->Add(header, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 8);

        auto* body = new wxPanel(card, wxID_ANY);
        body->SetBackgroundColour(Style::CardBg);
        auto* bodySizer = new wxBoxSizer(wxVERTICAL);
        fill(body, bodySizer);
        body->SetSizer(bodySizer);
        cardSizer->Add(body, 0, wxEXPAND);

        header->Bind(wxEVT_TOGGLEBUTTON, [header, body, card](wxCommandEvent&)
        {
            const bool expanded = header->GetValue();
            header->SetBitmap(LoadSvgBundle(
                expanded ? kChevronDownSvg : kChevronRightSvg,
                wxSize(12, 12), true));
            header->SetBitmapPosition(wxRIGHT);
            body->Show(expanded);
            card->Layout();
            if (card->GetParent()) card->GetParent()->Layout();
        });

        card->SetSizer(cardSizer);
        sizer->Add(card, 0, wxEXPAND | wxTOP, 8);
    };

    // Shared "Start" button for a card — styled like the mould-feature "Place"
    // button: RoundedButton, BtnPlace fill, white semibold, 32px tall.
    auto addStart = [this](wxWindow* body, wxBoxSizer* bs, const wxString& simName)
    {
        auto* startBtn = new RoundedButton(body, wxID_ANY, "Start",
            wxDefaultPosition, wxSize(-1, 32), wxBORDER_NONE);
        startBtn->SetBackgroundColour(Style::BtnPlace);
        startBtn->SetForegroundColour(*wxWHITE);
        startBtn->SetFont(wxFont(9, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
            wxFONTWEIGHT_SEMIBOLD, false, "Segoe UI"));
        startBtn->Bind(wxEVT_BUTTON,
            [this, simName](wxCommandEvent&) { OnStartSimulation(simName); });
        bs->Add(startBtn, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 10);
        bs->AddSpacer(6);
    };

    // ---- Draft Angle Checks -------------------------------------------------
    // Draft thresholds (fail/warn), then Start, then the result-overlay toggles
    // (1 = warnings, 2 = fails). Undercuts are NOT assessed here — see the
    // Separation Test.
    makeCard("Draft Angle Checks", [this, &addStart](wxWindow* body, wxBoxSizer* bs)
    {
        const wxString deg = wxString::FromUTF8("\xC2\xB0");
        m_failDraftCtrl = AddFieldRow(body, bs, "Fail below:", "1.0", deg);
        m_warnDraftCtrl = AddFieldRow(body, bs, "Warn below:", "3.0", deg);

        addStart(body, bs, "Draft Angle Checks");

        auto addOverlayBtn = [this, body, bs](const wxString& label, int category)
        {
            auto* b = new RoundedButton(body, wxID_ANY, label,
                wxDefaultPosition, wxSize(-1, 26), wxBORDER_NONE);
            b->SetBackgroundColour(Style::BtnSmall);
            b->SetForegroundColour(Style::TextPrimary);
            b->SetFont(wxFont(8, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
                wxFONTWEIGHT_NORMAL, false, "Segoe UI"));
            b->Bind(wxEVT_BUTTON,
                [this, category](wxCommandEvent&) { ShowDebugCategory(category); });
            bs->Add(b, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 10);
        };
        addOverlayBtn("Show Warnings", 1);
        addOverlayBtn("Show Fails", 2);
    });

    // ---- Separation Test ----------------------------------------------------
    // Lift distance, then Start. Covers trapping/undercuts via collision.
    makeCard("Separation Test", [this, &addStart](wxWindow* body, wxBoxSizer* bs)
    {
        m_liftCtrl = AddFieldRow(body, bs, "Lift:", "1.0", "mm");
        addStart(body, bs, "Separation Test");
    });

    sizer->AddSpacer(12);
    scrollWin->SetSizer(sizer);
    colSizer->Add(scrollWin, 1, wxEXPAND);

    column->SetSizer(colSizer);
    outerSizer->Add(column, 1, wxEXPAND);

    // Right border line, matching the Prepare panel's divider.
    auto* borderLine = new wxPanel(outer, wxID_ANY,
        wxDefaultPosition, wxSize(1, -1));
    borderLine->SetBackgroundColour(Style::Divider);
    outerSizer->Add(borderLine, 0, wxEXPAND);

    outer->SetSizer(outerSizer);
    return outer;
}


// ---------------------------------------------------------------------------
// Right panel — read-only results, styled to match the Prepare side: an AppBg
// column with a left divider border and a stack of CardBg cards. An
// "Information" card (shot volume) plus one result card per simulation.
// ---------------------------------------------------------------------------
wxPanel* PreviewPanel::BuildInfoPanel(wxWindow* parent)
{
    auto* outer = new wxPanel(parent, wxID_ANY,
        wxDefaultPosition, wxSize(240, -1));
    outer->SetBackgroundColour(Style::AppBg);
    m_infoPanel = outer;
    auto* outerSizer = new wxBoxSizer(wxHORIZONTAL);

    // Left border line (mirror of the sim panel's right border).
    auto* borderLine = new wxPanel(outer, wxID_ANY,
        wxDefaultPosition, wxSize(1, -1));
    borderLine->SetBackgroundColour(Style::Divider);
    outerSizer->Add(borderLine, 0, wxEXPAND);

    auto* column = new wxPanel(outer, wxID_ANY);
    column->SetBackgroundColour(Style::AppBg);
    auto* colSizer = new wxBoxSizer(wxVERTICAL);

    // Card factory: a CardBg card with a bold white title; returns the card so
    // the caller can drop content beneath the title.
    auto makeCard = [&](const wxString& titleText) -> std::pair<wxPanel*, wxBoxSizer*>
    {
        auto* card = new wxPanel(column, wxID_ANY);
        card->SetBackgroundColour(Style::CardBg);
        auto* cs = new wxBoxSizer(wxVERTICAL);

        auto* t = new wxStaticText(card, wxID_ANY, titleText);
        t->SetForegroundColour(*wxWHITE);
        t->SetFont(wxFont(9, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
            wxFONTWEIGHT_BOLD, false, "Segoe UI"));
        cs->Add(t, 0, wxLEFT | wxRIGHT | wxTOP, 10);

        card->SetSizer(cs);
        colSizer->Add(card, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 8);
        return { card, cs };
    };

    // ---- Information card: shot volume ------------------------------------
    {
        auto [card, cs] = makeCard("Information");

        auto* volLabel = new wxStaticText(card, wxID_ANY, "Shot Volume");
        volLabel->SetForegroundColour(Style::TextSubtle);
        volLabel->SetFont(wxFont(8, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
            wxFONTWEIGHT_NORMAL, false, "Segoe UI"));
        cs->Add(volLabel, 0, wxLEFT | wxRIGHT | wxTOP, 10);

        m_volPrimary = new wxStaticText(card, wxID_ANY, wxEmptyString);
        m_volPrimary->SetForegroundColour(Style::TextPrimary);
        m_volPrimary->SetFont(wxFont(13, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
            wxFONTWEIGHT_BOLD, false, "Segoe UI"));
        cs->Add(m_volPrimary, 0, wxLEFT | wxRIGHT | wxTOP, 10);

        m_volSecondary = new wxStaticText(card, wxID_ANY, wxEmptyString);
        m_volSecondary->SetForegroundColour(Style::TextMuted);
        m_volSecondary->SetFont(wxFont(9, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
            wxFONTWEIGHT_NORMAL, false, "Segoe UI"));
        cs->Add(m_volSecondary, 0, wxLEFT | wxRIGHT | wxBOTTOM, 10);
    }

    // ---- Result cards: one per simulation ---------------------------------
    auto makeVerdictCard = [&](const wxString& titleText) -> wxStaticText*
    {
        auto [card, cs] = makeCard(titleText);
        auto* value = new wxStaticText(card, wxID_ANY, "Not run");
        value->SetForegroundColour(Style::TextMuted);
        value->SetFont(wxFont(11, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
            wxFONTWEIGHT_BOLD, false, "Segoe UI"));
        cs->Add(value, 0, wxLEFT | wxRIGHT | wxTOP | wxBOTTOM, 10);
        return value;
    };

    m_draftStatus = makeVerdictCard("Draft Angle Checks");
    m_demouldStatus = makeVerdictCard("Demoulding");

    column->SetSizer(colSizer);
    outerSizer->Add(column, 1, wxEXPAND);

    outer->SetSizer(outerSizer);
    return outer;
}

// ---------------------------------------------------------------------------
// Fill the information panel's value labels from the current data.
// ---------------------------------------------------------------------------
void PreviewPanel::UpdateInfoPanel()
{
    // 1 cm³ = 1000 mm³; 1 in³ = 16387.064 mm³.
    const double cm3 = m_shotVolumeMm3 / 1000.0;
    const double in3 = m_shotVolumeMm3 / 16387.064;

    if (m_volPrimary)
    {
        if (m_hasShot)
        {
            // \xC2\xB3 is UTF-8 for the superscript-three; build via FromUTF8
            // so it renders regardless of source-file encoding.
            const wxString cm3u = wxString::FromUTF8("cm\xC2\xB3");
            m_volPrimary->SetLabel(wxString::Format("%.3f ", cm3) + cm3u);
            m_volPrimary->SetForegroundColour(Style::TextPrimary);
        }
        else
        {
            m_volPrimary->SetLabel("No shot model");
            m_volPrimary->SetForegroundColour(Style::TextMuted);
        }
    }

    if (m_volSecondary)
    {
        if (m_hasShot)
        {
            const wxString in3u = wxString::FromUTF8("in\xC2\xB3");
            m_volSecondary->SetLabel(wxString::Format("%.4f ", in3) + in3u);
        }
        else
        {
            m_volSecondary->SetLabel(wxEmptyString);
        }
    }

    if (m_draftStatus)
    {
        m_draftStatus->SetLabel("Not run");
        m_draftStatus->SetForegroundColour(Style::TextMuted);
    }
    if (m_demouldStatus)
    {
        m_demouldStatus->SetLabel("Not run");
        m_demouldStatus->SetForegroundColour(Style::TextMuted);
    }

    if (m_infoPanel) m_infoPanel->Layout();
}

// ---------------------------------------------------------------------------
// A simulation Start button was pressed.
// ---------------------------------------------------------------------------
void PreviewPanel::OnStartSimulation(const wxString& simName)
{
    if (simName == "Draft Angle Checks")
    {
        RunDemoldabilityCheck();
        return;
    }
    if (simName == "Separation Test")
    {
        RunSeparationCheck();
        return;
    }

    wxMessageBox(simName + " is not implemented yet.",
        "Simulation", wxOK | wxICON_INFORMATION, this);
}

// ---------------------------------------------------------------------------
// Compute (and cache) the demoldability result. No UI.
// ---------------------------------------------------------------------------
bool PreviewPanel::ComputeDemoldability()
{
    if (!m_hasShot || m_shotShape.IsNull())
    {
        m_hasResult = false;
        return false;
    }

    DesignChecks::Params params;
    params.checkUndercuts = false;   // draft-angle assessment only — undercuts
                                     // are covered by the demoulding test
    params.failDraftDeg = (float)std::clamp(ParseField(m_failDraftCtrl, 1.0), 0.0, 45.0);
    params.warnDraftDeg = (float)std::clamp(ParseField(m_warnDraftCtrl, 3.0), 0.0, 45.0);
    // Keep thresholds ordered: warn must be >= fail.
    if (params.warnDraftDeg < params.failDraftDeg)
        params.warnDraftDeg = params.failDraftDeg;

    m_lastResult = DesignChecks::CheckDemoldability(m_shotShape, params, &m_undercutRays);
    m_hasResult = true;
    return true;
}

// ---------------------------------------------------------------------------
// Demoldability: analyse the whole shot against the draft thresholds and the
// straight ±draw-axis pull, then report the verdict.
// ---------------------------------------------------------------------------
void PreviewPanel::RunDemoldabilityCheck()
{
    if (!ComputeDemoldability())
    {
        wxMessageBox("There is no shot model to analyse.",
            "Draft Angle Checks", wxOK | wxICON_INFORMATION, this);
        return;
    }

    const DesignChecks::DemoldabilityResult& res = m_lastResult;

    // ---- Verdict text + colour -------------------------------------------
    wxString verdict;
    wxColour verdictColour;
    long icon = wxICON_INFORMATION;
    switch (res.overall)
    {
    case DesignChecks::Severity::Pass:
        verdict = "PASS"; verdictColour = wxColour(0x26, 0xAB, 0x36);
        icon = wxICON_INFORMATION; break;
    case DesignChecks::Severity::Warning:
        verdict = "WARNING"; verdictColour = wxColour(0xE0, 0x9B, 0x20);
        icon = wxICON_WARNING; break;
    case DesignChecks::Severity::Fail:
        verdict = "FAIL"; verdictColour = wxColour(0xD0, 0x46, 0x46);
        icon = wxICON_ERROR; break;
    }

    if (m_draftStatus)
    {
        m_draftStatus->SetLabel(verdict);
        m_draftStatus->SetForegroundColour(verdictColour);
        if (m_infoPanel) m_infoPanel->Layout();
    }

    // ---- Detailed dialog --------------------------------------------------
    const wxString deg = wxString::FromUTF8("\xC2\xB0");
    wxString msg;
    msg << "Draft Angle Checks: " << verdict << "\n\n";
    if (res.issues.empty())
    {
        msg << "No draft or undercut problems found.\n\n";
    }
    else
    {
        for (const DesignChecks::Issue& is : res.issues)
            msg << wxString::FromUTF8(is.description.c_str()) << "\n";
        msg << "\n";
    }
    msg << "Minimum draft: "
        << wxString::Format("%.2f", res.minDraftDeg) << deg << "\n";
    msg << "Undercut faces: " << res.undercutCount << "\n";
    msg << "Below fail: " << res.failDraftCount << "\n";
    msg << "Below warn: " << res.warnDraftCount << "\n";
    msg << "Faces analysed: " << res.totalFaces;

    wxMessageBox(msg, "Draft Angle Checks", wxOK | icon, this);
}

// ---------------------------------------------------------------------------
// Separation/collision demoldability — lift each half off the shot and test
// for interference. Reports the verdict and shows the overlap region in red.
// ---------------------------------------------------------------------------
void PreviewPanel::RunSeparationCheck()
{
    if (!m_hasShot || m_shotShape.IsNull() || m_halfShapes.empty())
    {
        wxMessageBox("There is no shot model and mould halves to analyse.",
            "Separation Test", wxOK | wxICON_INFORMATION, this);
        return;
    }

    DesignChecks::SeparationParams params;
    params.liftMm = (float)std::clamp(ParseField(m_liftCtrl, 1.0), 0.05, 25.0);

    TopoDS_Shape overlap;
    const DesignChecks::SeparationResult res =
        DesignChecks::CheckSeparation(m_shotShape, m_halfShapes, params, &overlap);

    // Show the interference region (red), and clear the analytic overlays so
    // the separation view stands on its own for comparison.
    if (m_canvas)
    {
        m_canvas->ClearShotDebugColoring();
        m_canvas->ShowShotDebugRays(false);
        m_canvas->ShowShotDebugContacts(false);
        m_showRays = false;
        m_showContacts = false;
        m_activeDebugCategory = -1;

        m_canvas->SetShotDebugSolid(overlap, glm::vec3(0.90f, 0.15f, 0.15f));
        m_canvas->ShowShotDebugSolid(!overlap.IsNull());
    }

    // ---- Verdict + status -------------------------------------------------
    wxString verdict;
    wxColour verdictColour;
    long iconFlag = wxICON_INFORMATION;
    switch (res.overall)
    {
    case DesignChecks::Severity::Pass:
        verdict = "PASS"; verdictColour = wxColour(0x26, 0xAB, 0x36);
        iconFlag = wxICON_INFORMATION; break;
    case DesignChecks::Severity::Warning:
        verdict = "INCONCLUSIVE"; verdictColour = wxColour(0xE0, 0x9B, 0x20);
        iconFlag = wxICON_WARNING; break;
    case DesignChecks::Severity::Fail:
        verdict = "FAIL"; verdictColour = wxColour(0xD0, 0x46, 0x46);
        iconFlag = wxICON_ERROR; break;
    }

    if (m_demouldStatus)
    {
        m_demouldStatus->SetLabel(verdict);
        m_demouldStatus->SetForegroundColour(verdictColour);
        if (m_infoPanel) m_infoPanel->Layout();
    }

    // ---- Dialog -----------------------------------------------------------
    wxString msg;
    msg << "Separation test: " << verdict << "\n\n";
    msg << "Lift: " << wxString::Format("%.2f", params.liftMm) << " mm\n";
    msg << "Halves tested: " << res.halvesTested << "\n";
    msg << "Halves collided: " << res.halvesCollided << "\n";
    if (res.halvesFailedToEval > 0)
        msg << "Halves not evaluable: " << res.halvesFailedToEval
            << " (boolean failed)\n";
    msg << "Total overlap: "
        << wxString::Format("%.3f", res.totalOverlapVolume)
        << wxString::FromUTF8(" mm\xC2\xB3") << "\n\n";

    for (size_t i = 0; i < res.perHalfStatus.size(); ++i)
    {
        const char* s = res.perHalfStatus[i] == 1 ? "COLLISION"
            : res.perHalfStatus[i] == 2 ? "not evaluable" : "clear";
        msg << "  Half " << (int)(i + 1) << ": " << s
            << wxString::Format("  (overlap %.3f mm", res.perHalfVolume[i])
            << wxString::FromUTF8("\xC2\xB3)") << "\n";
    }

    if (res.halvesCollided > 0)
        msg << "\nRed overlay shows where steel drives into the body. Hide the "
               "Shot toggle to see it clearly.";

    wxMessageBox(msg, "Separation Test", wxOK | iconFlag, this);
}

// ---------------------------------------------------------------------------
// Push a debug overlay: partition display triangles by their source face's
// group and hand the groups to the canvas.
// ---------------------------------------------------------------------------
void PreviewPanel::ApplyFaceGroups(const std::unordered_map<int, int>& groupOfFace,
    const std::vector<glm::vec3>& colors, int defaultGroup)
{
    if (!m_canvas || colors.empty()) return;

    std::vector<GLCanvas::ShotDebugGroup> groups(colors.size());
    for (size_t g = 0; g < colors.size(); ++g) groups[g].color = colors[g];

    const std::vector<uint32_t>& I = m_shotMesh.indices;
    const size_t numTris = I.size() / 3;
    for (size_t t = 0; t < numTris; ++t)
    {
        const int fid = (t < m_shotFaceIds.size()) ? m_shotFaceIds[t] : 0;
        auto it = groupOfFace.find(fid);
        int g = (it != groupOfFace.end()) ? it->second : defaultGroup;
        if (g < 0 || g >= (int)colors.size()) g = defaultGroup;
        groups[(size_t)g].indices.push_back(I[t * 3 + 0]);
        groups[(size_t)g].indices.push_back(I[t * 3 + 1]);
        groups[(size_t)g].indices.push_back(I[t * 3 + 2]);
    }

    m_canvas->SetShotDebugGroups(m_shotHalfIndex, groups);
}

// ---------------------------------------------------------------------------
// Debug: recolour the shot for one category (red) vs the rest (green).
// ---------------------------------------------------------------------------
void PreviewPanel::ShowDebugCategory(int category)
{
    if (!m_canvas || m_shotHalfIndex < 0)
    {
        wxMessageBox("There is no shot model to visualise.",
            "Debug", wxOK | wxICON_INFORMATION, this);
        return;
    }

    // Pressing the active category again returns the shot to normal.
    if (m_activeDebugCategory == category)
    {
        m_canvas->ClearShotDebugColoring();
        m_activeDebugCategory = -1;
        return;
    }

    // Recompute against the current thresholds so the overlay always matches
    // the fields (the fail/warn categories depend on them).
    if (!ComputeDemoldability())
    {
        wxMessageBox("There is no shot model to analyse.",
            "Debug", wxOK | wxICON_INFORMATION, this);
        return;
    }

    const std::vector<int>* flaggedFaces = nullptr;
    switch (category)
    {
    case 0: flaggedFaces = &m_lastResult.undercutFaces;  break;
    case 1: flaggedFaces = &m_lastResult.warnDraftFaces; break;
    case 2: flaggedFaces = &m_lastResult.failDraftFaces; break;
    default: return;
    }

    // Group 0 = flagged (red), group 1 = everything else (green, default).
    std::unordered_map<int, int> groupOfFace;
    for (int f : *flaggedFaces)
        if (f > 0) groupOfFace[f] = 0;

    const std::vector<glm::vec3> colors = {
        glm::vec3(0.85f, 0.16f, 0.16f),   // red   — flagged
        glm::vec3(0.18f, 0.70f, 0.22f),   // green — rest
    };
    ApplyFaceGroups(groupOfFace, colors, /*defaultGroup*/ 1);
    m_activeDebugCategory = category;
}

// ---------------------------------------------------------------------------
// Debug: recolour the shot by draft sign (up / down / vertical / mixed) using
// the analytic normals the checks use — to expose any inverted normals.
// ---------------------------------------------------------------------------
void PreviewPanel::ShowDraftSign()
{
    if (!m_canvas || m_shotHalfIndex < 0 || m_shotShape.IsNull())
    {
        wxMessageBox("There is no shot model to visualise.",
            "Debug", wxOK | wxICON_INFORMATION, this);
        return;
    }

    const int kDraftSignCategory = 3;
    if (m_activeDebugCategory == kDraftSignCategory)
    {
        m_canvas->ClearShotDebugColoring();
        m_activeDebugCategory = -1;
        return;
    }

    DesignChecks::Params params;  // draw axis + vertical tolerance defaults
    const DesignChecks::DraftSignResult sign =
        DesignChecks::ClassifyDraftSign(m_shotShape, params);

    // Group 0 up, 1 down, 2 vertical (default), 3 mixed.
    std::unordered_map<int, int> groupOfFace;
    for (int f : sign.upFaces)       groupOfFace[f] = 0;
    for (int f : sign.downFaces)     groupOfFace[f] = 1;
    for (int f : sign.verticalFaces) groupOfFace[f] = 2;
    for (int f : sign.mixedFaces)    groupOfFace[f] = 3;

    const std::vector<glm::vec3> colors = {
        glm::vec3(0.20f, 0.45f, 0.95f),   // up       — blue
        glm::vec3(0.95f, 0.55f, 0.15f),   // down     — orange
        glm::vec3(0.55f, 0.55f, 0.58f),   // vertical — grey
        glm::vec3(0.65f, 0.30f, 0.80f),   // mixed    — purple
    };
    ApplyFaceGroups(groupOfFace, colors, /*defaultGroup*/ 2);
    m_activeDebugCategory = kDraftSignCategory;
}

// ---------------------------------------------------------------------------
// Build and upload the undercut-ray debug geometry from the last result.
// ---------------------------------------------------------------------------
void PreviewPanel::RefreshRayGeometry()
{
    if (!m_canvas) return;

    // Recompute so the rays match the current shot (undercut rays don't depend
    // on the draft thresholds, but this keeps everything consistent).
    if (!ComputeDemoldability()) return;

    std::vector<glm::vec3> rayVerts;     // GL_LINES pairs
    std::vector<glm::vec3> contactVerts; // GL_POINTS
    rayVerts.reserve(m_undercutRays.size() * 2);
    contactVerts.reserve(m_undercutRays.size());

    const float kRayLen = 10.0f;  // first 10 mm of each ray path
    for (const DesignChecks::UndercutRay& r : m_undercutRays)
    {
        rayVerts.push_back(r.origin);
        rayVerts.push_back(r.origin + r.dir * kRayLen);
        contactVerts.push_back(r.hit);
    }

    m_canvas->SetShotDebugRays(rayVerts, contactVerts);
}

// ---------------------------------------------------------------------------
// Toggle the ray-segment overlay (first 10 mm of each failing ray).
// ---------------------------------------------------------------------------
void PreviewPanel::ToggleDebugRays()
{
    if (!m_canvas || m_shotShape.IsNull())
    {
        wxMessageBox("There is no shot model to analyse.",
            "Debug", wxOK | wxICON_INFORMATION, this);
        return;
    }
    m_showRays = !m_showRays;
    if (m_showRays) RefreshRayGeometry();
    m_canvas->ShowShotDebugRays(m_showRays);
}

// ---------------------------------------------------------------------------
// Toggle the contact-point overlay (where failing rays struck the shot).
// ---------------------------------------------------------------------------
void PreviewPanel::ToggleDebugContacts()
{
    if (!m_canvas || m_shotShape.IsNull())
    {
        wxMessageBox("There is no shot model to analyse.",
            "Debug", wxOK | wxICON_INFORMATION, this);
        return;
    }
    m_showContacts = !m_showContacts;
    if (m_showContacts) RefreshRayGeometry();
    m_canvas->ShowShotDebugContacts(m_showContacts);
}

// ---------------------------------------------------------------------------
// (Re)build the show/hide visibility checkboxes for the current part set, into
// m_visPanel (left column). One per mould half, then "Shot" if present.
// ---------------------------------------------------------------------------
void PreviewPanel::BuildVisibilityChecks(int halfCount, bool hasShot)
{
    if (!m_visPanel) return;
    auto* vSizer = m_visPanel->GetSizer();

    const bool anyParts = (halfCount > 0) || hasShot;
    if (m_visEmptyLabel) m_visEmptyLabel->Show(!anyParts);

    auto addCheck = [&](int partIndex, const wxString& label, const wxString& tip,
        bool initialVisible)
    {
        auto* cb = new wxCheckBox(m_visPanel, kHalfToggleIdBase + partIndex, label);
        cb->SetForegroundColour(Style::TextPrimary);
        cb->SetBackgroundColour(Style::CardBg);
        cb->SetValue(initialVisible);
        cb->SetToolTip(tip);
        cb->Bind(wxEVT_CHECKBOX,
            [this, partIndex](wxCommandEvent& evt)
            {
                if (m_canvas) m_canvas->SetPreviewHalfVisible(partIndex, evt.IsChecked());
            });
        vSizer->Add(cb, 0, wxEXPAND | wxALL, 6);
        m_halfChecks.push_back(cb);
    };

    // One checkbox per mould half, indices [0 .. halfCount-1]. Half A (index 0)
    // starts hidden so the preview opens looking into the cavity / at the shot.
    for (int i = 0; i < halfCount; ++i)
    {
        const wxString label = "Half " + HalfLetter(i);
        addCheck(i, label, "Show / hide " + label, /*initialVisible*/ i != 0);
    }

    // Shot, loaded as preview part index == halfCount (appended after the
    // halves in LoadHalves, so the indices line up).
    if (hasShot)
        addCheck(halfCount, "Shot",
            "Show / hide the shot model (part + feed system)", /*initialVisible*/ true);

    m_visPanel->Layout();
    if (m_visPanel->GetParent()) m_visPanel->GetParent()->Layout();
}

// ---------------------------------------------------------------------------
// Drop the current visibility checkboxes and restore the empty-state message.
// ---------------------------------------------------------------------------
void PreviewPanel::ClearVisibilityChecks()
{
    auto* vSizer = m_visPanel ? m_visPanel->GetSizer() : nullptr;
    for (wxCheckBox* cb : m_halfChecks)
    {
        if (!cb) continue;
        if (vSizer) vSizer->Detach(cb);
        cb->Destroy();
    }
    m_halfChecks.clear();
    if (m_visEmptyLabel) m_visEmptyLabel->Show(true);
    if (m_visPanel) m_visPanel->Layout();
}

void PreviewPanel::LoadHalves()
{
    if (!m_canvas) return;

    // Mould halves first, in fixture order — their indices match the
    // visibility checkboxes built in BuildVisibilityChecks.
    for (size_t i = 0; i < m_pendingHalves.size(); ++i)
    {
        const std::string label = "Half " + HalfLetter((int)i);
        m_canvas->AddPreviewHalf(m_pendingHalves[i], label);
    }

    // Shot last, so its preview-part index equals m_pendingHalves.size(),
    // matching the shot checkbox. A distinct amber base colour separates it
    // from the grey mould halves. m_shotMesh is kept (not dropped) so the
    // design checks can analyse it on demand.
    if (m_hasShot)
    {
        const glm::vec3 shotColor(0.85f, 0.50f, 0.20f);
        m_canvas->AddPreviewHalf(m_shotMesh, "Shot", shotColor);
    }

    // Half meshes are now on the GPU; drop the CPU copies (the shot is kept).
    m_pendingHalves.clear();
    m_pendingHalves.shrink_to_fit();

    // Apply the initial checkbox states to the freshly loaded parts (parts are
    // added visible, so hide any whose checkbox starts unchecked — e.g. Half A).
    for (size_t i = 0; i < m_halfChecks.size(); ++i)
        if (m_halfChecks[i] && !m_halfChecks[i]->GetValue())
            m_canvas->SetPreviewHalfVisible((int)i, false);

    m_canvas->Refresh(false);
}
