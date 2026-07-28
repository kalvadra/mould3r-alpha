// AboutDialog.h
#pragma once
#include <wx/wx.h>

// ---------------------------------------------------------------------------
// "About Mould3r" dialog.
//
// The user-facing home for version identity: app mark, product name, version,
// build stamp and copyright, plus a "Copy Details" action that puts a support-
// friendly block of text (version, build, OS, wxWidgets build) on the
// clipboard — so a bug report can carry the exact build without the user
// having to hunt through Explorer's file properties.
//
// This is also where the update flow surfaces: the "Check for Updates..."
// button slot is marked in the .cpp and gets wired in U3, alongside the
// Help menu item.
//
// Styled to match the rest of the app (Style:: palette, Segoe UI, rounded
// buttons) rather than using the system dialog look, but keeps the standard
// caption bar so Esc and the system close button behave as expected.
// ---------------------------------------------------------------------------
class AboutDialog : public wxDialog
{
public:
    explicit AboutDialog(wxWindow* parent);

private:
    void OnCopyDetails(wxCommandEvent&);

    // The block of text "Copy Details" places on the clipboard.
    wxString BuildDetailsText() const;
};
