// ResultsDetails.cpp — see ResultsDetails.h.
#include "ResultsDetails.h"

#include <wx/clipbrd.h>
#include <wx/dcbuffer.h>
#include <wx/dcgraph.h>
#include <wx/file.h>
#include <wx/filedlg.h>

#include <algorithm>
#include <functional>

#include "RoundedButton.h"
#include "WindowEffects.h"
#include "style.h"

namespace ResultsDetails
{
    wxColour StatusColour(Status s)
    {
        switch (s)
        {
        case Status::Pass: return wxColour(0x26, 0xAB, 0x36);
        case Status::Info: return wxColour(0x79, 0x92, 0xC5);
        case Status::Warn: return wxColour(0xE0, 0x9B, 0x20);
        case Status::Fail: return wxColour(0xD0, 0x46, 0x46);
        default:           return wxColour(0xB0, 0xB8, 0xC8);
        }
    }

    const char* StatusName(Status s)
    {
        switch (s)
        {
        case Status::Pass: return "Pass";
        case Status::Info: return "Info";
        case Status::Warn: return "Warn";
        case Status::Fail: return "Fail";
        default:           return "";
        }
    }

    std::vector<wxString> Report::Tabs() const
    {
        std::vector<wxString> tabs;
        for (const Table& t : tables)
            if (std::find(tabs.begin(), tabs.end(), t.tab) == tabs.end()) tabs.push_back(t.tab);
        return tabs;
    }

    static wxString OneLine(const wxString& s)
    {
        wxString o = s;
        o.Replace("\n", wxString::FromUTF8("  \xc2\xb7  "));
        return o;
    }

    static wxString Cell(const wxString& s, wxChar sep)
    {
        wxString c = s;
        c.Replace("\r", " ");
        c.Replace("\n", " ");
        if (sep == ',')
        {
            if (c.find_first_of(",\"") != wxString::npos)
            {
                c.Replace("\"", "\"\"");
                c = "\"" + c + "\"";
            }
            return c;
        }
        c.Replace("\t", " ");
        return c;
    }

    wxString Report::ToText(wxChar sep) const
    {
        wxString out;
        out << Cell(test, sep) << sep << Cell(OneLine(verdict), sep) << "\n\n";
        if (!valid) return out;
        for (const Table& t : tables)
        {
            bool hasStatus = false;
            for (const Row& r : t.rows) hasStatus = hasStatus || r.status != Status::None;
            out << Cell(t.title.IsEmpty() ? t.tab : t.tab + " - " + t.title, sep) << "\n";
            if (!t.columns.empty())
            {
                wxString line;
                if (hasStatus) line << "Status" << sep;
                for (size_t c = 0; c < t.columns.size(); ++c)
                    line << (c ? wxString(sep) : wxString()) << Cell(t.columns[c], sep);
                out << line << "\n";
            }
            for (const Row& r : t.rows)
            {
                wxString line;
                if (hasStatus) line << StatusName(r.status) << sep;
                for (size_t c = 0; c < r.cells.size(); ++c)
                    line << (c ? wxString(sep) : wxString()) << Cell(r.cells[c], sep);
                out << line << "\n";
            }
            if (!t.note.IsEmpty()) out << Cell(t.note, sep) << "\n";
            out << "\n";
        }
        return out;
    }

    bool ExportCsv(const Report& report, wxWindow* parent)
    {
        if (!report.valid) return false;
        wxString name = report.test.IsEmpty() ? wxString("results") : report.test;
        name.Replace("/", "-");
        name.Replace("\\", "-");
        name.Replace(":", "");
        wxFileDialog dlg(parent, "Export " + report.test + " results", wxEmptyString, name + ".csv",
                         "CSV files (*.csv)|*.csv", wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
        if (dlg.ShowModal() != wxID_OK) return false;
        wxFile f;
        if (!f.Create(dlg.GetPath(), true) || !f.IsOpened())
        {
            wxMessageBox("Couldn't write " + dlg.GetPath(), "Export results", wxOK | wxICON_ERROR, parent);
            return false;
        }
        const wxScopedCharBuffer utf8 = report.ToText(',').utf8_str();
        static const unsigned char bom[3] = { 0xEF, 0xBB, 0xBF };
        f.Write(bom, 3);
        f.Write(utf8.data(), utf8.length());
        return true;
    }
}

using namespace ResultsDetails;

namespace
{
    wxFont UiFont(int pt, wxFontWeight w = wxFONTWEIGHT_NORMAL)
    {
        return wxFont(pt, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL, w, false, "Segoe UI");
    }
}

// ===========================================================================
// Header — the test name with its verdict pill, and the tab strip beneath
// (wrapping onto more rows when narrow). Painted, so it matches the dark UI.
// ===========================================================================
class ResultsDetailsDialog::Header : public wxWindow
{
public:
    std::function<void(int)> onSelect;

