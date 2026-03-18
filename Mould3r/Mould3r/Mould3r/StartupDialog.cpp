#include "StartupDialog.h"
#include <wx/filedlg.h>
#include <wx/dirdlg.h>
#include <wx/stdpaths.h>
#include <filesystem>

namespace fs = std::filesystem;

static const wxColour kBg(0x1E, 0x22, 0x2A);
static const wxColour kPanel(0x25, 0x2B, 0x36);
static const wxColour kAccent(0x00, 0x7A, 0xCC);
static const wxColour kText(0xC8, 0xD0, 0xDC);
static const wxColour kSubtext(0x66, 0x77, 0x88);

StartupDialog::StartupDialog(wxWindow* parent)
    : wxDialog(parent, wxID_ANY, "Mould3r — Select Fixture",
        wxDefaultPosition, wxSize(560, 480),
        wxDEFAULT_DIALOG_STYLE)
{
    SetBackgroundColour(kBg);

    // Default fixtures folder: 'fixtures/' next to the executable
    m_fixturesFolder = (fs::path(wxStandardPaths::Get()
        .GetExecutablePath().ToStdString())
        .parent_path() / "fixtures").string();

    auto* main = new wxBoxSizer(wxVERTICAL);

    // ---- Title area --------------------------------------------------------
    auto* titlePanel = new wxPanel(this, wxID_ANY);
    titlePanel->SetBackgroundColour(kPanel);
    auto* titleSizer = new wxBoxSizer(wxVERTICAL);

    auto* title = new wxStaticText(titlePanel, wxID_ANY, "Select Fixture");
    title->SetForegroundColour(kText);
    title->SetFont(wxFont(16, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
        wxFONTWEIGHT_BOLD, false, "Segoe UI"));

    auto* subtitle = new wxStaticText(titlePanel, wxID_ANY,
        "Choose a fixture to load, or create a new one.");
    subtitle->SetForegroundColour(kSubtext);
    subtitle->SetFont(wxFont(9, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
        wxFONTWEIGHT_NORMAL, false, "Segoe UI"));

    titleSizer->Add(title, 0, wxLEFT | wxTOP, 18);
    titleSizer->Add(subtitle, 0, wxLEFT | wxTOP | wxBOTTOM, 18);
    titlePanel->SetSizer(titleSizer);
    main->Add(titlePanel, 0, wxEXPAND);

    auto* sep = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxSize(-1, 1));
    sep->SetBackgroundColour(kAccent);
    main->Add(sep, 0, wxEXPAND);

    // ---- Folder row --------------------------------------------------------
    auto* folderRow = new wxBoxSizer(wxHORIZONTAL);

    m_lblFolder = new wxStaticText(this, wxID_ANY, m_fixturesFolder,
        wxDefaultPosition, wxDefaultSize,
        wxST_ELLIPSIZE_START);
    m_lblFolder->SetForegroundColour(kSubtext);
    m_lblFolder->SetFont(wxFont(8, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
        wxFONTWEIGHT_NORMAL, false, "Segoe UI"));

    auto* btnBrowse = new wxButton(this, ID_BrowseFolder, "Browse...",
        wxDefaultPosition, wxSize(80, 26));
    btnBrowse->SetBackgroundColour(wxColour(0x2A, 0x30, 0x3C));
    btnBrowse->SetForegroundColour(kText);

    folderRow->Add(m_lblFolder, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
    folderRow->Add(btnBrowse, 0, wxALIGN_CENTER_VERTICAL);
    main->Add(folderRow, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 14);

    // ---- Fixture list ------------------------------------------------------
    m_list = new wxListCtrl(this, wxID_ANY, wxDefaultPosition, wxDefaultSize,
        wxLC_REPORT | wxLC_SINGLE_SEL | wxBORDER_NONE);
    m_list->SetBackgroundColour(kPanel);
    m_list->SetForegroundColour(kText);
    m_list->SetFont(wxFont(9, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
        wxFONTWEIGHT_NORMAL, false, "Segoe UI"));

    m_list->InsertColumn(0, "Fixture", wxLIST_FORMAT_LEFT, 240);
    m_list->InsertColumn(1, "Model A", wxLIST_FORMAT_LEFT, 130);
    m_list->InsertColumn(2, "Model B", wxLIST_FORMAT_LEFT, 130);

    main->Add(m_list, 1, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 14);

    // ---- Preview box -------------------------------------------------------
    auto* previewBox = new wxStaticBoxSizer(wxVERTICAL, this, "Selected Fixture");
    previewBox->GetStaticBox()->SetForegroundColour(kSubtext);
    previewBox->GetStaticBox()->SetBackgroundColour(kBg);

    m_lblModelA = new wxStaticText(this, wxID_ANY, "Model A: —",
        wxDefaultPosition, wxDefaultSize,
        wxST_ELLIPSIZE_END);
    m_lblModelB = new wxStaticText(this, wxID_ANY, "Model B: —",
        wxDefaultPosition, wxDefaultSize,
        wxST_ELLIPSIZE_END);

    for (auto* lbl : { m_lblModelA, m_lblModelB })
    {
        lbl->SetForegroundColour(kText);
        lbl->SetFont(wxFont(8, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
            wxFONTWEIGHT_NORMAL, false, "Segoe UI"));
        previewBox->Add(lbl, 0, wxEXPAND | wxALL, 4);
    }

    main->Add(previewBox, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 14);

    // ---- Buttons -----------------------------------------------------------
    auto* btnSizer = new wxBoxSizer(wxHORIZONTAL);

    auto* btnNew = new wxButton(this, ID_NewFixture, "New Fixture...",
        wxDefaultPosition, wxSize(110, 30));
    btnNew->SetBackgroundColour(wxColour(0x2A, 0x30, 0x3C));
    btnNew->SetForegroundColour(kText);

    btnSizer->Add(btnNew, 0);
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

    main->Add(btnSizer, 0, wxEXPAND | wxALL, 14);

    SetSizer(main);
    CentreOnScreen();

    Bind(wxEVT_BUTTON, &StartupDialog::OnBrowseFolder, this, ID_BrowseFolder);
    Bind(wxEVT_BUTTON, &StartupDialog::OnNewFixture, this, ID_NewFixture);
    Bind(wxEVT_BUTTON, &StartupDialog::OnOK, this, wxID_OK);
    Bind(wxEVT_LIST_ITEM_SELECTED, &StartupDialog::OnListSelect, this, m_list->GetId());
    Bind(wxEVT_LIST_ITEM_ACTIVATED, &StartupDialog::OnListDoubleClick, this, m_list->GetId());

    ScanFixturesFolder();
}

