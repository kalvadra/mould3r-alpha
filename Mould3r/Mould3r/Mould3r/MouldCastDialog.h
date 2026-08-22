#pragma once
#include <wx/wx.h>
#include <wx/choice.h>
#include <wx/checkbox.h>
#include <string>

// ---------------------------------------------------------------------------
// MouldCastDialog — modal prompt for generating the "cast" bodies that hold
// the sand / silicone around a finished mould: a base that leaves a cavity in
// the shape of the shot, and walls that box the pour in.
//
// Reached from the Preview perspective's "Generate Mould Casts" button, which
// only appears once a mould has been generated. This dialog is UI scaffolding
// only for now — GetValues() reports the user's settings back to the caller,
// and the actual geometry generation (walls + a shot-shaped cavity in the
// base) is a later step.
//
// Both the base and the walls share the same three settings the user asked
// for: an enable toggle, a shape "type" (the only options for now are
// "Flange" for the base and "Clover" for the walls), and a thickness with a
// mm / in unit selector. They're captured symmetrically in MouldCastPart so
// the two sections read as siblings and can grow more types later.
// ---------------------------------------------------------------------------

// Settings for one cast part (either the base or the walls).
struct MouldCastPart
{
    bool        enabled = false;   // "Enable Generation" checkbox
    std::string type;              // selected type ("Flange" / "Clover" / ...)
    double      thickness = 0.0;   // thickness value, in `inches` ? inches : mm
    bool        inches = false;    // true when the thickness unit is "in"

    // Extra distance field (per part). For the BASE this is the "Extra Flange
    // Distance" — how far the base extends past the mould perimeter, added to
    // every side. For the WALLS this is the "Extra Wall Distance" — the amount
    // (on top of the wall thickness) each Clover wall extends past the perimeter
    // at its overlapping end. `extraDistanceInches` selects its unit.
    double      extraDistance = 0.0;
    bool        extraDistanceInches = false;

    // Wall tongue-and-groove joint (walls only). The tongue is added to the
    // clover-overhang face and the matching groove is cut into the neighbouring
    // wall's Flange 1, so adjacent walls key together. Width runs along the
    // mating face; thickness is how far the tongue stands proud; the groove is
    // enlarged by `grooveTolerance` for fit clearance.
    double      tongueWidth = 0.0;      bool tongueWidthInches = false;
    double      tongueThickness = 0.0;  bool tongueThicknessInches = false;
    double      grooveTolerance = 0.0;  bool grooveToleranceInches = false;

    // Thickness normalised to millimetres, regardless of the chosen unit, so
    // downstream geometry code has a single canonical value to build from.
    double ThicknessMm() const
    {
        return inches ? thickness * 25.4 : thickness;
    }

    // Extra distance normalised to millimetres.
    double ExtraDistanceMm() const
    {
        return extraDistanceInches ? extraDistance * 25.4 : extraDistance;
    }

    // Tongue / groove values normalised to millimetres.
    double TongueWidthMm() const
    {
        return tongueWidthInches ? tongueWidth * 25.4 : tongueWidth;
    }
    double TongueThicknessMm() const
    {
        return tongueThicknessInches ? tongueThickness * 25.4 : tongueThickness;
    }
    double GrooveToleranceMm() const
    {
        return grooveToleranceInches ? grooveTolerance * 25.4 : grooveTolerance;
    }
};

struct MouldCastValues
{
    MouldCastPart base;    // holds the sand / silicone; cavity = shot shape
    MouldCastPart walls;   // box around the pour
};

class MouldCastDialog : public wxDialog
{
public:
    explicit MouldCastDialog(wxWindow* parent,
        const MouldCastValues& initial = {});

    // Read the settings for both parts back. Numeric parse failures resolve to
    // 0.0, matching the forgiving stance the other dialogs take.
    MouldCastValues GetValues() const;

private:
    // Controls for one part section, bundled so the two sections are built by
    // a single shared helper.
    struct PartControls
    {
        wxCheckBox* enable = nullptr;
        wxChoice*   type = nullptr;
        wxTextCtrl* thickness = nullptr;
        wxChoice*   unit = nullptr;
        wxTextCtrl* extra = nullptr;      // Extra distance value (flange / wall)
        wxChoice*   extraUnit = nullptr;  // mm / in for the extra distance

        // Wall tongue-and-groove joint fields (walls only).
        wxTextCtrl* tongueW = nullptr;    wxChoice* tongueWUnit = nullptr;
        wxTextCtrl* tongueT = nullptr;    wxChoice* tongueTUnit = nullptr;
        wxTextCtrl* grooveTol = nullptr;  wxChoice* grooveTolUnit = nullptr;
    };

    // Build a titled section ("Base" / "Walls") with the enable / type /
    // thickness rows. `typeChoices` seeds the type dropdown; `defThickness` is
    // the pre-filled thickness. When `withExtra` is true an extra-distance row
    // (labelled `extraLabel`) is added — "Extra Flange" for the base, "Extra
    // Wall" for the walls. When `withJoint` is true the tongue-and-groove rows
    // are added (walls only). Returns the created controls.
    PartControls BuildPartSection(wxWindow* parent, wxSizer* into,
        const wxString& title, const wxArrayString& typeChoices,
        const MouldCastPart& initial, double defThickness,
        bool withExtra = false, double defExtra = 0.0,
        const wxString& extraLabel = "Extra Flange:",
        bool withJoint = false);

    // Enable / disable a section's type + thickness rows to match its enable
    // checkbox, so a disabled part reads as inert.
    void SyncEnabled(const PartControls& pc);

    // Read one section's controls back into a MouldCastPart.
    MouldCastPart ReadPart(const PartControls& pc) const;

    PartControls m_base;
    PartControls m_walls;
};
