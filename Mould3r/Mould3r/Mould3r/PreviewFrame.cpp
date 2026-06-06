#include "PreviewFrame.h"
#include "GLCanvas.h"
#include "style.h"
#include "DesignChecks.h"

#include <wx/spinctrl.h>

#include <algorithm>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Toggle id base. One sequential id per half toggle starting here, so the
// shared EVT_TOGGLEBUTTON handler can recover the half index from the id.
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

PreviewFrame::PreviewFrame(wxWindow* parent,
    const std::vector<FileImporter::MeshData>& halves,
    const ShotPreviewInput& shot)
    : wxFrame(parent, wxID_ANY, "Mould Preview",
        wxDefaultPosition, wxSize(1100, 700))
    , m_pendingHalves(halves)
{
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

    SetBackgroundColour(Style::AppBg);

    auto* root = new wxBoxSizer(wxVERTICAL);

    // ---- Toolbar (show/hide toggles), full width across the top -----------
    m_toolbar = new wxPanel(this, wxID_ANY);
    m_toolbar->SetBackgroundColour(Style::AppBg);
    auto* toolSizer = new wxBoxSizer(wxHORIZONTAL);

    auto* heading = new wxStaticText(m_toolbar, wxID_ANY, "Show / hide:");
    heading->SetForegroundColour(Style::TextPrimary);
    heading->SetFont(wxFont(9, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
        wxFONTWEIGHT_BOLD, false, "Segoe UI"));
    toolSizer->Add(heading, 0, wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, 10);

    m_toolbar->SetSizer(toolSizer);
    root->Add(m_toolbar, 0, wxEXPAND | wxTOP | wxBOTTOM, 6);

    BuildToggleBar((int)halves.size(), m_hasShot);

    // ---- Middle row: simulations | canvas | information -------------------
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

    if (parent) CentreOnParent();
    Show(true);
    Raise();

    // The GL upload must wait until the canvas is realized and its context is
    // valid. CallAfter runs once control returns to the event loop, by which
    // point Show() has taken effect.
    CallAfter([this]() { LoadHalves(); });
}

