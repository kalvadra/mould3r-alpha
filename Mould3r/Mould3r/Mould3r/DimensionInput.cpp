// Same as GLCanvas.cpp / NavCube.cpp: standard headers come before any wx
// header here, so wx's own CRT opt-out would arrive too late for the
// project's SDL checks.
#define _CRT_SECURE_NO_WARNINGS

#include "DimensionInput.h"

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#ifndef DIMINPUT_NO_WX
#include <wx/colour.h>
#include <wx/dialog.h>
#include <wx/event.h>
#include <wx/panel.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/toplevel.h>
#include <wx/window.h>
#endif

namespace DimInput
{
namespace
{
    constexpr double kMmPerInch = 25.4;

    // ---- Text clean-up --------------------------------------------------------
    std::string Trim(const std::string& s)
    {
        size_t a = 0, b = s.size();
        while (a < b && std::isspace((unsigned char)s[a])) ++a;
        while (b > a && std::isspace((unsigned char)s[b - 1])) --b;
        return s.substr(a, b - a);
    }

    void ReplaceAll(std::string& s, const std::string& from, const std::string& to)
    {
        for (size_t p = s.find(from); p != std::string::npos; p = s.find(from, p + to.size()))
            s.replace(p, from.size(), to);
    }

    std::string Lower(std::string s)
    {
        for (char& c : s) c = (char)std::tolower((unsigned char)c);
        return s;
    }

    bool EndsWith(const std::string& s, const std::string& tail)
    {
        return s.size() >= tail.size() && s.compare(s.size() - tail.size(), tail.size(), tail) == 0;
    }

    // Unicode vulgar fractions -> " a/b" (so "1½" reads as the mixed 1 1/2),
    // the typographic minus / multiply / divide signs and the inch prime
    // marks -> ASCII.
    void Normalise(std::string& s)
    {
        const std::pair<const char*, const char*> map[] = {
            { "\xC2\xBD", " 1/2" }, { "\xC2\xBC", " 1/4" }, { "\xC2\xBE", " 3/4" },
            { "\xE2\x85\x9B", " 1/8" }, { "\xE2\x85\x9C", " 3/8" },
            { "\xE2\x85\x9D", " 5/8" }, { "\xE2\x85\x9E", " 7/8" },
            { "\xE2\x85\x93", " 1/3" }, { "\xE2\x85\x94", " 2/3" },
            { "\xE2\x88\x92", "-" },     // minus sign
            { "\xC3\x97", "*" },          // multiplication sign
            { "\xC3\xB7", "/" },          // division sign
            { "\xE2\x80\xB3", "\"" },     // double prime
            { "\xE2\x80\x9D", "\"" },     // right double quote (smart quotes)
            { "\xC2\xA0", " " },          // no-break space
        };
        for (const auto& m : map) ReplaceAll(s, m.first, m.second);
    }

    // ---- Unit suffix ----------------------------------------------------------
    enum class Suffix { None, MM, CM, Inch, Degree, Percent };

    // Peel a unit off the end of `s` (already trimmed).
    Suffix TakeSuffix(std::string& s)
    {
        const std::string low = Lower(s);
        struct S { const char* t; Suffix u; };
        // Longest first so "inches" isn't read as "in" + "ches".
        const S table[] = {
            { "inches", Suffix::Inch }, { "inch", Suffix::Inch }, { "in", Suffix::Inch },
            { "\"", Suffix::Inch }, { "''", Suffix::Inch },
            { "mm", Suffix::MM }, { "cm", Suffix::CM },
            { "degrees", Suffix::Degree }, { "deg", Suffix::Degree }, { "\xC2\xB0", Suffix::Degree },
            { "%", Suffix::Percent },
        };
        for (const S& e : table)
            if (EndsWith(low, e.t))
            {
                s = Trim(s.substr(0, s.size() - std::string(e.t).size()));
                return e.u;
            }
        return Suffix::None;
    }

    // ---- Tokens ---------------------------------------------------------------
    struct Tok { char kind; double v; };   // kind: 'n' number, or the operator / bracket char

