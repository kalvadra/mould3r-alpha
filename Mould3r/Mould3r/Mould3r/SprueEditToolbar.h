#pragma once
#include <wx/wx.h>
#include <functional>
#include "MainFrame.h"   // for SprueEditTool (declared next to TransformMode)

// =============================================================================
// SprueEditToolbar - floating overlay toolbar for the Edit Sprue environment.
//
// A trimmed sibling of VentEditToolbar: one self-painted child window over the
// GL canvas, no nested native controls, every control a hit-tested owner-drawn
// cell (keeps it to one HWND over the wxGLCanvas). Same rounded-window shaping
// and top-centre pinning as VentEditToolbar; the two never show at once (their
// TransformModes are mutually exclusive), so they can share the canvas's
// top-centre slot.
//
// It carries exactly two cells for now:
//   [ Move ]  [ Select Injection Point ]
//
//   Move    - drag the endpoint of a RADIAL sprue on the parting plane. Enabled
//             only when a radial sprue exists; disabled (greyed) for an axial
//             sprue, since moving an axial endpoint would compromise demolding.
//   Select  - re-pick which injection point the sprue feeds from (the behaviour
//   Inj.Pt.   that used to be the whole "Edit Sprue" action). Enabled when the
//             fixture offers a choice of injection points.
//
// Like VentEditToolbar it is dumb about state: the owner (MainFrame) pushes
// display state in via Configure() and receives intent out through m_onTool.
// Visibility is the owner's job.
// =============================================================================
class SprueEditToolbar : public wxPanel
{
public:
    explicit SprueEditToolbar(wxWindow* parent);

    // Push display state from the owner.
    //   hasRadialSprue - a placed sprue whose injection point is radial (gates
    //                    the Move cell).
    //   canSelectIp    - the fixture offers injection points to pick from
    //                    (gates the Select Injection Point cell).
    //   tool           - the active sub-tool (drives the pressed/active look).
    // Triggers a repaint.
    void Configure(bool hasRadialSprue, bool canSelectIp, SprueEditTool tool);

    // Intent callback (set by the owner): fired when a usable cell is clicked.
    void SetOnTool(std::function<void(SprueEditTool)> cb) { m_onTool = std::move(cb); }

private:
    enum Cell { CELL_NONE, CELL_MOVE, CELL_SELECT_IP };

    void OnPaint(wxPaintEvent&);
    void OnLeftDown(wxMouseEvent&);
    void OnMotion(wxMouseEvent&);
    void OnLeave(wxMouseEvent&);
    void OnSize(wxSizeEvent&);

    void LayoutCells();
    void ApplyRoundedShape();          // clip the HWND to the drawn card outline
    Cell HitTest(const wxPoint& p) const;
    bool CellEnabled(Cell c) const;

    // Geometry (recomputed in LayoutCells)
    wxRect m_rMove, m_rSelectIp;

    // Display state
    bool          m_hasRadialSprue = false;
    bool          m_canSelectIp    = false;
    SprueEditTool m_tool           = SprueEditTool::Move;

    Cell m_hover = CELL_NONE;

    std::function<void(SprueEditTool)> m_onTool;
};
