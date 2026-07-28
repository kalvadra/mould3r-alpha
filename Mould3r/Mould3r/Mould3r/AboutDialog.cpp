// AboutDialog.cpp
#include "AboutDialog.h"

#include <wx/clipbrd.h>
#include <wx/filename.h>
#include <wx/file.h>
#include <wx/hyperlink.h>
#include <wx/stdpaths.h>

#include "Version.h"
#include "style.h"
#include "RoundedButton.h"
#include "UpdateCheckDialog.h"   // "Check for Updates..." action
#include "WindowEffects.h"   // DWM corner rounding for this dialog frame

namespace
{
    // Shared font for the dialog's buttons — 9pt Segoe UI semibold, matching
    // the ribbon buttons in MainFrame and the actions in StartupDialog.
    wxFont DialogBtnFont()
    {
        return wxFont(9, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
            wxFONTWEIGHT_SEMIBOLD, false, "Segoe UI");
    }

    wxFont UIFont(int pt, wxFontWeight weight = wxFONTWEIGHT_NORMAL)
    {
        return wxFont(pt, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
            weight, false, "Segoe UI");
    }

    // Loads an SVG from a path relative to the executable directory.
    //
    // NOTE: this duplicates MainFrame.cpp's LoadSvgBundle, which is a file-
    // static there. Third caller (after MainFrame and this) is the point at
    // which it's worth lifting into a shared SvgAssets.h/.cpp — not yet.
    wxBitmapBundle LoadSvgBundle(const wxString& svgPath, const wxSize& size)
    {
        if (svgPath.IsEmpty())
            return wxBitmapBundle();

        wxFileName fn(svgPath);
        if (fn.IsRelative())
        {
            wxFileName exeDir(wxStandardPaths::Get().GetExecutablePath());
            fn.MakeAbsolute(exeDir.GetPath());
        }

        wxFile file(fn.GetFullPath());
        if (!file.IsOpened())
            return wxBitmapBundle();

        wxString svg;
        file.ReadAll(&svg);

        const wxScopedCharBuffer utf8 = svg.utf8_str();
        return wxBitmapBundle::FromSVG(utf8.data(), size);
    }

    constexpr int kLogoPx = 64;
    const wxString kAppIconSvg = "res/logos/logo-icon-nobackground.svg";
}