    // Read an unsigned number at s[i]: digits with at most one '.' or ','.
    // Returns false if there's no digit. `integer` reports a bare integer.
    bool ReadNumber(const std::string& s, size_t& i, double& v, bool& integer)
    {
        const size_t start = i;
        std::string buf;
        bool point = false, digit = false;
        while (i < s.size())
        {
            const char c = s[i];
            if (std::isdigit((unsigned char)c)) { buf += c; digit = true; ++i; }
            else if ((c == '.' || c == ',') && !point) { buf += '.'; point = true; ++i; }
            else break;
        }
        if (!digit) { i = start; return false; }
        v = std::strtod(buf.c_str(), nullptr);   // buf is plain ASCII "123.45"
        integer = !point;
        return true;
    }

    // Tokenise. Mixed numbers become a single number token:
    //   * "a b/c" (whitespace between) wherever a number may start, unless the
    //     previous token was '/' ("6/2 1/2" has no sensible reading);
    //   * "a-b/c" only when that is the whole entry (handled by the caller).
    bool Tokenise(const std::string& s, std::vector<Tok>& out, std::string& err)
    {
        size_t i = 0;
        while (i < s.size())
        {
            const char c = s[i];
            if (std::isspace((unsigned char)c)) { ++i; continue; }
            if (c == '+' || c == '-' || c == '*' || c == '/' || c == '(' || c == ')')
            {
                out.push_back({ c, 0.0 });
                ++i;
                continue;
            }
            double v = 0.0;
            bool integer = false;
            if (ReadNumber(s, i, v, integer))
            {
                const bool afterDiv = !out.empty() && out.back().kind == '/';
                if (integer && !afterDiv)
                {
                    // Look for whitespace, integer, '/', integer.
                    size_t j = i;
                    while (j < s.size() && std::isspace((unsigned char)s[j])) ++j;
                    double n = 0.0, d = 0.0;
                    bool nInt = false, dInt = false;
                    size_t k = j;
                    if (j > i && ReadNumber(s, k, n, nInt) && nInt)
                    {
                        size_t m = k;
                        while (m < s.size() && std::isspace((unsigned char)s[m])) ++m;
                        if (m < s.size() && s[m] == '/')
                        {
                            ++m;
                            while (m < s.size() && std::isspace((unsigned char)s[m])) ++m;
                            if (ReadNumber(s, m, d, dInt) && dInt)
                            {
                                if (d == 0.0) { err = "Division by zero"; return false; }
                                out.push_back({ 'n', v + n / d });
                                i = m;
                                continue;
                            }
                        }
                    }
                }
                out.push_back({ 'n', v });
                continue;
            }
            err = std::string("Unexpected '") + c + "'";
            return false;
        }
        return true;
    }

    // ---- Recursive descent over the tokens ------------------------------------
    //   expr  := term (('+' | '-') term)*
    //   term  := unary (('*' | '/') unary)*
    //   unary := ('+' | '-') unary | atom
    //   atom  := number | '(' expr ')'
    struct Parser
    {
        const std::vector<Tok>& t;
        size_t p = 0;
        std::string err;

        bool Peek(char k) const { return p < t.size() && t[p].kind == k; }

        bool Expr(double& v)
        {
            if (!Term(v)) return false;
            while (Peek('+') || Peek('-'))
            {
                const char op = t[p++].kind;
                double r;
                if (!Term(r)) return false;
                v = (op == '+') ? v + r : v - r;
            }
            return true;
        }
        bool Term(double& v)
        {
            if (!Unary(v)) return false;
            while (Peek('*') || Peek('/'))
            {
                const char op = t[p++].kind;
                double r;
                if (!Unary(r)) return false;
                if (op == '/')
                {
                    if (r == 0.0) { err = "Division by zero"; return false; }
                    v /= r;
                }
                else
                    v *= r;
            }
            return true;
        }
        bool Unary(double& v)
        {
            if (Peek('-')) { ++p; if (!Unary(v)) return false; v = -v; return true; }
            if (Peek('+')) { ++p; return Unary(v); }
            return Atom(v);
        }
        bool Atom(double& v)
        {
            if (Peek('n')) { v = t[p++].v; return true; }
            if (Peek('('))
            {
                ++p;
                if (!Expr(v)) return false;
                if (!Peek(')')) { err = "Missing ')'"; return false; }
                ++p;
                return true;
            }
            err = (p < t.size()) ? std::string("Unexpected '") + t[p].kind + "'"
                                 : std::string("Incomplete entry");
            return false;
        }
    };