    explicit Header(wxWindow* parent) : wxWindow(parent, wxID_ANY)
    {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        m_fTitle = UiFont(13, wxFONTWEIGHT_BOLD);
        m_fPill  = UiFont(9, wxFONTWEIGHT_BOLD);
        m_fTab   = UiFont(9);
        m_fTabOn = UiFont(9, wxFONTWEIGHT_BOLD);
        Bind(wxEVT_PAINT, [this](wxPaintEvent&) { OnPaint(); });
        Bind(wxEVT_SIZE, [this](wxSizeEvent& e) { Relayout(); e.Skip(); });
        Bind(wxEVT_MOTION, [this](wxMouseEvent& e) { SetHover(HitTab(e.GetPosition())); });
        Bind(wxEVT_LEAVE_WINDOW, [this](wxMouseEvent&) { SetHover(-1); });
        Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent& e)
        {
            const int t = HitTab(e.GetPosition());
            if (t >= 0 && onSelect) onSelect(t);
        });
    }

    void SetContent(const wxString& title, const wxString& verdict, const wxColour& colour, bool valid,
                    const std::vector<wxString>& tabs, int selected)
    {
        m_title = title;
        m_verdict = OneLine(verdict);
        m_colour = valid ? colour : Style::BtnSmall;
        m_tabs = tabs;
        m_sel = selected;
        Relayout();
        Refresh();
    }

private:
    // Tab rectangles and the total height for the current width.
    int Measure(wxDC& dc)
    {
        const int M = FromDIP(16);
        dc.SetFont(m_fTitle);
        const int titleH = dc.GetTextExtent("Ag").y;
        int y = FromDIP(14) + titleH + FromDIP(10);
        m_rects.clear();
        if (m_tabs.empty()) return y;
        dc.SetFont(m_fTabOn);
        const int padX = FromDIP(12), tabH = dc.GetTextExtent("Ag").y + FromDIP(14);
        const int right = std::max(GetClientSize().x - M, M + FromDIP(80));
        int x = M - padX;
        for (const wxString& t : m_tabs)
        {
            const int w = dc.GetTextExtent(t).x + 2 * padX;
            if (x + w > right + padX && x > M - padX) { x = M - padX; y += tabH; }
            m_rects.emplace_back(x, y, w, tabH);
            x += w;
        }
        return y + tabH;
    }

    void Relayout()
    {
        wxClientDC cdc(this);
        wxGCDC dc(cdc);
        const int h = Measure(dc);
        if (GetMinSize().y != h)
        {
            SetMinSize(wxSize(-1, h));
            if (GetParent()) GetParent()->Layout();
        }
    }

    int HitTab(const wxPoint& p) const
    {
        for (size_t i = 0; i < m_rects.size(); ++i) if (m_rects[i].Contains(p)) return (int)i;
        return -1;
    }

    void SetHover(int t)
    {
        if (t == m_hover) return;
        m_hover = t;
        SetCursor(t >= 0 ? wxCursor(wxCURSOR_HAND) : wxNullCursor);
        Refresh();
    }

    void OnPaint()
    {
        wxBufferedPaintDC pdc(this);
        wxGCDC dc(pdc);
        const wxSize sz = GetClientSize();
        dc.SetPen(*wxTRANSPARENT_PEN);
        dc.SetBrush(wxBrush(Style::AppBg));
        dc.DrawRectangle(0, 0, sz.x, sz.y);
        Measure(dc);

        const int M = FromDIP(16);
        int y = FromDIP(14);
        dc.SetFont(m_fTitle);
        const wxSize ts = dc.GetTextExtent(m_title);
        dc.SetTextForeground(Style::TextPrimary);
        dc.DrawText(m_title, M, y);
        if (!m_verdict.IsEmpty())
        {
            dc.SetFont(m_fPill);
            const wxSize vs = dc.GetTextExtent(m_verdict);
            const int padX = FromDIP(9), padY = FromDIP(3);
            const int px = M + ts.x + FromDIP(12), ph = vs.y + 2 * padY, py = y + (ts.y - ph) / 2;
            dc.SetBrush(wxBrush(m_colour));
            dc.DrawRoundedRectangle(px, py, vs.x + 2 * padX, ph, FromDIP(4));
            dc.SetTextForeground(*wxWHITE);
            dc.DrawText(m_verdict, px + padX, py + padY);
        }

        // Tab strip over a divider line; the selected tab is underlined.
        dc.SetBrush(wxBrush(Style::Divider));
        dc.DrawRectangle(0, sz.y - 1, sz.x, 1);
        for (size_t i = 0; i < m_rects.size(); ++i)
        {
            const wxRect& r = m_rects[i];
            const bool on = (int)i == m_sel, hover = (int)i == m_hover;
            dc.SetFont(on ? m_fTabOn : m_fTab);
            const wxSize te = dc.GetTextExtent(m_tabs[i]);
            dc.SetTextForeground(on ? Style::TextPrimary : hover ? Style::TextSubtle : Style::TextMuted);
            dc.DrawText(m_tabs[i], r.x + (r.width - te.x) / 2, r.y + (r.height - te.y) / 2);
            if (on || hover)
            {
                dc.SetBrush(wxBrush(on ? Style::BtnPlace : Style::Divider));
                dc.DrawRectangle(r.x + FromDIP(6), r.GetBottom() - FromDIP(2), r.width - FromDIP(12), FromDIP(2));
            }
        }
    }

    wxString m_title, m_verdict;
    wxColour m_colour;
    std::vector<wxString> m_tabs;
    std::vector<wxRect> m_rects;
    int m_sel = 0, m_hover = -1;
    wxFont m_fTitle, m_fPill, m_fTab, m_fTabOn;
};

