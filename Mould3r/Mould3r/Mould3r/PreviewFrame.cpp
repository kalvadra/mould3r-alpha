#include "PreviewFrame.h"
#include "GLCanvas.h"
#include "style.h"

#include <string>

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
    const std::vector<FileImporter::MeshData>& halves)
    : wxFrame(parent, wxID_ANY, "Mould Preview",
        wxDefaultPosition, wxSize(960, 680))
    , m_pendingHalves(halves)
{
    SetBackgroundColour(Style::AppBg);

    auto* root = new wxBoxSizer(wxVERTICAL);

    // ---- Toolbar (show/hide toggles) --------------------------------------
    m_toolbar = new wxPanel(this, wxID_ANY);
    m_toolbar->SetBackgroundColour(Style::AppBg);
    auto* toolSizer = new wxBoxSizer(wxHORIZONTAL);

    auto* heading = new wxStaticText(m_toolbar, wxID_ANY, "Mould halves:");
    heading->SetForegroundColour(Style::TextPrimary);
    heading->SetFont(wxFont(9, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
        wxFONTWEIGHT_BOLD, false, "Segoe UI"));
    toolSizer->Add(heading, 0, wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, 10);

    m_toolbar->SetSizer(toolSizer);
    root->Add(m_toolbar, 0, wxEXPAND | wxTOP | wxBOTTOM, 6);

    BuildToggleBar((int)halves.size());

    // ---- Preview canvas ---------------------------------------------------
    m_canvas = new GLCanvas(this);
    m_canvas->SetPreviewMode(true);
    root->Add(m_canvas, 1, wxEXPAND);

    SetSizer(root);

    if (parent) CentreOnParent();
    Show(true);
    Raise();

    // The GL upload must wait until the canvas is realized and its context is
    // valid. CallAfter runs once control returns to the event loop, by which
    // point Show() has taken effect.
    CallAfter([this]() { LoadHalves(); });
}

void PreviewFrame::BuildToggleBar(int halfCount)
{
    auto* toolSizer = m_toolbar->GetSizer();

    for (int i = 0; i < halfCount; ++i)
    {
        const wxString label = "Half " + HalfLetter(i);
        auto* btn = new wxToggleButton(m_toolbar, kHalfToggleIdBase + i, label,
            wxDefaultPosition, wxSize(110, 28));
        btn->SetValue(true);                 // halves start visible
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

    m_toolbar->Layout();
}

void PreviewFrame::LoadHalves()
{
    if (!m_canvas) return;

    for (size_t i = 0; i < m_pendingHalves.size(); ++i)
    {
        const std::string label = "Half " + HalfLetter((int)i);
        m_canvas->AddPreviewHalf(m_pendingHalves[i], label);
    }

    // Meshes now live in GPU buffers on the canvas; drop the CPU copies.
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
