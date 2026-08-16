#pragma once
#include <optional>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// InjectionPoint — a sprue/gate location defined relative to the fixture
// origin.  Two physical interpretations are supported:
//
//   Radial  — material injected radially inward (e.g. a side gate on the
//              parting line).
//   Axial   — material injected along the fixture axis (e.g. a top/bottom
//              sprue perpendicular to the parting plane).
// ---------------------------------------------------------------------------
enum class InjectionType { Radial, Axial };

struct InjectionPoint
{
    std::string   label;
    float         x = 0.0f;
    float         y = 0.0f;
    float         z = 0.0f;
    InjectionType type = InjectionType::Radial;

    // "Fixture Perimeter" injection point (see FixtureDefinition::
    // allowPerimeterInjection). When true this point isn't a fixed location
    // authored in the fixture — it was placed by the user anywhere on the
    // fixture perimeter and is always Radial (y = 0). The distinction matters
    // at edit time: a perimeter point's injection LOCATION is draggable along
    // the perimeter (Edit Sprue > Move), whereas a fixed point's location is
    // immutable and Move instead drags the sprue endpoint. Fixed points loaded
    // from a fixture always leave this false.
    bool          perimeter = false;

    // Derive the injection type from a Y coordinate. The rule is fixed:
    // points sitting exactly on the parting plane (y = 0) are Radial —
    // material flows sideways into the cavity from the parting line.
    // Anything off the plane is Axial — material flows along the fixture
    // axis (typically vertical) into a top/bottom face. The downstream
    // sprue-generation code in GLCanvas branches on this distinction;
    // see RebuildSpruePath for the radial vs axial path construction.
    //
    // Exposed as a free static helper rather than a setter so callers
    // make the assignment explicit (`p.type = InjectionPoint::TypeFor(p.y)`)
    // — that reads as a one-line invariant restoration at every entry
    // point, and a search for "TypeFor" finds every site that maintains
    // the invariant.
    //
    // Exact equality on y is intentional. User-typed "0" and "0.0" parse
    // to exactly 0.0f via wxString::ToDouble, and points loaded from a
    // fixture file written by this app round-trip exactly. A non-zero
    // typed value (even something tiny like 0.0001) is the user's
    // explicit signal of "off the parting plane" → Axial.
    static InjectionType TypeFor(float y)
    {
        return (y == 0.0f) ? InjectionType::Radial : InjectionType::Axial;
    }
};

// ---------------------------------------------------------------------------
// Per-feature default overrides parsed from the fixture file's optional
// [<feature>_defaults] sections. Each field uses std::optional so the
// fixture can specify exactly which defaults it cares about — anything left
// unset falls back to the application's hardcoded UI defaults at apply
// time. Distance fields are stored in millimetres; angle fields in degrees,
// matching the file format and the rest of the data model.
// ---------------------------------------------------------------------------
struct VentDefaults
{
    std::optional<std::string> type;
    std::optional<float>       length;
    std::optional<float>       width;
    std::optional<float>       overrunStart;
    std::optional<float>       overrunEnd;
};

struct SprueDefaults
{
    std::optional<std::string> type;
    std::optional<float>       diameter;
    std::optional<float>       draftAngle;
    std::optional<float>       coldSlugLength;
    std::optional<float>       length;
    std::optional<float>       overrun;
};

struct RunnerDefaults
{
    std::optional<std::string> type;
    std::optional<float>       diameter;
    std::optional<float>       coldSlugLength;
};

struct GateDefaults
{
    std::optional<std::string> type;
    std::optional<float>       diameter;
    std::optional<float>       draftAngle;
};

struct SubRunnerDefaults
{
    std::optional<std::string> type;
    std::optional<float>       diameter;
};

struct EjectorDefaults
{
    std::optional<std::string> type;
    std::optional<float>       diameter;
    std::optional<float>       length;
};

