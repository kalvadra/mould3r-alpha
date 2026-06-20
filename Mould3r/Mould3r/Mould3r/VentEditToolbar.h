#pragma once
#include <wx/wx.h>
#include <functional>
#include "MainFrame.h"   // for PathEditTool (declared next to TransformMode)

// =============================================================================
// VentEditToolbar — floating overlay toolbar for authoring complex vent paths
// (Part 5).
//
// A single, fully self-painted child window placed over the GL canvas. It owns
// NO nested native controls (no wxButton / wxCheckBox children): every control
// is a hit-tested, owner-drawn cell. That keeps it to one HWND over the GL
// surface, sidestepping the compositing quirks that nested controls over a
// wxGLCanvas can hit.
//
// It is intentionally dumb about state — it never reads the canvas directly.
// The owner (MainFrame) pushes display state in via Configure() and receives
// intent out through the three callbacks. Visibility is also the owner's job
// (Show/Hide); the toolbar only paints and reports clicks.
//
// Layout (fixed size, top-centred over the viewport by the canvas):
//   row 1 : "VENT PATH"                              "Complex · 3 nodes"
//   row 2 : [ Move ][ Add Node ][ Remove Node ]  [x Smooth]   [ Make Simple ]
//
// When the edited vent is Simple, the node tools and Smooth read disabled and
// the toggle offers "Make Complex"; Add Node stays live because clicking it
// auto-converts the vent and drops the first waypoint.
// =============================================================================
class VentEditToolbar : public wxPanel
{
public:
    explicit VentEditToolbar(wxWindow* parent);

    // Push display state from the owner. hasSelection says whether a vent is
    // picked; complex/smooth/nodeCount describe it (meaningful only when
    // selected); tool is the active sub-tool. Triggers a repaint.
    void Configure(bool hasSelection, bool complex, bool smooth, int nodeCount,
        PathEditTool tool);

    // Intent callbacks (set by the owner).
    void SetOnTool(std::function<void(PathEditTool)> cb) { m_onTool = std::move(cb); }
    void SetOnSmooth(std::function<void(bool)> cb) { m_onSmooth = std::move(cb); }
    // wantComplex == true  -> user asked to convert Simple -> Complex
    // wantComplex == false -> user asked to convert Complex -> Simple
    void SetOnToggleComplex(std::function<void(bool)> cb) { m_onToggleComplex = std::move(cb); }

private:
    enum Cell { CELL_NONE, CELL_MOVE, CELL_ADD, CELL_REMOVE, CELL_SMOOTH, CELL_TOGGLE };

    void OnPaint(wxPaintEvent&);
    void OnLeftDown(wxMouseEvent&);
    void OnMotion(wxMouseEvent&);
    void OnLeave(wxMouseEvent&);

    void LayoutCells();
    Cell HitTest(const wxPoint& p) const;
    bool CellEnabled(Cell c) const;

    // Geometry (recomputed in LayoutCells)
    wxRect m_rMove, m_rAdd, m_rRemove, m_rSmooth, m_rToggle;

    // Display state
    bool         m_hasSelection = false;
    bool         m_complex = false;
    bool         m_smooth = false;
    int          m_nodeCount = 0;
    PathEditTool m_tool = PathEditTool::Move;

    Cell m_hover = CELL_NONE;

    std::function<void(PathEditTool)> m_onTool;
    std::function<void(bool)>         m_onSmooth;
    std::function<void(bool)>         m_onToggleComplex;
};