// ===========================================================================
// View — the selected tab's tables on a scrolled canvas: striped rows, status
// dots, numeric columns right-aligned. Layout and painting share one pass.
// ===========================================================================
class ResultsDetailsDialog::View : public wxScrolledCanvas
{
public:
    explicit View(wxWindow* parent)
        : wxScrolledCanvas(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                           wxVSCROLL | wxHSCROLL | wxFULL_REPAINT_ON_RESIZE)
    {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetBackgroundColour(Style::AppBg);
        SetScrollRate(FromDIP(12), FromDIP(12));
        m_fBody    = UiFont(9);
        m_fBold    = UiFont(9, wxFONTWEIGHT_BOLD);
        m_fSmall   = UiFont(8);
        m_fSection = UiFont(8, wxFONTWEIGHT_BOLD);
        Bind(wxEVT_PAINT, [this](wxPaintEvent&) { OnPaint(); });
        Bind(wxEVT_SIZE, [this](wxSizeEvent& e) { Relayout(); e.Skip(); });
    }

    // The tables to show (all on one tab); `valid` false shows the not-run note.
    void SetTables(std::vector<const Table*> tables, bool valid)
    {
        m_tables = std::move(tables);
        m_valid = valid;
        Relayout();
        Scroll(0, 0);
        Refresh();
    }

private:
    void Relayout()
    {
        wxClientDC cdc(this);
        wxGCDC dc(cdc);
        ApplyVirtualSize(Render(dc, false, 0, 0));
    }

    void ApplyVirtualSize(const wxSize& content)
    {
        const wxSize client = GetClientSize();
        const wxSize want(std::max(content.x, client.x), content.y);
        if (want != GetVirtualSize()) SetVirtualSize(want);
    }

    void OnPaint()
    {
        wxBufferedPaintDC pdc(this);
        wxGCDC dc(pdc);
        const wxSize client = GetClientSize();
        dc.SetPen(*wxTRANSPARENT_PEN);
        dc.SetBrush(wxBrush(Style::AppBg));
        dc.DrawRectangle(0, 0, client.x, client.y);
        const wxPoint origin = CalcUnscrolledPosition(wxPoint(0, 0));
        const wxSize content = Render(dc, true, origin.x, origin.y);
        const wxSize want(std::max(content.x, client.x), content.y);
        if (want != GetVirtualSize())
            CallAfter([this, content] { ApplyVirtualSize(content); Refresh(); });
    }

