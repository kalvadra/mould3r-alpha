#pragma once
#include <wx/wx.h>
#include <functional>
#include "MainFrame.h"   // for PathEditTool (declared next to TransformMode)

// =============================================================================
// VentEditToolbar - floating overlay toolbar for authoring complex feature
// paths (vents, runners, gate sub-runners).
//
// A single, fully self-painted child window placed over the GL canvas. It owns
// NO nested native controls (no wxButton / wxCheckBox children): every control
// is a hit-tested, owner-drawn cell. That keeps it to one HWND over the GL
// surface, sidestepping the compositing quirks that nested controls over a
// wxGLCanvas can hit.
//
// It is intentionally dumb about state - it never reads the canvas directly.
// The owner (MainFrame) pushes display state in via Configure() and receives
// intent out through the callbacks. Visibility is also the owner's job
// (Show/Hide); the toolbar only paints and reports clicks.
//
// Layout (fixed size, top-centred over the viewport by the canvas):
//   row 1 : "EDIT RUNNER PATH"                        "4 nodes"
//   row 2 : [ Move ][ Add Node ][ Remove Node ][ Place... ]  [x Smooth]
//
// Place... opens Precision Place for the selected node (the one the Move tool
// last grabbed); it stays disabled until such a node is picked.
//
// Remove Node is live only when the path actually has a removable node: the
// two endpoints are protected everywhere (origin/feed point and terminus), so
// a path needs at least three nodes before anything can come out.
//
// The window is shaped to a rounded rectangle (ApplyRoundedShape) so the
// corners are genuinely cut away and the viewport shows through them, rather
// than the card colour squaring off outside the drawn border.
//
// NOTE: the Simple <-> Complex toggle cell was removed - a Simple path becomes
// Complex by dropping a node on it, so the button was redundant. The
// SetOnToggleComplex hook is retained (unfired) so the owner's wiring still
// compiles and the conversion route stays available for future use.
// =============================================================================
class VentEditToolbar : public wxPanel
{
public:
    explicit VentEditToolbar(wxWindow* parent);

    // Push display state from the owner. hasSelection says whether a feature is
    // picked; complex/smooth/nodeCount describe it (meaningful only when
    // selected); tool is the active sub-tool. Triggers a repaint.
    // canPlaceNode says a Precision Place-eligible node is selected, which is
    // the only thing that enables the Place... cell.
    // nodeCount is the owner's raw count: 0 for a Simple path (which the bar
    // reports as its implicit 2 nodes), else the Complex node total.
    void Configure(bool hasSelection, bool complex, bool smooth, int nodeCount,
        PathEditTool tool, bool canPlaceNode);

    // Retitle the bar for the feature being edited (this same overlay is reused
    // for vents, runners and gate sub-runners). `title` is the header (e.g.
    // "EDIT RUNNER PATH"); `emptyStatus` is the right-hand hint shown when
    // nothing is selected.
    // Set-only (no repaint) - the following Configure() call repaints.
    void SetLabels(const wxString& title, const wxString& emptyStatus);

    // Intent callbacks (set by the owner).
    void SetOnTool(std::function<void(PathEditTool)> cb) { m_onTool = std::move(cb); }
    void SetOnSmooth(std::function<void(bool)> cb) { m_onSmooth = std::move(cb); }
    // RETAINED BUT NEVER FIRED - the toggle cell is gone. Kept so the owner's
    // existing wiring compiles and so a conversion control can be re-added
    // without re-plumbing.
    //   wantComplex == true  -> user asked to convert Simple -> Complex
    //   wantComplex == false -> user asked to convert Complex -> Simple
    void SetOnToggleComplex(std::function<void(bool)> cb) { m_onToggleComplex = std::move(cb); }
    // Precision Place for the selected node ("Place..." cell).
    void SetOnPlaceNode(std::function<void()> cb) { m_onPlaceNode = std::move(cb); }

private:
    enum Cell { CELL_NONE, CELL_MOVE, CELL_ADD, CELL_REMOVE, CELL_PLACE,
                CELL_SMOOTH };

    void OnPaint(wxPaintEvent&);
    void OnLeftDown(wxMouseEvent&);
    void OnMotion(wxMouseEvent&);
    void OnLeave(wxMouseEvent&);
    void OnSize(wxSizeEvent&);

    void LayoutCells();
    void ApplyRoundedShape();          // clip the HWND to the drawn card outline
    Cell HitTest(const wxPoint& p) const;
    bool CellEnabled(Cell c) const;

    // Node count as displayed: a Simple path is implicitly two nodes.
    int  DisplayNodeCount() const { return m_complex ? m_nodeCount : 2; }

    // Geometry (recomputed in LayoutCells)
    wxRect m_rMove, m_rAdd, m_rRemove, m_rPlace, m_rSmooth;

    // Display state
    wxString     m_title = wxT("EDIT VENT PATH");
    wxString     m_emptyStatus = wxT("Select a vent path");

    bool         m_hasSelection = false;
    bool         m_complex = false;
    bool         m_smooth = false;
    int          m_nodeCount = 0;
    bool         m_canPlaceNode = false;   // an eligible node is selected
    PathEditTool m_tool = PathEditTool::Move;

    Cell m_hover = CELL_NONE;

    std::function<void(PathEditTool)> m_onTool;
    std::function<void(bool)>         m_onSmooth;
    std::function<void(bool)>         m_onToggleComplex;   // retained, unfired
    std::function<void()>             m_onPlaceNode;
};
