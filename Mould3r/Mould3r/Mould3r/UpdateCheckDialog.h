// UpdateCheckDialog.h
#pragma once
#include <wx/wx.h>
#include <memory>

#include "UpdateChecker.h"

// ---------------------------------------------------------------------------
// "Check for Updates" dialog — the one user-facing surface for the Tier 1
// update flow. Opens in a "Checking..." state, runs an UpdateChecker, and
// swaps its content in place for one of three results:
//
//   * Up to date      — version confirmation, Close.
//   * Update available— old -> new version line, "View Download Page"
//                       (opens the browser; Tier 1 never downloads), Close.
//   * Error           — honest failure message, "Try Again", Close.
//
// Both entry points (Help -> Check for Updates... and the button in the
// About dialog) open this same dialog, so the behaviour can't drift
// between them.
//
// Closing the dialog mid-check cancels the request (the UpdateChecker is
// owned by the dialog and cancels on destruction), so no callback can
// arrive after the UI it would touch is gone.
// ---------------------------------------------------------------------------
class UpdateCheckDialog : public wxDialog
{
public:
    explicit UpdateCheckDialog(wxWindow* parent);

private:
    void StartCheck();
    void ShowResult(const UpdateChecker::Result& r);

    // Rebuilds the dialog's content area for the current state and refits.
    void SetBodyText(const wxString& headline, const wxString& detail);
    void SetButtons(bool showDownload, bool showRetry,
        const wxString& downloadUrl);

    std::unique_ptr<UpdateChecker> m_checker;

    wxStaticText* m_headline = nullptr;
    wxStaticText* m_detail = nullptr;
    wxBoxSizer* m_buttonRow = nullptr;
};