    static std::vector<wxString> Wrap(wxDC& dc, const wxString& text, int width)
    {
        std::vector<wxString> lines;
        wxString line;
        for (const wxString& w : wxSplit(text, ' ', '\0'))
        {
            const wxString trial = line.IsEmpty() ? w : line + " " + w;
            if (!line.IsEmpty() && dc.GetTextExtent(trial).x > width) { lines.push_back(line); line = w; }
            else line = trial;
        }
        if (!line.IsEmpty()) lines.push_back(line);
        return lines;
    }

    // "Numeric" cells right-align: a digit, or a sign / comparison / dot
    // followed by one. Placeholders ("-", em dash) and empty cells are neutral.
    static int NumericKind(const wxString& raw)
    {
        const wxString s = wxString(raw).Trim(true).Trim(false);
        if (s.IsEmpty() || s == "-" || s == wxString::FromUTF8("\xe2\x80\x94")) return 0;
        const wxUniChar c0 = s[0];
        auto digit = [](wxUniChar c) { return c >= '0' && c <= '9'; };
        if (digit(c0)) return 1;
        if (s.length() > 1 && (c0 == '-' || c0 == '+' || c0 == '<' || c0 == '>' || c0 == '~' || c0 == '.' ||
                               c0 == wxUniChar(0x2212)) &&
            (digit(s[1]) || s[1] == '.'))
            return 1;
        return -1;
    }

