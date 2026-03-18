#pragma once
#include <wx/wx.h>
#include <wx/listctrl.h>
#include "FixtureFile.h"

class StartupDialog : public wxDialog
{
public:
    StartupDialog(wxWindow* parent);
    FixtureDefinition GetFixture() const { return m_fixture; }

private:
    void ScanFixturesFolder();
    void OnListSelect(wxListEvent& evt);
    void OnListDoubleClick(wxListEvent& evt);
    void OnBrowseFolder(wxCommandEvent&);
    void OnNewFixture(wxCommandEvent&);
    void OnOK(wxCommandEvent&);

    void RefreshPreview();

    wxListCtrl* m_list = nullptr;
    wxStaticText* m_lblFolder = nullptr;
    wxStaticText* m_lblModelA = nullptr;
    wxStaticText* m_lblModelB = nullptr;

    std::string              m_fixturesFolder;
    std::vector<std::string> m_fixturePaths;   // parallel to list items
    FixtureDefinition        m_fixture;

    enum { ID_BrowseFolder = wxID_HIGHEST + 300, ID_NewFixture };
};