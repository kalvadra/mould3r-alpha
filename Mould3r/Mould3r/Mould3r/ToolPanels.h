#pragma once
#include <wx/wx.h>
#include <functional>

#include "PatternDialog.h"   // PatternValues

class RoundedButton;

// =============================================================================
// ToolPanels — the Place and Pattern tools' floating cards, styled to match
// the Move / Rotate / Scale TransformPanel: a CardBg card with a rounded
// outline pinned top-right of the viewport (below the nav cube), a title and
// selection status, the value rows, and Cancel / OK.
//
// Like TransformPanel each is a SIBLING of the GL canvas raised above it; the
// canvas pins it (GLCanvas::AddPinnedPanel). Each is dumb about the scene and
// reports intent through callbacks; Enter in a field is OK, Esc is Cancel.
//
//   PlacePanel   — absolute X / Z for the selection's centre (the average of
//                  the selected bodies' origins). OnChange fires as you type
//                  (whenever both fields read), so the owner previews live.
//   PatternPanel — Circular or Grid pattern options; the owner applies them on
//                  OK (the copies can't be previewed, they don't exist yet).
//
// Lengths display in the owner's unit (mm / in) and are handed out in mm.
// =============================================================================

// ---- Shared card chrome --------------------------------------------------------
class ToolCard : public wxPanel
{
public:
    void SetOnOk(std::function<void()> cb) { m_onOk = std::move(cb); }
    void SetOnCancel(std::function<void()> cb) { m_onCancel = std::move(cb); }

    // Status text ("1 selected" / "Select an object"); OK is disabled with
    // nothing selected.
    void SetSelectionCount(int selectionCount);

protected:
    ToolCard(wxWindow* parent, const wxString& title, int width);

    // Where subclasses add their rows (between the header and the buttons).
    wxBoxSizer* Body() { return m_body; }

    // Label / field / unit row in the TransformPanel style. Length / angle
    // rows are attached to DimInput (fractions, expressions, unit suffixes);
    // Enter in the field is OK. `unitOut` receives the unit label.
    wxTextCtrl* AddValueRow(wxSizer* into, const wxString& label, const wxString& unit,
                            bool dimension, wxStaticText** unitOut = nullptr);
    wxCheckBox* AddCheck(wxSizer* into, const wxString& label);
    wxChoice*   AddChoiceRow(wxSizer* into, const wxString& label, const wxArrayString& items);

    // Re-layout after rows were shown / hidden, resize to fit and re-pin.
    void Refit();

    void FireOk()     { if (m_onOk) m_onOk(); }
    void FireCancel() { if (m_onCancel) m_onCancel(); }

    int m_selectionCount = 0;

private:
    void OnPaint(wxPaintEvent&);
    void OnSize(wxSizeEvent&);
    void ApplyRoundedShape();

    int            m_width;
    wxBoxSizer*    m_main = nullptr;
    wxBoxSizer*    m_body = nullptr;
    wxStaticText*  m_title = nullptr;
    wxStaticText*  m_status = nullptr;
    RoundedButton* m_btnOk = nullptr;
    RoundedButton* m_btnCancel = nullptr;
    std::function<void()> m_onOk, m_onCancel;
};

// ---- Place ---------------------------------------------------------------------
class PlacePanel : public ToolCard
{
public:
    explicit PlacePanel(wxWindow* parent);

    // Show the selection's current centre (mm) in the given unit.
    void Begin(bool imperial, int selectionCount, double xMM, double zMM);

    // Both fields read (mm). False while one is mid-typing.
    bool GetTarget(double& xMM, double& zMM) const;

    void SetOnChange(std::function<void(double xMM, double zMM)> cb) { m_onChange = std::move(cb); }

private:
    bool          m_imperial = false;
    wxTextCtrl*   m_fx = nullptr;
    wxTextCtrl*   m_fz = nullptr;
    wxStaticText* m_ux = nullptr;
    wxStaticText* m_uz = nullptr;
    std::function<void(double, double)> m_onChange;
};

// ---- Pattern -------------------------------------------------------------------
class PatternPanel : public ToolCard
{
public:
    explicit PatternPanel(wxWindow* parent);

    // Open with the last-used settings, lengths shown in the given unit.
    void Begin(bool imperial, int selectionCount);

    // The settings, lengths in mm. Counts below 1 fall back to the defaults.
    PatternValues GetValues() const;

private:
    void SyncVisibility();

    bool          m_imperial = false;
    wxChoice*     m_type = nullptr;

    wxBoxSizer*   m_circular = nullptr;
    wxTextCtrl*   m_number = nullptr;
    wxCheckBox*   m_overrideRadius = nullptr;
    wxSizer*      m_radiusRow = nullptr;
    wxTextCtrl*   m_radius = nullptr;
    wxStaticText* m_radiusUnit = nullptr;
    wxCheckBox*   m_rotateCopies = nullptr;

    wxBoxSizer*   m_grid = nullptr;
    wxTextCtrl*   m_numH = nullptr;
    wxTextCtrl*   m_numV = nullptr;
    wxCheckBox*   m_mirrorH = nullptr;
    wxCheckBox*   m_mirrorV = nullptr;
    wxCheckBox*   m_overrideLW = nullptr;
    wxSizer*      m_lwRows = nullptr;
    wxTextCtrl*   m_length = nullptr;
    wxTextCtrl*   m_width = nullptr;
    wxStaticText* m_lengthUnit = nullptr;
    wxStaticText* m_widthUnit = nullptr;
};
