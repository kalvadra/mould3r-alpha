// MaterialEditorDialog.cpp
#include "MaterialEditorDialog.h"   // wx first (wx/wx.h) — see MaterialFile.cpp

#include <wx/dcbuffer.h>
#include <wx/graphics.h>
#include <wx/popupwin.h>

#include <cmath>
#include <filesystem>
#include <map>
#include <memory>
#include <system_error>

#include "WindowEffects.h"   // DWM corner rounding for the dialog frame

namespace fs = std::filesystem;

namespace
{
    constexpr int kLabelW = 265;   // shared by every section so the columns line up
    constexpr int kFieldW = 150;
    constexpr int kUnitW = 80;     // fixed, so the markers line up across sections
    constexpr int kWrapW = 560;
    constexpr double kPi = 3.14159265358979323846;   // (M_PI needs _USE_MATH_DEFINES on MSVC)

    wxString U8(const char* s) { return wxString::FromUTF8(s ? s : ""); }

    using Row = MaterialEditorDialog::Row;

    // -----------------------------------------------------------------------
    // The missing-value marker: an amber asterisk, underlined like a link to
    // say it can be clicked (it darkens under the mouse). Self-drawn rather
    // than a font glyph so it looks the same with any system font. Clicking
    // raises wxEVT_BUTTON. A `decorative` marker (the legend) has no
    // underline and doesn't click.
    // -----------------------------------------------------------------------
    class MissingMarker : public wxWindow
    {
    public:
        static constexpr int kW = 16, kH = 20;

        MissingMarker(wxWindow* parent, bool decorative = false)
            : wxWindow(parent, wxID_ANY, wxDefaultPosition, wxSize(kW, kH), wxBORDER_NONE)
            , m_decorative(decorative)
        {
            SetBackgroundStyle(wxBG_STYLE_PAINT);
            Bind(wxEVT_PAINT, &MissingMarker::OnPaint, this);
            if (!decorative)
            {
                SetCursor(wxCursor(wxCURSOR_HAND));
                Bind(wxEVT_ENTER_WINDOW, [this](wxMouseEvent&) { m_hover = true; Refresh(); });
                Bind(wxEVT_LEAVE_WINDOW, [this](wxMouseEvent&) { m_hover = false; Refresh(); });
                Bind(wxEVT_LEFT_UP, [this](wxMouseEvent&)
                {
                    wxCommandEvent ev(wxEVT_BUTTON, GetId());
                    ev.SetEventObject(this);
                    ProcessWindowEvent(ev);
                });
            }
        }

    private:
        void OnPaint(wxPaintEvent&)
        {
            wxAutoBufferedPaintDC dc(this);
            dc.SetBackground(wxBrush(GetParent()->GetBackgroundColour()));
            dc.Clear();
            std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::Create(dc));
            if (!gc) return;

            const wxColour ink = m_hover ? wxColour(0xA8, 0x6E, 0x00) : wxColour(0xE0, 0x9A, 0x00);
            wxGraphicsPenInfo pen(ink, 2.2);
            pen.Cap(wxCAP_ROUND);
            gc->SetPen(gc->CreatePen(pen));

            // Six-spoke asterisk centred in the upper part of the box.
            const double cx = kW * 0.5, cy = 7.5, r = 5.2;
            for (int i = 0; i < 3; ++i)
            {
                const double a = kPi / 2.0 + i * kPi / 3.0;   // vertical, then +-60 degrees
                const double dx = r * std::cos(a), dy = r * std::sin(a);
                gc->StrokeLine(cx - dx, cy - dy, cx + dx, cy + dy);
            }
            // Underline: "this is clickable".
            if (!m_decorative)
            {
                wxGraphicsPenInfo u(ink, 1.4);
                u.Cap(wxCAP_BUTT);
                gc->SetPen(gc->CreatePen(u));
                gc->StrokeLine(2.0, kH - 3.0, kW - 2.0, kH - 3.0);
            }
        }

