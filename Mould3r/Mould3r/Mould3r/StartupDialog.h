#pragma once
#include <wx/wx.h>
#include <wx/listctrl.h>
#include "FixtureFile.h"

class StartupDialog : public wxDialog
{
public:
    StartupDialog(wxWindow* parent);
    FixtureDefinition GetFixture() const { return m_fixture; }

    void PreSelectFixture(const std::string& path);

private:
    void ScanFixturesFolder();
    void OnListSelect(wxListEvent& evt);
    void OnListDoubleClick(wxListEvent& evt);
    void OnNewFixture(wxCommandEvent&);
    void OnOK(wxCommandEvent&);

    // Frameless drag handling — the custom title row stands in for the
    // missing system title bar so the user can still move the window.
    // wxMouseCaptureLost is required: wxWidgets asserts in debug builds if
    // a capture is silently abandoned (e.g. via Alt+Tab).
    void OnTitleMouseDown(wxMouseEvent&);
    void OnTitleMouseUp(wxMouseEvent&);
    void OnTitleMouseMove(wxMouseEvent&);
    void OnTitleCaptureLost(wxMouseCaptureLostEvent&);

    void RefreshPreview();

    wxPanel* m_titleRow = nullptr;
    wxListCtrl* m_list = nullptr;
    wxStaticText* m_lblModelA = nullptr;
    wxStaticText* m_lblModelB = nullptr;

    // Fixtures folder is computed once at construction (sibling 'fixtures/'
    // directory next to the executable) and no longer surfaced in the UI.
    // ScanFixturesFolder() still consumes it; if folder-switching needs to
    // come back, expose it via app settings rather than the inline Browse
    // row the prototype dropped.
    std::string              m_fixturesFolder;
    std::vector<std::string> m_fixturePaths;   // parallel to list items
    FixtureDefinition        m_fixture;

    // Drag-to-move state for the frameless title row. m_dragOffset is the
    // screen-space offset from the dialog's top-left to where the user
    // clicked, captured at MouseDown and held constant for the drag.
    wxPoint m_dragOffset;
    bool    m_dragging = false;

    enum { ID_NewFixture = wxID_HIGHEST + 300, ID_Cancel };
};