// ---------------------------------------------------------------------------
// GridDefaults — optional ground-plane grid configuration baked into the
// fixture and applied to the live grid when the fixture is loaded. Same
// presence-driven convention as the feature defaults above: only the fields
// the fixture specifies are set; anything unset falls back to the current
// application grid settings at apply time. Lengths are millimetres; `shape`
// is "rectangular" or "circular"; `spokes`/`majorEvery` are unitless counts.
// ---------------------------------------------------------------------------
struct GridDefaults
{
    std::optional<std::string> shape;       // "rectangular" | "circular"
    std::optional<float>       sizeX;       // mm (rectangular extent X)
    std::optional<float>       sizeY;       // mm (rectangular extent Y = world Z)
    std::optional<float>       radius;      // mm (circular)
    std::optional<int>         spokes;      // circular radial divisions
    std::optional<float>       spacing;     // mm (minor line spacing)
    std::optional<int>         majorEvery;  // major line every N divisions
};

// ---------------------------------------------------------------------------
// HalfTransform — per-half pose stored alongside the modelA / modelB paths.
//
// Captures any positioning the user does in the FixtureEditor (Move,
// Rotate, Scale, Center, Align Face) so a fixture file round-trips back to
// the same scene state. Identity is the default (zero pos, zero rot,
// scale = 1) and skips emission entirely on save — a hand-written fixture
// without a [half_*_transform] section behaves exactly like one with all
// fields explicitly zeroed.
//
// Distance is millimetres; rotation is degrees. Rotation axes are world-
// space X/Y/Z, applied in YXZ order (Ry * Rx * Rz) — matching
// SceneObject::BuildModelMatrix and FixtureCanvas::FixtureMesh::
// BuildModelMatrix. The X/Y/Z names here are chosen for readability in
// the file; the YXZ application order lives in the consuming code, not
// the schema. Doubles, not floats — these are persisted values where the
// ~7 digits of float precision could meaningfully truncate the user's
// alignment work.
// ---------------------------------------------------------------------------
struct HalfTransform
{
    double posX = 0.0;
    double posY = 0.0;
    double posZ = 0.0;
    double rotX = 0.0;     // degrees, around world X (= pitch in canvas)
    double rotY = 0.0;     // degrees, around world Y (= yaw   in canvas)
    double rotZ = 0.0;     // degrees, around world Z (= roll  in canvas)
    double scale = 1.0;

    // True when every field is at its identity value. Used by
    // FixtureFile::Save to skip writing a [half_*_transform] section for
    // halves the user never touched.
    bool IsIdentity() const
    {
        return posX == 0.0 && posY == 0.0 && posZ == 0.0
            && rotX == 0.0 && rotY == 0.0 && rotZ == 0.0
            && scale == 1.0;
    }
};

// ---------------------------------------------------------------------------
// FixtureKind — where a fixture's blank geometry comes from.
//
//   Library    — the two mould halves are loaded from the modelA / modelB
//                STEP (or mesh) files. This is the only kind that exists on
//                disk as a .fixture file and the only kind IsValid() requires
//                model paths for.
//   Parametric — a generic rectangular-prism fixture generated at select time
//                from fixed X/Y/Z dimensions (see ParametricFixtureParams).
//                Centred on the origin and split into two equal halves at the
//                y = 0 parting plane.
//   Dynamic    — a rectangular-prism fixture that resizes to envelope every
//                scene body (with the origin folded in) plus a per-axis
//                minimum clearance (see DynamicFixtureParams). Rebuilt
//                whenever the scene changes.
//
// Parametric and Dynamic fixtures are NEVER read from the fixture library —
// they are chosen directly from the fixture-select menu and their geometry is
// built by GLCanvas::CreateProceduralFixture rather than ImportFileAsFixture.
// modelAPath / modelBPath stay empty for both.
// ---------------------------------------------------------------------------
enum class FixtureKind { Library, Parametric, Dynamic };

// Fixed dimensions for a Parametric fixture. Total extents in millimetres;
// the box is centred on the origin, so each half spans sizeY/2 above and
// below the y = 0 parting plane (and +/-sizeX/2, +/-sizeZ/2 in the other two
// axes).
struct ParametricFixtureParams
{
    float sizeX = 100.0f;   // mm, total width  (X)
    float sizeY = 60.0f;    // mm, total height (Y) — split evenly at y = 0
    float sizeZ = 100.0f;   // mm, total depth  (Z)
};