        bool m_decorative = false;
        bool m_hover = false;
    };

    // -----------------------------------------------------------------------
    // The marker's details: a small self-coloured popup (explicit colours, so it
    // reads the same under any system theme) that closes on the next click
    // anywhere else.
    // -----------------------------------------------------------------------
    class MissingInfoPopup : public wxPopupTransientWindow
    {
    public:
        MissingInfoPopup(wxWindow* parent, const wxString& title, const wxString& body)
            : wxPopupTransientWindow(parent, wxBORDER_NONE)
        {
            const wxColour border(0xC9, 0x96, 0x10), bg(0xFF, 0xF8, 0xDC), text(0x2A, 0x2A, 0x2A);
            SetBackgroundColour(border);                      // 1-px frame
            auto* panel = new wxPanel(this);
            panel->SetBackgroundColour(bg);
            auto* s = new wxBoxSizer(wxVERTICAL);
            auto* t = new wxStaticText(panel, wxID_ANY, title);
            t->SetFont(t->GetFont().Bold());
            t->SetForegroundColour(text);
            s->Add(t, 0, wxLEFT | wxRIGHT | wxTOP, 10);
            auto* b = new wxStaticText(panel, wxID_ANY, body);
            b->SetForegroundColour(text);
            b->Wrap(420);
            s->Add(b, 0, wxALL, 10);
            panel->SetSizer(s);
            auto* outer = new wxBoxSizer(wxVERTICAL);
            outer->Add(panel, 1, wxEXPAND | wxALL, 1);
            SetSizerAndFit(outer);
        }

    protected:
        // One-shot: free it once it's been dismissed (deferred, as we're
        // still inside its own dismissal handling here).
        void OnDismiss() override
        {
            CallAfter([this] { Destroy(); });
        }
    };

    // One static box of label | field | unit | marker rows for `sec`.
    template <class Data>
    void BuildSection(wxWindow* parent, wxSizer* into, const MaterialSectionSpec& sec,
                      const std::vector<MaterialFieldSpec<Data>>& fields, bool isInjection,
                      bool showComment, std::vector<Row>& rows)
    {
        auto* box = new wxStaticBoxSizer(wxVERTICAL, parent, U8(sec.title));
        wxWindow* bp = box->GetStaticBox();

        if (showComment && sec.comment && *sec.comment)
        {
            auto* c = new wxStaticText(bp, wxID_ANY, U8(sec.comment));
            c->SetForegroundColour(wxSystemSettings::GetColour(wxSYS_COLOUR_GRAYTEXT));
            c->Wrap(kWrapW - 40);
            box->Add(c, 0, wxALL, 6);
        }

        auto* grid = new wxFlexGridSizer(4, 5, 8);
        for (size_t i = 0; i < fields.size(); ++i)
        {
            const MaterialFieldSpec<Data>& f = fields[i];
            if (std::string(f.section) != sec.name) continue;
            const std::string key = f.key;
            const wxString help = U8(f.help);

            Row r;
            r.fieldIndex = (int)i;
            r.id = std::string(f.section) + "." + key;
            r.label = U8(f.label);

            auto* lbl = new wxStaticText(bp, wxID_ANY, r.label + (key == "name" ? " *" : ""),
                                         wxDefaultPosition, wxSize(kLabelW, -1));
            grid->Add(lbl, 0, wxALIGN_CENTER_VERTICAL);
            wxWindow* tipTarget = nullptr;
            if (isInjection && key == "structure")
            {
                wxArrayString opts;
                opts.Add("");
                opts.Add("semi-crystalline");
                opts.Add("amorphous");
                r.choice = new wxChoice(bp, wxID_ANY, wxDefaultPosition, wxSize(kFieldW, -1), opts);
                r.choice->SetSelection(0);
                tipTarget = r.choice;
                grid->Add(r.choice, 0, wxALIGN_CENTER_VERTICAL);
            }
            else if (isInjection && key == "filler")
            {
                // Text + "No filler" (writes "none" and disables the text).
                auto* hs = new wxBoxSizer(wxHORIZONTAL);
                r.text = new wxTextCtrl(bp, wxID_ANY, "", wxDefaultPosition, wxSize(kFieldW * 2 - 100, -1));
                r.text->SetHint("e.g. 30% glass fibre");
                r.noneCheck = new wxCheckBox(bp, wxID_ANY, "No filler");
                hs->Add(r.text, 0, wxALIGN_CENTER_VERTICAL);
                hs->Add(r.noneCheck, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, 10);
                tipTarget = r.text;
                grid->Add(hs, 0, wxALIGN_CENTER_VERTICAL);
            }
            else if (key == "notes")
            {
                r.text = new wxTextCtrl(bp, wxID_ANY, "", wxDefaultPosition, wxSize(kFieldW * 2, 54),
                                        wxTE_MULTILINE);
                tipTarget = r.text;
                grid->Add(r.text, 0, wxALIGN_CENTER_VERTICAL);
            }
            else if (f.text)
            {
                r.text = new wxTextCtrl(bp, wxID_ANY, "", wxDefaultPosition, wxSize(kFieldW * 2, -1));
                if (!help.empty()) r.text->SetHint(help);
                tipTarget = r.text;
                grid->Add(r.text, 0, wxALIGN_CENTER_VERTICAL);
            }
            else
            {
                r.text = new wxTextCtrl(bp, wxID_ANY, "", wxDefaultPosition, wxSize(kFieldW, -1));
                tipTarget = r.text;
                grid->Add(r.text, 0, wxALIGN_CENTER_VERTICAL);
            }
            if (!help.empty())
            {
                lbl->SetToolTip(help);
                tipTarget->SetToolTip(help);
            }
            auto* unit = new wxStaticText(bp, wxID_ANY, U8(f.unit), wxDefaultPosition, wxSize(kUnitW, -1));
            grid->Add(unit, 0, wxALIGN_CENTER_VERTICAL);

            // Only numeric values can be "missing" for a simulation.
            if (!f.text)
            {
                r.star = new MissingMarker(bp);
                r.star->SetToolTip("Missing value: click to see which tests it affects");
                grid->Add(r.star, 0, wxALIGN_CENTER_VERTICAL | wxRESERVE_SPACE_EVEN_IF_HIDDEN);
                r.star->Hide();
            }
            else
                grid->Add(MissingMarker::kW, MissingMarker::kH);
            rows.push_back(std::move(r));
        }
        box->Add(grid, 0, wxALL, 6);
        into->Add(box, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 8);
    }

    // Read the rows into `out`. Unparseable numbers read as unset, are named
    // in `bad`, and tint their field.
    template <class Data>
    void CollectRows(std::vector<Row>& rows, const std::vector<MaterialFieldSpec<Data>>& fields,
                     Data& out, std::vector<wxString>& bad)
    {
        static const wxColour kBadBg(0xFF, 0xD6, 0xD6);
        out = Data{};
        for (Row& r : rows)
        {
            const MaterialFieldSpec<Data>& f = fields[(size_t)r.fieldIndex];
            if (r.choice)
            {
                out.*(f.text) = std::string(r.choice->GetStringSelection().ToUTF8());
                continue;
            }
            if (r.noneCheck && r.noneCheck->GetValue())
            {
                out.*(f.text) = "none";
                continue;
            }
            wxString v = r.text->GetValue();
            v.Trim(true).Trim(false);
            if (f.text)
            {
                out.*(f.text) = std::string(v.ToUTF8());
                continue;
            }
            bool ok = true;
            if (!v.empty())
            {
                double d = 0.0;
                if (MaterialFile::ParseValue(std::string(v.ToUTF8()), d)) out.*(f.number) = d;
                else ok = false;
            }
            if (!ok) bad.push_back(r.label);
            const bool tinted = r.text->GetBackgroundColour() == kBadBg;
            if (tinted == ok)
            {
                r.text->SetBackgroundColour(ok ? wxNullColour : kBadBg);
                r.text->Refresh();
            }
        }
    }

    // ASCII-only, Windows-safe file name from the (UTF-8) material name. The
    // display name keeps its accents: it lives in the file's `name` value.
    std::string SafeFileName(const wxString& name)
    {
        wxString out;
        for (wxUniChar c : name)
        {
            const bool ascii = c.IsAscii() && c >= 0x20 && c != 0x7F;
            const bool reserved = ascii && wxString("<>:\"/\\|?*").Find(c) != wxNOT_FOUND;
            out += (ascii && !reserved) ? c : wxUniChar('_');
        }
        out.Trim(true).Trim(false);
        while (!out.empty() && (out.Last() == '.' || out.Last() == ' ')) out.RemoveLast();
        if (out.length() > 100) out.Truncate(100);
        if (out.empty()) out = "Material";
        // Reserved device names can't be file names on Windows.
        static const char* kDevices[] = { "CON", "PRN", "AUX", "NUL",
            "COM1", "COM2", "COM3", "COM4", "COM5", "COM6", "COM7", "COM8", "COM9",
            "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9" };
        for (const char* d : kDevices)
            if (out.CmpNoCase(d) == 0) { out = "_" + out; break; }
        return std::string(out.ToAscii());
    }
} // namespace

