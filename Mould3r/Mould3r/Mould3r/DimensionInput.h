#pragma once

#include <functional>
#include <string>

#ifndef DIMINPUT_NO_WX
class wxTextCtrl;
class wxStaticText;
class wxWindow;
#endif

// =============================================================================
// DimensionInput — typed dimension values for the feature-card fields
// (MainFrame's sidebar and the Fixture Editor).
//
// What a field accepts:
//
//     0.5    .5    12,5             decimals (either '.' or ',' as the point,
//                                   whatever the system locale)
//     1/2    3/8                    fractions
//     1 1/2    1-1/2    1½          mixed numbers (the hyphen form only as a
//                                   whole entry, so "1-1/2" is 1.5 but
//                                   "3 - 1/2" is subtraction)
//     3/8 + 1/16    2*0.75    (10-2)/4
//                                   arithmetic: + - * / and brackets
//     1/2"    1/2 in    12mm    2.5 cm
//                                   a unit on the end converts into the
//                                   field's current unit (lengths only)
//     15°    15 deg                 degrees on an angle field
//
// What it shows: when you press Enter or leave the field, anything that isn't
// already a plain number is replaced by its decimal value ("1/2" -> "0.5"),
// so what you see is always what's used. While the text can't be read the
// field is tinted red, and reads return the last value that could, so a
// half-typed "1/" never snaps a feature to some default.
//
// The evaluator is plain C++ (no wx) so it can be unit-tested; the wx helpers
// are compiled unless DIMINPUT_NO_WX is defined.
// =============================================================================
namespace DimInput
{
    enum class Unit { None, MM, Inch, Degree, Percent };

    // Evaluate `text` (UTF-8) for a field currently showing `fieldUnit`.
    // On failure returns false and, if `error` is given, a short reason.
    bool Evaluate(const std::string& text, Unit fieldUnit, double& out,
                  std::string* error = nullptr);

    // True when `text` is already just a number ("12", "-0.5", "3.0") —
    // such entries are left exactly as typed rather than reformatted.
    bool IsPlainNumber(const std::string& text);

    // Decimal text for `v` in `unit`: trailing zeros trimmed, at least one
    // decimal place kept ("0.5", "12.7", "13.0"). Inches keep up to 6
    // places so every 1/64 is exact; mm and degrees up to 4.
    std::string Format(double v, Unit unit);

#ifndef DIMINPUT_NO_WX
    // The unit a field's label shows: "mm", "in", the degree sign, "%";
    // anything else is Unit::None.
    Unit UnitFromLabel(const wxStaticText* label);

    // Make `field` a dimension field. Its unit is read live from `unitLabel`
    // (so a metric / imperial switch that relabels it is picked up), or is
    // `fixed` when no label is given.
    void Attach(wxTextCtrl* field, wxStaticText* unitLabel);
    void Attach(wxTextCtrl* field, Unit fixed);
    // ...or from any live source, e.g. an mm / in dropdown beside the field.
    void Attach(wxTextCtrl* field, std::function<Unit()> unitOf);

    // The field's value in its current unit. For an attached field whose text
    // can't be read right now, the last value that could be (still true).
    // False only when nothing valid has ever been entered. Works on any
    // wxTextCtrl; an unattached one is simply evaluated with Unit::None.
    bool Read(const wxTextCtrl* field, double& out);

    // The field's CURRENT text only, in its unit: false while it can't be
    // read (no last-value fallback). For callers that must tell "mid-typing"
    // from a real value, like the live Move / Rotate / Scale panel.
    bool ReadExact(const wxTextCtrl* field, double& out);

    // Re-check the field after the program rewrote it with ChangeValue()
    // (which sends no text event): clears a stale red tint, updates the
    // last good value.
    void Revalidate(wxTextCtrl* field);

    // Metric / imperial switch: rewrite every attached field under top-level
    // window `root` whose unit is currently `from` (MM or Inch) into `to`.
    // Call BEFORE relabelling the unit labels.
    void ConvertLengths(wxWindow* root, Unit from, Unit to);

    // Enter in an attached field commits it (decimal shown) and takes the
    // focus off it. The focus goes to the window registered here for the
    // field's top-level window (the main window and the Fixture Editor pass
    // their 3D view), else to the nearest enclosing panel. Fields that use
    // Enter themselves (wxTE_PROCESS_ENTER: the tool cards' OK) and fields in
    // dialogs (the default button) keep it -- they are committed first.
    void SetEnterFocusTarget(wxWindow* topLevel, wxWindow* target);
#endif
}