// Per-axis minimum clearance for a Dynamic fixture, in millimetres. The box
// extends past the scene's bounding box (with the origin folded in as a
// zero-size body) by at least this much on every side — so an empty scene, or
// one where everything sits at the origin, yields a 2*clearance box.
struct DynamicFixtureParams
{
    float clearanceX = 10.0f;
    float clearanceY = 10.0f;
    float clearanceZ = 10.0f;
};

struct FixtureDefinition
{
    std::string modelAPath;   // always stored as absolute internally
    std::string modelBPath;
    std::string fixturePath;  // directory anchor for relative path resolution

    // Which kind of fixture this is. Library (the default) uses the modelA /
    // modelB paths above; Parametric / Dynamic ignore them and generate box
    // geometry from the params below via GLCanvas::CreateProceduralFixture.
    // See FixtureKind.
    FixtureKind kind = FixtureKind::Library;

    // Procedural parameters. Only the struct matching `kind` is meaningful;
    // the other is ignored. Both default to sensible starting dimensions so a
    // freshly-chosen procedural fixture is valid before the user edits it.
    ParametricFixtureParams parametric;
    DynamicFixtureParams    dynamic;

    // Per-half pose. Identity by default — no pose data on disk means the
    // half loads at origin with no rotation / unit scale. The FixtureEditor
    // populates these from canvas state at save time; MainFrame's loader
    // forwards them into GLCanvas::ImportFileAsFixture, which copies them
    // onto the new SceneObject (axis mapping: rotX→pitchDeg, rotY→yawDeg,
    // rotZ→rollDeg, matching the YXZ Euler order used by both
    // BuildModelMatrix implementations).
    HalfTransform halfATransform;
    HalfTransform halfBTransform;

    std::vector<InjectionPoint> injectionPoints;

    // "Fixture Perimeter" injection option. When true, in addition to any fixed
    // injectionPoints above, the user may place the sprue's injection point
    // anywhere on the fixture perimeter (snapping to it) during Edit Sprue >
    // Select Injection Point, and drag it along the perimeter with Move. See
    // GLCanvas::PickActivateInjectionPoint / MoveSprueInjectionPoint. Authored
    // via a checkbox on the FixtureEditor injection-points card.
    bool allowPerimeterInjection = false;

    // Optional per-feature defaults. All fields default-construct to empty
    // optionals — i.e. "no override". MainFrame::ApplyFixtureDefaults walks
    // these and writes only the specified values into the side-panel UI.
    VentDefaults      ventDefaults;
    SprueDefaults     sprueDefaults;
    RunnerDefaults    runnerDefaults;
    GateDefaults      gateDefaults;
    SubRunnerDefaults subRunnerDefaults;
    EjectorDefaults   ejectorDefaults;

    // Optional grid configuration (see GridDefaults). Empty optionals mean
    // "no override" — the live grid keeps its current settings on load.
    GridDefaults      gridDefaults;

    bool IsValid() const
    {
        // Procedural fixtures carry no file paths — their geometry is
        // generated from `kind` + params, so they are always valid. Only a
        // Library fixture needs both half models present.
        if (kind != FixtureKind::Library)
            return true;
        return !modelAPath.empty() && !modelBPath.empty();
    }
};

class FixtureFile
{
public:
    static bool Load(const std::string& path,
        FixtureDefinition& out,
        std::string& error);

    static bool Save(const std::string& path,
        const FixtureDefinition& def,
        std::string& error);

private:
    // Get the directory component of a path
    static std::string GetDirectory(const std::string& path);

    // Make absPath relative to baseDir
    static std::string MakeRelative(const std::string& absPath,
        const std::string& baseDir);

    // Resolve a relative path against baseDir to get an absolute path
    static std::string ResolveRelative(const std::string& relPath,
        const std::string& baseDir);
};