void StartupDialog::ScanFixturesFolder()
{
    m_list->DeleteAllItems();
    m_fixturePaths.clear();
    m_fixture = FixtureDefinition{};
    RefreshPreview();

    if (!fs::exists(m_fixturesFolder) || !fs::is_directory(m_fixturesFolder))
    {
        m_lblFolder->SetLabel(m_fixturesFolder + "  (folder not found)");
        return;
    }

    m_lblFolder->SetLabel(m_fixturesFolder);

    for (const auto& entry : fs::recursive_directory_iterator(m_fixturesFolder))
    {
        if (!entry.is_regular_file()) continue;
        if (entry.path().extension() != ".fixture") continue;

        const std::string path = entry.path().string();

        FixtureDefinition def;
        std::string error;
        if (!FixtureFile::Load(path, def, error)) continue;

        const std::string nameA = fs::path(def.modelAPath).stem().string();
        const std::string nameB = fs::path(def.modelBPath).stem().string();
        const std::string fixtureName = entry.path().stem().string();

        const long idx = m_list->InsertItem((long)m_fixturePaths.size(),
            fixtureName);
        m_list->SetItem(idx, 1, nameA);
        m_list->SetItem(idx, 2, nameB);

        m_fixturePaths.push_back(path);
    }

    if (m_fixturePaths.empty())
        m_lblFolder->SetLabel(m_fixturesFolder + "  (no fixtures found)");
}

