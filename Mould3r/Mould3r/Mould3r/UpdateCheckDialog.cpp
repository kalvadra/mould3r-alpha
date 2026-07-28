// UpdateCheckDialog.cpp
#include "UpdateCheckDialog.h"

#include <wx/utils.h>       // wxLaunchDefaultBrowser

#include "Version.h"
#include "style.h"
#include "RoundedButton.h"
#include "WindowEffects.h"  // DWM corner rounding for this dialog frame

namespace
{
    // 9pt Segoe UI semibold — the shared dialog-button font (matches
    // AboutDialog / StartupDialog / the ribbon).
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
}

UpdateCheckDialog::UpdateCheckDialog(wxWindow* parent)
    : wxDialog(parent, wxID_ANY, "Check for Updates",
        wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE)
{
    SetBackgroundColour(Style::AppBg);
    WindowEffects::ApplyRoundedCorners(this);

    auto* main = new wxBoxSizer(wxVERTICAL);

    m_headline = new wxStaticText(this, wxID_ANY, wxEmptyString);
    m_headline->SetFont(UIFont(12, wxFONTWEIGHT_SEMIBOLD));
    m_headline->SetForegroundColour(Style::TextPrimary);
    main->Add(m_headline, 0, wxALIGN_CENTER_HORIZONTAL | wxTOP | wxLEFT | wxRIGHT, 24);

    m_detail = new wxStaticText(this, wxID_ANY, wxEmptyString,
        wxDefaultPosition, wxDefaultSize, wxALIGN_CENTRE_HORIZONTAL);
    m_detail->SetFont(UIFont(9));
    m_detail->SetForegroundColour(Style::TextSubtle);
    main->Add(m_detail, 0, wxALIGN_CENTER_HORIZONTAL | wxTOP | wxLEFT | wxRIGHT, 8);

    m_buttonRow = new wxBoxSizer(wxHORIZONTAL);
    main->Add(m_buttonRow, 0, wxALIGN_CENTER_HORIZONTAL | wxTOP | wxBOTTOM, 20);

    main->SetMinSize(wxSize(380, -1));
    SetSizerAndFit(main);
    CentreOnParent();

    // Esc closes (and cancels, via m_checker's destructor when the dialog
    // dies). RoundedButton isn't a native wxButton so SetEscapeId can't
    // find one — bind the key directly, as AboutDialog does.
    Bind(wxEVT_CHAR_HOOK, [this](wxKeyEvent& e)
        {
            if (e.GetKeyCode() == WXK_ESCAPE) EndModal(wxID_CANCEL);
            else                              e.Skip();
        });

    StartCheck();
}

// ---------------------------------------------------------------------------
// StartCheck — enter the "Checking..." state and kick off the request.
// ---------------------------------------------------------------------------
void UpdateCheckDialog::StartCheck()
{
    SetBodyText("Checking for updates\u2026",
        wxString::Format("You are running Mould3r %s.", Mould3r::Version::String));
    SetButtons(/*showDownload=*/false, /*showRetry=*/false, wxEmptyString);

    m_checker = std::make_unique<UpdateChecker>();

    const bool started = m_checker->Start(
        [this](const UpdateChecker::Result& r) { ShowResult(r); });

    if (!started)
    {
        // Backend unavailable — a build problem, not a network problem, so
        // say so plainly rather than blaming the connection.
        UpdateChecker::Result r;
        r.outcome = UpdateChecker::Outcome::Error;
        r.message = "This build of Mould3r cannot check for updates\n"
            "(web request support is missing).";
        ShowResult(r);
    }
}

// ---------------------------------------------------------------------------
// ShowResult — swap the content in place for the terminal state.
// ---------------------------------------------------------------------------
void UpdateCheckDialog::ShowResult(const UpdateChecker::Result& r)
{
    using Outcome = UpdateChecker::Outcome;

    switch (r.outcome)
    {
    case Outcome::UpToDate:
        SetBodyText("You're up to date",
            wxString::Format("Mould3r %s is the latest version.",
                Mould3r::Version::String));
        SetButtons(false, false, wxEmptyString);
        break;

    case Outcome::UpdateAvailable:
    {
        SetBodyText(
            wxString::Format("Mould3r %s is available", r.latestVersion),
            wxString::Format("You are running %s.", Mould3r::Version::String));

        // Where "View Download Page" goes, in order of preference: the
        // changelog (best context for "should I update?"), then the raw
        // installer URL, then the website. Tier 1 never downloads in-app,
        // so all three roads lead to the browser.
        wxString target = r.notesUrl;
        if (target.empty()) target = r.downloadUrl;
        if (target.empty()) target = Mould3r::Version::Website;

        SetButtons(/*showDownload=*/true, /*showRetry=*/false, target);
        break;
    }

    case Outcome::Error:
        SetBodyText("Could not check for updates", r.message);
        SetButtons(/*showDownload=*/false, /*showRetry=*/true, wxEmptyString);
        break;
    }
}

// ---------------------------------------------------------------------------
// Content helpers
// ---------------------------------------------------------------------------
void UpdateCheckDialog::SetBodyText(const wxString& headline, const wxString& detail)
{
    m_headline->SetLabel(headline);
    m_detail->SetLabel(detail);
}

void UpdateCheckDialog::SetButtons(bool showDownload, bool showRetry,
    const wxString& downloadUrl)
{
    // Rebuild the row from scratch each state change — three buttons at
    // most, so simplicity beats show/hide bookkeeping.
    m_buttonRow->Clear(/*delete_windows=*/true);

    if (showDownload)
    {
        auto* dl = new RoundedButton(this, wxID_ANY, "View Download Page",
            wxDefaultPosition, wxSize(150, 30));
        dl->SetBackgroundColour(Style::BtnGenerate);   // the "go" action
        dl->SetForegroundColour(*wxWHITE);
        dl->SetFont(DialogBtnFont());
        dl->Bind(wxEVT_BUTTON, [downloadUrl](wxCommandEvent&)
            {
                wxLaunchDefaultBrowser(downloadUrl);
            });
        m_buttonRow->Add(dl, 0, wxRIGHT, 8);
    }

    if (showRetry)
    {
        auto* retry = new RoundedButton(this, wxID_ANY, "Try Again",
            wxDefaultPosition, wxSize(110, 30));
        retry->SetBackgroundColour(Style::BtnSecondary);
        retry->SetForegroundColour(*wxWHITE);
        retry->SetFont(DialogBtnFont());
        retry->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { StartCheck(); });
        m_buttonRow->Add(retry, 0, wxRIGHT, 8);
    }

    auto* close = new RoundedButton(this, wxID_CANCEL, "Close",
        wxDefaultPosition, wxSize(90, 30));
    close->SetBackgroundColour(Style::AppBg);          // dismissive: text-style
    close->SetForegroundColour(Style::TextPrimary);
    close->SetFont(DialogBtnFont());
    close->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { EndModal(wxID_CANCEL); });
    m_buttonRow->Add(close, 0);

    // Content changed size — refit and stay centred on the parent so the
    // state swap doesn't leave the dialog visually off-balance.
    GetSizer()->SetSizeHints(this);
    Layout();
    CentreOnParent();
}
