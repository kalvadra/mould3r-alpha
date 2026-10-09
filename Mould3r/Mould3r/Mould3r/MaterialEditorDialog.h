#pragma once
// ===========================================================================
// MaterialEditorDialog — "Add to Injection / Mould Material Library".
//
// One row per field of the material format, generated from the field table
// (MaterialFile::InjectionFields / MouldFields), so the dialog can never drift
// from the file format:
//   Identity            name (required), manufacturer, grade, family, ...
//   Datasheet values    one box per section (Physical, Thermal, ...)
//   Additional values   collapsed by default (Cross-WLF, Tait pvT, ...)
// Every value is optional except the name.
//
// MISSING-VALUE MARKERS: after every edit the simulation resolver
// (MaterialResolve.h) runs on what has been typed. Each fallback it would have
// to use names the input fields whose absence caused it (MaterialNote::fields);
// those rows get an underlined amber asterisk. Clicking one shows which tests
// the missing value affects and what would be used instead.
//
// OK writes <name>.material into the kind's library folder (asking before it
// replaces an existing file) and ends the dialog; SavedPath() is then the file
// written. The caller rescans the library.
// ===========================================================================

#include <wx/wx.h>
#include <wx/collpane.h>
#include <wx/scrolwin.h>

#include <string>
#include <vector>

#include "MaterialFile.h"
#include "MaterialResolve.h"

class MaterialEditorDialog : public wxDialog
{
public:
    MaterialEditorDialog(wxWindow* parent, MaterialKind kind);

    // The file written by a successful OK (native narrow path).
    const std::string& SavedPath() const { return m_savedPath; }

    // One generated input row.
    struct Row
    {
        int         fieldIndex = -1;    // into the kind's field table
        std::string id;                 // "section.key", matches MaterialNote::fields
        wxString    label;              // for messages
        wxTextCtrl* text = nullptr;     // numeric / free-text fields
        wxChoice*   choice = nullptr;   // the injection "structure" field
        wxCheckBox* noneCheck = nullptr;// "No filler" beside the filler field
        wxWindow*   star = nullptr;     // missing-value asterisk (hidden when nothing is missing)
        std::vector<MaterialNote> starNotes;   // the fallbacks behind the asterisk
    };

private:
    void BuildUi();
    void OnAnyEdit();                  // revalidate + re-place the missing-value markers
    void OnMarkerClicked(size_t rowIndex);
    void OnOk(wxCommandEvent&);

    // Read the rows into `inj` / `mould` (by kind). Invalid numbers are
    // listed in `bad` (labels) and their fields tinted; they read as unset.
    void Collect(InjectionMaterialData& inj, MouldMaterialData& mould, std::vector<wxString>& bad);

    MaterialKind      m_kind;
    std::vector<Row>  m_rows;
    wxScrolledWindow* m_scroll = nullptr;
    wxTextCtrl*       m_nameCtrl = nullptr;
    wxStaticText*     m_legendCount = nullptr;   // "N missing values"
    wxStaticText*     m_badNumbers = nullptr;    // red line naming unparseable fields
    std::string       m_savedPath;
};