AboutDialog::AboutDialog(wxWindow* parent)
    : wxDialog(parent, wxID_ANY, "About Mould3r",
        wxDefaultPosition, wxDefaultSize,
        wxDEFAULT_DIALOG_STYLE)
{
    SetBackgroundColour(Style::AppBg);
    WindowEffects::ApplyRoundedCorners(this);

    auto* main = new wxBoxSizer(wxVERTICAL);

    // ---- App mark ----------------------------------------------------------
    // Missing/unreadable asset degrades to no logo rather than an empty box —
    // the dialog still reads correctly without it.
    {
        wxBitmapBundle logo = LoadSvgBundle(kAppIconSvg, wxSize(kLogoPx, kLogoPx));
        if (logo.IsOk())
        {
            auto* mark = new wxStaticBitmap(this, wxID_ANY, logo.GetBitmapFor(this));
            main->Add(mark, 0, wxALIGN_CENTER_HORIZONTAL | wxTOP, 24);
        }
    }

    // ---- Product name ------------------------------------------------------
    {
        auto* name = new wxStaticText(this, wxID_ANY, Mould3r::Version::ProductName);
        name->SetFont(UIFont(18, wxFONTWEIGHT_SEMIBOLD));
        name->SetForegroundColour(Style::TextPrimary);
        main->Add(name, 0, wxALIGN_CENTER_HORIZONTAL | wxTOP, 12);
    }

    // ---- Version + build stamp --------------------------------------------
    // The build stamp is what distinguishes two local builds carrying the
    // same version number, which is exactly the ambiguity that bites during
    // "are you actually running the fixed build?" support conversations.
    {
        auto* ver = new wxStaticText(this, wxID_ANY,
            wxString::Format("Version %s", Mould3r::Version::String));
        ver->SetFont(UIFont(10));
        ver->SetForegroundColour(Style::TextSubtle);
        main->Add(ver, 0, wxALIGN_CENTER_HORIZONTAL | wxTOP, 6);

        auto* build = new wxStaticText(this, wxID_ANY,
            wxString::Format("Build %s", Mould3r::Version::BuildDate));
        build->SetFont(UIFont(8));
        build->SetForegroundColour(Style::TextMuted);
        main->Add(build, 0, wxALIGN_CENTER_HORIZONTAL | wxTOP, 3);
    }

    // ---- Divider -----------------------------------------------------------
    // A 1-px wxPanel rather than a wxStaticLine: the native static-line
    // control ignores SetBackgroundColour on MSW, so it would paint in the
    // system 3D-edge colours and read as a light seam on the dark panel.
    {
        auto* line = new wxPanel(this, wxID_ANY,
            wxDefaultPosition, wxSize(-1, 1));
        line->SetBackgroundColour(Style::Divider);
        main->Add(line, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 24);
    }

    // ---- Copyright + website ----------------------------------------------
    {
        auto* copyright = new wxStaticText(this, wxID_ANY, Mould3r::Version::Copyright);
        copyright->SetFont(UIFont(8));
        copyright->SetForegroundColour(Style::TextMuted);
        main->Add(copyright, 0, wxALIGN_CENTER_HORIZONTAL | wxTOP, 16);

        auto* link = new wxHyperlinkCtrl(this, wxID_ANY,
            Mould3r::Version::Website, Mould3r::Version::Website);
        link->SetFont(UIFont(8));
        // wxHyperlinkCtrl defaults to blue-on-white; recolour all three states
        // so it reads correctly against the dark background.
        link->SetNormalColour(Style::TextSubtle);
        link->SetHoverColour(Style::TextPrimary);
        link->SetVisitedColour(Style::TextSubtle);
        link->SetBackgroundColour(Style::AppBg);
        main->Add(link, 0, wxALIGN_CENTER_HORIZONTAL | wxTOP, 4);
    }

    // ---- Actions -----------------------------------------------------------
    {
        auto* row = new wxBoxSizer(wxHORIZONTAL);

        // Opens the same UpdateCheckDialog as Help -> Check for Updates...
        // rather than reporting inline — one surface for the update flow
        // means the two entry points can't drift apart.
        auto* updateBtn = new RoundedButton(this, wxID_ANY, "Check for Updates...",
            wxDefaultPosition, wxSize(150, 30));
        updateBtn->SetBackgroundColour(Style::BtnSecondary);
        updateBtn->SetForegroundColour(*wxWHITE);
        updateBtn->SetFont(DialogBtnFont());
        updateBtn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&)
            {
                UpdateCheckDialog dlg(this);
                dlg.ShowModal();
            });
        row->Add(updateBtn, 0, wxRIGHT, 8);

        auto* copyBtn = new RoundedButton(this, wxID_ANY, "Copy Details",
            wxDefaultPosition, wxSize(110, 30));
        copyBtn->SetBackgroundColour(Style::BtnSecondary);
        copyBtn->SetForegroundColour(*wxWHITE);
        copyBtn->SetFont(DialogBtnFont());
        copyBtn->Bind(wxEVT_BUTTON, &AboutDialog::OnCopyDetails, this);

        // Dismissive action: background blended into AppBg so it reads as a
        // text label rather than competing with Copy Details.
        auto* closeBtn = new RoundedButton(this, wxID_CANCEL, "Close",
            wxDefaultPosition, wxSize(90, 30));
        closeBtn->SetBackgroundColour(Style::AppBg);
        closeBtn->SetForegroundColour(Style::TextPrimary);
        closeBtn->SetFont(DialogBtnFont());
        closeBtn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { EndModal(wxID_CANCEL); });

        row->Add(copyBtn, 0, wxRIGHT, 8);
        row->Add(closeBtn, 0);
        main->Add(row, 0, wxALIGN_CENTER_HORIZONTAL | wxTOP | wxBOTTOM, 20);
    }

    main->SetMinSize(wxSize(360, -1));
    SetSizerAndFit(main);
    CentreOnParent();

    // Esc closes. RoundedButton isn't a real wxButton, so SetAffirmativeId /
    // SetEscapeId can't find one to map to — bind the key directly.
    Bind(wxEVT_CHAR_HOOK, [this](wxKeyEvent& e)
        {
            if (e.GetKeyCode() == WXK_ESCAPE) EndModal(wxID_CANCEL);
            else                              e.Skip();
        });
}

// ---------------------------------------------------------------------------
// BuildDetailsText — the support block. Kept plain-text and one-fact-per-line
// so it survives being pasted into an email, a forum post or a chat window.
// ---------------------------------------------------------------------------
wxString AboutDialog::BuildDetailsText() const
{
    wxString s;
    s << Mould3r::Version::ProductName << " " << Mould3r::Version::String << "\n"
        << "Build: " << Mould3r::Version::BuildDate
        << " " << Mould3r::Version::BuildTime << "\n"
        << "OS: " << wxGetOsDescription() << "\n"
        << "Toolkit: " << wxVERSION_STRING << "\n";
    return s;
}

void AboutDialog::OnCopyDetails(wxCommandEvent&)
{
    // wxClipboard must be opened and closed around every access; Flush() is
    // what keeps the text on the clipboard after Mould3r exits (without it,
    // Windows drops the data when the owning process dies).
    if (!wxTheClipboard->Open())
        return;

    wxTheClipboard->SetData(new wxTextDataObject(BuildDetailsText()));
    wxTheClipboard->Flush();
    wxTheClipboard->Close();
}