    wxSize Render(wxDC& dc, bool paint, int ox, int oy)
    {
        const int M = FromDIP(16);
        const int width = std::max(GetClientSize().x, FromDIP(320));
        const int textW = width - 2 * M;
        int y = FromDIP(14);
        int maxX = width;
        const int viewTop = oy, viewBottom = oy + GetClientSize().y;
        auto visible = [&](int top, int h) { return top + h >= viewTop && top <= viewBottom; };
        auto text = [&](const wxString& s, int x, int yy, const wxColour& c)
        {
            if (!paint) return;
            dc.SetTextForeground(c);
            dc.DrawText(s, x - ox, yy - oy);
        };

        dc.SetFont(m_fBody);
        const int lh = dc.GetTextExtent("Ag").y;
        dc.SetFont(m_fSmall);
        const int lhSmall = dc.GetTextExtent("Ag").y;

        if (!m_valid)
        {
            dc.SetFont(m_fBody);
            text(wxString::FromUTF8("Not run yet \xe2\x80\x94 press Start on the test's card (left column)."),
                 M, y, Style::TextMuted);
            return wxSize(maxX, y + lh + M);
        }

        const int pad = FromDIP(10);
        const int rowH = lh + FromDIP(7);
        const int maxColW = FromDIP(420);
        const wxColour rowA = Style::CardBg, rowB(0x44, 0x53, 0x6E), headBg = Style::SectionHeaderBg;
        const bool titled = m_tables.size() > 1;

        for (const Table* tp : m_tables)
        {
            const Table& t = *tp;
            if (titled)
            {
                dc.SetFont(m_fSection);
                text((t.title.IsEmpty() ? t.tab : t.title).Upper(), M, y, Style::TextSubtle);
                y += dc.GetTextExtent("Ag").y + FromDIP(5);
            }

            const bool keyValue = t.columns.empty();
            size_t ncols = t.columns.size();
            bool hasStatus = false;
            for (const Row& row : t.rows)
            {
                ncols = std::max(ncols, row.cells.size());
                hasStatus = hasStatus || row.status != Status::None;
            }
            if (ncols == 0) continue;
            const int dotW = (hasStatus || keyValue) ? FromDIP(18) : 0;   // key/value: columns line up

            std::vector<int> colW(ncols, 0), numeric(ncols, 0);
            dc.SetFont(m_fBold);
            for (size_t c = 0; c < t.columns.size(); ++c)
                colW[c] = std::max(colW[c], dc.GetTextExtent(t.columns[c]).x);
            dc.SetFont(m_fBody);
            for (const Row& row : t.rows)
                for (size_t c = 0; c < row.cells.size(); ++c)
                {
                    colW[c] = std::max(colW[c], std::min(maxColW, dc.GetTextExtent(row.cells[c]).x));
                    const int k = NumericKind(row.cells[c]);
                    if (k < 0) numeric[c] = -1;
                    else if (k > 0 && numeric[c] == 0) numeric[c] = 1;
                }
            if (keyValue)
            {
                numeric.assign(ncols, -1);
                colW[0] = std::max(colW[0], FromDIP(190));
            }
            numeric[0] = -1;

            int tableW = dotW;
            for (size_t c = 0; c < ncols; ++c) tableW += colW[c] + 2 * pad;
            const int drawW = std::max(tableW, textW);
            maxX = std::max(maxX, M + tableW + M);

            auto drawCells = [&](const std::vector<wxString>& cells, int yy, bool header)
            {
                int x = M + dotW;
                for (size_t c = 0; c < ncols; ++c)
                {
                    const wxString raw = c < cells.size() ? cells[c] : wxString();
                    wxString s = raw;
                    if (dc.GetTextExtent(s).x > colW[c])
                        s = wxControl::Ellipsize(raw, dc, wxELLIPSIZE_END, colW[c]);
                    const int tw = dc.GetTextExtent(s).x;
                    const int tx = (numeric[c] > 0) ? x + pad + colW[c] - tw : x + pad;
                    const wxColour col = header ? Style::TextMuted
                                       : (keyValue && c == 0) ? Style::TextMuted : Style::TextPrimary;
                    text(s, tx, yy + (rowH - lh) / 2, col);
                    x += colW[c] + 2 * pad;
                }
            };

            if (!keyValue)
            {
                if (paint && visible(y, rowH))
                {
                    dc.SetPen(*wxTRANSPARENT_PEN);
                    dc.SetBrush(wxBrush(headBg));
                    dc.DrawRectangle(M - ox, y - oy, drawW, rowH);
                    dc.SetFont(m_fBold);
                    drawCells(t.columns, y, true);
                }
                y += rowH;
            }
            dc.SetFont(m_fBody);
            for (size_t i = 0; i < t.rows.size(); ++i)
            {
                const Row& row = t.rows[i];
                if (paint && visible(y, rowH))
                {
                    dc.SetPen(*wxTRANSPARENT_PEN);
                    dc.SetBrush(wxBrush((i % 2) ? rowB : rowA));
                    dc.DrawRectangle(M - ox, y - oy, drawW, rowH);
                    if (row.status != Status::None)
                    {
                        dc.SetBrush(wxBrush(StatusColour(row.status)));
                        dc.DrawCircle(M + dotW / 2 + FromDIP(2) - ox, y + rowH / 2 - oy, FromDIP(4));
                    }
                    drawCells(row.cells, y, false);
                }
                y += rowH;
            }
            if (!t.note.IsEmpty())
            {
                y += FromDIP(4);
                dc.SetFont(m_fSmall);
                for (const wxString& l : Wrap(dc, t.note, textW))
                {
                    text(l, M, y, Style::TextMuted);
                    y += lhSmall;
                }
            }
            y += FromDIP(16);
        }
        return wxSize(maxX, y + M);
    }

    std::vector<const Table*> m_tables;
    bool m_valid = false;
    wxFont m_fBody, m_fBold, m_fSmall, m_fSection;
};

