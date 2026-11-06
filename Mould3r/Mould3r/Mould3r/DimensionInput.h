#pragma once

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

    // The field's value in its current unit. For an attached field whose text
    // can't be read right now, the last value that could be (still true).
    // False only when nothing valid has ever been entered. Works on any
    // wxTextCtrl; an unattached one is simply evaluated with Unit::None.
    bool Read(const wxTextCtrl* field, double& out);

    // Metric / imperial switch: rewrite every attached field under top-level
    // window `root` whose unit is currently `from` (MM or Inch) into `to`.
    // Call BEFORE relabelling the unit labels.
    void ConvertLengths(wxWindow* root, Unit from, Unit to);
#endif
}
