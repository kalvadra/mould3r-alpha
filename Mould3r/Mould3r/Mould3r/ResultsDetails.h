#pragma once
// ===========================================================================
// ResultsDetails — the numbers behind a results card.
//
// Each analysis (Draft Angle Checks, Separation Test, Hele-Shaw flow) fills a
// Report: its verdict and titled tables of the numerical results, grouped into
// tabs. The results card's "Details" button opens a ResultsDetailsDialog on it
// (modeless and resizable, so it can stay open beside the model; it refreshes
// when the analysis is re-run), and its "Export" button saves the same tables
// as CSV. A test with a view in the Preview (Report::view >= 0) also gets an
// "Open View" button: switch to the Preview on that Sim Viewer view.
// ===========================================================================

#include <wx/wx.h>

#include <functional>
#include <initializer_list>
#include <vector>

namespace ResultsDetails
{
    // Row severity, drawn as a coloured dot; None draws nothing.
    enum class Status { None, Pass, Info, Warn, Fail };

    struct Row
    {
        std::vector<wxString> cells;
        Status status = Status::None;
    };

    // A table on a tab. Tables sharing a tab stack on it (each under its
    // `title`); a tab with one table shows just the table. `columns` empty = a
    // key / value list (no header row). Numeric columns right-align.
    struct Table
    {
        wxString tab;
        wxString title;
        std::vector<wxString> columns;
        std::vector<Row> rows;
        wxString note;                           // small print under the table
        int      view = -1;                      // Sim Viewer view for its tab (-1: the report's)

        Table() = default;
        explicit Table(const wxString& tabName, std::initializer_list<wxString> cols = {})
            : tab(tabName), columns(cols) {}
        Table& Add(std::initializer_list<wxString> cells, Status s = Status::None)
        {
            rows.push_back(Row{ std::vector<wxString>(cells), s });
            return *this;
        }
    };

    struct Report
    {
        bool     valid = false;                  // false: the test hasn't run (or was cleared)
        wxString test;                           // "Draft Angle Checks"
        wxString verdict;                        // as on the card (lines joined for one-line use)
        wxColour verdictColour = wxColour(0xB0, 0xB8, 0xC8);
        int      view = -1;                      // Sim Viewer view that shows it (-1: none)
        std::vector<Table> tables;

        std::vector<wxString> Tabs() const;      // in order of first appearance
        // Plain-text export: `sep` '\t' for the clipboard, ',' for CSV (quoted).
        wxString ToText(wxChar sep) const;
    };

    wxColour StatusColour(Status s);
    const char* StatusName(Status s);            // "Fail", "Warn", ... ("" for None)

    // Ask for a file and save the report as CSV (UTF-8 with a BOM, so Excel
    // reads the degree / superscript signs). False when cancelled or failed.
    bool ExportCsv(const Report& report, wxWindow* parent);
}

class ResultsDetailsDialog : public wxDialog
{
public:
    ResultsDetailsDialog(wxWindow* parent, const wxString& title);

    // Replace what the window shows; stays on the same tab when the new report
    // has it (a re-run), else opens on the first.
    void SetReport(const ResultsDetails::Report& report);
    const ResultsDetails::Report& GetReport() const { return m_report; }
    void SelectTab(int tab);                     // index into GetReport().Tabs()

    // "Open View": called with the Sim Viewer view for the tab on show.
    std::function<void(int view)> onOpenView;

    class Header;                                // title, verdict and tab strip (ResultsDetails.cpp)
    class View;                                  // the selected tab's tables

private:
    void OnCopy();
    int  CurrentView() const;                    // the selected tab's view, else the report's

    Header* m_header = nullptr;
    wxWindow* m_openViewBtn = nullptr;
    View*   m_view = nullptr;
    ResultsDetails::Report m_report;
    std::vector<wxString>  m_tabs;
    int                    m_tab = 0;
};