    // "a-b/c" as the whole entry: a mixed number, tape-measure style.
    bool HyphenMixed(const std::string& s, double& v)
    {
        size_t i = 0;
        bool neg = false;
        if (i < s.size() && s[i] == '-') { neg = true; ++i; }
        double a, b, c;
        bool ia, ib, ic;
        if (!ReadNumber(s, i, a, ia) || !ia) return false;
        if (i >= s.size() || s[i] != '-') return false;
        ++i;
        if (!ReadNumber(s, i, b, ib) || !ib) return false;
        if (i >= s.size() || s[i] != '/') return false;
        ++i;
        if (!ReadNumber(s, i, c, ic) || !ic || c == 0.0) return false;
        if (i != s.size()) return false;
        v = a + b / c;
        if (neg) v = -v;
        return true;
    }
}

// =============================================================================
// Evaluate / Format
// =============================================================================
bool Evaluate(const std::string& text, Unit fieldUnit, double& out, std::string* error)
{
    auto fail = [&](const std::string& why) { if (error) *error = why; return false; };

    std::string s = text;
    Normalise(s);
    s = Trim(s);
    if (s.empty()) return fail("Empty");

    const Suffix suf = TakeSuffix(s);
    if (s.empty()) return fail("No number");

    double v = 0.0;
    if (!HyphenMixed(s, v))
    {
        std::vector<Tok> toks;
        std::string err;
        if (!Tokenise(s, toks, err)) return fail(err);
        Parser ps{ toks };
        if (!ps.Expr(v)) return fail(ps.err);
        if (ps.p != toks.size())
            return fail(toks[ps.p].kind == 'n' ? std::string("Unexpected number")
                                               : "Unexpected '" + std::string(1, toks[ps.p].kind) + "'");
    }
    if (!std::isfinite(v)) return fail("Not a finite number");

    // Unit suffix -> the field's unit.
    switch (suf)
    {
    case Suffix::None:
        break;
    case Suffix::MM: case Suffix::CM: case Suffix::Inch:
    {
        const double mm = (suf == Suffix::MM) ? v : (suf == Suffix::CM) ? v * 10.0 : v * kMmPerInch;
        if (fieldUnit == Unit::MM)        v = mm;
        else if (fieldUnit == Unit::Inch) v = mm / kMmPerInch;
        else return fail("This field isn't a length");
        break;
    }
    case Suffix::Degree:
        if (fieldUnit != Unit::Degree && fieldUnit != Unit::None) return fail("This field isn't an angle");
        break;
    case Suffix::Percent:
        if (fieldUnit != Unit::Percent && fieldUnit != Unit::None) return fail("This field isn't a percentage");
        break;
    }
    out = v;
    return true;
}

bool IsPlainNumber(const std::string& text)
{
    const std::string s = Trim(text);
    size_t i = 0;
    if (i < s.size() && (s[i] == '-' || s[i] == '+')) ++i;
    double v;
    bool integer;
    if (!ReadNumber(s, i, v, integer)) return false;
    return i == s.size() && s.find(',') == std::string::npos;
}

std::string Format(double v, Unit unit)
{
    const int places = (unit == Unit::Inch) ? 6 : 4;
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.*f", places, v);
    std::string s = buf;
    // Trim trailing zeros, keeping one decimal place.
    const size_t dot = s.find('.');
    if (dot != std::string::npos)
    {
        size_t end = s.size();
        while (end > dot + 2 && s[end - 1] == '0') --end;
        s.resize(end);
    }
    if (s == "-0.0") s = "0.0";
    return s;
}

// =============================================================================
// wx glue
// =============================================================================
#ifndef DIMINPUT_NO_WX
namespace
{
    struct FieldInfo
    {
        wxStaticText* unitLabel = nullptr;   // live unit from a label, or...
        std::function<Unit()> unitOf;        // ...from any other source, or...
        Unit          fixed = Unit::None;    // ...this
        double        lastGood = 0.0;
        bool          hasGood = false;
        bool          tinted = false;
        wxColour      normalBg;
    };

    std::unordered_map<const wxTextCtrl*, FieldInfo>& Registry()
    {
        static std::unordered_map<const wxTextCtrl*, FieldInfo> r;
        return r;
    }