// ---------------------------------------------------------------------------
// Left panel — runnable simulations, each with a Start button.
// ---------------------------------------------------------------------------
wxPanel* PreviewFrame::BuildSimPanel(wxWindow* parent)
{
    auto* panel = new wxPanel(parent, wxID_ANY);
    panel->SetBackgroundColour(Style::CardBg);
    panel->SetMinSize(wxSize(220, -1));

    auto* sizer = new wxBoxSizer(wxVERTICAL);

    auto* title = new wxStaticText(panel, wxID_ANY, "Simulations");
    title->SetForegroundColour(Style::TextPrimary);
    title->SetFont(wxFont(11, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
        wxFONTWEIGHT_BOLD, false, "Segoe UI"));
    sizer->Add(title, 0, wxALL, 12);

    // Available simulations. Add entries here as they come online; each gets a
    // titled card with its own Start button.
    struct SimEntry { wxString name; };
    const std::vector<SimEntry> sims = { { "Design Checks" }, { "Separation Test" } };

    for (const SimEntry& sim : sims)
    {
        auto* card = new wxPanel(panel, wxID_ANY);
        card->SetBackgroundColour(Style::SectionHeaderBg);
        auto* cardSizer = new wxBoxSizer(wxVERTICAL);

        auto* name = new wxStaticText(card, wxID_ANY, sim.name);
        name->SetForegroundColour(Style::TextPrimary);
        name->SetFont(wxFont(10, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
            wxFONTWEIGHT_NORMAL, false, "Segoe UI"));
        cardSizer->Add(name, 0, wxLEFT | wxRIGHT | wxTOP, 10);

        // A labelled spin control row, shared by the cards' parameter fields.
        auto addSpinRow = [&](const wxString& label, double mn, double mx,
            double initial, double step, int digits) -> wxSpinCtrlDouble*
        {
            auto* row = new wxBoxSizer(wxHORIZONTAL);
            auto* lbl = new wxStaticText(card, wxID_ANY, label);
            lbl->SetForegroundColour(Style::TextSubtle);
            lbl->SetFont(wxFont(9, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
                wxFONTWEIGHT_NORMAL, false, "Segoe UI"));
            row->Add(lbl, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);

            auto* spin = new wxSpinCtrlDouble(card, wxID_ANY, wxEmptyString,
                wxDefaultPosition, wxSize(70, -1),
                wxSP_ARROW_KEYS, mn, mx, initial, step);
            spin->SetDigits(digits);
            row->Add(spin, 0, wxALIGN_CENTER_VERTICAL);

            cardSizer->Add(row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 10);
            return spin;
        };

        // Design Checks: draft thresholds. A facet whose draft is below the
        // fail value is a failure; below the warn value (but >= fail) is a
        // warning.
        if (sim.name == "Design Checks")
        {
            m_failDraftCtrl = addSpinRow(wxString::FromUTF8("Fail below (\xC2\xB0)"),
                0.0, 45.0, 1.0, 0.5, 1);
            m_warnDraftCtrl = addSpinRow(wxString::FromUTF8("Warn below (\xC2\xB0)"),
                0.0, 45.0, 3.0, 0.5, 1);
        }
        // Separation Test: how far to lift each half off the shot before testing
        // for interference. A larger lift gives a thicker, easier-to-see overlap.
        else if (sim.name == "Separation Test")
        {
            m_liftCtrl = addSpinRow("Lift (mm)", 0.05, 25.0, 1.0, 0.25, 2);
        }

        auto* startBtn = new wxButton(card, wxID_ANY, "Start",
            wxDefaultPosition, wxSize(-1, 28));
        startBtn->SetBackgroundColour(Style::BtnActive);
        startBtn->SetForegroundColour(Style::TextPrimary);

        const wxString simName = sim.name;
        startBtn->Bind(wxEVT_BUTTON,
            [this, simName](wxCommandEvent&) { OnStartSimulation(simName); });
        cardSizer->Add(startBtn, 0, wxEXPAND | wxALL, 10);

        card->SetSizer(cardSizer);
        sizer->Add(card, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);
    }

    // ---- Debug visualisation (below the cards) ----------------------------
    // Recolour the shot to show which facets a given category flagged: red for
    // the category, green for the rest. Pressing the active one again clears it.
    auto* dbgLabel = new wxStaticText(panel, wxID_ANY, "Debug");
    dbgLabel->SetForegroundColour(Style::TextSubtle);
    dbgLabel->SetFont(wxFont(9, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
        wxFONTWEIGHT_NORMAL, false, "Segoe UI"));
    sizer->Add(dbgLabel, 0, wxLEFT | wxRIGHT | wxTOP, 12);

    struct DbgBtn { wxString label; int category; };
    const DbgBtn dbgButtons[] = {
        { "Show Undercuts", 0 },
        { "Show Warnings",  1 },
        { "Show Fails",     2 },
    };
    for (const DbgBtn& d : dbgButtons)
    {
        auto* btn = new wxButton(panel, wxID_ANY, d.label,
            wxDefaultPosition, wxSize(-1, 26));
        btn->SetBackgroundColour(Style::SectionHeaderBg);
        btn->SetForegroundColour(Style::TextPrimary);
        const int category = d.category;
        btn->Bind(wxEVT_BUTTON,
            [this, category](wxCommandEvent&) { ShowDebugCategory(category); });
        sizer->Add(btn, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 8);
    }

    // Draft-sign overlay (up/down/vertical/mixed) — diagnoses normal direction.
    {
        auto* btn = new wxButton(panel, wxID_ANY, "Show Draft Sign",
            wxDefaultPosition, wxSize(-1, 26));
        btn->SetBackgroundColour(Style::SectionHeaderBg);
        btn->SetForegroundColour(Style::TextPrimary);
        btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { ShowDraftSign(); });
        sizer->Add(btn, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 8);
    }

    // Accessibility-ray overlays — show the undercut test's rays + contacts.
    {
        auto* btnR = new wxButton(panel, wxID_ANY, "Show Rays",
            wxDefaultPosition, wxSize(-1, 26));
        btnR->SetBackgroundColour(Style::SectionHeaderBg);
        btnR->SetForegroundColour(Style::TextPrimary);
        btnR->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { ToggleDebugRays(); });
        sizer->Add(btnR, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 8);

        auto* btnC = new wxButton(panel, wxID_ANY, "Show Contacts",
            wxDefaultPosition, wxSize(-1, 26));
        btnC->SetBackgroundColour(Style::SectionHeaderBg);
        btnC->SetForegroundColour(Style::TextPrimary);
        btnC->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { ToggleDebugContacts(); });
        sizer->Add(btnC, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 8);
    }

    panel->SetSizer(sizer);
    return panel;
}

