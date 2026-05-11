#pragma once
#include <wx/wx.h>
#include <string>

// ---------------------------------------------------------------------------
// InjectionPointDialog — modal prompt for adding or editing a single
// injection point on a fixture. Captures the four fields the FixtureEditor
// surfaces in its sidebar list: a free-text label and an X / Y / Z position
// in millimetres relative to the fixture origin.
//
// Doubles as the "Edit" dialog: pass an `initial` value through the
// constructor to pre-populate every field. Title is configurable so the
// caller can present "Add..." vs "Edit..." accordingly.
//
// The InjectionPoint struct in FixtureFile.h also carries a `type` field
// (Radial / Axial) — that's intentionally absent here. The fixture editor's
// add/edit flow defaults new points to Radial, matching the file format's
// "anything other than 'axial' is radial" parser stance, and keeps the
// dialog narrow to the four fields the user requested. If the editor grows
// a type picker later, this dialog can grow a wxChoice next to the label.
// ---------------------------------------------------------------------------
struct InjectionPointValues
{
    std::string label;
    float       x = 0.0f;   // mm
    float       y = 0.0f;
    float       z = 0.0f;
};

class InjectionPointDialog : public wxDialog
{
public:
    explicit InjectionPointDialog(wxWindow* parent,
        const wxString& title = "Add Injection Point",
        const InjectionPointValues& initial = {});

    // Reads the four fields back. ToDouble parse failures resolve to 0.0
    // for the numeric fields, matching the forgiving stance taken
    // elsewhere in the editor (a stray typo in one field shouldn't block
    // the user from saving — the value just goes to its default).
    InjectionPointValues GetValues() const;

private:
    wxTextCtrl* m_ctrlLabel = nullptr;
    wxTextCtrl* m_ctrlX = nullptr;
    wxTextCtrl* m_ctrlY = nullptr;
    wxTextCtrl* m_ctrlZ = nullptr;

    float ParseField(wxTextCtrl* ctrl) const;
};