void StartupDialog::OnListSelect(wxListEvent& evt)
{
    const long idx = evt.GetIndex();
    if (idx < 0 || idx >= (long)m_fixturePaths.size()) return;

    std::string error;
    if (!FixtureFile::Load(m_fixturePaths[idx], m_fixture, error))
    {
        wxMessageBox(error, "Load Error", wxOK | wxICON_ERROR, this);
        return;
    }

    RefreshPreview();
}

void StartupDialog::OnListDoubleClick(wxListEvent& evt)
{
    OnListSelect(evt);
    if (m_fixture.IsValid())
        EndModal(wxID_OK);
}

void StartupDialog::OnBrowseFolder(wxCommandEvent&)
{
    wxDirDialog dlg(this, "Select Fixtures Folder", m_fixturesFolder,
        wxDD_DEFAULT_STYLE | wxDD_DIR_MUST_EXIST);
    if (dlg.ShowModal() != wxID_OK) return;

    m_fixturesFolder = dlg.GetPath().ToStdString();
    ScanFixturesFolder();
}

void StartupDialog::OnNewFixture(wxCommandEvent&)
{
    wxFileDialog dlgA(this, "Select Model A", "", "",
        "STEP files (*.step;*.stp)|*.step;*.stp",
        wxFD_OPEN | wxFD_FILE_MUST_EXIST);
    if (dlgA.ShowModal() != wxID_OK) return;

    wxFileDialog dlgB(this, "Select Model B", "", "",
        "STEP files (*.step;*.stp)|*.step;*.stp",
        wxFD_OPEN | wxFD_FILE_MUST_EXIST);
    if (dlgB.ShowModal() != wxID_OK) return;

    // Default save location is the fixtures folder
    wxFileDialog dlgSave(this, "Save Fixture As",
        m_fixturesFolder, "new_fixture.fixture",
        "Fixture files (*.fixture)|*.fixture",
        wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
    if (dlgSave.ShowModal() != wxID_OK) return;

    FixtureDefinition def;
    def.modelAPath = dlgA.GetPath().ToStdString();
    def.modelBPath = dlgB.GetPath().ToStdString();

    std::string error;
    if (!FixtureFile::Save(dlgSave.GetPath().ToStdString(), def, error))
    {
        wxMessageBox(error, "Save Failed", wxOK | wxICON_ERROR, this);
        return;
    }

    // Rescan so the new fixture appears in the list
    ScanFixturesFolder();

    // Auto-select the newly created fixture
    const std::string newPath = dlgSave.GetPath().ToStdString();
    for (int i = 0; i < (int)m_fixturePaths.size(); ++i)
    {
        if (m_fixturePaths[i] == newPath)
        {
            m_list->SetItemState(i, wxLIST_STATE_SELECTED, wxLIST_STATE_SELECTED);
            m_list->EnsureVisible(i);
            FixtureFile::Load(newPath, m_fixture, error);
            RefreshPreview();
            break;
        }
    }
}

void StartupDialog::OnOK(wxCommandEvent&)
{
    if (!m_fixture.IsValid())
    {
        wxMessageBox("Please select a fixture before continuing.",
            "No Fixture Selected", wxOK | wxICON_WARNING, this);
        return;
    }
    EndModal(wxID_OK);
}

void StartupDialog::RefreshPreview()
{
    if (m_fixture.IsValid())
    {
        m_lblModelA->SetLabel("Model A: " + m_fixture.modelAPath);
        m_lblModelB->SetLabel("Model B: " + m_fixture.modelBPath);
    }
    else
    {
        m_lblModelA->SetLabel("Model A: —");
        m_lblModelB->SetLabel("Model B: —");
    }
    Layout();
}