// ---------------------------------------------------------------------------
// Right panel — read-only information about the shot. For now: its volume.
// ---------------------------------------------------------------------------
wxPanel* PreviewFrame::BuildInfoPanel(wxWindow* parent)
{
    auto* panel = new wxPanel(parent, wxID_ANY);
    panel->SetBackgroundColour(Style::CardBg);
    panel->SetMinSize(wxSize(220, -1));

    auto* sizer = new wxBoxSizer(wxVERTICAL);

    auto* title = new wxStaticText(panel, wxID_ANY, "Information");
    title->SetForegroundColour(Style::TextPrimary);
    title->SetFont(wxFont(11, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
        wxFONTWEIGHT_BOLD, false, "Segoe UI"));
    sizer->Add(title, 0, wxALL, 12);

    auto* volLabel = new wxStaticText(panel, wxID_ANY, "Shot Volume");
    volLabel->SetForegroundColour(Style::TextSubtle);
    volLabel->SetFont(wxFont(9, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
        wxFONTWEIGHT_NORMAL, false, "Segoe UI"));
    sizer->Add(volLabel, 0, wxLEFT | wxRIGHT | wxTOP, 12);

    if (m_hasShot)
    {
        // \xC2\xB3 is UTF-8 for the superscript-three (mm cubed); build via
        // FromUTF8 so it renders regardless of source-file encoding.
        const wxString mm3 = wxString::FromUTF8("mm\xC2\xB3");
        const wxString cm3 = wxString::FromUTF8("cm\xC2\xB3");

        auto* primary = new wxStaticText(panel, wxID_ANY,
            wxString::Format("%.1f ", m_shotVolumeMm3) + mm3);
        primary->SetForegroundColour(Style::TextPrimary);
        primary->SetFont(wxFont(13, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
            wxFONTWEIGHT_BOLD, false, "Segoe UI"));
        sizer->Add(primary, 0, wxLEFT | wxRIGHT | wxTOP, 12);

        auto* secondary = new wxStaticText(panel, wxID_ANY,
            wxString::Format("%.3f ", m_shotVolumeMm3 / 1000.0) + cm3);
        secondary->SetForegroundColour(Style::TextMuted);
        secondary->SetFont(wxFont(9, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
            wxFONTWEIGHT_NORMAL, false, "Segoe UI"));
        sizer->Add(secondary, 0, wxLEFT | wxRIGHT | wxBOTTOM, 12);
    }
    else
    {
        auto* none = new wxStaticText(panel, wxID_ANY, "No shot model");
        none->SetForegroundColour(Style::TextMuted);
        none->SetFont(wxFont(10, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
            wxFONTWEIGHT_NORMAL, false, "Segoe UI"));
        sizer->Add(none, 0, wxLEFT | wxRIGHT | wxBOTTOM, 12);
    }

    // Design-check verdict — updated when Design Checks is run.
    sizer->AddSpacer(8);
    auto* checkLabel = new wxStaticText(panel, wxID_ANY, "Demoldability");
    checkLabel->SetForegroundColour(Style::TextSubtle);
    checkLabel->SetFont(wxFont(9, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
        wxFONTWEIGHT_NORMAL, false, "Segoe UI"));
    sizer->Add(checkLabel, 0, wxLEFT | wxRIGHT | wxTOP, 12);

    m_checkStatus = new wxStaticText(panel, wxID_ANY, "Not run");
    m_checkStatus->SetForegroundColour(Style::TextMuted);
    m_checkStatus->SetFont(wxFont(11, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
        wxFONTWEIGHT_BOLD, false, "Segoe UI"));
    sizer->Add(m_checkStatus, 0, wxLEFT | wxRIGHT | wxBOTTOM, 12);

    panel->SetSizer(sizer);
    return panel;
}

