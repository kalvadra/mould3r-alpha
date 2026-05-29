#pragma once

#include <wx/wx.h>
#include <wx/tglbtn.h>
#include <vector>

#include "FileImporter.h"   // FileImporter::MeshData

class GLCanvas;

// ===========================================================================
// PreviewFrame
//
// A standalone, non-modal top-level window that shows the post-cut mould
// halves on their own. It hosts a GLCanvas running in preview mode (grid +
// halves, full orbit/pan/dolly navigation, no editing) and a small toolbar
// with one show/hide toggle per half.
//
// A fresh PreviewFrame is created every time the mould is generated; the
// caller (MainFrame) destroys any previous instance first so the window always
// reflects the latest generation.
// ===========================================================================
class PreviewFrame : public wxFrame
{
public:
    // `halves` are the world-space post-cut meshes (one per fixture, in
    // fixture order) captured by GLCanvas::GenerateMould. They are copied into
    // the preview's own GL context on the first idle pass, once the canvas is
    // realized.
    PreviewFrame(wxWindow* parent,
        const std::vector<FileImporter::MeshData>& halves);

private:
    // Build one show/hide toggle per half. Called synchronously from the
    // constructor (count + labels are known up front); the GL upload that
    // gives those toggles something to control is deferred to LoadHalves.
    void BuildToggleBar(int halfCount);

    // Upload the captured half meshes into the canvas's context and enable the
    // toggles. Run via CallAfter so the canvas window is fully realized (its
    // GL context valid) before any GL call is issued.
    void LoadHalves();

    // Apply the app palette to a toggle based on its current value, so a
    // "shown" half reads as active and a "hidden" one as inactive.
    void StyleToggle(wxToggleButton* btn, bool visible) const;

    GLCanvas* m_canvas = nullptr;
    wxPanel* m_toolbar = nullptr;
    std::vector<wxToggleButton*> m_halfToggles;

    // Pending meshes held between construction and the deferred LoadHalves.
    std::vector<FileImporter::MeshData> m_pendingHalves;
};
