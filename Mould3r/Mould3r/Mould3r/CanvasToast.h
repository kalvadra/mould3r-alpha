#pragma once
#include <wx/wx.h>

// ---------------------------------------------------------------------------
// CanvasToast — a small self-painted hint pill pinned to the bottom-centre of
// the 3D viewport.
//
// Why: a few modes are signalled only by the cursor (PlaceInsert swaps to a
// hand, meaning "click a body to parent to"), which is easy to miss. This says
// it in words, without stealing focus or needing to be dismissed.
//
// Construction rules match VentEditToolbar: a SIBLING of the canvas raised
// above it (a true child of a wxGLCanvas can be overdrawn by SwapBuffers), one
// HWND, no nested native controls, everything painted here. Shared by the
// main window (MainFrame) and the Fixture Editor; each owning canvas pins it
// (RepositionCanvasToast).
//
// NOT time-limited: it appears when a mode raises it and goes away when that
// mode ends. A guidance hint that fades out mid-task is worse than one that
// stays put, and the mode is the natural lifetime. Swapping in an auto-fade is
// a wxTimer away if that turns out to be the wrong call.
//
// The pill is opaque and does swallow clicks in its own footprint. It sits low
// and is only as wide as its text, so it rarely covers anything you need to
// click; if it ever does, WS_EX_TRANSPARENT on the HWND makes it click-through.
// ---------------------------------------------------------------------------
class CanvasToast : public wxPanel
{
public:
    explicit CanvasToast(wxWindow* parent);

    // Set the message, resize to fit it, show and raise. Re-showing the message
    // already on screen is a no-op, so it is safe to call every mode change.
    // The CALLER re-pins us afterwards (GLCanvas::RepositionCanvasToast) — the
    // size just changed and the canvas owns the centring.
    void ShowMessage(const wxString& msg);
    void HideToast();

private:
    void OnPaint(wxPaintEvent&);
    void ApplyPillShape();

    wxString m_msg;
};