// ---------------------------------------------------------------------------
// A simulation Start button was pressed.
// ---------------------------------------------------------------------------
void PreviewFrame::OnStartSimulation(const wxString& simName)
{
    if (simName == "Design Checks")
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
bool PreviewFrame::ComputeDemoldability()
{
    if (!m_hasShot || m_shotShape.IsNull())
    {
        m_hasResult = false;
        return false;
    }

    DesignChecks::Params params;
    if (m_failDraftCtrl) params.failDraftDeg = (float)m_failDraftCtrl->GetValue();
    if (m_warnDraftCtrl) params.warnDraftDeg = (float)m_warnDraftCtrl->GetValue();
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
void PreviewFrame::RunDemoldabilityCheck()
{
    if (!ComputeDemoldability())
    {
        wxMessageBox("There is no shot model to analyse.",
            "Design Checks", wxOK | wxICON_INFORMATION, this);
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

    if (m_checkStatus)
    {
        m_checkStatus->SetLabel(verdict);
        m_checkStatus->SetForegroundColour(verdictColour);
        m_checkStatus->GetParent()->Layout();
    }

    // ---- Detailed dialog --------------------------------------------------
    const wxString deg = wxString::FromUTF8("\xC2\xB0");
    wxString msg;
    msg << "Demoldability: " << verdict << "\n\n";
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

    wxMessageBox(msg, "Design Checks — Demoldability", wxOK | icon, this);
}

// ---------------------------------------------------------------------------
// Separation/collision demoldability — lift each half off the shot and test
// for interference. Reports the verdict and shows the overlap region in red.
// ---------------------------------------------------------------------------
void PreviewFrame::RunSeparationCheck()
{
    if (!m_hasShot || m_shotShape.IsNull() || m_halfShapes.empty())
    {
        wxMessageBox("There is no shot model and mould halves to analyse.",
            "Separation Test", wxOK | wxICON_INFORMATION, this);
        return;
    }

    DesignChecks::SeparationParams params;
    if (m_liftCtrl) params.liftMm = (float)m_liftCtrl->GetValue();

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

    if (m_checkStatus)
    {
        m_checkStatus->SetLabel("Separation: " + verdict);
        m_checkStatus->SetForegroundColour(verdictColour);
        m_checkStatus->GetParent()->Layout();
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
void PreviewFrame::ApplyFaceGroups(const std::unordered_map<int, int>& groupOfFace,
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
void PreviewFrame::ShowDebugCategory(int category)
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
void PreviewFrame::ShowDraftSign()
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
void PreviewFrame::RefreshRayGeometry()
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
void PreviewFrame::ToggleDebugRays()
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
void PreviewFrame::ToggleDebugContacts()
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

void PreviewFrame::BuildToggleBar(int halfCount, bool hasShot)
{
    auto* toolSizer = m_toolbar->GetSizer();

    // One toggle per mould half, indices [0 .. halfCount-1].
    for (int i = 0; i < halfCount; ++i)
    {
        const wxString label = "Half " + HalfLetter(i);
        auto* btn = new wxToggleButton(m_toolbar, kHalfToggleIdBase + i, label,
            wxDefaultPosition, wxSize(110, 28));
        btn->SetValue(true);                 // parts start visible
        btn->SetToolTip("Show / hide " + label);
        StyleToggle(btn, true);

        btn->Bind(wxEVT_TOGGLEBUTTON,
            [this, i](wxCommandEvent& evt)
            {
                const bool visible = evt.IsChecked();
                if (m_canvas) m_canvas->SetPreviewHalfVisible(i, visible);
                if (i >= 0 && i < (int)m_halfToggles.size())
                    StyleToggle(m_halfToggles[i], visible);
            });

        toolSizer->Add(btn, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
        m_halfToggles.push_back(btn);
    }

    // Shot toggle, loaded as preview part index == halfCount (it is appended
    // after the halves in LoadHalves, so the indices line up).
    if (hasShot)
    {
        const int shotIndex = halfCount;
        auto* btn = new wxToggleButton(m_toolbar, kHalfToggleIdBase + shotIndex,
            "Shot", wxDefaultPosition, wxSize(110, 28));
        btn->SetValue(true);
        btn->SetToolTip("Show / hide the shot model (part + feed system)");
        StyleToggle(btn, true);

        btn->Bind(wxEVT_TOGGLEBUTTON,
            [this, shotIndex](wxCommandEvent& evt)
            {
                const bool visible = evt.IsChecked();
                if (m_canvas) m_canvas->SetPreviewHalfVisible(shotIndex, visible);
                if (shotIndex >= 0 && shotIndex < (int)m_halfToggles.size())
                    StyleToggle(m_halfToggles[shotIndex], visible);
            });

        toolSizer->Add(btn, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
        m_halfToggles.push_back(btn);
    }

    m_toolbar->Layout();
}

void PreviewFrame::LoadHalves()
{
    if (!m_canvas) return;

    // Mould halves first, in fixture order — their indices match the half
    // toggles built in BuildToggleBar.
    for (size_t i = 0; i < m_pendingHalves.size(); ++i)
    {
        const std::string label = "Half " + HalfLetter((int)i);
        m_canvas->AddPreviewHalf(m_pendingHalves[i], label);
    }

    // Shot last, so its preview-part index equals m_pendingHalves.size(),
    // matching the shot toggle. A distinct amber base colour separates it from
    // the grey mould halves. m_shotMesh is kept (not dropped) so the design
    // checks can analyse it on demand.
    if (m_hasShot)
    {
        const glm::vec3 shotColor(0.85f, 0.50f, 0.20f);
        m_canvas->AddPreviewHalf(m_shotMesh, "Shot", shotColor);
    }

    // Half meshes are now on the GPU; drop the CPU copies (the shot is kept).
    m_pendingHalves.clear();
    m_pendingHalves.shrink_to_fit();

    m_canvas->Refresh(false);
}

void PreviewFrame::StyleToggle(wxToggleButton* btn, bool visible) const
{
    if (!btn) return;
    // Active (shown) reads as the accent toggle colour; inactive (hidden) uses
    // the neutral default-button colour. Matches the app's toggle convention.
    btn->SetBackgroundColour(visible ? Style::BtnActive : Style::BtnDefault);
    btn->SetForegroundColour(Style::TextPrimary);
    btn->Refresh();
}