// ===========================================================================
// Dialog
// ===========================================================================
ResultsDetailsDialog::ResultsDetailsDialog(wxWindow* parent, const wxString& title)
    : wxDialog(parent, wxID_ANY, title, wxDefaultPosition, wxDefaultSize,
               wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
{
    SetBackgroundColour(Style::AppBg);
    WindowEffects::ApplyRoundedCorners(this);

    auto* root = new wxBoxSizer(wxVERTICAL);
    m_header = new Header(this);
    m_header->onSelect = [this](int t) { SelectTab(t); };
    root->Add(m_header, 0, wxEXPAND);
    m_view = new View(this);
    root->Add(m_view, 1, wxEXPAND);

    auto* divider = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxSize(-1, 1));
    divider->SetBackgroundColour(Style::Divider);
    root->Add(divider, 0, wxEXPAND);

    const wxFont btnFont = UiFont(9, wxFONTWEIGHT_SEMIBOLD);
    auto makeBtn = [&](const wxString& label, const wxColour& bg, wxWindowID id = wxID_ANY)
    {
        auto* b = new RoundedButton(this, id, label, wxDefaultPosition, FromDIP(wxSize(96, 30)), wxBORDER_NONE);
        b->SetBackgroundColour(bg);
        b->SetForegroundColour(*wxWHITE);
        b->SetFont(btnFont);
        return b;
    };
    auto* bar = new wxBoxSizer(wxHORIZONTAL);
    RoundedButton* openBtn = makeBtn("Open View", Style::BtnSmall);
    openBtn->SetToolTip("Show this result in the Preview: the Sim Viewer on this tab's view, with only the "
                        "shot body visible.");
    m_openViewBtn = openBtn;
    bar->Add(openBtn, 0);
    bar->AddStretchSpacer(1);
    RoundedButton* copyBtn = makeBtn("Copy", Style::BtnSmall);
    copyBtn->SetToolTip("Copy every tab as tab-separated text (pastes into a spreadsheet).");
    RoundedButton* closeBtn = makeBtn("Close", Style::BtnPlace, wxID_CLOSE);
    bar->Add(copyBtn, 0, wxRIGHT, FromDIP(8));
    bar->Add(closeBtn, 0);
    root->Add(bar, 0, wxEXPAND | wxALL, FromDIP(12));

    copyBtn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { OnCopy(); });
    openBtn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&)
    {
        const int v = CurrentView();
        if (v >= 0 && onOpenView) onOpenView(v);
    });
    closeBtn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { Hide(); });
    // Modeless and reused: closing only hides it (the panel keeps the instance).
    Bind(wxEVT_CLOSE_WINDOW, [this](wxCloseEvent& e)
    {
        if (e.CanVeto()) { e.Veto(); Hide(); }
        else e.Skip();
    });
    SetEscapeId(wxID_CLOSE);

    SetSizer(root);
    SetSize(FromDIP(wxSize(760, 560)));
    SetMinSize(FromDIP(wxSize(420, 300)));
    SetReport(m_report);
}

void ResultsDetailsDialog::SetReport(const Report& report)
{
    const wxString previous = (m_tab >= 0 && (size_t)m_tab < m_tabs.size()) ? m_tabs[(size_t)m_tab] : wxString();
    m_report = report;
    m_tabs = m_report.valid ? m_report.Tabs() : std::vector<wxString>();
    auto it = std::find(m_tabs.begin(), m_tabs.end(), previous);
    m_tab = (it != m_tabs.end()) ? (int)(it - m_tabs.begin()) : 0;
    SetTitle(report.test.IsEmpty() ? wxString("Results") : report.test + wxString::FromUTF8(" \xe2\x80\x94 Details"));
    SelectTab(m_tab);
}

void ResultsDetailsDialog::SelectTab(int tab)
{
    m_tab = m_tabs.empty() ? 0 : std::clamp(tab, 0, (int)m_tabs.size() - 1);
    m_header->SetContent(m_report.test, m_report.verdict, m_report.verdictColour, m_report.valid, m_tabs, m_tab);
    std::vector<const Table*> shown;
    if (!m_tabs.empty())
        for (const Table& t : m_report.tables)
            if (t.tab == m_tabs[(size_t)m_tab]) shown.push_back(&t);
    m_view->SetTables(std::move(shown), m_report.valid);
    if (m_openViewBtn)
    {
        m_openViewBtn->Show(m_report.valid && m_report.view >= 0);   // only tests with a view
        m_openViewBtn->Refresh();
    }
    Layout();
}

int ResultsDetailsDialog::CurrentView() const
{
    if (!m_report.valid) return -1;
    if (m_tab >= 0 && (size_t)m_tab < m_tabs.size())
        for (const Table& t : m_report.tables)
            if (t.tab == m_tabs[(size_t)m_tab] && t.view >= 0) return t.view;
    return m_report.view;
}

void ResultsDetailsDialog::OnCopy()
{
    if (wxTheClipboard->Open())
    {
        wxTheClipboard->SetData(new wxTextDataObject(m_report.ToText('\t')));
        wxTheClipboard->Close();
    }
}