// ---------------------------------------------------------------------------
MaterialEditorDialog::MaterialEditorDialog(wxWindow* parent, MaterialKind kind)
    : wxDialog(parent, wxID_ANY,
               kind == MaterialKind::Injection ? "Add to Injection Material Library"
                                               : "Add to Mould Material Library",
               wxDefaultPosition, wxSize(700, 760),
               wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
    , m_kind(kind)
{
    BuildUi();
    SetMinSize(wxSize(600, 480));
    CentreOnParent();
    WindowEffects::ApplyRoundedCorners(this);
    OnAnyEdit();   // initial markers (everything blank)
    if (m_nameCtrl) m_nameCtrl->SetFocus();
}

void MaterialEditorDialog::BuildUi()
{
    const bool inj = (m_kind == MaterialKind::Injection);
    auto* main = new wxBoxSizer(wxVERTICAL);

    auto* intro = new wxStaticText(this, wxID_ANY, inj
        ? wxString::FromUTF8("Enter the values from the material's datasheet, in the units shown. Leave "
                             "anything you don't have blank \xe2\x80\x94 Mould3r derives what it can. Only the "
                             "name is required.")
        : wxString::FromUTF8("Enter the values from the mould material's datasheet, in the units shown. "
                             "Leave anything you don't have blank. Only the name is required."));
    intro->Wrap(kWrapW + 60);
    main->Add(intro, 0, wxALL, 12);

    // ---- Scrolled body --------------------------------------------------------
    m_scroll = new wxScrolledWindow(this, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                                    wxVSCROLL | wxBORDER_THEME);
    m_scroll->SetScrollRate(0, 12);
    auto* body = new wxBoxSizer(wxVERTICAL);

    const auto& sections = inj ? MaterialFile::InjectionSections() : MaterialFile::MouldSections();

    auto build = [&](wxWindow* parent, wxSizer* into, const MaterialSectionSpec& s, bool comment)
    {
        if (inj) BuildSection(parent, into, s, MaterialFile::InjectionFields(), true, comment, m_rows);
        else     BuildSection(parent, into, s, MaterialFile::MouldFields(), false, comment, m_rows);
    };

    // Identity + datasheet sections.
    bool datasheetHeader = false;
    for (const MaterialSectionSpec& s : sections)
    {
        if (s.group == MaterialFieldGroup::Additional) continue;
        if (s.group == MaterialFieldGroup::Datasheet && !datasheetHeader)
        {
            auto* h = new wxStaticText(m_scroll, wxID_ANY, "Datasheet values");
            h->SetFont(h->GetFont().Bold());
            body->Add(h, 0, wxLEFT | wxRIGHT | wxTOP, 10);
            datasheetHeader = true;
        }
        build(m_scroll, body, s, false);
    }

    // Additional values, collapsed by default.
    auto* pane = new wxCollapsiblePane(m_scroll, wxID_ANY,
        wxString::FromUTF8("Additional values \xe2\x80\x94 measured / study data (optional)"),
        wxDefaultPosition, wxDefaultSize, wxCP_DEFAULT_STYLE | wxCP_NO_TLW_RESIZE);
    {
        wxWindow* pw = pane->GetPane();
        auto* ps = new wxBoxSizer(wxVERTICAL);
        auto* note = new wxStaticText(pw, wxID_ANY,
            "Each value here overrides the estimate Mould3r would otherwise derive from the datasheet values.");
        note->SetForegroundColour(wxSystemSettings::GetColour(wxSYS_COLOUR_GRAYTEXT));
        note->Wrap(kWrapW);
        ps->Add(note, 0, wxLEFT | wxRIGHT | wxTOP, 8);
        for (const MaterialSectionSpec& s : sections)
            if (s.group == MaterialFieldGroup::Additional)
                build(pw, ps, s, true);
        pw->SetSizer(ps);
    }
    pane->Bind(wxEVT_COLLAPSIBLEPANE_CHANGED, [this](wxCollapsiblePaneEvent&)
    {
        m_scroll->FitInside();
        m_scroll->Layout();
    });
    body->Add(pane, 0, wxEXPAND | wxALL, 6);
    body->AddSpacer(8);
    m_scroll->SetSizer(body);
    m_scroll->FitInside();
    main->Add(m_scroll, 1, wxEXPAND | wxLEFT | wxRIGHT, 12);

    // ---- Legend: what a marker means + how many there are -----------------------------
    {
        auto* legend = new wxBoxSizer(wxHORIZONTAL);
        legend->Add(new MissingMarker(this, /*decorative=*/true), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
        m_legendCount = new wxStaticText(this, wxID_ANY, "");
        m_legendCount->SetFont(m_legendCount->GetFont().Bold());
        legend->Add(m_legendCount, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
        auto* txt = new wxStaticText(this, wxID_ANY,
            wxString::FromUTF8("generic values will be used \xe2\x80\x94 click an asterisk for details."));
        txt->SetForegroundColour(wxSystemSettings::GetColour(wxSYS_COLOUR_GRAYTEXT));
        legend->Add(txt, 1, wxALIGN_CENTER_VERTICAL);
        main->Add(legend, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 12);

        m_badNumbers = new wxStaticText(this, wxID_ANY, "");
        m_badNumbers->SetForegroundColour(wxColour(0xC0, 0x30, 0x30));
        main->Add(m_badNumbers, 0, wxLEFT | wxRIGHT | wxTOP | wxRESERVE_SPACE_EVEN_IF_HIDDEN, 12);
        m_badNumbers->Hide();
    }

    // ---- Buttons --------------------------------------------------------------------
    auto* buttons = CreateStdDialogButtonSizer(wxOK | wxCANCEL);
    if (wxWindow* ok = FindWindow(wxID_OK)) ok->SetLabel("Add to Library");
    main->Add(buttons, 0, wxEXPAND | wxALL, 12);
    Bind(wxEVT_BUTTON, &MaterialEditorDialog::OnOk, this, wxID_OK);

    // Live revalidation on every edit; marker clicks.
    for (size_t i = 0; i < m_rows.size(); ++i)
    {
        Row& r = m_rows[i];
        if (r.text) r.text->Bind(wxEVT_TEXT, [this](wxCommandEvent&) { OnAnyEdit(); });
        if (r.choice) r.choice->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) { OnAnyEdit(); });
        if (r.noneCheck)
            r.noneCheck->Bind(wxEVT_CHECKBOX, [this, i](wxCommandEvent&)
            {
                Row& row = m_rows[i];
                row.text->Enable(!row.noneCheck->GetValue());
                OnAnyEdit();
            });
        if (r.star) r.star->Bind(wxEVT_BUTTON, [this, i](wxCommandEvent&) { OnMarkerClicked(i); });
        if (r.id == "material.name") m_nameCtrl = r.text;
    }

    SetSizer(main);
    Layout();
}

void MaterialEditorDialog::Collect(InjectionMaterialData& inj, MouldMaterialData& mould,
                                   std::vector<wxString>& bad)
{
    if (m_kind == MaterialKind::Injection) CollectRows(m_rows, MaterialFile::InjectionFields(), inj, bad);
    else                                   CollectRows(m_rows, MaterialFile::MouldFields(), mould, bad);
}

// Revalidate the numbers, re-run the resolver on what's been typed, and put a
// marker on every field whose absence forces a fallback.
void MaterialEditorDialog::OnAnyEdit()
{
    InjectionMaterialData inj;
    MouldMaterialData mould;
    std::vector<wxString> bad;
    Collect(inj, mould, bad);

    const std::vector<MaterialNote> notes = (m_kind == MaterialKind::Injection)
        ? ResolveInjectionMaterial(inj).notes : ResolveMouldMaterial(mould).notes;

    std::map<std::string, std::vector<MaterialNote>> byField;
    for (const MaterialNote& n : notes)
        if (n.level == MaterialNote::Level::Fallback)
            for (const std::string& f : n.fields) byField[f].push_back(n);

    int starred = 0;
    bool changed = false;
    for (Row& r : m_rows)
    {
        if (!r.star) continue;
        auto it = byField.find(r.id);
        const bool show = (it != byField.end());
        r.starNotes = show ? it->second : std::vector<MaterialNote>{};
        if (show) ++starred;
        if (r.star->IsShown() != show) { r.star->Show(show); changed = true; }
    }

    if (m_legendCount)
        m_legendCount->SetLabel(starred == 0 ? wxString("Nothing missing:")
                                             : wxString::Format("%d missing:", starred));
    if (m_badNumbers)
    {
        wxString t;
        if (!bad.empty())
        {
            t = "Not a number (use '.' as the decimal point): ";
            for (size_t i = 0; i < bad.size(); ++i) t << (i ? ", " : "") << bad[i];
        }
        m_badNumbers->SetLabel(t);
        if (m_badNumbers->IsShown() != !bad.empty()) { m_badNumbers->Show(!bad.empty()); changed = true; }
    }
    if (changed) Layout();
}

// A marker: which tests the missing value affects, and what's used instead.
void MaterialEditorDialog::OnMarkerClicked(size_t rowIndex)
{
    if (rowIndex >= m_rows.size()) return;
    const Row& r = m_rows[rowIndex];
    if (r.starNotes.empty() || !r.star) return;

    unsigned uses = 0;
    for (const MaterialNote& n : r.starNotes) uses |= n.uses;

    wxString msg = "Affects:\n";
    for (const std::string& d : MaterialUseDescriptions(uses))
        msg << wxString::FromUTF8("  \xe2\x80\xa2 ") << wxString::FromUTF8(d.c_str()) << "\n";
    msg << "\nUntil it's given:\n";
    for (const MaterialNote& n : r.starNotes)
        msg << wxString::FromUTF8("  \xe2\x80\xa2 ") << wxString::FromUTF8(n.quantity.c_str()) << ": "
            << wxString::FromUTF8(n.message.c_str()) << "\n";
    msg.Trim(true);

    auto* popup = new MissingInfoPopup(this, "Missing: " + r.label, msg);   // frees itself on dismissal
    // Just below the star, left edge aligned to it.
    popup->Position(r.star->ClientToScreen(wxPoint(0, 0)), wxSize(0, r.star->GetSize().y + 2));
    popup->Popup();
}

void MaterialEditorDialog::OnOk(wxCommandEvent&)
{
    InjectionMaterialData inj;
    MouldMaterialData mould;
    std::vector<wxString> bad;
    Collect(inj, mould, bad);
    const bool isInj = (m_kind == MaterialKind::Injection);

    if (!bad.empty())
    {
        wxString msg = "These values aren't numbers:\n\n";
        for (const wxString& b : bad) msg << wxString::FromUTF8("\xe2\x80\xa2 ") << b << "\n";
        msg << "\nUse '.' as the decimal point (for example 0.905), or clear the field.";
        wxMessageBox(msg, GetTitle(), wxOK | wxICON_WARNING, this);
        return;
    }

    const wxString name = wxString::FromUTF8((isInj ? inj.name : mould.name).c_str());
    if (name.empty())
    {
        wxMessageBox("Give the material a name: it's what the material dropdowns show.",
                     GetTitle(), wxOK | wxICON_WARNING, this);
        if (m_nameCtrl) m_nameCtrl->SetFocus();
        return;
    }

    std::string err;
    if (!MaterialFile::EnsureLibraryFolders(err))
    {
        wxMessageBox(wxString(err), GetTitle(), wxOK | wxICON_ERROR, this);
        return;
    }

    const std::string file = SafeFileName(name) + kMaterialFileExtension;
    const fs::path path = fs::path(MaterialFile::LibraryFolder(m_kind)) / file;
    std::error_code ec;
    if (fs::exists(path, ec))
    {
        wxMessageDialog ask(this,
            wxString::Format("The %s library already has a file named \"%s\".\n\nReplace it with this material?",
                             isInj ? "injection material" : "mould material", wxString(file)),
            GetTitle(), wxYES_NO | wxNO_DEFAULT | wxICON_QUESTION);
        ask.SetYesNoLabels("Replace", "Cancel");
        if (ask.ShowModal() != wxID_YES) return;
    }

    const std::string p = path.string();
    const bool ok = isInj ? MaterialFile::Save(p, inj, err) : MaterialFile::Save(p, mould, err);
    if (!ok)
    {
        wxMessageBox(wxString(err), GetTitle(), wxOK | wxICON_ERROR, this);
        return;
    }
    m_savedPath = p;
    EndModal(wxID_OK);
}