    // Error tint that suits the field: dark red on the app's dark inputs,
    // pale red on native light ones (so black text stays readable).
    wxColour ErrorBg(const wxColour& normal)
    {
        const bool light = normal.IsOk() &&
            (0.299 * normal.Red() + 0.587 * normal.Green() + 0.114 * normal.Blue()) > 140.0;
        return light ? wxColour(255, 206, 206) : wxColour(112, 42, 48);
    }

    std::string Utf8(const wxString& s)
    {
        const wxScopedCharBuffer b = s.utf8_str();
        return std::string(b.data(), b.length());
    }

    Unit FieldUnit(const FieldInfo& fi)
    {
        if (fi.unitLabel) return UnitFromLabel(fi.unitLabel);
        if (fi.unitOf)    return fi.unitOf();
        return fi.fixed;
    }

    // Re-read the text: remember it if valid, tint the field if not.
    void Validate(wxTextCtrl* f)
    {
        auto it = Registry().find(f);
        if (it == Registry().end()) return;
        FieldInfo& fi = it->second;
        double v = 0.0;
        std::string err;
        const wxString text = f->GetValue();
        const bool ok = Evaluate(Utf8(text), FieldUnit(fi), v, &err);
        if (ok) { fi.lastGood = v; fi.hasGood = true; }

        const bool bad = !ok && !text.Strip(wxString::both).IsEmpty();
        if (bad != fi.tinted)
        {
            fi.tinted = bad;
            f->SetBackgroundColour(bad ? ErrorBg(fi.normalBg) : fi.normalBg);
            f->Refresh();
        }
        if (bad)
            f->SetToolTip(wxString::FromUTF8(err) +
                "\nTry 0.5, 1/2, 1 1/2, 3/8 + 1/16 or 12mm.");
        else
            f->UnsetToolTip();
    }

    // Replace a valid non-plain entry with its decimal value.
    void Commit(wxTextCtrl* f)
    {
        auto it = Registry().find(f);
        if (it == Registry().end()) return;
        const FieldInfo& fi = it->second;
        const std::string text = Utf8(f->GetValue());
        double v = 0.0;
        if (IsPlainNumber(text) || !Evaluate(text, FieldUnit(fi), v)) return;
        // ChangeValue: no wxEVT_TEXT, so live handlers don't fire twice for
        // the same value (they already saw it through Read()).
        f->ChangeValue(wxString::FromUTF8(Format(v, FieldUnit(fi))));
        Validate(f);
    }

    std::unordered_map<const wxWindow*, wxWindow*>& FocusTargets()
    {
        static std::unordered_map<const wxWindow*, wxWindow*> t;
        return t;
    }

    // After Enter: move the keyboard focus off `f` so the field visibly lets go.
    void Defocus(wxTextCtrl* f)
    {
        wxWindow* top = wxGetTopLevelParent(f);
        auto it = FocusTargets().find(top);
        if (it != FocusTargets().end() && it->second && it->second->IsShownOnScreen())
        {
            it->second->SetFocus();
            return;
        }
        // No registered target: the nearest panel takes it itself (a plain
        // SetFocus would hand it straight back to its first child).
        for (wxWindow* w = f->GetParent(); w && !w->IsTopLevel(); w = w->GetParent())
            if (auto* panel = wxDynamicCast(w, wxPanel))
            {
                panel->SetFocusIgnoringChildren();
                return;
            }
        if (top) top->SetFocus();
    }

