#include "PreviewFrame.h"
#include "GLCanvas.h"
#include "style.h"

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
    const FileImporter::MeshData* shot,
    double shotVolumeMm3)
    : wxFrame(parent, wxID_ANY, "Mould Preview",
        wxDefaultPosition, wxSize(1100, 700))
    , m_pendingHalves(halves)
    , m_shotVolumeMm3(shotVolumeMm3)
{
    if (shot)
    {
        m_pendingShot = *shot;
        m_hasShot = true;
    }

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
    const std::vector<SimEntry> sims = { { "Design Checks" } };

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

    panel->SetSizer(sizer);
    return panel;
}

// ---------------------------------------------------------------------------
// Stub: a simulation Start button was pressed. The UI is wired; the actual
// checks are not implemented yet.
// ---------------------------------------------------------------------------
void PreviewFrame::OnStartSimulation(const wxString& simName)
{
    wxMessageBox(simName + " is not implemented yet.",
        "Simulation", wxOK | wxICON_INFORMATION, this);
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
    // the grey mould halves.
    if (m_hasShot)
    {
        const glm::vec3 shotColor(0.85f, 0.50f, 0.20f);
        m_canvas->AddPreviewHalf(m_pendingShot, "Shot", shotColor);
    }

    // Meshes now live in GPU buffers on the canvas; drop the CPU copies.
    m_pendingHalves.clear();
    m_pendingHalves.shrink_to_fit();
    m_pendingShot = FileImporter::MeshData{};

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
