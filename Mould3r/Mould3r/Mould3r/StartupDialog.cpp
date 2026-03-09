#include "StartupDialog.h"
#include <wx/filedlg.h>
#include <wx/statline.h>

static const wxColour kBg(0x1E, 0x22, 0x2A);
static const wxColour kPanel(0x25, 0x2B, 0x36);
static const wxColour kAccent(0x00, 0x7A, 0xCC);
static const wxColour kText(0xC8, 0xD0, 0xDC);
static const wxColour kSubtext(0x66, 0x77, 0x88);

StartupDialog::StartupDialog(wxWindow* parent)
    : wxDialog(parent, wxID_ANY, "Mould3r — New Project",
        wxDefaultPosition, wxSize(520, 340),
        wxDEFAULT_DIALOG_STYLE)
{
    SetBackgroundColour(kBg);

    auto* main = new wxBoxSizer(wxVERTICAL);

    // ---- Title area --------------------------------------------------------
    auto* titlePanel = new wxPanel(this, wxID_ANY);
    titlePanel->SetBackgroundColour(kPanel);

    auto* titleSizer = new wxBoxSizer(wxVERTICAL);

    auto* title = new wxStaticText(titlePanel, wxID_ANY, "New Project");
    title->SetForegroundColour(kText);
    title->SetFont(wxFont(16, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
        wxFONTWEIGHT_BOLD, false, "Segoe UI"));

    auto* subtitle = new wxStaticText(titlePanel, wxID_ANY,
        "Select two STEP models to load into the environment.");
    subtitle->SetForegroundColour(kSubtext);
    subtitle->SetFont(wxFont(9, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
        wxFONTWEIGHT_NORMAL, false, "Segoe UI"));

    titleSizer->Add(title, 0, wxLEFT | wxTOP, 18);
    titleSizer->Add(subtitle, 0, wxLEFT | wxTOP | wxBOTTOM, 18);
    titlePanel->SetSizer(titleSizer);

    main->Add(titlePanel, 0, wxEXPAND);

    // 1px accent line under title
    auto* sep = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxSize(-1, 1));
    sep->SetBackgroundColour(kAccent);
    main->Add(sep, 0, wxEXPAND);

    // ---- File row helper ---------------------------------------------------
    auto addFileRow = [&](const wxString& label, wxTextCtrl*& ctrl, int browseId)
        {
            auto* row = new wxBoxSizer(wxHORIZONTAL);

            auto* lbl = new wxStaticText(this, wxID_ANY, label,
                wxDefaultPosition, wxSize(70, -1));
            lbl->SetForegroundColour(kText);
            lbl->SetFont(wxFont(9, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
                wxFONTWEIGHT_SEMIBOLD, false, "Segoe UI"));

            ctrl = new wxTextCtrl(this, wxID_ANY, "",
                wxDefaultPosition, wxSize(300, -1),
                wxTE_READONLY);
            ctrl->SetBackgroundColour(kPanel);
            ctrl->SetForegroundColour(kText);

            auto* browse = new wxButton(this, browseId, "Browse...",
                wxDefaultPosition, wxSize(80, -1));
            browse->SetBackgroundColour(wxColour(0x2A, 0x30, 0x3C));
            browse->SetForegroundColour(kText);

            row->Add(lbl, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
            row->Add(ctrl, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
            row->Add(browse, 0, wxALIGN_CENTER_VERTICAL);

            main->Add(row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 18);
        };

    addFileRow("Model A:", m_pathA, ID_BrowseA);
    addFileRow("Model B:", m_pathB, ID_BrowseB);

    // ---- Buttons -----------------------------------------------------------
    main->AddStretchSpacer();

    auto* btnSizer = new wxBoxSizer(wxHORIZONTAL);
    btnSizer->AddStretchSpacer();

    auto* btnCancel = new wxButton(this, wxID_CANCEL, "Cancel",
        wxDefaultPosition, wxSize(90, 30));
    auto* btnOK = new wxButton(this, wxID_OK, "Open",
        wxDefaultPosition, wxSize(90, 30));

    btnOK->SetBackgroundColour(kAccent);
    btnOK->SetForegroundColour(*wxWHITE);
    btnOK->SetFont(wxFont(9, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
        wxFONTWEIGHT_SEMIBOLD, false, "Segoe UI"));

    btnCancel->SetBackgroundColour(wxColour(0x2A, 0x30, 0x3C));
    btnCancel->SetForegroundColour(kText);

    btnSizer->Add(btnCancel, 0, wxRIGHT, 8);
    btnSizer->Add(btnOK, 0);

    main->Add(btnSizer, 0, wxEXPAND | wxALL, 18);

    SetSizer(main);
    CentreOnScreen();

    // ---- Binds -------------------------------------------------------------
    Bind(wxEVT_BUTTON, &StartupDialog::OnBrowseA, this, ID_BrowseA);
    Bind(wxEVT_BUTTON, &StartupDialog::OnBrowseB, this, ID_BrowseB);
    Bind(wxEVT_BUTTON, &StartupDialog::OnOK, this, wxID_OK);
}

void StartupDialog::OnBrowseA(wxCommandEvent&)
{
    wxFileDialog dlg(this, "Select Model A", "", "",
        "STEP files (*.step;*.stp)|*.step;*.stp|All files (*.*)|*.*",
        wxFD_OPEN | wxFD_FILE_MUST_EXIST);
    if (dlg.ShowModal() == wxID_OK)
        m_pathA->SetValue(dlg.GetPath());
}

void StartupDialog::OnBrowseB(wxCommandEvent&)
{
    wxFileDialog dlg(this, "Select Model B", "", "",
        "STEP files (*.step;*.stp)|*.step;*.stp|All files (*.*)|*.*",
        wxFD_OPEN | wxFD_FILE_MUST_EXIST);
    if (dlg.ShowModal() == wxID_OK)
        m_pathB->SetValue(dlg.GetPath());
}

void StartupDialog::OnOK(wxCommandEvent&)
{
    if (m_pathA->GetValue().IsEmpty() || m_pathB->GetValue().IsEmpty())
    {
        wxMessageBox("Please select both models before continuing.",
            "Missing Files", wxOK | wxICON_WARNING, this);
        return;
    }
    EndModal(wxID_OK);
}

StartupConfig StartupDialog::GetConfig() const
{
    return { m_pathA->GetValue().ToStdString(),
             m_pathB->GetValue().ToStdString() };
}