    void AttachImpl(wxTextCtrl* field, wxStaticText* label, Unit fixed,
                    std::function<Unit()> unitOf = {})
    {
        if (!field) return;
        FieldInfo fi;
        fi.unitLabel = label;
        fi.unitOf = std::move(unitOf);
        fi.fixed = fixed;
        fi.normalBg = field->GetBackgroundColour();
        Registry()[field] = fi;
        Validate(field);

        field->Bind(wxEVT_TEXT, [field](wxCommandEvent& e) { Validate(field); e.Skip(); });
        field->Bind(wxEVT_KILL_FOCUS, [field](wxFocusEvent& e) { Commit(field); e.Skip(); });
        // Enter. Caught as a CHAR_HOOK, which reaches the focused field before
        // anything else: a plain single-line field on Windows never sees
        // Enter as a KEY_DOWN -- tab navigation swallows it first.
        field->Bind(wxEVT_CHAR_HOOK, [field](wxKeyEvent& e)
        {
            if (e.GetKeyCode() != WXK_RETURN && e.GetKeyCode() != WXK_NUMPAD_ENTER)
            {
                e.Skip();
                return;
            }
            Commit(field);
            // Owners that use Enter (a card's OK, a dialog's default button)
            // get it, now that the value is committed.
            if (field->HasFlag(wxTE_PROCESS_ENTER) ||
                wxDynamicCast(wxGetTopLevelParent(field), wxDialog))
            {
                e.Skip();
                return;
            }
            Defocus(field);   // consumed: apply and let go of the field
        });
        // Belt and braces where Enter does arrive as a key (wxTE_PROCESS_ENTER
        // fields, other platforms): committing twice is harmless.
        field->Bind(wxEVT_KEY_DOWN, [field](wxKeyEvent& e)
        {
            if (e.GetKeyCode() == WXK_RETURN || e.GetKeyCode() == WXK_NUMPAD_ENTER)
                Commit(field);
            e.Skip();
        });
        field->Bind(wxEVT_DESTROY, [field](wxWindowDestroyEvent& e)
        {
            if (e.GetEventObject() == field) Registry().erase(field);
            e.Skip();
        });
    }
}

Unit UnitFromLabel(const wxStaticText* label)
{
    if (!label) return Unit::None;
    const wxString t = label->GetLabel().Strip(wxString::both).Lower();
    if (t == "mm") return Unit::MM;
    if (t == "in" || t == "\"") return Unit::Inch;
    if (t == wxString::FromUTF8("\xC2\xB0") || t == "deg") return Unit::Degree;
    if (t == "%") return Unit::Percent;
    return Unit::None;
}

void SetEnterFocusTarget(wxWindow* topLevel, wxWindow* target)
{
    if (!topLevel) return;
    FocusTargets()[topLevel] = target;
    // Forget it with the window (top-level windows can be recreated).
    topLevel->Bind(wxEVT_DESTROY, [topLevel](wxWindowDestroyEvent& e)
    {
        if (e.GetEventObject() == topLevel) FocusTargets().erase(topLevel);
        e.Skip();
    });
}

void Attach(wxTextCtrl* field, wxStaticText* unitLabel) { AttachImpl(field, unitLabel, Unit::None); }
void Attach(wxTextCtrl* field, Unit fixed) { AttachImpl(field, nullptr, fixed); }
void Attach(wxTextCtrl* field, std::function<Unit()> unitOf)
{
    AttachImpl(field, nullptr, Unit::None, std::move(unitOf));
}

bool ReadExact(const wxTextCtrl* field, double& out)
{
    if (!field) return false;
    auto it = Registry().find(field);
    const Unit u = (it == Registry().end()) ? Unit::None : FieldUnit(it->second);
    return Evaluate(Utf8(field->GetValue()), u, out);
}

void Revalidate(wxTextCtrl* field) { if (field) Validate(field); }

bool Read(const wxTextCtrl* field, double& out)
{
    if (!field) return false;
    auto it = Registry().find(field);
    if (it == Registry().end())
        return Evaluate(Utf8(field->GetValue()), Unit::None, out);

    const FieldInfo& fi = it->second;
    double v = 0.0;
    if (Evaluate(Utf8(field->GetValue()), FieldUnit(fi), v)) { out = v; return true; }
    if (fi.hasGood) { out = fi.lastGood; return true; }
    return false;
}

void ConvertLengths(wxWindow* root, Unit from, Unit to)
{
    if (from == to || (from != Unit::MM && from != Unit::Inch)) return;
    const double factor = (from == Unit::MM) ? 1.0 / kMmPerInch : kMmPerInch;
    for (auto& [ctrl, fi] : Registry())
    {
        auto* f = const_cast<wxTextCtrl*>(ctrl);
        if (wxGetTopLevelParent(f) != root || FieldUnit(fi) != from) continue;
        double v = 0.0;
        if (!Read(f, v)) continue;
        f->ChangeValue(wxString::FromUTF8(Format(v * factor, to)));
        fi.lastGood = v * factor;
        fi.hasGood = true;
        if (fi.tinted) { fi.tinted = false; f->SetBackgroundColour(fi.normalBg); f->UnsetToolTip(); f->Refresh(); }
    }
}
#endif
}
