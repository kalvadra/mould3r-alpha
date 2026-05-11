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

struct FixtureDefinition
{
    std::string modelAPath;   // always stored as absolute internally
    std::string modelBPath;
    std::string fixturePath;  // directory anchor for relative path resolution

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

    // Optional per-feature defaults. All fields default-construct to empty
    // optionals — i.e. "no override". MainFrame::ApplyFixtureDefaults walks
    // these and writes only the specified values into the side-panel UI.
    VentDefaults      ventDefaults;
    SprueDefaults     sprueDefaults;
    RunnerDefaults    runnerDefaults;
    GateDefaults      gateDefaults;
    SubRunnerDefaults subRunnerDefaults;
    EjectorDefaults   ejectorDefaults;

    bool IsValid() const
    {
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